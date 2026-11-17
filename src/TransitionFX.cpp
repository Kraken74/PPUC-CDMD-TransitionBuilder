// license:GPLv3+
//
// TransitionFX: a plugin for the PPUC Colorizing DMD editor.
//
// Plugins menu -> "TransitionFX...". One window (a live form of the editor API version 10)
// holds everything: the transition, its option (its direction, its type ...), the number of
// frames, where the new frames go, and a preview of the transition on the author's own frame.
// The fields follow each other while the author fills them in: the options belong to the
// transition, the numbers of frames are exactly those that work for the transition on this
// frame, and the preview shows the frames Apply would make.
//
// The window does not block the editor. It follows the current frame: the author can click
// another frame while it is open, and the window starts from that one.
//
// The plugin then adds that many new frames, right after the starting frame or at the end of the
// project, each a step further along the transition than the one before; or it makes a scene of
// them that the starting frame starts. The last one is not completely black. All of it is one
// step of the editor's history (one Undo).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ColorizingDMDPlugin.h"
#include "Preview.h"
#include "Transitions.h"
#include "plugins/MsgPlugin.h"

namespace {

const MsgPluginAPI* g_msg = nullptr;
const ColorizingDMDEditorAPI* g_editor = nullptr;
uint32_t g_endpoint = 0;
unsigned int g_getApiId = 0;
unsigned int g_selectionChangedId = 0;
unsigned int g_projectChangedId = 0;

constexpr const char* kTitle = "TransitionFX";
constexpr const char* kUndoName = "TransitionFX";

// The fields of the window. Their values are kept as settings when the author clicks Apply, so
// the window opens on the last choices.
constexpr const char* kFieldSource = "source"; // the note that names the starting frame
constexpr const char* kFieldTransition = "transition";
constexpr const char* kFieldOption = "option";
constexpr const char* kFieldTarget = "target"; // "black" or "white": what the transition goes to
constexpr const char* kFieldFrames = "frames";
// A note that draws a thin line between the groups of fields (a note starting with a tag is HTML).
constexpr const char* kSeparator = "<hr>";
// A blank line between the transition and its option; shown with the option only.
constexpr const char* kFieldGap = "gap";
constexpr const char* kGap = "<span>&nbsp;</span>";
constexpr const char* kFieldPlacement = "placement"; // "after", "end" or "scene"
constexpr const char* kFieldDuration = "duration";   // the scene's frame duration
constexpr const char* kFieldPreview = "preview";

// The scenes libserum plays need ROM frames of this size.
constexpr uint32_t kSceneWidth = 128;
constexpr uint32_t kSceneHeight = 32;

// A plane of the colorized frame: its pixels, and the size of the grid they lie on.
struct Plane
{
   int hd = 0;
   uint32_t width = 0;
   uint32_t height = 0;
   std::vector<uint16_t> pixels;
};

// The frame the transition starts from.
struct Source
{
   uint32_t frame = CDMDPI_NONE;
   ColorizingDMDFrameInfo info{};
   uint32_t colorCount = 0;
   uint32_t originalWidth = 0;
   uint32_t originalHeight = 0;
   std::vector<uint8_t> original;
   std::vector<Plane> planes;
   std::vector<uint8_t> zones; // the dynamic zones, set per pixel, 255 for none
   uint32_t zonesWidth = 0;
   uint32_t zonesHeight = 0;
   std::vector<uint16_t> dynamicColors; // colors[set * colorCount + shade]
};

// The second frame of a transition to another frame, made to fit under the first: the dynamic
// zones of both are on one plane, and the dynamic sets that both use for different colors have to
// be told apart.
struct Combined
{
   std::vector<uint8_t> zones1;       // the zones of the first frame on the plane of both (255: none)
   std::vector<uint8_t> zones2;       // those of the second, its sets renamed where they clashed
   uint32_t zonesWidth = 0;
   uint32_t zonesHeight = 0;
   bool hasZones = false;             // either frame has dynamic zones
   // The dynamic sets the new frames get from the second frame: the colors of its sets as they
   // are now numbered (the first frame's own sets come along when a frame is made like it).
   std::vector<std::pair<uint32_t, std::vector<uint16_t>>> sets;
   std::string caveat;
};

// The open window: the frame it starts from (read again whenever the current frame or the
// project changes), and the effect its fields describe.
struct Window
{
   bool open = false;
   Source source;
   std::string sourceProblem; // why there is no source; empty when there is one
   bool scenesPossible = false;
   const transitions::Transition* transition = nullptr;
   const transitions::Option* option = nullptr; // null for a transition without options
   std::unique_ptr<transitions::Effect> effect;
   bool toWhite = false; // the transition goes to white instead of black
   // The transition goes to another frame: the frame you clicked last is the start, the other
   // selected frame the end. `second` is then the end frame and `combined` how it fits under
   // the first one.
   bool toFrame = false;
   Source second;
   std::string secondProblem; // why there is no second frame; empty when there is one
   Combined combined;
   // The number of frames the author asked for: the list shows it whenever the transition can
   // make that many, and the most it can make otherwise.
   std::string framesWanted;
};
Window g_window;

void message(const char* title, const std::string& text)
{
   if (g_editor)
      g_editor->ShowMessage(g_endpoint, title, text.c_str());
}

// Reads a buffer the editor fills: the first call (capacity 0) asks for the size.
template <class T, class Call>
bool fetch(Call call, std::vector<T>& out, uint32_t& width, uint32_t& height)
{
   T probe{};
   width = 0;
   height = 0;
   const uint32_t count = call(&probe, 0u, &width, &height);
   out.clear();
   if (count == 0)
      return false;
   out.assign(count, T{});
   if (call(out.data(), count, &width, &height) != count)
   {
      out.clear();
      return false;
   }
   return true;
}

std::string setting(const char* key, const char* fallback)
{
   char buffer[128];
   const uint32_t size = g_editor->GetSetting(g_endpoint, key, buffer, sizeof(buffer));
   if (size == 0 || size > sizeof(buffer))
      return fallback;
   return buffer;
}

void saveSetting(const char* key, const std::string& value)
{
   g_editor->SetSetting(g_endpoint, key, value.c_str());
}

// What a field of the open window shows now.
std::string formValue(const char* key, const char* fallback)
{
   char buffer[128];
   const uint32_t size = g_editor->GetFormValue(g_endpoint, key, buffer, sizeof(buffer));
   if (size == 0 || size > sizeof(buffer))
      return fallback;
   return buffer;
}

// The option a transition was last used with is kept per transition.
std::string optionKey(const transitions::Transition& transition) { return std::string("option_") + transition.id; }

// What the author chose, in words: "Wipe (Spiral) to White".
std::string describe(const transitions::Transition& transition, const transitions::Option* option,
                     const std::string& target)
{
   std::string what = transition.label;
   if (option)
      what += std::string(" (") + option->label + ")";
   return what + " to " + target;
}

// What the window's "Transition to" says now: "Black", "White" or "frame 12".
std::string targetName()
{
   if (g_window.toFrame)
      return "frame " + std::to_string(g_window.second.frame);
   return g_window.toWhite ? "White" : "Black";
}

// ---- Reading the frame ----------------------------------------------------------------------

// Reads the frame the transition starts from. Returns an empty string when it worked.
std::string loadSource(uint32_t frame, Source& source)
{
   source = Source{};
   ColorizingDMDProjectInfo info{};
   if (!g_editor->GetProjectInfo(&info) || !info.hasProject)
      return "Open a project first.";
   if (frame == CDMDPI_NONE || frame >= info.frameCount)
      return "Select the frame to start the transition from first.";

   source.frame = frame;
   source.colorCount = info.colorCount;
   if (!g_editor->GetFrameInfo(frame, &source.info))
      return "The frame does not exist.";

   if (!fetch<uint8_t>(
          [&](uint8_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
             return g_editor->GetOriginalFrame(frame, p, cap, w, h);
          },
          source.original, source.originalWidth, source.originalHeight))
      return "The editor could not give the original frame.";

   for (int hd = 0; hd <= 1; ++hd)
   {
      if (hd == 1 && !source.info.hasHd)
         continue;
      Plane plane;
      plane.hd = hd;
      if (fetch<uint16_t>(
             [&](uint16_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
                return g_editor->GetColorizedFrame(frame, hd, p, cap, w, h);
             },
             plane.pixels, plane.width, plane.height))
         source.planes.push_back(std::move(plane));
   }
   if (source.planes.empty())
      return "The editor could not give the colorized frame.";

   if (source.info.hasDynamicPixels)
   {
      fetch<uint8_t>(
         [&](uint8_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
            return g_editor->GetDynamicMask(frame, p, cap, w, h);
         },
         source.zones, source.zonesWidth, source.zonesHeight);
      uint16_t probe = 0;
      const uint32_t count = g_editor->GetDynamicColors(frame, &probe, 0);
      if (count > 0)
      {
         source.dynamicColors.assign(count, 0);
         if (g_editor->GetDynamicColors(frame, source.dynamicColors.data(), count) != count)
            source.dynamicColors.clear();
      }
   }
   return std::string();
}

// What the effects need to know about the frame.
// The largest difference between two colors of the same pixel of the two frames, in units of a
// 5-bit channel (green has 6 bits, so it counts half).
uint32_t crossSpanOf(const Source& a, const Source& b)
{
   uint32_t span = 0;
   for (size_t i = 0; i < a.planes.size() && i < b.planes.size(); ++i)
   {
      const std::vector<uint16_t>& x = a.planes[i].pixels;
      const std::vector<uint16_t>& y = b.planes[i].pixels;
      for (size_t k = 0; k < x.size() && k < y.size(); ++k)
      {
         if (x[k] == y[k])
            continue;
         auto diff = [](int u, int v) { return static_cast<uint32_t>(u > v ? u - v : v - u); };
         const uint32_t dr = diff((x[k] >> 11) & 31, (y[k] >> 11) & 31);
         const uint32_t dg = (diff((x[k] >> 5) & 63, (y[k] >> 5) & 63) + 1u) / 2u;
         const uint32_t db = diff(x[k] & 31, y[k] & 31);
         span = std::max({span, dr, dg, db});
      }
   }
   return span;
}

transitions::SourceInfo sourceInfoOf(const Source& source, bool toWhite, const Source* second = nullptr)
{
   transitions::SourceInfo info;
   info.toWhite = toWhite;
   if (second)
   {
      info.toFrame = true;
      info.crossSpan = crossSpanOf(source, *second);
   }
   info.width = source.originalWidth;
   info.height = source.originalHeight;
   info.colorCount = source.colorCount;
   if (!source.original.empty())
   {
      info.brightestShade = *std::max_element(source.original.begin(), source.original.end());
      info.darkestShade = *std::min_element(source.original.begin(), source.original.end());
   }
   std::vector<bool> seen(65536, false);
   for (const Plane& plane : source.planes)
      for (uint16_t pixel : plane.pixels)
         if (!seen[pixel])
         {
            seen[pixel] = true;
            info.colors.push_back(pixel);
         }
   return info;
}

// Puts the second frame under the first: the zones of both on one plane, and where both use the
// same dynamic set for different colors, the second frame's zones moved to a set nobody uses, with
// that set's colors, so the first frame's set gradually gives way to the second frame's.
void combine(const Source& a, const Source& b, Combined& out)
{
   out = Combined{};
   if (a.zones.empty() && b.zones.empty())
      return;
   out.hasZones = true;
   out.zonesWidth = a.zones.empty() ? b.zonesWidth : a.zonesWidth;
   out.zonesHeight = a.zones.empty() ? b.zonesHeight : a.zonesHeight;
   const size_t size = static_cast<size_t>(out.zonesWidth) * out.zonesHeight;
   out.zones1 = a.zones.empty() ? std::vector<uint8_t>(size, 255) : a.zones;
   out.zones2 = b.zones.empty() ? std::vector<uint8_t>(size, 255) : b.zones;

   bool used1[CDMDPI_DYNAMIC_SETS] = {};
   bool used2[CDMDPI_DYNAMIC_SETS] = {};
   for (uint8_t set : out.zones1)
      if (set < CDMDPI_DYNAMIC_SETS)
         used1[set] = true;
   for (uint8_t set : out.zones2)
      if (set < CDMDPI_DYNAMIC_SETS)
         used2[set] = true;

   const uint32_t count = a.colorCount;
   auto colorsOf = [&](const Source& source, uint32_t set) {
      std::vector<uint16_t> colors;
      if (count > 0 && static_cast<size_t>(set + 1) * count <= source.dynamicColors.size())
         colors.assign(source.dynamicColors.begin() + static_cast<size_t>(set) * count,
                       source.dynamicColors.begin() + static_cast<size_t>(set + 1) * count);
      return colors;
   };

   // Where the second frame's sets go: its own number unless the first frame uses that set for
   // other colors.
   uint32_t target[CDMDPI_DYNAMIC_SETS];
   bool taken[CDMDPI_DYNAMIC_SETS] = {};
   for (uint32_t set = 0; set < CDMDPI_DYNAMIC_SETS; ++set)
      target[set] = set;
   std::vector<uint32_t> clashing;
   for (uint32_t set = 0; set < CDMDPI_DYNAMIC_SETS; ++set)
   {
      if (!used2[set])
         continue;
      if (used1[set] && colorsOf(a, set) != colorsOf(b, set))
         clashing.push_back(set);
      else if (!used1[set])
         out.sets.emplace_back(set, colorsOf(b, set)); // its own number is free: just bring the colors
   }
   std::string lost;
   for (uint32_t set : clashing)
   {
      uint32_t free = CDMDPI_NONE;
      for (uint32_t f = 0; f < CDMDPI_DYNAMIC_SETS && free == CDMDPI_NONE; ++f)
         if (!used1[f] && !used2[f] && !taken[f])
            free = f;
      if (free == CDMDPI_NONE)
      {
         target[set] = 255; // no set left: the zone cannot come along
         lost += (lost.empty() ? "" : ", ") + std::to_string(set + 1);
         continue;
      }
      taken[free] = true;
      target[set] = free;
      out.sets.emplace_back(free, colorsOf(b, set));
      out.caveat += "Both frames use dynamic set " + std::to_string(set + 1) + " for different colors: frame " +
                    std::to_string(b.frame) + "'s zones of that set use dynamic set " + std::to_string(free + 1) +
                    " in the new frames.\n";
   }
   if (!lost.empty())
      out.caveat += "No dynamic set was free for frame " + std::to_string(b.frame) + "'s set(s) " + lost +
                    ": those zones are not in the new frames.\n";
   for (uint8_t& set : out.zones2)
      if (set < CDMDPI_DYNAMIC_SETS)
         set = static_cast<uint8_t>(target[set]);
}

// The other selected frame: the frame you clicked last is the start, the other frame of exactly
// two selected frames the end. Returns the reason there is none, or an empty string.
std::string chooseSecond(uint32_t current, uint32_t& second)
{
   second = CDMDPI_NONE;
   std::vector<uint32_t> selected;
   uint32_t probe = 0;
   const uint32_t total = g_editor->GetSelectedFrames(&probe, 0);
   if (total > 0 && total < 100000)
   {
      selected.assign(total, 0);
      selected.resize(std::min(total, g_editor->GetSelectedFrames(selected.data(), total)));
   }
   std::vector<uint32_t> others;
   for (uint32_t frame : selected)
      if (frame != current)
         others.push_back(frame);
   if (others.empty())
      return "Ctrl+click a second frame in the frame list: the frame you clicked last is the start, the "
             "other selected frame the end.";
   if (others.size() > 1)
      return "Select exactly two frames: the frame you clicked last is the start, the other one the end.";
   second = others.front();
   return std::string();
}

// ---- Making the frames ----------------------------------------------------------------------

// What Apply makes.
struct Request
{
   uint32_t steps = 0;
   std::string placement; // "after", "end" or "scene"
   uint32_t sceneDurationMs = 0;
};

// What Apply made, for the status bar.
struct Result
{
   std::vector<uint32_t> shown; // the new frames (of the scene)
   uint32_t sceneId = CDMDPI_NONE;
   std::string caveat;
};

// One colorized plane of a step: the source's, with the second frame underneath when the
// transition goes to another frame, else the black or white fill.
void drawPlane(const Plane& plane, const Plane* under, uint16_t fill, const transitions::Effect& effect,
               uint32_t step, uint32_t steps, std::vector<uint16_t>& out)
{
   const Source& source = g_window.source;
   if (under && effect.crossFades())
      preview::renderCross<uint16_t>(plane.pixels, under->pixels, out, effect, step, steps, preview::mixColor);
   else
      preview::renderStep<uint16_t>(plane.pixels, out, plane.width, plane.height, source.originalWidth,
                                    source.originalHeight, effect, step, steps, fill,
                                    [&](uint16_t color) { return effect.color(color, step, steps); },
                                    under ? &under->pixels : nullptr);
}

// Creates the transition from the window's source frame and effect. Returns an empty string
// when it worked, else what went wrong (and nothing was changed).
std::string createTransition(const Request& request, Result& result)
{
   const Source& source = g_window.source;
   const transitions::Effect& effect = *g_window.effect;
   const uint32_t steps = request.steps;
   const bool scene = request.placement == "scene";

   // The window offers only numbers that work; this is the net under it.
   if (steps < 1 || steps > effect.maxSteps())
      return "This transition can make at most " + std::to_string(effect.maxSteps()) +
             " different frames from this frame.";
   if (scene && !g_window.scenesPossible)
      return "Scenes need a project of 128 x 32 frames.";
   const bool toFrame = g_window.toFrame;
   if (toFrame && !g_window.secondProblem.empty())
      return g_window.secondProblem;
   const Source& second = g_window.second;
   const Combined& combined = g_window.combined;

   if (source.info.background != CDMDPI_NONE)
      result.caveat += "The frame shows a background. Where the background shows through, the pixels "
                       "stay as they are; only the frame's own pixels are changed by the transition.\n";
   if (toFrame && second.info.background != CDMDPI_NONE)
      result.caveat += "Frame " + std::to_string(second.frame) + " shows a background; the new frames keep the "
                       "background of frame " + std::to_string(source.frame) + ".\n";
   if (toFrame)
      result.caveat += combined.caveat;
   if (scene && source.info.triggerId >= 50000 && source.info.triggerId <= 61999)
      result.caveat += "Frame " + std::to_string(source.frame) + " started scene " +
                       std::to_string(source.info.triggerId) + " before; it now starts the new scene.\n";

   if (!g_editor->BeginEdit(g_endpoint, kUndoName))
      return "The editor cannot start an edit right now (another edit is open).";

   auto fail = [&](const std::string& why) {
      g_editor->EndEdit(g_endpoint, 0); // takes every change back
      return why + " Nothing was changed.";
   };

   const uint32_t ow = source.originalWidth, oh = source.originalHeight;
   std::vector<uint8_t> stepOriginal;
   std::vector<uint16_t> stepPixels;
   std::vector<uint8_t> stepZones;
   std::vector<uint16_t> stepColors(source.colorCount);
   std::vector<uint32_t> created;
   // What a pixel the transition has taken away becomes: black (shade 0, RGB565 0) or white (the
   // brightest shade, RGB565 0xFFFF). Such a pixel is in no dynamic zone.
   const uint8_t fillShade = g_window.toWhite ? static_cast<uint8_t>(source.colorCount - 1u) : static_cast<uint8_t>(0);
   const uint16_t fillColor = g_window.toWhite ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(0);

   for (uint32_t step = 1; step <= steps; ++step)
   {
      const std::string which = "frame " + std::to_string(step) + " of " + std::to_string(steps);

      // The original: the pixels taken away get the shade of black (0) or white (the brightest),
      // or the pixel of the other frame; a cross-fade blends the shades.
      if (toFrame && effect.crossFades())
         preview::renderCross<uint8_t>(source.original, second.original, stepOriginal, effect, step, steps,
                                       preview::mixShade);
      else
         preview::renderStep<uint8_t>(source.original, stepOriginal, ow, oh, ow, oh, effect, step, steps, fillShade,
                                      [&](uint8_t shade) { return effect.shade(shade, step, steps); },
                                      toFrame ? &second.original : nullptr);

      // The new frame is made like the source: its settings and pictures come along.
      uint32_t newFrame = CDMDPI_NONE;
      if (!g_editor->AddFrame(g_endpoint, stepOriginal.data(), static_cast<uint32_t>(stepOriginal.size()),
                              source.frame, &newFrame) ||
          newFrame == CDMDPI_NONE)
         return fail("The editor refused to add " + which + ".");
      created.push_back(newFrame);

      // No comparison mask on the new frames (the source's mask came along with the copy). The
      // shape mode stays as it is.
      if (!g_editor->SetFrameComparison(g_endpoint, newFrame, CDMDPI_NONE, source.info.shapeMode))
         return fail("The editor refused to clear the comparison mask of " + which + ".");

      // The colorized planes follow the same decisions as the original.
      for (size_t index = 0; index < source.planes.size(); ++index)
      {
         const Plane& plane = source.planes[index];
         drawPlane(plane, toFrame ? &second.planes[index] : nullptr, fillColor, effect, step, steps, stepPixels);
         if (!g_editor->SetFramePicture(g_endpoint, newFrame, plane.hd, stepPixels.data(), nullptr,
                                        static_cast<uint32_t>(stepPixels.size())))
            return fail("The editor refused the picture of " + which + ".");
      }

      // Dynamic zones color a pixel at run time from its shade, so they move with the picture,
      // and a pixel that was taken away must not stay in a zone.
      if (toFrame ? combined.hasZones : !source.zones.empty())
      {
         const std::vector<uint8_t>& zones = toFrame ? combined.zones1 : source.zones;
         const uint32_t zw = toFrame ? combined.zonesWidth : source.zonesWidth;
         const uint32_t zh = toFrame ? combined.zonesHeight : source.zonesHeight;
         if (toFrame && effect.crossFades())
            preview::renderCross<uint8_t>(zones, combined.zones2, stepZones, effect, step, steps, preview::mixZone);
         else
            preview::renderStep<uint8_t>(zones, stepZones, zw, zh, ow, oh, effect, step, steps, 255,
                                         [](uint8_t set) { return set; }, toFrame ? &combined.zones2 : nullptr);
         if (!g_editor->SetDynamicZones(g_endpoint, newFrame, stepZones.data(), static_cast<uint32_t>(stepZones.size())))
            return fail("The editor refused the dynamic zones of " + which + ".");
      }
      if (toFrame)
      {
         // The sets the second frame's zones use, with their colors; and no color rotation, which
         // would keep turning the colors of the first frame over the second one's pixels.
         for (const auto& entry : combined.sets)
            if (entry.second.size() == source.colorCount &&
                !g_editor->SetDynamicColors(g_endpoint, newFrame, entry.first, entry.second.data(), source.colorCount))
               return fail("The editor refused the dynamic colors of " + which + ".");
         if (!g_editor->ClearColorRotations(g_endpoint, newFrame))
            return fail("The editor refused to clear the color rotations of " + which + ".");
      }

      if (effect.changesColors())
      {
         // The colors a dynamic zone shows are the frame's dynamic colors, so they fade too.
         if (!source.dynamicColors.empty() && source.colorCount > 0)
            for (uint32_t set = 0; set < source.dynamicColors.size() / source.colorCount; ++set)
            {
               for (uint32_t shade = 0; shade < source.colorCount; ++shade)
                  stepColors[shade] =
                     effect.color(source.dynamicColors[static_cast<size_t>(set) * source.colorCount + shade], step, steps);
               if (!g_editor->SetDynamicColors(g_endpoint, newFrame, set, stepColors.data(), source.colorCount))
                  return fail("The editor refused the dynamic colors of " + which + ".");
            }
         // A color rotation would keep turning the frame's colors at full brightness.
         if (!g_editor->ClearColorRotations(g_endpoint, newFrame))
            return fail("The editor refused to clear the color rotations of " + which + ".");
      }
   }

   if (scene)
   {
      // A scene of copies of the new frames, which the starting frame starts. The plain frames
      // were only the material: they go again. The scene's frames lie behind them, so they move
      // down by as many frames.
      ColorizingDMDScene definition{};
      const std::string name = describe(*g_window.transition, g_window.option, targetName());
      definition.sceneId = CDMDPI_NONE;
      definition.name = name.c_str();
      definition.groups = 1;
      definition.durationMs = request.sceneDurationMs;
      definition.repeat = 0;
      definition.finish = CDMDPI_SCENE_KEEP_LAST;
      uint32_t sceneId = CDMDPI_NONE, first = CDMDPI_NONE;
      if (!g_editor->CreateScene(g_endpoint, &definition, created.data(), static_cast<uint32_t>(created.size()),
                                 &sceneId, &first) ||
          sceneId == CDMDPI_NONE || first == CDMDPI_NONE)
         return fail("The editor refused to make the scene.");
      if (!g_editor->SetFrameScene(g_endpoint, source.frame, sceneId))
         return fail("The editor refused to let frame " + std::to_string(source.frame) + " start the scene.");
      if (!g_editor->DeleteFrames(g_endpoint, created.data(), static_cast<uint32_t>(created.size())))
         return fail("The editor refused to delete the frames the scene was made from.");
      first -= static_cast<uint32_t>(created.size());
      for (uint32_t i = 0; i < created.size(); ++i)
         created[i] = first + i;
      result.sceneId = sceneId;
      // The scene's frames lie behind the last frame of the project; move them to just behind
      // the starting frame, as the plain frames go (nothing to do when they already are there).
      if (created.front() != source.frame + 1)
      {
         if (!g_editor->MoveFrames(g_endpoint, created.data(), static_cast<uint32_t>(created.size()), source.frame + 1))
            return fail("The editor refused to move the scene's frames behind the starting frame.");
         for (uint32_t i = 0; i < created.size(); ++i)
            created[i] = source.frame + 1 + i;
      }
   }
   // Right after the starting frame: the new frames were appended, so move them as one block to
   // just behind it (nothing to do when they already are there).
   else if (request.placement == "after" && created.front() != source.frame + 1)
   {
      if (!g_editor->MoveFrames(g_endpoint, created.data(), static_cast<uint32_t>(created.size()), source.frame + 1))
         return fail("The editor refused to move the new frames behind the starting frame.");
      for (uint32_t i = 0; i < created.size(); ++i)
         created[i] = source.frame + 1 + i; // where they are now, in their order
   }

   if (!g_editor->EndEdit(g_endpoint, 1))
      return "The editor did not record the new frames (the status bar may say why). Nothing was changed.";

   result.shown = created;
   g_editor->ShowFrames(result.shown.front(), result.shown.data(), static_cast<uint32_t>(result.shown.size()));
   return std::string();
}

// ---- The window -----------------------------------------------------------------------------

// Changes one field of the open window.
void update(const char* key, const ColorizingDMDFormFieldUpdate& change)
{
   g_editor->SetFormField(g_endpoint, key, &change);
}

ColorizingDMDFormFieldUpdate changeOf(uint32_t changes)
{
   ColorizingDMDFormFieldUpdate change{};
   change.changes = changes;
   return change;
}

std::string optionChoices(const transitions::Transition& transition)
{
   std::string choices;
   for (size_t i = 0; i < transition.optionCount; ++i)
   {
      if (i > 0)
         choices += '\n';
      choices += std::string(transition.options[i].id) + "=" + transition.options[i].label;
   }
   return choices;
}

std::string placementChoices(bool scenes)
{
   std::string choices = "after=Immediately after the starting frame\nend=At the end of the project";
   if (scenes)
      choices += "\nscene=As a scene that the starting frame starts";
   return choices;
}

// The effect of the transition and option the window shows, on the source frame.
void makeEffect()
{
   g_window.effect.reset();
   const bool ready = g_window.sourceProblem.empty() && (!g_window.toFrame || g_window.secondProblem.empty());
   if (ready && g_window.transition)
      g_window.effect = g_window.transition->make(
         g_window.option ? g_window.option->id : "",
         sourceInfoOf(g_window.source, g_window.toWhite, g_window.toFrame ? &g_window.second : nullptr));
}

// Reads the second frame (the other selected frame) and fits it under the first, when the window
// goes to another frame.
void loadSecond()
{
   g_window.second = Source{};
   g_window.combined = Combined{};
   g_window.secondProblem.clear();
   if (!g_window.toFrame || !g_window.sourceProblem.empty())
      return;
   uint32_t frame = CDMDPI_NONE;
   g_window.secondProblem = chooseSecond(g_window.source.frame, frame);
   if (!g_window.secondProblem.empty())
      return;
   g_window.secondProblem = loadSource(frame, g_window.second);
   if (!g_window.secondProblem.empty())
      return;
   const Source& a = g_window.source;
   const Source& b = g_window.second;
   bool same = a.originalWidth == b.originalWidth && a.originalHeight == b.originalHeight &&
               a.planes.size() == b.planes.size();
   for (size_t i = 0; same && i < a.planes.size(); ++i)
      same = a.planes[i].hd == b.planes[i].hd && a.planes[i].width == b.planes[i].width &&
             a.planes[i].height == b.planes[i].height && a.planes[i].pixels.size() == b.planes[i].pixels.size();
   if (same && !a.zones.empty() && !b.zones.empty())
      same = a.zonesWidth == b.zonesWidth && a.zonesHeight == b.zonesHeight;
   if (!same)
   {
      g_window.secondProblem = "Frame " + std::to_string(b.frame) + " is not made like frame " + std::to_string(a.frame) +
                               " (another size, or only one of them has an HD picture).";
      return;
   }
   combine(a, b, g_window.combined);
}

uint32_t mostFrames() { return g_window.effect ? g_window.effect->maxSteps() : 0; }

// The frames list: exactly 1 .. the most this transition makes from this frame. A list cannot
// be empty: when the transition can do nothing here it holds one entry that says so.
std::string framesChoices()
{
   const uint32_t most = mostFrames();
   if (g_window.toFrame && !g_window.secondProblem.empty())
      return "0=None (select the second frame first)";
   if (most < 1)
      return "0=None (this transition can do nothing with this frame)";
   std::string choices;
   for (uint32_t n = 1; n <= most; ++n)
   {
      if (n > 1)
         choices += '\n';
      choices += std::to_string(n);
   }
   return choices;
}

std::string framesHelp()
{
   if (g_window.toFrame && !g_window.secondProblem.empty())
      return g_window.secondProblem;
   if (!g_window.effect)
      return std::string();
   if (mostFrames() < 1)
      return "This transition can do nothing with this frame (" + std::string(g_window.effect->limitReason()) +
             "). For a fade this means the picture is already all black, or all white" +
             (g_window.toFrame ? ", or the two frames hardly differ." : ".");
   return "Up to " + std::to_string(mostFrames()) + " different frames are possible from this frame (" +
          g_window.effect->limitReason() + ").";
}

// The number of frames to open the list on: the one asked for, within what works here.
std::string framesValue(const std::string& wanted)
{
   const uint32_t most = mostFrames();
   if (most < 1)
      return "0";
   const long number = std::strtol(wanted.c_str(), nullptr, 10);
   return std::to_string(std::clamp<long>(number, 1, most));
}

std::string sourceNote()
{
   if (!g_window.sourceProblem.empty())
      return g_window.sourceProblem;
   if (g_window.toFrame)
      return g_window.secondProblem.empty()
                ? "From frame " + std::to_string(g_window.source.frame) + " to frame " +
                     std::to_string(g_window.second.frame) +
                     ". The frame you clicked last is the start, the other selected frame the end."
                : "Starting from frame " + std::to_string(g_window.source.frame) + ". " + g_window.secondProblem;
   return "Starting from frame " + std::to_string(g_window.source.frame) +
          ". Click another frame in the frame list to start from that one.";
}

// The preview: a few steps of the transition, drawn from the source frame's first colorized
// plane as Apply would draw them.
struct PreviewPictures
{
   std::vector<std::vector<uint16_t>> pixels;
   std::vector<std::string> captions;
   std::vector<ColorizingDMDFormImage> images;
};

PreviewPictures makePreview(uint32_t steps)
{
   PreviewPictures out;
   if (!g_window.effect || steps < 1 || steps > mostFrames() || g_window.source.planes.empty())
      return out;
   const Plane& plane = g_window.source.planes.front();
   const transitions::Effect& effect = *g_window.effect;
   const uint16_t fillColor = g_window.toWhite ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(0);
   const std::vector<uint32_t> shown = preview::previewSteps(steps);
   out.pixels.resize(shown.size());
   out.captions.resize(shown.size());
   for (size_t i = 0; i < shown.size(); ++i)
   {
      const uint32_t step = shown[i];
      drawPlane(plane, g_window.toFrame ? &g_window.second.planes.front() : nullptr, fillColor, effect, step, steps,
                out.pixels[i]);
      out.captions[i] = "Frame " + std::to_string(step) + " of " + std::to_string(steps);
   }
   // The images point into the pixels and captions, which are complete now.
   for (size_t i = 0; i < shown.size(); ++i)
      out.images.push_back({out.pixels[i].data(), plane.width, plane.height, preview::previewScale(plane.width),
                            out.captions[i].c_str()});
   return out;
}

void updatePreview()
{
   const uint32_t steps = static_cast<uint32_t>(std::strtol(formValue(kFieldFrames, "0").c_str(), nullptr, 10));
   const PreviewPictures pictures = makePreview(steps);
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_IMAGES | CDMDPI_FORM_SET_VISIBLE);
   change.visible = pictures.images.empty() ? 0 : 1;
   change.images = pictures.images.data();
   change.imageCount = static_cast<uint32_t>(pictures.images.size());
   update(kFieldPreview, change);
}

