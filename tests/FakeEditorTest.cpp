// license:GPLv3+
//
// Tests of the Transition Builder: the effects on their own (every transition, every option, on
// several frame sizes), and the plugin against a fake editor that implements the part of the
// editor API the plugin uses, with edit batches that can be taken back. The windows are
// scripted the way the author uses them: menu entry -> transition -> option -> frames.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ColorizingDMDPlugin.h"
#include "Preview.h"
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

uint32_t g_w = 128, g_h = 32; // the original frame and the SD plane
constexpr uint32_t kColors = 4;

struct Frame
{
   std::vector<uint8_t> original;
   std::vector<uint16_t> sd;
   std::vector<uint8_t> zones;
   std::vector<uint16_t> dynamicColors; // 32 sets x kColors, empty: none
   int rotations = 0;
   uint32_t background = CDMDPI_NONE;
   uint32_t comparisonMask = CDMDPI_NONE;
   uint32_t shapeMode = 0;
};

std::vector<Frame> g_frames;
std::vector<Frame> g_snapshot;
bool g_inBatch = false;
uint64_t g_revision = 1;
uint32_t g_current = 0;
int g_addFrameLimit = -1;
int g_addFrameCalls = 0;

std::map<std::string, std::string> g_settings;
std::vector<std::string> g_messages;
std::vector<std::string> g_status;
int g_endEditApplied = 0;
std::vector<uint32_t> g_shown;
cdmdpi_action_callback g_menuCallback = nullptr;

// The window the plugin opened last.
struct Form
{
   std::string title, accept;
   struct Field
   {
      std::string key, label, def, choices;
      uint32_t kind;
   };
   std::vector<Field> fields;
   cdmdpi_settings_callback callback = nullptr;
   bool open = false;
};
Form g_form;
int g_formsOpened = 0;

Frame makeSourceFrame(bool withZones)
{
   Frame f;
   f.original.resize(static_cast<size_t>(g_w) * g_h);
   f.sd.resize(f.original.size());
   for (uint32_t y = 0; y < g_h; ++y)
      for (uint32_t x = 0; x < g_w; ++x)
      {
         const size_t i = static_cast<size_t>(y) * g_w + x;
         f.original[i] = static_cast<uint8_t>(1 + (x + 2 * y) % 3);
         // RGB565 colors that differ from pixel to pixel but never 0 (black).
         f.sd[i] = static_cast<uint16_t>(0x0841 * (1 + f.original[i]) + (i % 5) * 0x0020);
      }
   if (withZones)
   {
      f.zones.resize(f.original.size());
      for (size_t i = 0; i < f.zones.size(); ++i)
         f.zones[i] = static_cast<uint8_t>(i % 4);
      f.dynamicColors.resize(32 * kColors);
      for (size_t i = 0; i < f.dynamicColors.size(); ++i)
         f.dynamicColors[i] = static_cast<uint16_t>(0x2104 * (1 + i % kColors));
      f.rotations = 2;
   }
   return f;
}

// ---- The fake editor API --------------------------------------------------------------------

int MSGPIAPI fGetProjectInfo(ColorizingDMDProjectInfo* info)
{
   std::memset(info, 0, sizeof(*info));
   info->hasProject = 1;
   info->frameCount = static_cast<uint32_t>(g_frames.size());
   info->colorCount = kColors;
   info->originalWidth = g_w;
   info->originalHeight = g_h;
   info->sdWidth = g_w;
   info->sdHeight = g_h;
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
   info->hasHd = 0;
   info->triggerId = CDMDPI_NONE;
   info->section = CDMDPI_NONE;
   return 1;
}

template <class T>
uint32_t give(const std::vector<T>& data, T* out, uint32_t capacity, uint32_t* width, uint32_t* height)
{
   *width = g_w;
   *height = g_h;
   if (data.empty())
      return 0;
   if (capacity >= data.size())
      std::memcpy(out, data.data(), data.size() * sizeof(T));
   return static_cast<uint32_t>(data.size());
}

uint32_t MSGPIAPI fGetOriginalFrame(uint32_t frame, uint8_t* px, uint32_t cap, uint32_t* w, uint32_t* h)
{
   return give(g_frames.at(frame).original, px, cap, w, h);
}
uint32_t MSGPIAPI fGetColorizedFrame(uint32_t frame, int hd, uint16_t* px, uint32_t cap, uint32_t* w, uint32_t* h)
{
   if (hd)
      return 0;
   return give(g_frames.at(frame).sd, px, cap, w, h);
}
uint32_t MSGPIAPI fGetDynamicMask(uint32_t frame, uint8_t* px, uint32_t cap, uint32_t* w, uint32_t* h)
{
   return give(g_frames.at(frame).zones, px, cap, w, h);
}
uint32_t MSGPIAPI fGetDynamicColors(uint32_t frame, uint16_t* colors, uint32_t cap)
{
   const auto& c = g_frames.at(frame).dynamicColors;
   if (c.empty())
      return 0;
   if (cap >= c.size())
      std::memcpy(colors, c.data(), c.size() * sizeof(uint16_t));
   return static_cast<uint32_t>(c.size());
}

uint32_t MSGPIAPI fGetCurrentFrame() { return g_current; }

int MSGPIAPI fShowFrames(uint32_t, const uint32_t* frames, uint32_t count)
{
   g_shown.assign(frames, frames + count);
   return 1;
}

void MSGPIAPI fAddMenuEntry(uint32_t, const char*, cdmdpi_action_callback cb, void*) { g_menuCallback = cb; }
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

