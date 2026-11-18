// license:GPLv3+
//
// Drawing the steps of a transition: the pictures of the new frames, and the preview of the
// window, which is drawn the same way from the author's own frame, so it cannot disagree with
// what Apply makes.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Transitions.h"

namespace preview {

// Makes the picture of one step: `source` is a picture of width x height pixels (the original
// frame, a colorized plane or the dynamic zones). The effect decides on the original frame's
// grid (originalWidth x originalHeight) where each pixel comes from. A picture on a finer grid
// (the HD plane is twice as large) follows the same decision, and a pixel keeps its place inside
// its cell, so a move is as smooth as the picture is fine. Pixels that show nothing get
// `black`; `recolor` changes the color of the ones that do (the fade).
template <class T, class Recolor>
void renderStep(const std::vector<T>& source, std::vector<T>& out, uint32_t width, uint32_t height,
                uint32_t originalWidth, uint32_t originalHeight, const transitions::Effect& effect,
                uint32_t step, uint32_t steps, T black, Recolor recolor, const std::vector<T>* under = nullptr)
{
   out.assign(source.size(), black);
   // The pixels that show nothing of the source show the picture underneath (the other frame of
   // a transition to another frame), in the same place, when there is one.
   if (under && under->size() == source.size())
      out = *under;
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

// The picture of one step of a cross-fade: every pixel blends from the source toward the picture
// underneath by the effect's weight.
template <class T, class Mix>
void renderCross(const std::vector<T>& source, const std::vector<T>& under, std::vector<T>& out,
                 const transitions::Effect& effect, uint32_t step, uint32_t steps, Mix mix)
{
   const uint32_t weight = effect.weight(step, steps);
   out.resize(source.size());
   for (size_t i = 0; i < source.size(); ++i)
      out[i] = i < under.size() ? mix(source[i], under[i], weight) : source[i];
}

// Blends two RGB565 colors (weight of b: 0 .. 65536), each channel on its own.
inline uint16_t mixColor(uint16_t a, uint16_t b, uint32_t weight)
{
   auto channel = [&](int x, int y) {
      return static_cast<uint32_t>(x + static_cast<int>((static_cast<int64_t>(y - x) * weight + 32768) >> 16));
   };
   const uint32_t r = channel((a >> 11) & 31, (b >> 11) & 31);
   const uint32_t g = channel((a >> 5) & 63, (b >> 5) & 63);
   const uint32_t bl = channel(a & 31, b & 31);
   return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

// Zones and shades cannot blend: a pixel keeps the zone and the shade of the starting frame until
// the halfway point, then has those of the other frame.
inline uint8_t mixZone(uint8_t a, uint8_t b, uint32_t weight) { return weight < 32768 ? a : b; }

// The pictures the preview shows.
inline constexpr uint32_t kPreviewPictures = 4;

// The steps the preview shows for a transition of `steps` frames: spread evenly over it, the
// last one always among them, each at most once (fewer pictures when there are fewer steps).
inline std::vector<uint32_t> previewSteps(uint32_t steps)
{
   std::vector<uint32_t> out;
   for (uint32_t i = 1; i <= kPreviewPictures; ++i)
   {
      const uint32_t step = std::max(1u, (steps * i + kPreviewPictures - 1) / kPreviewPictures);
      if (step <= steps && (out.empty() || out.back() != step))
         out.push_back(step);
   }
   return out;
}

// How large the preview draws a picture of this width: small frames twice, so that their
// pixels can be told apart.
inline uint32_t previewScale(uint32_t width) { return width <= 128 ? 2 : 1; }

} // namespace preview