// The frames list for the effect the window shows now, keeping the number chosen when it still
// works; then the preview.
void updateFrames()
{
   const std::string& wanted = g_window.framesWanted;
   const std::string choices = framesChoices();
   const std::string help = framesHelp();
   const std::string value = framesValue(wanted);
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_CHOICES | CDMDPI_FORM_SET_VALUE | CDMDPI_FORM_SET_HELP);
   change.choices = choices.c_str();
   change.value = value.c_str();
   change.help = help.c_str();
   update(kFieldFrames, change);
   updatePreview();
}

// The option field for the transition the window shows: its own label and list, the option it
// was used with last; hidden for a transition without options.
void updateOption()
{
   const transitions::Transition& transition = *g_window.transition;
   if (transition.optionCount == 0)
   {
      g_window.option = nullptr;
      ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_VISIBLE);
      change.visible = 0;
      update(kFieldOption, change);
      update(kFieldGap, change);
      return;
   }
   g_window.option = transitions::findOption(transition, setting(optionKey(transition).c_str(), "").c_str());
   const std::string choices = optionChoices(transition);
   const std::string help = transition.description;
   ColorizingDMDFormFieldUpdate change =
      changeOf(CDMDPI_FORM_SET_VISIBLE | CDMDPI_FORM_SET_LABEL | CDMDPI_FORM_SET_CHOICES | CDMDPI_FORM_SET_VALUE |
               CDMDPI_FORM_SET_HELP);
   change.visible = 1;
   change.label = transition.optionLabel;
   change.choices = choices.c_str();
   change.value = g_window.option->id;
   change.help = help.c_str();
   update(kFieldOption, change);
   ColorizingDMDFormFieldUpdate gap = changeOf(CDMDPI_FORM_SET_VISIBLE);
   gap.visible = 1;
   update(kFieldGap, gap);
}