int MSGPIAPI fAddFrame(uint32_t, const uint8_t* original, uint32_t count, uint32_t like, uint32_t* frame)
{
   if (!g_inBatch || like >= g_frames.size() || count != g_w * g_h)
      return 0;
   if (g_addFrameLimit >= 0 && g_addFrameCalls >= g_addFrameLimit)
      return 0;
   ++g_addFrameCalls;
   Frame f = g_frames[like];
   f.original.assign(original, original + count);
   g_frames.push_back(f);
   if (frame)
      *frame = static_cast<uint32_t>(g_frames.size() - 1);
   return 1;
}

int MSGPIAPI fSetFramePicture(uint32_t, uint32_t frame, int hd, const uint16_t* pixels, const uint8_t* write, uint32_t count)
{
   if (!g_inBatch || frame >= g_frames.size() || hd)
      return 0;
   auto& target = g_frames[frame].sd;
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

int MSGPIAPI fSetDynamicColors(uint32_t, uint32_t frame, uint32_t set, const uint16_t* colors, uint32_t count)
{
   if (!g_inBatch || frame >= g_frames.size() || g_frames[frame].dynamicColors.empty() || count != kColors || set >= 32)
      return 0;
   std::memcpy(&g_frames[frame].dynamicColors[static_cast<size_t>(set) * kColors], colors, count * sizeof(uint16_t));
   return 1;
}

int MSGPIAPI fClearColorRotations(uint32_t, uint32_t frame)
{
   if (!g_inBatch || frame >= g_frames.size())
      return 0;
   g_frames[frame].rotations = 0;
   return 1;
}

int MSGPIAPI fShowForm(uint32_t, const char* title, const char* accept, const ColorizingDMDSettingField* fields,
                       uint32_t count, cdmdpi_settings_callback cb, void*)
{
   ++g_formsOpened;
   g_form = Form{};
   g_form.title = title;
   g_form.accept = accept ? accept : "";
   g_form.callback = cb;
   g_form.open = true;
   for (uint32_t i = 0; i < count; ++i)
      g_form.fields.push_back({fields[i].key ? fields[i].key : "", fields[i].label ? fields[i].label : "",
                               fields[i].defaultValue ? fields[i].defaultValue : "",
                               fields[i].choices ? fields[i].choices : "", fields[i].kind});
   return 1;
}

ColorizingDMDEditorAPI g_api{};

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
   g_api.GetDynamicColors = fGetDynamicColors;
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
   g_api.SetDynamicColors = fSetDynamicColors;
   g_api.ClearColorRotations = fClearColorRotations;
   g_api.SetFrameComparison = fSetFrameComparison;
   g_api.ShowForm = fShowForm;
   g_bus.version = 1;
   g_bus.GetMsgID = bGetMsgID;
   g_bus.BroadcastMsg = bBroadcast;
   g_bus.ReleaseMsgID = bRelease;
}

void resetProject(uint32_t w, uint32_t h, bool withZones)
{
   g_w = w;
   g_h = h;
   g_frames.clear();
   g_frames.push_back(makeSourceFrame(withZones));
   g_frames.push_back(makeSourceFrame(withZones));
   g_inBatch = false;
   g_current = 0;
   g_settings.clear();
   g_messages.clear();
   g_status.clear();
   g_shown.clear();
   g_endEditApplied = 0;
   g_addFrameLimit = -1;
   g_form = Form{};
   g_formsOpened = 0;
}

// ---- Scripting the windows ------------------------------------------------------------------

const Form::Field* fieldOf(const char* key)
{
   for (const auto& f : g_form.fields)
      if (f.key == key)
         return &f;
   return nullptr;
}

// The values a CHOICE field offers ("value=label" lines or plain values).
std::vector<std::string> choiceValues(const Form::Field& f)
{
   std::vector<std::string> out;
   size_t start = 0;
   while (start <= f.choices.size())
   {
      size_t end = f.choices.find('\n', start);
      if (end == std::string::npos)
         end = f.choices.size();
      std::string line = f.choices.substr(start, end - start);
      const size_t eq = line.find('=');
      if (eq != std::string::npos)
         line = line.substr(0, eq);
      if (!line.empty())
         out.push_back(line);
      start = end + 1;
   }
   return out;
}

// The author closes the open window: with Apply/Next (saved) or Cancel.
void closeForm(bool saved)
{
   CHECK(g_form.open);
   g_form.open = false;
   cdmdpi_settings_callback cb = g_form.callback;
   cb(nullptr, saved ? 1 : 0);
}

// Picks a value in the open window's field, as the editor stores it, then closes it with Next.
void pick(const char* key, const std::string& value)
{
   CHECK(fieldOf(key) != nullptr);
   g_settings[key] = value;
   closeForm(true);
}

void openMenu()
{
   CHECK(g_menuCallback != nullptr);
   g_form.open = false;
   g_menuCallback(nullptr);
}

// Runs the whole flow. option may be null for a transition without options; frames may be
// negative to take whatever the window opened on.
bool runFlow(const char* transition, const char* option, long frames)
{
   openMenu();
   if (!g_form.open)
      return false;
   pick("transition", transition);
   if (option)
   {
      CHECK(g_form.open);
      CHECK(g_form.accept == "Next");
      pick((std::string("option_") + transition).c_str(), option);
   }
   if (!g_form.open)
      return false;
   CHECK(g_form.accept == "Apply");
   if (frames >= 0)
      g_settings["frames"] = std::to_string(frames);
   closeForm(true);
   return true;
}

// ---- Helpers for the effects ----------------------------------------------------------------

