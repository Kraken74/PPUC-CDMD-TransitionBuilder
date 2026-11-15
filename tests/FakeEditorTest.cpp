// license:GPLv3+
//
// Tests of the Transition Builder plugin against a fake editor: the fake implements the part of
// the editor API the plugin uses, with edit batches that can be taken back, so the whole path
// (menu entry -> form -> new frames) runs as in the editor.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ColorizingDMDPlugin.h"
#include "Transitions.h"
#include "plugins/MsgPlugin.h"

MSGPI_EXPORT void MSGPIAPI TransitionBuilderPluginLoad(const uint32_t, const MsgPluginAPI*);
MSGPI_EXPORT void MSGPIAPI TransitionBuilderPluginUnload();

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                      \
   do                                                                                    \
   {                                                                                     \
      if (!(cond))                                                                       \
      {                                                                                  \
         std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
         ++g_failures;                                                                   \
      }                                                                                  \
   } while (0)

// ---- The fake project -----------------------------------------------------------------------

constexpr uint32_t kW = 128, kH = 32; // the original and the SD plane
constexpr uint32_t kHdW = 256, kHdH = 64;

struct Frame
{
   std::vector<uint8_t> original;
   std::vector<uint16_t> sd;
   std::vector<uint16_t> hd; // empty: no HD
   std::vector<uint8_t> zones;
   uint32_t background = CDMDPI_NONE;
   uint32_t comparisonMask = CDMDPI_NONE;
   uint32_t shapeMode = 0;
};

std::vector<Frame> g_frames;
std::vector<Frame> g_snapshot;
bool g_inBatch = false;
uint64_t g_revision = 1;
uint32_t g_current = 0;
int g_addFrameLimit = -1; // fail AddFrame after this many successful calls in a batch
int g_addFrameCalls = 0;

std::map<std::string, std::string> g_settings;
std::vector<std::string> g_messages;
std::vector<std::string> g_status;
int g_endEditApplied = 0;
int g_endEditCalls = 0;
std::vector<uint32_t> g_shown;
cdmdpi_action_callback g_menuCallback = nullptr;
cdmdpi_settings_callback g_formCallback = nullptr;
int g_formOpened = 0;

Frame makeSourceFrame(bool withHd, bool withZones)
{
   Frame f;
   f.original.resize(kW * kH);
   f.sd.resize(kW * kH);
   for (uint32_t i = 0; i < kW * kH; ++i)
   {
      f.original[i] = static_cast<uint8_t>(1 + i % 3); // no black pixel at all
      f.sd[i] = 0xFFFF;
   }
   if (withHd)
      f.hd.assign(kHdW * kHdH, 0xFFFF);
   if (withZones)
      f.zones.assign(kW * kH, 0); // every pixel in dynamic set 0
   return f;
}

// ---- The fake editor API --------------------------------------------------------------------

int MSGPIAPI fGetProjectInfo(ColorizingDMDProjectInfo* info)
{
   std::memset(info, 0, sizeof(*info));
   info->hasProject = 1;
   info->frameCount = static_cast<uint32_t>(g_frames.size());
   info->colorCount = 4;
   info->originalWidth = kW;
   info->originalHeight = kH;
   info->sdWidth = kW;
   info->sdHeight = kH;
   info->hdWidth = kHdW;
   info->hdHeight = kHdH;
   info->revision = g_revision;
   return 1;
}

int MSGPIAPI fGetFrameInfo(uint32_t frame, ColorizingDMDFrameInfo* info)
{
   if (frame >= g_frames.size())
      return 0;
   std::memset(info, 0, sizeof(*info));
   info->comparisonMask = g_frames[frame].comparisonMask;
   info->shapeMode = g_frames[frame].shapeMode;
   info->background = g_frames[frame].background;
   info->hasDynamicPixels = g_frames[frame].zones.empty() ? 0 : 1;
   info->hasHd = g_frames[frame].hd.empty() ? 0 : 1;
   info->triggerId = CDMDPI_NONE;
   info->section = CDMDPI_NONE;
   return 1;
}

