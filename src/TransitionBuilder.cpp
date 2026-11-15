// license:GPLv3+
//
// Transition Builder: a plugin for the PPUC Colorizing DMD editor.
//
// Plugins menu -> "Transition Builder...": the author picks a transition and a number of frames
// for the current frame. The plugin appends that many new frames at the end of the project, each
// a step further along the transition than the one before. The last one is not completely black.
// All of it is one step of the editor's history (one Undo).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "ColorizingDMDPlugin.h"
#include "Transitions.h"
#include "plugins/MsgPlugin.h"

namespace {

const MsgPluginAPI* g_msg = nullptr;
const ColorizingDMDEditorAPI* g_editor = nullptr;
uint32_t g_endpoint = 0;
unsigned int g_getApiId = 0;

constexpr const char* kSettingTransition = "transition";
constexpr const char* kSettingFrames = "frames";
constexpr const char* kUndoName = "Transition Builder";

// What the form was opened for. The form does not block the editor, so the project can change
// while it is open; the revision tells.
struct PendingRun
{
   uint32_t frame = CDMDPI_NONE;
   uint64_t revision = 0;
};
PendingRun g_pending;

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

const transitions::Transition* findTransition(const std::string& id)
{
   for (const auto& transition : transitions::kTransitions)
      if (id == transition.id)
         return &transition;
   return nullptr;
}

std::string setting(const char* key, const char* fallback)
{
   char buffer[128];
   const uint32_t size = g_editor->GetSetting(g_endpoint, key, buffer, sizeof(buffer));
   if (size == 0 || size > sizeof(buffer))
      return fallback;
   return buffer;
}

// Makes the picture of one step: `source` is a picture of width x height pixels (the original
// frame, a colorized plane or the dynamic zones). The transition decides on the original
// frame's grid (originalWidth x originalHeight) where each pixel comes from. A picture on a
// finer grid (the HD plane is twice as large) follows the same decision, and a pixel keeps its
// place inside its cell, so a move is as smooth as the picture is fine. Pixels that show
// nothing get `black`.
template <class T>
void renderStep(const std::vector<T>& source, std::vector<T>& out, uint32_t width,
                uint32_t height, uint32_t originalWidth, uint32_t originalHeight,
                const transitions::Transition& transition, uint32_t step, uint32_t steps, T black)
{
   out.assign(source.size(), black);
   for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
      {
         const uint32_t cellX = static_cast<uint32_t>(static_cast<uint64_t>(x) * originalWidth / width);
         const uint32_t cellY =
            static_cast<uint32_t>(static_cast<uint64_t>(y) * originalHeight / height);
         const transitions::SourcePixel from =
            transition.map(cellX, cellY, step, steps, originalWidth, originalHeight);
         if (!from.visible)
            continue;
         // The pixel's place inside its cell, then the same place in the source cell.
         const uint32_t insideX =
            x - static_cast<uint32_t>(static_cast<uint64_t>(cellX) * width / originalWidth);
         const uint32_t insideY =
            y - static_cast<uint32_t>(static_cast<uint64_t>(cellY) * height / originalHeight);
         const uint32_t sourceX = std::min(
            width - 1,
            static_cast<uint32_t>(static_cast<uint64_t>(from.x) * width / originalWidth) + insideX);
         const uint32_t sourceY = std::min(
            height - 1,
            static_cast<uint32_t>(static_cast<uint64_t>(from.y) * height / originalHeight) + insideY);
         out[static_cast<size_t>(y) * width + x] = source[static_cast<size_t>(sourceY) * width + sourceX];
      }
}

// A plane of the colorized frame: its pixels, and the size of the grid they lie on.
struct Plane
{
   int hd = 0;
   uint32_t width = 0;
   uint32_t height = 0;
   std::vector<uint16_t> pixels;
};

// Creates the transition. Returns an empty string when it worked, else what went wrong.
std::string createTransition(uint32_t source, const transitions::Transition& transition,
                             uint32_t steps, bool& hadCaveat, std::string& caveat,
                             uint32_t& firstNew)
{
   ColorizingDMDProjectInfo info{};
   if (!g_editor->GetProjectInfo(&info) || !info.hasProject)
      return "No project is open.";
   if (info.revision != g_pending.revision || source >= info.frameCount)
      return "The project changed while the window was open. Please start again.";

   ColorizingDMDFrameInfo frameInfo{};
   if (!g_editor->GetFrameInfo(source, &frameInfo))
      return "The frame does not exist any more.";

   // Read the source frame once; every new frame is made from it.
   std::vector<uint8_t> original;
   uint32_t originalWidth = 0;
   uint32_t originalHeight = 0;
   if (!fetch<uint8_t>(
          [&](uint8_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
             return g_editor->GetOriginalFrame(source, p, cap, w, h);
          },
          original, originalWidth, originalHeight))
      return "The editor could not give the original frame.";

   std::vector<Plane> planes;
   for (int hd = 0; hd <= 1; ++hd)
   {
      if (hd == 1 && !frameInfo.hasHd)
         continue;
      Plane plane;
      plane.hd = hd;
      if (fetch<uint16_t>(
             [&](uint16_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
                return g_editor->GetColorizedFrame(source, hd, p, cap, w, h);
             },
             plane.pixels, plane.width, plane.height))
         planes.push_back(std::move(plane));
   }
   if (planes.empty())
      return "The editor could not give the colorized frame.";

   std::vector<uint8_t> zones; // the dynamic zones, set per pixel, 255 for none
   uint32_t zonesWidth = 0;
   uint32_t zonesHeight = 0;
   if (frameInfo.hasDynamicPixels)
      fetch<uint8_t>(
         [&](uint8_t* p, uint32_t cap, uint32_t* w, uint32_t* h) {
            return g_editor->GetDynamicMask(source, p, cap, w, h);
         },
         zones, zonesWidth, zonesHeight);

   if (frameInfo.background != CDMDPI_NONE)
   {
      hadCaveat = true;
      caveat += "The frame shows a background. Where the background shows through, the "
                "pixels stay as they are; only the frame's own pixels dissolve or move.\n";
   }

   if (!g_editor->BeginEdit(g_endpoint, kUndoName))
      return "The editor cannot start an edit right now (another edit is open).";

   auto fail = [&](const std::string& why) {
      g_editor->EndEdit(g_endpoint, 0); // takes every change back
      return why;
   };

   std::vector<uint8_t> stepOriginal;
   std::vector<uint16_t> stepPixels;
   std::vector<uint8_t> stepZones;
   std::vector<uint32_t> created;

   for (uint32_t step = 1; step <= steps; ++step)
   {
      // The original: black pixels are shade 0, so every new frame differs from the others.
      renderStep<uint8_t>(original, stepOriginal, originalWidth, originalHeight, originalWidth,
                          originalHeight, transition, step, steps, 0);

      // The new frame is made like the source: its settings and pictures come along.
      uint32_t newFrame = CDMDPI_NONE;
      if (!g_editor->AddFrame(g_endpoint, stepOriginal.data(),
                              static_cast<uint32_t>(stepOriginal.size()), source, &newFrame) ||
          newFrame == CDMDPI_NONE)
         return fail("The editor refused to add frame " + std::to_string(step) + " of " +
                     std::to_string(steps) + ". Nothing was changed.");
      created.push_back(newFrame);

      // No comparison mask on the new frames (the source's mask came along with the copy). The
      // shape mode stays as it is.
      if (!g_editor->SetFrameComparison(g_endpoint, newFrame, CDMDPI_NONE, frameInfo.shapeMode))
         return fail("The editor refused to clear the comparison mask of frame " +
                     std::to_string(step) + ". Nothing was changed.");

      // The colorized planes follow the same decisions as the original.
      for (const Plane& plane : planes)
      {
         renderStep<uint16_t>(plane.pixels, stepPixels, plane.width, plane.height, originalWidth,
                              originalHeight, transition, step, steps, 0); // RGB565 black
         if (!g_editor->SetFramePicture(g_endpoint, newFrame, plane.hd, stepPixels.data(),
                                        nullptr, static_cast<uint32_t>(stepPixels.size())))
            return fail("The editor refused the picture of frame " + std::to_string(step) +
                        ". Nothing was changed.");
      }

      // Dynamic zones color a pixel at run time from its shade, so they move with the picture,
      // and a black pixel must not stay in a zone.
      if (!zones.empty())
      {
         renderStep<uint8_t>(zones, stepZones, zonesWidth, zonesHeight, originalWidth,
                             originalHeight, transition, step, steps, 255);
         if (!g_editor->SetDynamicZones(g_endpoint, newFrame, stepZones.data(),
                                        static_cast<uint32_t>(stepZones.size())))
            return fail("The editor refused the dynamic zones of frame " +
                        std::to_string(step) + ". Nothing was changed.");
      }
   }

   if (!g_editor->EndEdit(g_endpoint, 1))
      return "The editor did not record the new frames (the status bar may say why). "
             "Nothing was changed.";

   firstNew = created.front();
   g_editor->ShowFrames(created.front(), created.data(), static_cast<uint32_t>(created.size()));
   return std::string();
}

void MSGPIAPI onFormClosed(void* /*userData*/, int saved)
{
   if (!saved || !g_editor)
      return;

   const transitions::Transition* transition = findTransition(setting(kSettingTransition, ""));
   if (!transition)
   {
      message("Transition Builder", "Please choose a transition from the list.");
      return;
   }

   const std::string framesText = setting(kSettingFrames, "");
   char* end = nullptr;
   const long frames = std::strtol(framesText.c_str(), &end, 10);
   if (framesText.empty() || *end != '\0' || frames < 1 ||
       frames > static_cast<long>(transitions::kMaxSteps))
   {
      message("Transition Builder",
              "The number of frames must be a whole number from 1 to " +
                 std::to_string(transitions::kMaxSteps) + ".");
      return;
   }

   bool hadCaveat = false;
   std::string caveat;
   uint32_t firstNew = CDMDPI_NONE;
   const std::string problem = createTransition(g_pending.frame, *transition,
                                                static_cast<uint32_t>(frames), hadCaveat, caveat,
                                                firstNew);
   if (!problem.empty())
   {
      message("Transition Builder", problem);
      return;
   }

   g_editor->ShowStatus(g_endpoint, ("Transition Builder: added " + std::to_string(frames) +
                                     " frame(s) of " + transition->label + ", starting at frame " +
                                     std::to_string(firstNew) + ". Undo takes them back.")
                                       .c_str());
   if (hadCaveat)
      message("Transition Builder", caveat);
}

void MSGPIAPI onMenuEntry(void* /*userData*/)
{
   if (!g_editor)
      return;

   ColorizingDMDProjectInfo info{};
   if (!g_editor->GetProjectInfo(&info) || !info.hasProject)
   {
      message("Transition Builder", "Open a project first.");
      return;
   }
   const uint32_t frame = g_editor->GetCurrentFrame();
   if (frame == CDMDPI_NONE || frame >= info.frameCount)
   {
      message("Transition Builder", "Select the frame to start the transition from first.");
      return;
   }
   g_pending.frame = frame;
   g_pending.revision = info.revision;

   // The drop-down list, one "id=Label" per line, from the transitions the plugin has.
   std::string choices;
   for (const auto& transition : transitions::kTransitions)
   {
      if (!choices.empty())
         choices += '\n';
      choices += std::string(transition.id) + "=" + transition.label;
   }

   const std::string note = "Starting from frame " + std::to_string(frame) +
                            ". The new frames are added at the end of the project.";
   const ColorizingDMDSettingField fields[] = {
      {nullptr, note.c_str(), CDMDPI_FIELD_NOTE, nullptr, nullptr, nullptr},
      {kSettingTransition, "Transition", CDMDPI_FIELD_CHOICE,
       transitions::kTransitions[0].id, choices.c_str(), nullptr},
      {kSettingFrames, "Number of frames", CDMDPI_FIELD_TEXT, "10", nullptr,
       "How many new frames the transition lasts (1 to 256)."},
   };
   if (!g_editor->ShowForm(g_endpoint, "Transition Builder", "Apply", fields,
                           static_cast<uint32_t>(sizeof(fields) / sizeof(fields[0])),
                           onFormClosed, nullptr))
      message("Transition Builder",
              "The window could not be opened. Close other plugin windows and try again.");
}

} // namespace

MSGPI_EXPORT void MSGPIAPI TransitionBuilderPluginLoad(const uint32_t endpointId,
                                                       const MsgPluginAPI* api)
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
      // needs AddFrame (version 6) on top of the form, the pictures and the edit batches.
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
   g_editor = nullptr;
   g_msg = nullptr;
}