struct Case
{
   const transitions::Transition* transition;
   const transitions::Option* option; // null without options
   std::string name() const
   {
      return std::string(transition->label) + (option ? std::string(" / ") + option->label : std::string());
   }
};

std::vector<Case> allCases()
{
   std::vector<Case> cases;
   for (const auto& t : transitions::kTransitions)
   {
      if (t.optionCount == 0)
         cases.push_back({&t, nullptr});
      for (size_t i = 0; i < t.optionCount; ++i)
         cases.push_back({&t, &t.options[i]});
   }
   return cases;
}

transitions::SourceInfo makeInfo(uint32_t w, uint32_t h)
{
   transitions::SourceInfo info;
   info.width = w;
   info.height = h;
   info.colorCount = kColors;
   info.brightestShade = 3;
   info.darkestShade = 1;
   for (uint32_t s = 1; s <= 3; ++s)
      for (uint32_t k = 0; k < 5; ++k)
         info.colors.push_back(static_cast<uint16_t>(0x0841 * (1 + s) + k * 0x0020));
   return info;
}

std::unique_ptr<transitions::Effect> makeEffect(const Case& c, uint32_t w, uint32_t h)
{
   return c.transition->make(c.option ? c.option->id : "", makeInfo(w, h));
}

bool isFade(const Case& c) { return transitions::same(c.transition->id, "fade"); }

// What the frame at `step` of `steps` shows: for the moving effects the rules, for the fade the
// colors of a few shades and colors.
std::vector<uint64_t> signatureOf(const transitions::Effect& e, uint32_t w, uint32_t h, uint32_t step, uint32_t steps)
{
   std::vector<uint64_t> sig;
   if (e.changesColors())
   {
      for (uint32_t s = 0; s < kColors; ++s)
         sig.push_back(e.shade(static_cast<uint8_t>(s), step, steps));
      for (uint32_t s = 1; s <= 3; ++s)
         for (uint32_t k = 0; k < 5; ++k)
            sig.push_back(e.color(static_cast<uint16_t>(0x0841 * (1 + s) + k * 0x0020), step, steps));
      return sig;
   }
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
      {
         const transitions::Rule r = e.rule(x, y, step, steps);
         sig.push_back(r.visible ? (1ull << 40) | (static_cast<uint64_t>(r.x) << 20) | r.y : 0ull);
      }
   return sig;
}

size_t visibleCount(const transitions::Effect& e, uint32_t w, uint32_t h, uint32_t step, uint32_t steps)
{
   size_t n = 0;
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
         n += e.rule(x, y, step, steps).visible ? 1 : 0;
   return n;
}

// ---- Effect tests ---------------------------------------------------------------------------

void testRegistry()
{
   CHECK(sizeof(transitions::kTransitions) / sizeof(transitions::kTransitions[0]) == transitions::kTransitionCount);
   for (const auto& t : transitions::kTransitions)
   {
      CHECK(transitions::findTransition(t.id) == &t);
      for (size_t i = 0; i < t.optionCount; ++i)
         CHECK(transitions::findOption(t, t.options[i].id) == &t.options[i]);
      if (t.optionCount)
         CHECK(transitions::findOption(t, "no-such-option") == &t.options[0]);
      else
         CHECK(transitions::findOption(t, "x") == nullptr);
   }
   CHECK(transitions::findTransition("nope") == nullptr);
   CHECK(transitions::findTransition("") == nullptr);
}

void testBayerIsAPermutation()
{
   std::set<uint32_t> seen;
   for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x)
         seen.insert(transitions::bayer16(x, y));
   CHECK(seen.size() == 256);
   CHECK(*seen.begin() == 0 && *seen.rbegin() == 255);
}

void testEveryEffectOnEverySize()
{
   const uint32_t sizes[][2] = {{128, 32}, {192, 64}, {256, 64}, {129, 33}};
   for (const auto& sz : sizes)
      for (const Case& c : allCases())
      {
         const uint32_t w = sz[0], h = sz[1];
         auto e = makeEffect(c, w, h);
         const uint32_t most = e->maxSteps();
         if (most < 1)
         {
            std::printf("no steps for %s on %ux%u\n", c.name().c_str(), w, h);
            CHECK(most >= 1);
            continue;
         }
         CHECK(most <= transitions::kMaxSteps);
         CHECK(e->limitReason() && *e->limitReason());

         // All frames at the maximum differ from each other.
         std::set<std::vector<uint64_t>> seen;
         for (uint32_t step = 1; step <= most; ++step)
            seen.insert(signatureOf(*e, w, h, step, most));
         if (seen.size() != most)
         {
            std::printf("repeated frames: %s on %ux%u, %zu different of %u\n", c.name().c_str(), w, h, seen.size(), most);
            CHECK(seen.size() == most);
         }

         // Fewer frames than the maximum differ as well (the window offers every number).
         for (uint32_t steps : {1u, 2u, std::max(1u, most / 2), most})
         {
            std::set<std::vector<uint64_t>> s;
            for (uint32_t step = 1; step <= steps; ++step)
               s.insert(signatureOf(*e, w, h, step, steps));
            if (s.size() != steps)
            {
               std::printf("repeated frames: %s on %ux%u with %u frames\n", c.name().c_str(), w, h, steps);
               CHECK(s.size() == steps);
            }
         }

         // The maximum is tight: one more and a frame repeats (when 255 does not cap it).
         if (most < transitions::kMaxSteps)
         {
            std::set<std::vector<uint64_t>> s;
            for (uint32_t step = 1; step <= most + 1; ++step)
               s.insert(signatureOf(*e, w, h, step, most + 1));
            if (s.size() == most + 1)
               std::printf("maximum not tight: %s on %ux%u (%u)\n", c.name().c_str(), w, h, most);
            CHECK(s.size() < most + 1);
         }

         // No frame equals the starting frame, and the last frame is not completely black.
         if (!isFade(c))
         {
            std::vector<uint64_t> identity;
            for (uint32_t y = 0; y < h; ++y)
               for (uint32_t x = 0; x < w; ++x)
                  identity.push_back((1ull << 40) | (static_cast<uint64_t>(x) << 20) | y);
            for (uint32_t step = 1; step <= most; ++step)
               CHECK(signatureOf(*e, w, h, step, most) != identity);
            CHECK(visibleCount(*e, w, h, most, most) > 0);
            // Pixels only leave: strictly fewer visible pixels with every step.
            size_t before = static_cast<size_t>(w) * h;
            for (uint32_t step = 1; step <= most; ++step)
            {
               const size_t now = visibleCount(*e, w, h, step, most);
               CHECK(now < before);
               before = now;
            }
            // A visible pixel comes from inside the frame.
            for (uint32_t y = 0; y < h; ++y)
               for (uint32_t x = 0; x < w; ++x)
               {
                  const auto r = e->rule(x, y, most, most);
                  if (r.visible)
                  {
                     CHECK(r.x < w && r.y < h);
                  }
               }
         }
      }
}