template <class T>
uint32_t give(const std::vector<T>& data, T* out, uint32_t capacity, uint32_t w, uint32_t h,
              uint32_t* width, uint32_t* height)
{
   *width = w;
   *height = h;
   if (data.empty())
      return 0;
   if (capacity >= data.size())
      std::memcpy(out, data.data(), data.size() * sizeof(T));
   return static_cast<uint32_t>(data.size());
}

uint32_t MSGPIAPI fGetOriginalFrame(uint32_t frame, uint8_t* px, uint32_t cap, uint32_t* w,
                                    uint32_t* h)
{
   return give(g_frames.at(frame).original, px, cap, kW, kH, w, h);
}
uint32_t MSGPIAPI fGetColorizedFrame(uint32_t frame, int hd, uint16_t* px, uint32_t cap,
                                     uint32_t* w, uint32_t* h)
{
   return hd ? give(g_frames.at(frame).hd, px, cap, kHdW, kHdH, w, h)
             : give(g_frames.at(frame).sd, px, cap, kW, kH, w, h);
}
uint32_t MSGPIAPI fGetDynamicMask(uint32_t frame, uint8_t* px, uint32_t cap, uint32_t* w,
                                  uint32_t* h)
{
   return give(g_frames.at(frame).zones, px, cap, kW, kH, w, h);
}

uint32_t MSGPIAPI fGetCurrentFrame() { return g_current; }

int MSGPIAPI fShowFrames(uint32_t current, const uint32_t* frames, uint32_t count)
{
   g_shown.assign(frames, frames + count);
   (void)current;
   return 1;
}

void MSGPIAPI fAddMenuEntry(uint32_t, const char*, cdmdpi_action_callback cb, void*)
{
   g_menuCallback = cb;
}
void MSGPIAPI fRemoveMenuEntries(uint32_t) { g_menuCallback = nullptr; }
void MSGPIAPI fShowMessage(uint32_t, const char*, const char* text) { g_messages.push_back(text); }
void MSGPIAPI fShowStatus(uint32_t, const char* text) { g_status.push_back(text); }

uint32_t MSGPIAPI fGetSetting(uint32_t, const char* key, char* value, uint32_t capacity)
{
   auto it = g_settings.find(key);
   if (it == g_settings.end())
      return 0;
   const uint32_t size = static_cast<uint32_t>(it->second.size() + 1);
   if (capacity >= size)
      std::memcpy(value, it->second.c_str(), size);
   return size;
}
void MSGPIAPI fSetSetting(uint32_t, const char* key, const char* value) { g_settings[key] = value; }

int MSGPIAPI fBeginEdit(uint32_t, const char*)
{
   if (g_inBatch)
      return 0;
   g_inBatch = true;
   g_snapshot = g_frames;
   g_addFrameCalls = 0;
   return 1;
}
int MSGPIAPI fEndEdit(uint32_t, int apply)
{
   ++g_endEditCalls;
   if (!g_inBatch)
      return 0;
   g_inBatch = false;
   if (apply)
   {
      ++g_endEditApplied;
      ++g_revision;
   }
   else
      g_frames = g_snapshot;
   return 1;
}

int MSGPIAPI fAddFrame(uint32_t, const uint8_t* original, uint32_t count, uint32_t like,
                       uint32_t* frame)
{
   if (!g_inBatch || like >= g_frames.size() || count != kW * kH)
      return 0;
   if (g_addFrameLimit >= 0 && g_addFrameCalls >= g_addFrameLimit)
      return 0;
   ++g_addFrameCalls;
   Frame f = g_frames[like]; // made like the source: pictures and settings come along
   f.original.assign(original, original + count);
   g_frames.push_back(f);
   if (frame)
      *frame = static_cast<uint32_t>(g_frames.size() - 1);
   return 1;
}