void updateDuration()
{
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_VISIBLE);
   change.visible = formValue(kFieldPlacement, "after") == "scene" ? 1 : 0;
   update(kFieldDuration, change);
}

// The current frame or the project changed: the window starts from the current frame now.
void reloadSource()
{
   g_window.sourceProblem = loadSource(g_editor->GetCurrentFrame(), g_window.source);
   loadSecond();
   ColorizingDMDProjectInfo info{};
   g_window.scenesPossible = g_editor->GetProjectInfo(&info) && info.hasProject &&
                             info.originalWidth == kSceneWidth && info.originalHeight == kSceneHeight;
   makeEffect();
}

void refreshWindow()
{
   reloadSource();
   const std::string note = sourceNote();
   ColorizingDMDFormFieldUpdate change = changeOf(CDMDPI_FORM_SET_LABEL);
   change.label = note.c_str();
   update(kFieldSource, change);
   updateFrames();
}

// The author changed a field.
void MSGPIAPI onFieldChanged(void* /*userData*/, const char* key)
{
   if (!g_editor || !g_window.open)
      return;
   const std::string field = key ? key : "";
   if (field == kFieldTransition)
   {
      const transitions::Transition* transition =
         transitions::findTransition(formValue(kFieldTransition, "").c_str());
      if (!transition)
         return;
      g_window.transition = transition;
      updateOption();
      makeEffect();
      updateFrames();
   }
   else if (field == kFieldOption && g_window.transition)
   {
      g_window.option = transitions::findOption(*g_window.transition, formValue(kFieldOption, "").c_str());
      if (g_window.option)
         saveSetting(optionKey(*g_window.transition).c_str(), g_window.option->id);
      makeEffect();
      updateFrames();
   }
   else if (field == kFieldTarget)
   {
      g_window.toWhite = formValue(kFieldTarget, "black") == "white";
      g_window.toFrame = formValue(kFieldTarget, "black") == "frame";
      refreshWindow(); // another frame needs the second frame read, and the note names it
   }
   else if (field == kFieldFrames)
   {
      g_window.framesWanted = formValue(kFieldFrames, "10");
      updatePreview();
   }
   else if (field == kFieldPlacement)
      updateDuration();
}