void testDissolveSpecifics()
{
   const uint32_t w = 128, h = 32;
   // Orderly keeps one pixel in 256 at the last step.
   {
      auto e = transitions::kTransitions[0].make("orderly", makeInfo(w, h));
      CHECK(visibleCount(*e, w, h, 255, 255) >= w * h / 256);
      // A pixel that is gone stays gone.
      for (uint32_t step = 1; step < 20; ++step)
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
               if (!e->rule(x, y, step, 20).visible)
                  CHECK(!e->rule(x, y, step + 1, 20).visible);
   }
   // Random: the same every time, and spread over the picture.
   {
      auto a = transitions::kTransitions[0].make("random", makeInfo(w, h));
      auto b = transitions::kTransitions[0].make("random", makeInfo(w, h));
      CHECK(signatureOf(*a, w, h, 5, 10) == signatureOf(*b, w, h, 5, 10));
      size_t quad[4] = {0, 0, 0, 0};
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
            if (!a->rule(x, y, 5, 10).visible)
               ++quad[(y >= h / 2) * 2 + (x >= w / 2)];
      for (size_t q : quad)
      {
         const double share = static_cast<double>(q) / (w * h / 4.0);
         CHECK(share > 0.35 && share < 0.65);
      }
   }
   // Centre-Out hides the pixels near the centre first, Edge-In the ones at the edges first.
   for (const char* kind : {"centre_out", "edge_in"})
   {
      auto e = transitions::kTransitions[0].make(kind, makeInfo(w, h));
      auto dist = [&](uint32_t x, uint32_t y) {
         const double dx = (x + 0.5 - w / 2.0) / (w / 2.0), dy = (y + 0.5 - h / 2.0) / (h / 2.0);
         return dx * dx + dy * dy;
      };
      for (uint32_t step : {2u, 5u, 8u})
      {
         double hiddenMax = -1, hiddenMin = 1e9, shownMax = -1, shownMin = 1e9;
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
               const double d = dist(x, y);
               if (e->rule(x, y, step, 10).visible)
                  shownMax = std::max(shownMax, d), shownMin = std::min(shownMin, d);
               else
                  hiddenMax = std::max(hiddenMax, d), hiddenMin = std::min(hiddenMin, d);
            }
         if (std::strcmp(kind, "centre_out") == 0)
            CHECK(hiddenMax <= shownMin + 0.02);
         else
            CHECK(hiddenMin >= shownMax - 0.02);
      }
   }
   // Spiral: gone pixels stay gone, and the first ones to go are next to each other.
   {
      auto e = transitions::kTransitions[0].make("spiral", makeInfo(w, h));
      for (uint32_t step = 1; step < 20; ++step)
         for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
               if (!e->rule(x, y, step, 20).visible)
                  CHECK(!e->rule(x, y, step + 1, 20).visible);
      // The gone region at a small step is one connected piece (4-neighbour flood fill).
      std::vector<uint8_t> gone(w * h);
      size_t total = 0, start = 0;
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
            if (!e->rule(x, y, 2, 40).visible)
               gone[y * w + x] = 1, ++total, start = y * w + x;
      CHECK(total > 0);
      std::vector<size_t> stack{start};
      std::vector<uint8_t> done(w * h);
      size_t reached = 0;
      done[start] = 1;
      while (!stack.empty())
      {
         const size_t i = stack.back();
         stack.pop_back();
         ++reached;
         const uint32_t x = static_cast<uint32_t>(i % w), y = static_cast<uint32_t>(i / w);
         const int nx[4] = {int(x) - 1, int(x) + 1, int(x), int(x)}, ny[4] = {int(y), int(y), int(y) - 1, int(y) + 1};
         for (int k = 0; k < 4; ++k)
            if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < int(w) && ny[k] < int(h))
            {
               const size_t j = size_t(ny[k]) * w + size_t(nx[k]);
               if (gone[j] && !done[j])
                  done[j] = 1, stack.push_back(j);
            }
      }
      CHECK(reached == total);
   }
}

