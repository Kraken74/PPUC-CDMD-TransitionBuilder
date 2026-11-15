// license:GPLv3+
//
// The transitions of the Transition Builder plugin.
//
// A transition answers one question for every pixel of the new frame: "where does this pixel
// come from?" It names a pixel of the original frame, or says the pixel is black. The plugin
// asks that on the original frame's grid and applies the answer to the original frame and to
// every colorized plane (SD and HD) and the dynamic zones, so a transition does not need to
// know anything about the editor. A dissolve keeps every pixel where it is or blackens it; a
// split moves pixels sideways.
//
// To add a transition: write a function like dissolveMap below and add one line to
// kTransitions. It then shows up in the plugin's drop-down list.
//
// Steps run from 1 to `steps`. The picture you start from is step 0 and is not created. The
// last step is never completely black: a thin rest of the picture is left.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace transitions {

// The most frames one transition may create. The dissolve has 256 levels, so more frames than
// this would only repeat pictures.
inline constexpr uint32_t kMaxSteps = 256;

struct SourcePixel
{
   bool visible; // false: the pixel is black, x and y mean nothing
   uint32_t x;   // the pixel of the original frame it shows
   uint32_t y;
};

// width and height are the original frame's size; (x, y) is a pixel of the new frame.
using MapFunction = SourcePixel (*)(uint32_t x, uint32_t y, uint32_t step, uint32_t steps,
                                    uint32_t width, uint32_t height);

// ---- Dissolve -------------------------------------------------------------------------------

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

// Dissolve to black: at step `step` of `steps` a share of about step / steps of the pixels is
// black, in a fixed pattern. A pixel that is black at one step stays black at every later step.
// The last step keeps one pixel in 256 (the one the pattern blackens last).
inline SourcePixel dissolveMap(uint32_t x, uint32_t y, uint32_t step, uint32_t steps,
                               uint32_t /*width*/, uint32_t /*height*/)
{
   const uint32_t level = (255u * step + steps / 2u) / steps; // 0 .. 255, rounded
   return {bayer16(x, y) >= level, x, y};
}

// ---- Split ----------------------------------------------------------------------------------

// How far each half has moved apart at this step, in pixels along an axis `extent` long. It
// grows from 0 to the distance at which only a thin strip of each half is still on screen
// (1/32 of the half, at least one pixel).
inline uint32_t splitShift(uint32_t extent, uint32_t step, uint32_t steps)
{
   const uint32_t half = extent / 2u;
   const uint32_t tail = std::max(1u, half / 32u);
   const uint32_t travel = half > tail ? half - tail : 0u;
   return (travel * step + steps / 2u) / steps;
}

// One axis of a split. The first half moves toward 0 and the second half toward the end by
// `shift`; between them is black. Returns the position the pixel shows, or false.
inline bool splitAxis(uint32_t pos, uint32_t extent, uint32_t shift, uint32_t& source)
{
   const uint32_t half = extent / 2u;
   if (pos + shift < half) // the first half, moved toward 0
   {
      source = pos + shift;
      return true;
   }
   if (pos >= half + shift) // the second half, moved toward the end
   {
      source = pos - shift;
      return true;
   }
   return false;
}

// Split Vertical: the frame is cut down the middle by a vertical line; the left half slides
// to the left and the right half slides to the right, off the screen.
inline SourcePixel splitVerticalMap(uint32_t x, uint32_t y, uint32_t step, uint32_t steps,
                                    uint32_t width, uint32_t /*height*/)
{
   uint32_t source = 0;
   if (!splitAxis(x, width, splitShift(width, step, steps), source))
      return {false, 0, 0};
   return {true, source, y};
}

// Split Horizontal: the frame is cut across the middle by a horizontal line; the top half
// slides up and the bottom half slides down, off the screen.
inline SourcePixel splitHorizontalMap(uint32_t x, uint32_t y, uint32_t step, uint32_t steps,
                                      uint32_t /*width*/, uint32_t height)
{
   uint32_t source = 0;
   if (!splitAxis(y, height, splitShift(height, step, steps), source))
      return {false, 0, 0};
   return {true, x, source};
}

// ---- The list -------------------------------------------------------------------------------

struct Transition
{
   const char* id;    // kept in the plugin's settings
   const char* label; // shown in the drop-down list
   MapFunction map;
};

inline constexpr Transition kTransitions[] = {
   {"dissolve", "Dissolve", &dissolveMap},
   {"split_vertical", "Split Vertical", &splitVerticalMap},
   {"split_horizontal", "Split Horizontal", &splitHorizontalMap},
};
inline constexpr std::size_t kTransitionCount = sizeof(kTransitions) / sizeof(kTransitions[0]);

} // namespace transitions