// The window was closed: with Apply, make the frames.
void MSGPIAPI onWindowClosed(void* /*userData*/, uint32_t result)
{
   g_window.open = false;
   if (result != CDMDPI_FORM_ACCEPTED || !g_editor)
      return;

   // The project may have changed since the window last looked: start from what is there now.
   g_window.toFrame = formValue(kFieldTarget, "black") == "frame";
   g_window.toWhite = formValue(kFieldTarget, "black") == "white";
   reloadSource();
   if (!g_window.sourceProblem.empty())
   {
      message(kTitle, g_window.sourceProblem);
      return;
   }
   if (g_window.toFrame && !g_window.secondProblem.empty())
   {
      message(kTitle, g_window.secondProblem);
      return;
   }
   g_window.transition = transitions::findTransition(formValue(kFieldTransition, "").c_str());
   if (!g_window.transition)
   {
      message(kTitle, "Please choose a transition from the list.");
      return;
   }
   g_window.option = transitions::findOption(*g_window.transition, formValue(kFieldOption, "").c_str());
   if (g_window.option)
      saveSetting(optionKey(*g_window.transition).c_str(), g_window.option->id);
   makeEffect();

   const std::string what = describe(*g_window.transition, g_window.option, targetName());
   if (mostFrames() < 1)
   {
      message(kTitle, what + " can do nothing with this frame (" + g_window.effect->limitReason() +
                         "). For a fade this means the picture is already all black, or all white.");
      return;
   }

   Request request;
   const std::string framesText = formValue(kFieldFrames, "");
   char* end = nullptr;
   const long frames = std::strtol(framesText.c_str(), &end, 10);
   if (framesText.empty() || *end != '\0' || frames < 1)
   {
      message(kTitle, "Please choose the number of frames from the list.");
      return;
   }
   request.steps = static_cast<uint32_t>(frames);
   request.placement = formValue(kFieldPlacement, "after");
   if (request.placement == "scene")
   {
      const std::string durationText = formValue(kFieldDuration, "");
      const long duration = std::strtol(durationText.c_str(), &end, 10);
      if (durationText.empty() || *end != '\0' || duration < 1 || duration > 65535)
      {
         message(kTitle, "The frame duration of the scene is a number of milliseconds from 1 to 65535.");
         return;
      }
      request.sceneDurationMs = static_cast<uint32_t>(duration);
   }

   Result made;
   const std::string problem = createTransition(request, made);
   if (!problem.empty())
   {
      message(kTitle, problem);
      return;
   }

   std::string status = std::string(kTitle) + ": added " + std::to_string(frames) + " frame(s) of " + what;
   if (made.sceneId != CDMDPI_NONE)
      status += " as scene " + std::to_string(made.sceneId) + " (frames " + std::to_string(made.shown.front()) +
                " to " + std::to_string(made.shown.back()) + "), started by frame " +
                std::to_string(g_window.source.frame);
   else
      status += ", as frames " + std::to_string(made.shown.front()) + " to " + std::to_string(made.shown.back());
   g_editor->ShowStatus(g_endpoint, (status + ". Undo takes them back.").c_str());
   if (!made.caveat.empty())
      message(kTitle, made.caveat);
}