transitions::Rule ruleOf(const char* transition, const char* option, uint32_t x, uint32_t y, uint32_t step, uint32_t steps,
                         uint32_t w = 128, uint32_t h = 32)
{
   return transitions::findTransition(transition)->make(option, makeInfo(w, h))->rule(x, y, step, steps);
}

void testMovingSpecifics()
{
   const uint32_t w = 128, h = 32;
   // Slide: the picture moves whole; a visible pixel comes from the same row (left/right) or column.
   for (uint32_t step = 1; step <= 10; ++step)
   {
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            auto l = ruleOf("slide", "left", x, y, step, 10);
            auto r = ruleOf("slide", "right", x, y, step, 10);
            auto u = ruleOf("slide", "up", x, y, step, 10);
            auto d = ruleOf("slide", "down", x, y, step, 10);
            if (l.visible) { CHECK(l.y == y && l.x > x); }
            if (r.visible) { CHECK(r.y == y && r.x < x); }
            if (u.visible) { CHECK(u.x == x && u.y > y); }
            if (d.visible) { CHECK(d.x == x && d.y < y); }
         }
   }
   // The offset between where a pixel is and where it comes from is the same for every pixel.
   {
      auto a = ruleOf("slide", "left", 3, 4, 5, 10);
      auto b = ruleOf("slide", "left", 20, 9, 5, 10);
      CHECK(a.visible && b.visible && a.x - 3 == b.x - 20);
   }
   // Wipe: a pixel that shows is the same pixel; the bar covers the side it moves from.
   for (uint32_t step = 1; step <= 10; ++step)
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
            for (const char* dir : {"left", "right", "up", "down"})
            {
               auto r = ruleOf("wipe", dir, x, y, step, 10);
               if (r.visible)
                  CHECK(r.x == x && r.y == y);
            }
   // The bar grows: a row-wise check on the left wipe, and the right wipe is its mirror.
   {
      size_t leftEdgeHidden = 0, rightEdgeHidden = 0;
      for (uint32_t x = 0; x < w; ++x)
      {
         leftEdgeHidden += !ruleOf("wipe", "left", x, 0, 5, 10).visible;
         rightEdgeHidden += !ruleOf("wipe", "right", x, 0, 5, 10).visible;
      }
      CHECK(leftEdgeHidden == rightEdgeHidden && leftEdgeHidden > 0);
      CHECK(!ruleOf("wipe", "left", w - 1, 0, 5, 10).visible);  // the bar sits at the far (right) side
      CHECK(ruleOf("wipe", "left", 0, 0, 5, 10).visible);
      CHECK(!ruleOf("wipe", "right", 0, 0, 5, 10).visible);
      CHECK(ruleOf("wipe", "right", w - 1, 0, 5, 10).visible);
      CHECK(!ruleOf("wipe", "up", 0, h - 1, 5, 10).visible);
      CHECK(ruleOf("wipe", "up", 0, 0, 5, 10).visible);
      CHECK(!ruleOf("wipe", "down", 0, 0, 5, 10).visible);
      CHECK(ruleOf("wipe", "down", 0, h - 1, 5, 10).visible);
   }
   // Alternating slide: odd rows (1st, 3rd ...) go left, even rows right; columns up and down.
   for (uint32_t step = 1; step <= 6; ++step)
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
         {
            auto hz = ruleOf("alternating_slide", "horizontal", x, y, step, 6);
            if (hz.visible)
            {
               CHECK(hz.y == y);
               CHECK((y % 2 == 0) ? hz.x > x : hz.x < x);
            }
            auto vt = ruleOf("alternating_slide", "vertical", x, y, step, 6);
            if (vt.visible)
            {
               CHECK(vt.x == x);
               CHECK((x % 2 == 0) ? vt.y > y : vt.y < y);
            }
         }
   // Split: the left half moves left, the right half moves right, rows stay.
   for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
      {
         auto r = ruleOf("split_vertical", "", x, y, 4, 10);
         if (r.visible)
         {
            CHECK(r.y == y);
            CHECK(x < w / 2 ? r.x > x : r.x < x);
         }
         auto q = ruleOf("split_horizontal", "", x, y, 4, 10);
         if (q.visible)
         {
            CHECK(q.x == x);
            CHECK(y < h / 2 ? q.y > y : q.y < y);
         }
      }
   // Blinds: the hidden bars repeat at a regular interval and grow.
   for (const char* o : {"horizontal", "vertical"})
   {
      const bool rows = std::strcmp(o, "horizontal") == 0;
      const uint32_t n = rows ? h : w;
      auto e = transitions::findTransition("blinds")->make(o, makeInfo(w, h));
      const uint32_t most = e->maxSteps();
      std::vector<uint32_t> hiddenPerStep;
      for (uint32_t step = 1; step <= most; ++step)
      {
         uint32_t hidden = 0;
         std::vector<uint8_t> line(n);
         for (uint32_t i = 0; i < n; ++i)
         {
            const bool v = rows ? e->rule(0, i, step, most).visible : e->rule(i, 0, step, most).visible;
            line[i] = v;
            hidden += !v;
         }
         hiddenPerStep.push_back(hidden);
         // Regular: the pattern repeats with the pitch (the line is a whole number of pitches here).
         uint32_t pitch = 0;
         for (uint32_t p = 1; p < n && !pitch; ++p)
         {
            bool ok = n % p == 0;
            for (uint32_t i = 0; ok && i + p < n; ++i)
               ok = line[i] == line[i + p];
            if (ok)
               pitch = p;
         }
         CHECK(pitch > 0 && pitch < n);
      }
      for (size_t i = 1; i < hiddenPerStep.size(); ++i)
         CHECK(hiddenPerStep[i] > hiddenPerStep[i - 1]);
   }
}