int MSGPIAPI fSetFramePicture(uint32_t, uint32_t frame, int hd, const uint16_t* pixels,
                              const uint8_t* write, uint32_t count)
{
   if (!g_inBatch || frame >= g_frames.size())
      return 0;
   auto& target = hd ? g_frames[frame].hd : g_frames[frame].sd;
   if (count != target.size())
      return 0;
   for (uint32_t i = 0; i < count; ++i)
      if (!write || write[i])
         target[i] = pixels[i];
   return 1;
}

int MSGPIAPI fSetFrameComparison(uint32_t, uint32_t frame, uint32_t mask, uint32_t shapeMode)
{
   if (!g_inBatch || frame >= g_frames.size())
      return 0;
   g_frames[frame].comparisonMask = mask;
   g_frames[frame].shapeMode = shapeMode;
   return 1;
}

int MSGPIAPI fSetDynamicZones(uint32_t, uint32_t frame, const uint8_t* sets, uint32_t count)
{
   if (!g_inBatch || frame >= g_frames.size() || count != g_frames[frame].zones.size())
      return 0;
   g_frames[frame].zones.assign(sets, sets + count);
   return 1;
}

int MSGPIAPI fShowForm(uint32_t, const char*, const char*, const ColorizingDMDSettingField* fields,
                       uint32_t count, cdmdpi_settings_callback cb, void*)
{
   ++g_formOpened;
   g_formCallback = cb;
   // As the editor does: the form's fields must make sense.
   bool hasChoice = false, hasText = false;
   for (uint32_t i = 0; i < count; ++i)
   {
      if (fields[i].kind == CDMDPI_FIELD_CHOICE && std::strstr(fields[i].choices, "dissolve=Dissolve"))
         hasChoice = true;
      if (fields[i].kind == CDMDPI_FIELD_TEXT && fields[i].key && !std::strcmp(fields[i].key, "frames"))
         hasText = true;
   }
   CHECK(hasChoice);
   CHECK(hasText);
   return 1;
}

ColorizingDMDEditorAPI g_api{};

// ---- The fake plugin bus --------------------------------------------------------------------

unsigned int MSGPIAPI bGetMsgID(const char*, const char*) { return 7; }
void MSGPIAPI bBroadcast(const uint32_t, const unsigned int msgId, void* data)
{
   if (msgId == 7)
      *static_cast<const ColorizingDMDEditorAPI**>(data) = &g_api;
}
void MSGPIAPI bRelease(const unsigned int) {}
MsgPluginAPI g_bus{};

void setUpApi()
{
   g_api.version = 8;
   g_api.GetProjectInfo = fGetProjectInfo;
   g_api.GetFrameInfo = fGetFrameInfo;
   g_api.GetOriginalFrame = fGetOriginalFrame;
   g_api.GetColorizedFrame = fGetColorizedFrame;
   g_api.GetDynamicMask = fGetDynamicMask;
   g_api.GetCurrentFrame = fGetCurrentFrame;
   g_api.ShowFrames = fShowFrames;
   g_api.AddMenuEntry = fAddMenuEntry;
   g_api.RemoveMenuEntries = fRemoveMenuEntries;
   g_api.ShowMessage = fShowMessage;
   g_api.ShowStatus = fShowStatus;
   g_api.GetSetting = fGetSetting;
   g_api.SetSetting = fSetSetting;
   g_api.BeginEdit = fBeginEdit;
   g_api.EndEdit = fEndEdit;
   g_api.AddFrame = fAddFrame;
   g_api.SetFramePicture = fSetFramePicture;
   g_api.SetDynamicZones = fSetDynamicZones;
   g_api.SetFrameComparison = fSetFrameComparison;
   g_api.ShowForm = fShowForm;
   g_bus.version = 1;
   g_bus.GetMsgID = bGetMsgID;
   g_bus.BroadcastMsg = bBroadcast;
   g_bus.ReleaseMsgID = bRelease;
}

