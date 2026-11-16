// license:GPLv3+
//
// Transition Builder: a plugin for the PPUC Colorizing DMD editor.
//
// Plugins menu -> "Transition Builder...". The author goes through up to three windows:
//
//   1. the transition (and whether to show pictures of them),
//   2. the transition's option: its direction, its type ... (not for every transition),
//   3. the number of frames, from a list that holds exactly the numbers that work.
//
// The editor's windows cannot change one field when another does, so what depends on an earlier
// choice (the options, the most frames) is in the next window. The plugin then adds that many
// new frames, right after the starting frame or at the end of the project as the author chose,
// each a step further along the transition than the one before. The last one is not completely black. All of it is one step of the editor's
// history (one Undo).

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

constexpr const char* kTitle = "Transition Builder";
constexpr const char* kSettingTransition = "transition";
constexpr const char* kSettingPictures = "pictures";
constexpr const char* kSettingFrames = "frames";
constexpr const char* kSettingBack = "back"; // the tick box "go back" of the second and third window
constexpr const char* kSettingPlacement = "placement"; // "after" the starting frame, or "end"
constexpr const char* kUndoName = "Transition Builder";

// A plane of the colorized frame: its pixels, and the size of the grid they lie on.
struct Plane
{
   int hd = 0;
   uint32_t width = 0;
   uint32_t height = 0;
   std::vector<uint16_t> pixels;
};