void testFadeSpecifics()
{
   for (const char* o : {"black", "white"})
   {
      const bool toWhite = std::strcmp(o, "white") == 0;
      auto e = transitions::findTransition("fade")->make(o, makeInfo(128, 32));
      CHECK(e->changesColors());
      // Geometry never changes.
      auto r = e->rule(3, 4, 2, 5);
      CHECK(r.visible && r.x == 3 && r.y == 4);
      const uint32_t most = e->maxSteps();
      auto lum = [](uint16_t c) { return int((c >> 11) & 31) + int((c >> 5) & 63) / 2 + int(c & 31); };
      const uint16_t sample = 0x0841 * 4 + 0x0020;
      int previous = lum(sample);
      for (uint32_t step = 1; step <= most; ++step)
      {
         const int now = lum(e->color(sample, step, most));
         if (toWhite)
            CHECK(now >= previous);
         else
            CHECK(now <= previous);
         previous = now;
      }
      // Almost black / almost white, never exactly.
      uint16_t brightest = 0, darkest = 0xFFFF;
      for (uint16_t c : makeInfo(128, 32).colors)
         if (lum(c) > lum(brightest)) brightest = c;
      for (uint16_t c : makeInfo(128, 32).colors)
         if (lum(c) < lum(darkest)) darkest = c;
      if (toWhite)
         CHECK(e->color(darkest, most, most) != 0xFFFF);
      else
         CHECK(e->color(brightest, most, most) != 0);
      if (toWhite)
         CHECK(lum(e->color(darkest, most, most)) > lum(darkest));
      else
         CHECK(lum(e->color(brightest, most, most)) < lum(brightest));
      // An all-black picture cannot fade to black: no steps.
      transitions::SourceInfo black = makeInfo(128, 32);
      black.colors = {0};
      black.brightestShade = black.darkestShade = 0;
      if (!toWhite)
         CHECK(transitions::findTransition("fade")->make(o, black)->maxSteps() == 0);
   }
}

void testIcons()
{
   for (const Case& c : allCases())
   {
      const preview::Icon icon = preview::makeIcon(*c.transition, c.option);
      CHECK(icon.width == 16 && icon.height == 4 && icon.gray.size() == 64);
      const auto [lo, hi] = std::minmax_element(icon.gray.begin(), icon.gray.end());
      // Every picture has light and dark in it.
      if (*lo == *hi)
         std::printf("flat icon for %s\n", c.name().c_str());
      CHECK(*lo < *hi);
   }
   std::vector<preview::GalleryItem> items;
   for (const auto& t : transitions::kTransitions)
      items.push_back({t.label, preview::makeIcon(t, t.optionCount ? &t.options[0] : nullptr)});
   const std::string html = preview::galleryHtml("a <b> & c", items);
   CHECK(!html.empty() && html[0] == '<');
   CHECK(html.find("a &lt;b&gt; &amp; c") != std::string::npos);
   CHECK(html.find("background-color:#") != std::string::npos);
}

// ---- Plugin tests ---------------------------------------------------------------------------

size_t countShade(const std::vector<uint8_t>& v, uint8_t shade)
{
   return static_cast<size_t>(std::count(v.begin(), v.end(), shade));
}

void checkNewFrames(size_t added, bool zones)
{
   CHECK(g_frames.size() == 2 + added);
   CHECK(g_endEditApplied == 1);
   CHECK(g_shown.size() == added);
   for (size_t i = 2; i < g_frames.size(); ++i)
   {
      CHECK(g_frames[i].comparisonMask == CDMDPI_NONE);
      CHECK(g_frames[i].original.size() == static_cast<size_t>(g_w) * g_h);
      CHECK(g_frames[i].sd.size() == static_cast<size_t>(g_w) * g_h);
      if (zones)
         CHECK(g_frames[i].zones.size() == static_cast<size_t>(g_w) * g_h);
   }
   // The starting frame is never changed.
   CHECK(g_frames[0].original == makeSourceFrame(zones).original);
   CHECK(g_frames[0].sd == makeSourceFrame(zones).sd);
}