void resetProject(bool withHd, bool withZones)
{
   g_frames.clear();
   g_frames.push_back(makeSourceFrame(withHd, withZones));
   g_frames.push_back(makeSourceFrame(withHd, withZones));
   g_inBatch = false;
   g_current = 0;
   g_settings.clear();
   g_messages.clear();
   g_status.clear();
   g_shown.clear();
   g_endEditApplied = 0;
   g_endEditCalls = 0;
   g_addFrameLimit = -1;
}

// What the author does: click the menu entry, fill in the form, click Apply (saved = 1).
void runForm(const char* transition, const char* frames, bool apply = true)
{
   CHECK(g_menuCallback != nullptr);
   g_formCallback = nullptr;
   g_menuCallback(nullptr);
   CHECK(g_formCallback != nullptr);
   if (!g_formCallback)
      return;
   if (apply)
   {
      g_settings["transition"] = transition; // the editor keeps the values on Apply
      g_settings["frames"] = frames;
   }
   g_formCallback(nullptr, apply ? 1 : 0);
}

size_t countBlack(const std::vector<uint8_t>& v)
{
   size_t n = 0;
   for (uint8_t x : v)
      n += x == 0;
   return n;
}

// ---- The tests ------------------------------------------------------------------------------

void testBayerIsAPermutation()
{
   std::vector<int> seen(256, 0);
   for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x)
         ++seen[transitions::bayer16(x, y)];
   for (int count : seen)
      CHECK(count == 1);
}

void testDissolveShape()
{
   for (uint32_t steps : {1u, 2u, 7u, 10u, 100u, 256u})
   {
      size_t previous = 0;
      for (uint32_t step = 1; step <= steps; ++step)
      {
         size_t black = 0;
         for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x)
               black += transitions::dissolveIsBlack(x, y, step, steps, 16, 16);
         CHECK(black >= previous); // never fewer black pixels than the step before
         previous = black;
      }
      CHECK(previous == 256); // the last step is completely black
   }
   // A pixel that is black stays black.
   for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x)
         for (uint32_t step = 1; step < 10; ++step)
            if (transitions::dissolveIsBlack(x, y, step, 10, 16, 16))
               CHECK(transitions::dissolveIsBlack(x, y, step + 1, 10, 16, 16));
}

void testDissolveCreatesFrames(bool withHd, bool withZones)
{
   resetProject(withHd, withZones);
   g_frames[0].comparisonMask = 5; // the source has a comparison mask and the shape mode
   g_frames[0].shapeMode = 1;
   runForm("dissolve", "10");

   CHECK(g_messages.empty());
   CHECK(g_frames.size() == 2 + 10); // the new frames go behind the last one
   CHECK(g_endEditApplied == 1);     // one step of the history
   CHECK(g_status.size() == 1);
   CHECK(g_shown.size() == 10 && g_shown.front() == 2 && g_shown.back() == 11);

   // The source frame is untouched.
   CHECK(countBlack(g_frames[0].original) == 0);
   CHECK(g_frames[0].comparisonMask == 5 && g_frames[0].shapeMode == 1);
   // The new frames have no comparison mask; the shape mode is the source's.
   for (uint32_t f = 2; f < g_frames.size(); ++f)
   {
      CHECK(g_frames[f].comparisonMask == CDMDPI_NONE);
      CHECK(g_frames[f].shapeMode == 1);
   }

   size_t previousBlack = 0;
   for (uint32_t k = 1; k <= 10; ++k)
   {
      const Frame& f = g_frames[1 + k]; // frame 2 is step 1
      const size_t black = countBlack(f.original);
      CHECK(black > previousBlack || k == 10);
      CHECK(black >= previousBlack);
      previousBlack = black;

      // Black in the original, in the SD plane, in the HD plane and out of the zones are the
      // same pixels, and the other pixels are as in the source.
      for (uint32_t y = 0; y < kH; ++y)
         for (uint32_t x = 0; x < kW; ++x)
         {
            const size_t i = static_cast<size_t>(y) * kW + x;
            const bool black = f.original[i] == 0;
            CHECK((f.sd[i] == 0) == black);
            if (!black)
            {
               CHECK(f.original[i] == g_frames[0].original[i]);
               CHECK(f.sd[i] == 0xFFFF);
            }
            if (withZones)
               CHECK((f.zones[i] == 255) == black);
            if (withHd)
               for (uint32_t dy = 0; dy < 2; ++dy)
                  for (uint32_t dx = 0; dx < 2; ++dx)
                     CHECK((f.hd[static_cast<size_t>(2 * y + dy) * kHdW + 2 * x + dx] == 0) ==
                           black);
         }
      CHECK(f.hd.empty() == !withHd);
   }
   // Step 1 of 10 dissolves a tenth of the picture, the last step all of it.
   const size_t total = kW * kH;
   const size_t first = countBlack(g_frames[2].original);
   CHECK(first > total * 8 / 100 && first < total * 12 / 100);
   CHECK(countBlack(g_frames[11].original) == total);
}

