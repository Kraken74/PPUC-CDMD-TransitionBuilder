// license:GPLv3+
//
// The transitions of the Transition Builder plugin.
//
// A transition decides, for every pixel of the original frame's grid, whether that pixel is
// black at a given step of the transition. The plugin applies that decision to the original
// frame and to every colorized plane (SD and HD), so a transition does not need to know
// anything about the editor.
//
// To add a transition: write a function like dissolveIsBlack below and add one line to
// kTransitions. It then shows up in the plugin's drop-down list.

#pragma once

#include <cstddef>
#include <cstdint>

namespace transitions {

// The most frames one transition may create. The dissolve has 256 distinct levels, so more
// frames than this would only repeat pictures.
inline constexpr uint32_t kMaxSteps = 256;

// A 16x16 ordered-dither (Bayer) matrix, computed: every value 0 .. 255 appears exactly once,
// and the values are spread evenly, so thresholding it gives a regular, fine pattern.
inline uint32_t bayer16(uint32_t x, uint32_t y)
{
   x &= 15u;
   y &= 15u;
   const uint32_t diagonal = x ^ y;
   uint32_t value = 0;
   for (uint32_t bit = 0; bit < 4; ++bit)
   {
      const uint32_t shift = 2u * (3u - bit);
      value |= ((diagonal >> bit) & 1u) << (shift + 1u);
      value |= ((y >> bit) & 1u) << shift;
   }
   return value;
}

// Dissolve to black: at step `step` of `steps` (1 .. steps) a share of step / steps of the
// pixels is black, in a fixed pattern. A pixel that is black at one step stays black at every
// later step, and the last step is completely black.
inline bool dissolveIsBlack(uint32_t x, uint32_t y, uint32_t step, uint32_t steps,
                            uint32_t /*width*/, uint32_t /*height*/)
{
   const uint32_t level = (256u * step + steps / 2u) / steps; // 0 .. 256, rounded
   return bayer16(x, y) < level;
}

struct Transition
{
   const char* id;    // kept in the plugin's settings
   const char* label; // shown in the drop-down list
   // Is the pixel (x, y) of the original frame's grid black at this step (1 .. steps)?
   bool (*isBlack)(uint32_t x, uint32_t y, uint32_t step, uint32_t steps, uint32_t width,
                   uint32_t height);
};

inline constexpr Transition kTransitions[] = {
   {"dissolve", "Dissolve", &dissolveIsBlack},
};
inline constexpr std::size_t kTransitionCount = sizeof(kTransitions) / sizeof(kTransitions[0]);

} // namespace transitions