void testEveryTransitionThroughTheWindows(uint32_t w, uint32_t h, bool zones)
{
   for (const Case& c : allCases())
   {
      resetProject(w, h, zones);
      g_frames[0].comparisonMask = 3; // the source has a mask; the new frames must not
      const uint32_t frames = 5;
      if (!runFlow(c.transition->id, c.option ? c.option->id : nullptr, frames))
      {
         std::printf("flow failed for %s on %ux%u\n", c.name().c_str(), w, h);
         CHECK(false);
         continue;
      }
      if (g_frames.size() != 2 + frames)
      {
         std::printf("wrong frame count for %s on %ux%u (messages: %s)\n", c.name().c_str(), w, h,
                     g_messages.empty() ? "none" : g_messages.back().c_str());
         CHECK(g_frames.size() == 2 + frames);
         continue;
      }
      checkNewFrames(frames, zones);
      CHECK(g_status.size() == 1 && g_status[0].find("added 5 frame(s)") != std::string::npos);
      // All new frames differ from one another and from the start, in both the shades and the colors.
      std::set<std::pair<std::vector<uint8_t>, std::vector<uint16_t>>> seen;
      for (size_t i = 2; i < g_frames.size(); ++i)
      {
         CHECK(g_frames[i].original != g_frames[0].original || g_frames[i].sd != g_frames[0].sd);
         seen.insert({g_frames[i].original, g_frames[i].sd});
      }
      CHECK(seen.size() == frames);
      // The last frame is not all black (shade 0 / color 0) unless it is a fade, which keeps
      // its geometry and so is never all black either.
      const Frame& last = g_frames.back();
      CHECK(countShade(last.original, 0) < last.original.size() || isFade(c));
      CHECK(std::count(last.sd.begin(), last.sd.end(), 0) < static_cast<long>(last.sd.size()));
      // Black pixels in the original are black in the picture, too, and out of the zones.
      if (!isFade(c))
         for (size_t i = 2; i < g_frames.size(); ++i)
            for (size_t p = 0; p < g_frames[i].original.size(); ++p)
            {
               const bool black = g_frames[i].sd[p] == 0;
               CHECK(black == (g_frames[i].original[p] == 0));
               if (zones && black)
                  CHECK(g_frames[i].zones[p] == 255);
            }
      if (isFade(c) && zones)
         for (size_t i = 2; i < g_frames.size(); ++i)
         {
            CHECK(g_frames[i].rotations == 0);
            CHECK(g_frames[i].dynamicColors != g_frames[0].dynamicColors);
         }
      if (!isFade(c) && zones)
         for (size_t i = 2; i < g_frames.size(); ++i)
            CHECK(g_frames[i].rotations == 2); // other transitions leave the rotations alone
   }
}

void testWindowsOfTheFlow()
{
   resetProject(128, 32, false);
   openMenu();
   // Window 1: the transition list holds every transition; the pictures check box exists.
   CHECK(g_form.open && g_form.accept == "Next");
   const Form::Field* t = fieldOf("transition");
   CHECK(t && t->kind == CDMDPI_FIELD_CHOICE);
   if (t)
   {
      const auto values = choiceValues(*t);
      CHECK(values.size() == transitions::kTransitionCount);
      for (size_t i = 0; i < values.size(); ++i)
         CHECK(values[i] == transitions::kTransitions[i].id);
   }
   const Form::Field* pic = fieldOf("pictures");
   CHECK(pic && pic->kind == CDMDPI_FIELD_CHECKBOX);
   size_t notes = 0;
   for (const auto& f : g_form.fields)
      notes += f.kind == CDMDPI_FIELD_NOTE;
   CHECK(notes == 2); // the intro and the gallery
   pick("transition", "dissolve");

   // Window 2: the dissolve types.
   CHECK(g_form.open && g_form.accept == "Next");
   const Form::Field* o = fieldOf("option_dissolve");
   CHECK(o && o->kind == CDMDPI_FIELD_CHOICE);
   if (o)
   {
      const auto values = choiceValues(*o);
      const std::vector<std::string> want = {"orderly", "random", "centre_out", "edge_in", "spiral"};
      CHECK(values == want);
      CHECK(o->label == "Dissolve Type");
   }
   pick("option_dissolve", "spiral");

   // Window 3: exactly 1 .. maxSteps, no free text.
   CHECK(g_form.open && g_form.accept == "Apply");
   const Form::Field* f = fieldOf("frames");
   CHECK(f && f->kind == CDMDPI_FIELD_CHOICE);
   if (f)
   {
      const auto values = choiceValues(*f);
      CHECK(values.size() == 255);
      for (size_t i = 0; i < values.size(); ++i)
         CHECK(values[i] == std::to_string(i + 1));
      CHECK(f->def == "10");
   }
   closeForm(true);
   CHECK(g_frames.size() == 12); // 10 new frames
   checkNewFrames(10, false);
}

void testFramesListMatchesTheMaximum()
{
   const uint32_t sizes[][2] = {{128, 32}, {192, 64}, {256, 64}};
   for (const auto& sz : sizes)
      for (const Case& c : allCases())
      {
         resetProject(sz[0], sz[1], false);
         openMenu();
         pick("transition", c.transition->id);
         if (c.option)
            pick((std::string("option_") + c.transition->id).c_str(), c.option->id);
         CHECK(g_form.open && g_form.accept == "Apply");
         const Form::Field* f = fieldOf("frames");
         if (!f)
         {
            CHECK(false);
            continue;
         }
         auto e = makeEffect(c, sz[0], sz[1]);
         // The plugin's own effect sees the real frame, whose colors differ from makeInfo: only
         // the geometry-bound transitions have to agree exactly.
         if (!isFade(c))
            CHECK(choiceValues(*f).size() == e->maxSteps());
         CHECK(!choiceValues(*f).empty());
      }
}

void testSavedFramesAreClamped()
{
   // The saved number is larger than this transition allows: the window opens on the most.
   resetProject(128, 32, false);
   g_settings["frames"] = "200";
   openMenu();
   pick("transition", "split_horizontal");
   CHECK(g_form.open);
   const Form::Field* f = fieldOf("frames");
   CHECK(f != nullptr);
   if (f)
   {
      const size_t most = choiceValues(*f).size();
      CHECK(most == 15);
      CHECK(f->def == std::to_string(most));
      CHECK(g_settings["frames"] == std::to_string(most));
   }
   // And one saved as nonsense opens on a valid number.
   resetProject(128, 32, false);
   g_settings["frames"] = "banana";
   openMenu();
   pick("transition", "split_horizontal");
   const Form::Field* g = fieldOf("frames");
   CHECK(g && !g->def.empty() && std::atol(g->def.c_str()) >= 1 && std::atol(g->def.c_str()) <= 15);
}