// The menu entry: opens the window on the current frame and the last choices.
void MSGPIAPI onMenuEntry(void* /*userData*/)
{
   if (!g_editor || g_window.open)
      return;

   g_window.toFrame = setting(kFieldTarget, "black") == "frame";
   reloadSource();
   if (!g_window.sourceProblem.empty())
   {
      message(kTitle, g_window.sourceProblem);
      return;
   }

   g_window.transition = transitions::findTransition(setting(kFieldTransition, "").c_str());
   if (!g_window.transition)
      g_window.transition = &transitions::kTransitions[0];
   const transitions::Transition& transition = *g_window.transition;
   g_window.option =
      transition.optionCount ? transitions::findOption(transition, setting(optionKey(transition).c_str(), "").c_str())
                             : nullptr;
   g_window.toWhite = setting(kFieldTarget, "black") == "white";
   makeEffect();

   std::string transitionChoices;
   for (const auto& entry : transitions::kTransitions)
   {
      if (!transitionChoices.empty())
         transitionChoices += '\n';
      transitionChoices += std::string(entry.id) + "=" + entry.label;
   }
   const std::string note = sourceNote();
   const std::string options = transition.optionCount ? optionChoices(transition) : "-";
   const std::string optionValue = g_window.option ? g_window.option->id : "-";
   const std::string optionHelp = transition.description;
   const std::string frames = framesChoices();
   g_window.framesWanted = setting(kFieldFrames, "10");
   const std::string framesStart = framesValue(g_window.framesWanted);
   const std::string help = framesHelp();
   std::string placement = setting(kFieldPlacement, "after");
   if (placement != "end" && !(placement == "scene" && g_window.scenesPossible))
      placement = "after";
   const std::string placements = placementChoices(g_window.scenesPossible);
   std::string duration = setting(kFieldDuration, "");
   if (duration.empty())
      duration = std::to_string(g_window.source.info.durationMs ? g_window.source.info.durationMs : 33);
   const PreviewPictures pictures = makePreview(static_cast<uint32_t>(std::strtol(framesStart.c_str(), nullptr, 10)));

   const ColorizingDMDFormField fields[] = {
      {kFieldSource, note.c_str(), CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldTransition, "Transition", CDMDPI_FIELD_RADIO, transition.id, transitionChoices.c_str(), nullptr, 0,
       nullptr, 0},
      {kFieldGap, kGap, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, transition.optionCount ? 0u : CDMDPI_FORM_HIDDEN,
       nullptr, 0},
      {kFieldOption, transition.optionLabel ? transition.optionLabel : "Option", CDMDPI_FIELD_RADIO,
       optionValue.c_str(), options.c_str(), optionHelp.c_str(), transition.optionCount ? 0u : CDMDPI_FORM_HIDDEN,
       nullptr, 0},
      {"sep1", kSeparator, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldTarget, "Transition to", CDMDPI_FIELD_RADIO,
       g_window.toFrame ? "frame" : (g_window.toWhite ? "white" : "black"),
       "black=Black\nwhite=White\nframe=Another frame", nullptr, 0, nullptr, 0},
      {"sep2", kSeparator, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldFrames, "Number of frames", CDMDPI_FIELD_CHOICE, framesStart.c_str(), frames.c_str(), help.c_str(), 0,
       nullptr, 0},
      {"sep3", kSeparator, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldPlacement, "Place the new frames", CDMDPI_FIELD_RADIO, placement.c_str(), placements.c_str(),
       "A scene is made of the new frames, and the starting frame starts it. Scenes need a project of "
       "128 x 32 frames.",
       0, nullptr, 0},
      {kFieldDuration, "Frame duration of the scene (ms)", CDMDPI_FIELD_TEXT, duration.c_str(), nullptr,
       "How long each frame of the scene shows. The other settings of the scene are the editor's defaults; "
       "change them in the Scenes section.",
       placement == "scene" ? 0u : CDMDPI_FORM_HIDDEN, nullptr, 0},
      {"sep4", kSeparator, CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr, 0, nullptr, 0},
      {kFieldPreview, "Preview", CDMDPI_FIELD_IMAGE, nullptr, nullptr, nullptr,
       pictures.images.empty() ? CDMDPI_FORM_HIDDEN : 0u, pictures.images.data(),
       static_cast<uint32_t>(pictures.images.size())},
   };
   if (!g_editor->ShowLiveForm(g_endpoint, kTitle, "Apply", nullptr, 0, fields,
                               static_cast<uint32_t>(sizeof(fields) / sizeof(fields[0])), onFieldChanged,
                               onWindowClosed, nullptr))
   {
      message(kTitle, "The window could not be opened. Close other plugin windows and try again.");
      return;
   }
   g_window.open = true;
}