void testBadInputsChangeNothing()
{
   for (const char* frames : {"abc", "", "0", "-3", "257", "10x", "1.5"})
   {
      resetProject(true, true);
      runForm("dissolve", frames);
      CHECK(g_frames.size() == 2);
      CHECK(g_messages.size() == 1);
      CHECK(g_endEditCalls == 0);
   }
   resetProject(true, true);
   runForm("nonsense", "10");
   CHECK(g_frames.size() == 2);
   CHECK(g_messages.size() == 1);
}

void testCancelChangesNothing()
{
   resetProject(true, true);
   runForm("dissolve", "10", false);
   CHECK(g_frames.size() == 2);
   CHECK(g_messages.empty());
   CHECK(g_endEditCalls == 0);
}

void testRefusalRollsBack()
{
   resetProject(true, true);
   g_addFrameLimit = 4; // the editor refuses the fifth frame
   runForm("dissolve", "10");
   CHECK(g_frames.size() == 2); // the four frames made so far were taken back
   CHECK(g_endEditApplied == 0);
   CHECK(g_messages.size() == 1);
   CHECK(!g_inBatch);
}

void testProjectChangedWhileFormIsOpen()
{
   resetProject(true, true);
   g_menuCallback(nullptr);
   ++g_revision; // the author edited something meanwhile
   g_settings["transition"] = "dissolve";
   g_settings["frames"] = "5";
   g_formCallback(nullptr, 1);
   CHECK(g_frames.size() == 2);
   CHECK(g_messages.size() == 1);
}

void testNoFrameSelected()
{
   resetProject(true, true);
   g_current = CDMDPI_NONE;
   g_formOpened = 0;
   g_menuCallback(nullptr);
   CHECK(g_formOpened == 0);
   CHECK(g_messages.size() == 1);
}

void testBackgroundCaveat()
{
   resetProject(true, true);
   g_frames[0].background = 3;
   runForm("dissolve", "3");
   CHECK(g_frames.size() == 5); // it still works...
   CHECK(g_messages.size() == 1); // ...and says what it cannot do
}

} // namespace

int main()
{
   setUpApi();
   TransitionBuilderPluginLoad(1, &g_bus);
   CHECK(g_menuCallback != nullptr);

   testBayerIsAPermutation();
   testDissolveShape();
   testDissolveCreatesFrames(true, true);
   testDissolveCreatesFrames(false, false);
   testDissolveCreatesFrames(true, false);
   testBadInputsChangeNothing();
   testCancelChangesNothing();
   testRefusalRollsBack();
   testProjectChangedWhileFormIsOpen();
   testNoFrameSelected();
   testBackgroundCaveat();

   TransitionBuilderPluginUnload();
   CHECK(g_menuCallback == nullptr);

   // An editor that is too old: the plugin does nothing and adds no menu entry.
   g_api.version = 5;
   TransitionBuilderPluginLoad(1, &g_bus);
   CHECK(g_menuCallback == nullptr);
   TransitionBuilderPluginUnload();

   if (g_failures == 0)
      std::printf("All tests passed.\n");
   return g_failures == 0 ? 0 : 1;
}