// The frame the transition starts from, read when the first window opens. The windows do not
// block the editor, so the project can change while they are open; the revision tells.
struct Source
{
   uint32_t frame = CDMDPI_NONE;
   uint64_t revision = 0;
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

// Where the author is in the windows.
struct Wizard
{
   Source source;
   const transitions::Transition* transition = nullptr;
   const transitions::Option* option = nullptr; // null for a transition without options
   std::unique_ptr<transitions::Effect> effect;
};
Wizard g_wizard;

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

bool picturesWanted() { return setting(kSettingPictures, "1") != "0"; }

// ---- Reading the frame ----------------------------------------------------------------------

// Reads the frame the transition starts from. Returns an empty string when it worked.
std::string loadSource(uint32_t frame, Source& source)
{
   ColorizingDMDProjectInfo info{};
   if (!g_editor->GetProjectInfo(&info) || !info.hasProject)
      return "Open a project first.";
   if (frame == CDMDPI_NONE || frame >= info.frameCount)
      return "Select the frame to start the transition from first.";

   source = Source{};
   source.frame = frame;
   source.revision = info.revision;
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
transitions::SourceInfo sourceInfoOf(const Source& source)
{
   transitions::SourceInfo info;
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

// ---- Making the frames ----------------------------------------------------------------------

// Makes the picture of one step: `source` is a picture of width x height pixels (the original
// frame, a colorized plane or the dynamic zones). The effect decides on the original frame's
// grid (originalWidth x originalHeight) where each pixel comes from. A picture on a finer grid
// (the HD plane is twice as large) follows the same decision, and a pixel keeps its place inside
// its cell, so a move is as smooth as the picture is fine. Pixels that show nothing get
// `black`; `recolor` changes the color of the ones that do (the fade).
template <class T, class Recolor>
void renderStep(const std::vector<T>& source, std::vector<T>& out, uint32_t width, uint32_t height,
                uint32_t originalWidth, uint32_t originalHeight, const transitions::Effect& effect,
                uint32_t step, uint32_t steps, T black, Recolor recolor)
{
   out.assign(source.size(), black);
   for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
      {
         const uint32_t cellX = static_cast<uint32_t>(static_cast<uint64_t>(x) * originalWidth / width);
         const uint32_t cellY = static_cast<uint32_t>(static_cast<uint64_t>(y) * originalHeight / height);
         const transitions::Rule from = effect.rule(cellX, cellY, step, steps);
         if (!from.visible)
            continue;
         // The pixel's place inside its cell, then the same place in the source cell.
         const uint32_t insideX = x - static_cast<uint32_t>(static_cast<uint64_t>(cellX) * width / originalWidth);
         const uint32_t insideY = y - static_cast<uint32_t>(static_cast<uint64_t>(cellY) * height / originalHeight);
         const uint32_t sourceX = std::min(
            width - 1, static_cast<uint32_t>(static_cast<uint64_t>(from.x) * width / originalWidth) + insideX);
         const uint32_t sourceY = std::min(
            height - 1, static_cast<uint32_t>(static_cast<uint64_t>(from.y) * height / originalHeight) + insideY);
         out[static_cast<size_t>(y) * width + x] = recolor(source[static_cast<size_t>(sourceY) * width + sourceX]);
      }
}

// Creates the transition from the wizard's source frame and effect. Returns an empty string
// when it worked, else what went wrong (and nothing was changed).
std::string createTransition(uint32_t steps, bool placeAfter, bool& hadCaveat, std::string& caveat,
                             std::vector<uint32_t>& shown)
{
   const Source& source = g_wizard.source;
   const transitions::Effect& effect = *g_wizard.effect;

   ColorizingDMDProjectInfo info{};
   if (!g_editor->GetProjectInfo(&info) || !info.hasProject)
      return "No project is open.";
   if (info.revision != source.revision || source.frame >= info.frameCount)
      return "The project changed while the windows were open. Please start again.";
   // The window offers only numbers that work; this is the net under it.
   if (steps < 1 || steps > effect.maxSteps())
      return "This transition can make at most " + std::to_string(effect.maxSteps()) +
             " different frames from this frame. Please start again.";

   if (source.info.background != CDMDPI_NONE)
   {
      hadCaveat = true;
      caveat += "The frame shows a background. Where the background shows through, the pixels "
                "stay as they are; only the frame's own pixels are changed by the transition.\n";
   }

   if (!g_editor->BeginEdit(g_endpoint, kUndoName))
      return "The editor cannot start an edit right now (another edit is open).";

   auto fail = [&](const std::string& why) {
      g_editor->EndEdit(g_endpoint, 0); // takes every change back
      return why;
   };

   const uint32_t ow = source.originalWidth, oh = source.originalHeight;
   std::vector<uint8_t> stepOriginal;
   std::vector<uint16_t> stepPixels;
   std::vector<uint8_t> stepZones;
   std::vector<uint16_t> stepColors(source.colorCount);
   std::vector<uint32_t> created;

   for (uint32_t step = 1; step <= steps; ++step)
   {
      const std::string which = "frame " + std::to_string(step) + " of " + std::to_string(steps);

      // The original: black pixels are shade 0, so every new frame differs from the others.
      renderStep<uint8_t>(source.original, stepOriginal, ow, oh, ow, oh, effect, step, steps, 0,
                          [&](uint8_t shade) { return effect.shade(shade, step, steps); });

      // The new frame is made like the source: its settings and pictures come along.
      uint32_t newFrame = CDMDPI_NONE;
      if (!g_editor->AddFrame(g_endpoint, stepOriginal.data(), static_cast<uint32_t>(stepOriginal.size()),
                              source.frame, &newFrame) ||
          newFrame == CDMDPI_NONE)
         return fail("The editor refused to add " + which + ". Nothing was changed.");
      created.push_back(newFrame);

      // No comparison mask on the new frames (the source's mask came along with the copy). The
      // shape mode stays as it is.
      if (!g_editor->SetFrameComparison(g_endpoint, newFrame, CDMDPI_NONE, source.info.shapeMode))
         return fail("The editor refused to clear the comparison mask of " + which + ". Nothing was changed.");

      // The colorized planes follow the same decisions as the original.
      for (const Plane& plane : source.planes)
      {
         renderStep<uint16_t>(plane.pixels, stepPixels, plane.width, plane.height, ow, oh, effect, step, steps,
                              0, // RGB565 black
                              [&](uint16_t color) { return effect.color(color, step, steps); });
         if (!g_editor->SetFramePicture(g_endpoint, newFrame, plane.hd, stepPixels.data(), nullptr,
                                        static_cast<uint32_t>(stepPixels.size())))
            return fail("The editor refused the picture of " + which + ". Nothing was changed.");
      }

      // Dynamic zones color a pixel at run time from its shade, so they move with the picture,
      // and a black pixel must not stay in a zone.
      if (!source.zones.empty())
      {
         renderStep<uint8_t>(source.zones, stepZones, source.zonesWidth, source.zonesHeight, ow, oh, effect,
                             step, steps, 255, [](uint8_t set) { return set; });
         if (!g_editor->SetDynamicZones(g_endpoint, newFrame, stepZones.data(), static_cast<uint32_t>(stepZones.size())))
            return fail("The editor refused the dynamic zones of " + which + ". Nothing was changed.");
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
                  return fail("The editor refused the dynamic colors of " + which + ". Nothing was changed.");
            }
         // A color rotation would keep turning the frame's colors at full brightness.
         if (!g_editor->ClearColorRotations(g_endpoint, newFrame))
            return fail("The editor refused to clear the color rotations of " + which + ". Nothing was changed.");
      }
   }

   // Right after the starting frame: the new frames were appended, so move them as one block to
   // just behind it (nothing to do when they already are there).
   if (placeAfter && created.front() != source.frame + 1)
   {
      if (!g_editor->MoveFrames(g_endpoint, created.data(), static_cast<uint32_t>(created.size()), source.frame + 1))
         return fail("The editor refused to move the new frames behind the starting frame. Nothing was changed.");
      for (uint32_t i = 0; i < created.size(); ++i)
         created[i] = source.frame + 1 + i; // where they are now, in their order
   }

   if (!g_editor->EndEdit(g_endpoint, 1))
      return "The editor did not record the new frames (the status bar may say why). Nothing was changed.";

   shown = created;
   g_editor->ShowFrames(shown.front(), shown.data(), static_cast<uint32_t>(shown.size()));
   return std::string();
}

// ---- The windows ----------------------------------------------------------------------------

void showFramesForm();
void showOptionForm();
void showTransitionForm();

// The editor's windows have one button besides Cancel, so "Back" is a tick box: ticked, the
// button of the window goes back one window instead of on. It is cleared whenever it is read.
bool backRequested()
{
   const bool back = setting(kSettingBack, "0") == "1";
   saveSetting(kSettingBack, "0");
   return back;
}

const ColorizingDMDSettingField kBackField = {
   kSettingBack, "Go back to the previous window", CDMDPI_FIELD_CHECKBOX, "0", nullptr,
   "Tick this and click the button to go back one window instead of on. Your choices are kept."};

// The third window was closed: make the frames.
void MSGPIAPI onFramesChosen(void* /*userData*/, int saved)
{
   if (!saved || !g_editor || !g_wizard.effect)
      return;
   if (backRequested())
   {
      if (g_wizard.transition && g_wizard.transition->optionCount > 0)
         showOptionForm();
      else
         showTransitionForm();
      return;
   }

   const std::string framesText = setting(kSettingFrames, "");
   char* end = nullptr;
   const long frames = std::strtol(framesText.c_str(), &end, 10);
   if (framesText.empty() || *end != '\0' || frames < 1)
   {
      message(kTitle, "Please choose the number of frames from the list.");
      return;
   }

   bool hadCaveat = false;
   std::string caveat;
   std::vector<uint32_t> shown;
   const bool placeAfter = setting(kSettingPlacement, "after") != "end";
   const std::string problem = createTransition(static_cast<uint32_t>(frames), placeAfter, hadCaveat, caveat, shown);
   if (!problem.empty())
   {
      message(kTitle, problem);
      return;
   }

   std::string what = g_wizard.transition->label;
   if (g_wizard.option)
      what += std::string(" (") + g_wizard.option->label + ")";
   g_editor->ShowStatus(g_endpoint, (std::string(kTitle) + ": added " + std::to_string(frames) + " frame(s) of " +
                                     what + ", as frames " + std::to_string(shown.front()) + " to " +
                                     std::to_string(shown.back()) + ". Undo takes them back.")
                                       .c_str());
   if (hadCaveat)
      message(kTitle, caveat);
}

// The third window: the number of frames, from a list of exactly the numbers that work.
void showFramesForm()
{
   const transitions::Transition& transition = *g_wizard.transition;
   g_wizard.effect = transition.make(g_wizard.option ? g_wizard.option->id : "", sourceInfoOf(g_wizard.source));
   const uint32_t most = g_wizard.effect->maxSteps();

   std::string what = transition.label;
   if (g_wizard.option)
      what += std::string(" (") + g_wizard.option->label + ")";

   if (most < 1)
   {
      message(kTitle, what + " can do nothing with this frame (" + g_wizard.effect->limitReason() +
                         "). For a fade this means the picture is already all black, or all white.");
      return;
   }

   // The number the list opens on: the last one chosen, or 10, within what works here. The
   // editor opens a list on its first entry when the stored value is not in it.
   const long remembered = std::strtol(setting(kSettingFrames, "10").c_str(), nullptr, 10);
   const uint32_t start = static_cast<uint32_t>(std::clamp<long>(remembered, 1, most));
   saveSetting(kSettingFrames, std::to_string(start));
   saveSetting(kSettingBack, "0");

   std::string choices;
   for (uint32_t n = 1; n <= most; ++n)
   {
      if (n > 1)
         choices += '\n';
      choices += std::to_string(n);
   }

   const std::string note = what + ": from frame " + std::to_string(g_wizard.source.frame) + ", up to " +
                            std::to_string(most) + " different frames are possible (" +
                            g_wizard.effect->limitReason() + ").";
   // The form points into these strings, so they must live until ShowForm has returned.
   const std::string startText = std::to_string(start);
   const ColorizingDMDSettingField fields[] = {
      {nullptr, note.c_str(), CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr},
      {kSettingFrames, "Number of frames", CDMDPI_FIELD_CHOICE, startText.c_str(), choices.c_str(), nullptr},
      {kSettingPlacement, "Place the new frames", CDMDPI_FIELD_CHOICE, "after",
       "after=Right after the starting frame\nend=At the end of the project", nullptr},
      kBackField,
   };
   if (!g_editor->ShowForm(g_endpoint, what.c_str(), "Apply", fields, 4, onFramesChosen, nullptr))
      message(kTitle, "The window could not be opened. Close other plugin windows and try again.");
}

std::string optionKey(const transitions::Transition& transition) { return std::string("option_") + transition.id; }

// The second window was closed: the option is chosen, ask for the frames.
void MSGPIAPI onOptionChosen(void* /*userData*/, int saved)
{
   if (!saved || !g_editor || !g_wizard.transition)
      return;
   if (backRequested())
   {
      showTransitionForm();
      return;
   }
   g_wizard.option = transitions::findOption(*g_wizard.transition, setting(optionKey(*g_wizard.transition).c_str(), "").c_str());
   showFramesForm();
}

// The second window: the transition's option (its direction, its type ...).
void showOptionForm()
{
   const transitions::Transition& transition = *g_wizard.transition;
   std::string choices;
   for (size_t i = 0; i < transition.optionCount; ++i)
   {
      if (i > 0)
         choices += '\n';
      choices += std::string(transition.options[i].id) + "=" + transition.options[i].label;
   }

   saveSetting(kSettingBack, "0");
   const std::string key = optionKey(transition);
   std::vector<ColorizingDMDSettingField> fields;
   const std::string description = transition.description;
   fields.push_back({nullptr, description.c_str(), CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr});
   fields.push_back({key.c_str(), transition.optionLabel, CDMDPI_FIELD_CHOICE, transition.options[0].id, choices.c_str(), nullptr});

   fields.push_back(kBackField);

   std::string gallery;
   if (picturesWanted())
   {
      std::vector<preview::GalleryItem> items;
      for (size_t i = 0; i < transition.optionCount; ++i)
         items.push_back({transition.options[i].label, preview::makeIcon(transition, &transition.options[i])});
      gallery = preview::galleryHtml("Halfway through, white is the picture and black is what the transition has taken away:", items);
      fields.push_back({nullptr, gallery.c_str(), CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr});
   }

   if (!g_editor->ShowForm(g_endpoint, transition.label, "Next", fields.data(), static_cast<uint32_t>(fields.size()),
                           onOptionChosen, nullptr))
      message(kTitle, "The window could not be opened. Close other plugin windows and try again.");
}

// The first window was closed: the transition is chosen.
void MSGPIAPI onTransitionChosen(void* /*userData*/, int saved)
{
   if (!saved || !g_editor)
      return;
   g_wizard.transition = transitions::findTransition(setting(kSettingTransition, "").c_str());
   g_wizard.option = nullptr;
   g_wizard.effect.reset();
   if (!g_wizard.transition)
   {
      message(kTitle, "Please choose a transition from the list.");
      return;
   }
   if (g_wizard.transition->optionCount > 0)
      showOptionForm();
   else
      showFramesForm();
}

// The menu entry: reads the selected frame, then opens the first window.
void MSGPIAPI onMenuEntry(void* /*userData*/)
{
   if (!g_editor)
      return;

   const std::string problem = loadSource(g_editor->GetCurrentFrame(), g_wizard.source);
   if (!problem.empty())
   {
      message(kTitle, problem);
      return;
   }
   showTransitionForm();
}

// The first window: the transition.
void showTransitionForm()
{
   std::string choices;
   for (const auto& transition : transitions::kTransitions)
   {
      if (!choices.empty())
         choices += '\n';
      choices += std::string(transition.id) + "=" + transition.label;
   }

   const std::string note = "Starting from frame " + std::to_string(g_wizard.source.frame) + ".";
   std::vector<ColorizingDMDSettingField> fields;
   fields.push_back({nullptr, note.c_str(), CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr});
   fields.push_back({kSettingTransition, "Transition", CDMDPI_FIELD_CHOICE, transitions::kTransitions[0].id,
                     choices.c_str(), nullptr});
   fields.push_back({kSettingPictures, "Show pictures of the transitions", CDMDPI_FIELD_CHECKBOX, "1", nullptr,
                     "Turn this off if the pictures do not look right. It applies from the next window on."});

   std::string gallery;
   if (picturesWanted())
   {
      std::vector<preview::GalleryItem> items;
      for (const auto& transition : transitions::kTransitions)
         items.push_back({transition.label, preview::makeIcon(transition, transition.optionCount ? &transition.options[0] : nullptr)});
      gallery = preview::galleryHtml("Halfway through, white is the picture and black is what the transition has taken away:", items);
      fields.push_back({nullptr, gallery.c_str(), CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr});
   }

   if (!g_editor->ShowForm(g_endpoint, kTitle, "Next", fields.data(), static_cast<uint32_t>(fields.size()),
                           onTransitionChosen, nullptr))
      message(kTitle, "The window could not be opened. Close other plugin windows and try again.");
}

} // namespace

MSGPI_EXPORT void MSGPIAPI TransitionBuilderPluginLoad(const uint32_t endpointId, const MsgPluginAPI* api)
{
   g_msg = api;
   g_endpoint = endpointId;

   // Ask the editor for its plugin API (see the header of ColorizingDMDPlugin.h).
   const ColorizingDMDEditorAPI* editor = nullptr;
   g_getApiId = g_msg->GetMsgID(CDMDPI_NAMESPACE, CDMDPI_MSG_GET_API);
   g_msg->BroadcastMsg(g_endpoint, g_getApiId, &editor);
   if (!editor || editor->version < 6)
   {
      // Not hosted by the PPUC Colorizing DMD editor, or by one too old for this plugin: it
      // needs AddFrame (version 6) on top of the forms, the pictures and the edit batches.
      g_editor = nullptr;
      return;
   }
   g_editor = editor;
   g_editor->AddMenuEntry(g_endpoint, "Transition Builder...", onMenuEntry, nullptr);
}

MSGPI_EXPORT void MSGPIAPI TransitionBuilderPluginUnload()
{
   if (g_editor)
      g_editor->RemoveMenuEntries(g_endpoint);
   if (g_msg)
      g_msg->ReleaseMsgID(g_getApiId);
   g_wizard = Wizard{};
   g_editor = nullptr;
   g_msg = nullptr;
}