// The editor's events: while the window is open, it follows the current frame and the project.
void MSGPIAPI onEditorEvent(const unsigned int /*msgId*/, void* /*context*/, void* /*msgData*/)
{
   if (g_editor && g_window.open)
      refreshWindow();
}

} // namespace

MSGPI_EXPORT void MSGPIAPI TransitionFXPluginLoad(const uint32_t endpointId, const MsgPluginAPI* api)
{
   g_msg = api;
   g_endpoint = endpointId;

   // Ask the editor for its plugin API (see the header of ColorizingDMDPlugin.h).
   const ColorizingDMDEditorAPI* editor = nullptr;
   g_getApiId = g_msg->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_MSG_GET_API);
   g_msg->BroadcastMsg(g_endpoint, g_getApiId, &editor);
   if (!editor || editor->version < 10)
   {
      // Not hosted by the PPUC Colorizing DMD editor, or by one too old for this plugin: it
      // needs the live forms and scenes of version 9 and the radio buttons of version 10.
      g_editor = nullptr;
      return;
   }
   g_editor = editor;
   g_selectionChangedId = g_msg->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_EVT_ON_SELECTION_CHANGED);
   g_projectChangedId = g_msg->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_EVT_ON_PROJECT_CHANGED);
   g_msg->SubscribeMsg(g_endpoint, g_selectionChangedId, onEditorEvent, nullptr);
   g_msg->SubscribeMsg(g_endpoint, g_projectChangedId, onEditorEvent, nullptr);
   g_editor->AddMenuEntry(g_endpoint, "TransitionFX...", onMenuEntry, nullptr);
}

MSGPI_EXPORT void MSGPIAPI TransitionFXPluginUnload()
{
   if (g_editor)
   {
      if (g_window.open)
         g_editor->CloseForm(g_endpoint);
      g_editor->RemoveMenuEntries(g_endpoint);
      g_msg->UnsubscribeMsg(g_selectionChangedId, onEditorEvent, nullptr);
      g_msg->UnsubscribeMsg(g_projectChangedId, onEditorEvent, nullptr);
      g_msg->ReleaseMsgID(g_selectionChangedId);
      g_msg->ReleaseMsgID(g_projectChangedId);
   }
   if (g_msg)
      g_msg->ReleaseMsgID(g_getApiId);
   g_window = Window{};
   g_editor = nullptr;
   g_msg = nullptr;
}