void testCancelChangesNothing()
{
   // Cancel in each of the three windows.
   for (int where = 0; where < 3; ++where)
   {
      resetProject(128, 32, false);
      openMenu();
      if (where == 0)
         closeForm(false);
      else
      {
         pick("transition", "dissolve");
         if (where == 1)
            closeForm(false);
         else
         {
            pick("option_dissolve", "random");
            closeForm(false);
         }
      }
      CHECK(g_frames.size() == 2);
      CHECK(g_endEditApplied == 0);
      CHECK(!g_form.open);
      CHECK(g_messages.empty());
   }
}

void testRefusalRollsBack()
{
   resetProject(128, 32, true);
   g_addFrameLimit = 3; // the fourth AddFrame is refused
   runFlow("dissolve", "orderly", 6);
   CHECK(g_frames.size() == 2);
   CHECK(g_endEditApplied == 0);
   CHECK(!g_inBatch);
   CHECK(g_messages.size() == 1 && g_messages[0].find("Nothing was changed") != std::string::npos);
   CHECK(g_status.empty());
}

void testProjectChangedWhileWindowsAreOpen()
{
   resetProject(128, 32, false);
   openMenu();
   pick("transition", "split_vertical");
   CHECK(g_form.open);
   ++g_revision; // the author edits the project while the window is open
   closeForm(true);
   CHECK(g_frames.size() == 2);
   CHECK(g_messages.size() == 1 && g_messages[0].find("changed") != std::string::npos);
}

void testNoFrameSelected()
{
   resetProject(128, 32, false);
   g_current = CDMDPI_NONE;
   openMenu();
   CHECK(!g_form.open);
   CHECK(g_messages.size() == 1);
   resetProject(128, 32, false);
   g_current = 99;
   openMenu();
   CHECK(!g_form.open);
   CHECK(g_messages.size() == 1);
}

void testAllBlackFrameCannotFadeToBlack()
{
   resetProject(128, 32, false);
   for (auto& f : g_frames)
   {
      std::fill(f.original.begin(), f.original.end(), 0);
      std::fill(f.sd.begin(), f.sd.end(), 0);
   }
   openMenu();
   pick("transition", "fade");
   pick("option_fade", "black");
   CHECK(!g_form.open);
   CHECK(g_messages.size() == 1);
   CHECK(g_frames.size() == 2);
}

void testBackgroundCaveat()
{
   resetProject(128, 32, false);
   g_frames[0].background = 1;
   runFlow("wipe", "left", 4);
   CHECK(g_frames.size() == 6);
   CHECK(g_messages.size() == 1 && g_messages[0].find("background") != std::string::npos);
}

void testPicturesCanBeSwitchedOff()
{
   resetProject(128, 32, false);
   g_settings["pictures"] = "0";
   openMenu();
   size_t notes = 0;
   for (const auto& f : g_form.fields)
      notes += f.kind == CDMDPI_FIELD_NOTE;
   CHECK(notes == 1); // only the intro
   pick("transition", "slide");
   notes = 0;
   for (const auto& f : g_form.fields)
      notes += f.kind == CDMDPI_FIELD_NOTE;
   CHECK(notes == 1); // only the description
}

void testNoRepeatedFramesInThePlugin()
{
   // Every frame count the window offers gives all-different frames: the maximum of two
   // geometry-bound transitions, through the whole plugin.
   for (const char* id : {"split_horizontal", "split_vertical"})
   {
      resetProject(128, 32, false);
      openMenu();
      pick("transition", id);
      const Form::Field* f = fieldOf("frames");
      const long most = f ? static_cast<long>(choiceValues(*f).size()) : 0;
      g_settings["frames"] = std::to_string(most);
      closeForm(true);
      CHECK(g_frames.size() == static_cast<size_t>(2 + most));
      std::set<std::vector<uint8_t>> seen;
      for (size_t i = 2; i < g_frames.size(); ++i)
         seen.insert(g_frames[i].sd.empty() ? g_frames[i].original : std::vector<uint8_t>(g_frames[i].original));
      CHECK(seen.size() == static_cast<size_t>(most));
   }
}

} // namespace

int main()
{
   setUpApi();
   TransitionBuilderPluginLoad(1, &g_bus);
   CHECK(g_menuCallback != nullptr);

   testRegistry();
   testBayerIsAPermutation();
   testEveryEffectOnEverySize();
   testDissolveSpecifics();
   testMovingSpecifics();
   testFadeSpecifics();
   testIcons();

   testWindowsOfTheFlow();
   testFramesListMatchesTheMaximum();
   testSavedFramesAreClamped();
   testCancelChangesNothing();
   testRefusalRollsBack();
   testProjectChangedWhileWindowsAreOpen();
   testNoFrameSelected();
   testAllBlackFrameCannotFadeToBlack();
   testBackgroundCaveat();
   testPicturesCanBeSwitchedOff();
   testNoRepeatedFramesInThePlugin();
   // Every transition through the windows, on the sizes of real projects (single plane at the
   // ROM resolution for 192x64 and 256x64), without and with dynamic zones.
   const uint32_t sizes[][2] = {{128, 32}, {192, 64}, {256, 64}};
   for (const auto& sz : sizes)
   {
      testEveryTransitionThroughTheWindows(sz[0], sz[1], false);
      testEveryTransitionThroughTheWindows(sz[0], sz[1], true);
   }

   TransitionBuilderPluginUnload();
   CHECK(g_menuCallback == nullptr);

   if (g_failures)
   {
      std::printf("%d check(s) failed\n", g_failures);
      return 1;
   }
   std::printf("all tests passed\n");
   return 0;
}
