// license:GPLv3+
//
// The transitions of the Transition Builder plugin.
//
// A transition is an Effect. For every pixel of the new frame it answers "where does this pixel
// come from?": a pixel of the starting frame, or black. The plugin asks that on the original
// frame's grid and applies the answer to the original frame, to every colorized plane (SD and
// HD) and to the dynamic zones, so an effect does not need to know anything about the editor. A
// dissolve keeps each pixel where it is or blackens it; a slide moves pixels; a split moves the
// two halves apart. Only the fade changes colors: it also gives the new color of a color.
//
// To add a transition: write an Effect, a function that makes it, and one entry in kTransitions
// (with its options, if it has any). It then shows up in the plugin's windows.
//
// Steps run from 1 to `steps`. The starting frame is step 0 and is not created. The last step
// is never completely black (and a fade to white never completely white): something of the
// picture is left. Every frame of a transition differs from the others and from the starting
// frame, so each effect has a most frames (maxSteps) for the frame it is made for.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string_view>
#include <vector>

namespace transitions {

// The most frames any transition may create.
inline constexpr uint32_t kMaxSteps = 255;

struct Rule
{
   bool visible; // false: the pixel is black, x and y mean nothing
   uint32_t x;   // the pixel of the starting frame it shows
   uint32_t y;
};

// What an effect needs to know about the frame it starts from.
struct SourceInfo
{
   uint32_t width = 0; // the original frame's size
   uint32_t height = 0;
   uint32_t colorCount = 4;       // shades of the original frame
   uint32_t brightestShade = 0;   // the brightest and the darkest shade the original uses
   uint32_t darkestShade = 0;
   std::vector<uint16_t> colors;  // the distinct RGB565 colors of the colorized pictures
};

class Effect
{
public:
   virtual ~Effect() = default;

   // The most steps that still give frames that all differ (from each other and from the
   // starting frame). 0: the effect can do nothing with this frame.
   virtual uint32_t maxSteps() const = 0;
   // Why there is a most, in a few words, for the window that asks for the number of frames.
   virtual const char* limitReason() const = 0;

   // Where the pixel (x, y) of the new frame comes from (the identity by default).
   virtual Rule rule(uint32_t x, uint32_t y, uint32_t /*step*/, uint32_t /*steps*/) const
   {
      return {true, x, y};
   }

   // Effects that change colors (the fade) say so and give the new color of an RGB565 color
   // and of an original shade.
   virtual bool changesColors() const { return false; }
   virtual uint16_t color(uint16_t c, uint32_t /*step*/, uint32_t /*steps*/) const { return c; }
   virtual uint8_t shade(uint8_t s, uint32_t /*step*/, uint32_t /*steps*/) const { return s; }
};

// ---- Helpers --------------------------------------------------------------------------------

// A hash that spreads every input over 64 bits (splitmix64). The same on every compiler, so a
// random dissolve looks the same everywhere.
inline uint64_t mix64(uint64_t z)
{
   z += 0x9E3779B97F4A7C15ull;
   z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
   z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
   return z ^ (z >> 31);
}

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

// Dissolve, Orderly: at step `step` of `steps` a share of about step / steps of the pixels is
// black, in the fixed pattern of the Bayer matrix. A pixel that is black at one step stays
// black at every later step. The last step keeps one pixel in 256.
class OrderlyDissolve final : public Effect
{
public:
   OrderlyDissolve(uint32_t width, uint32_t height) : m_pixels(static_cast<uint64_t>(width) * height) {}
   uint32_t maxSteps() const override { return static_cast<uint32_t>(std::min<uint64_t>(kMaxSteps, m_pixels)); }
   const char* limitReason() const override { return "the pattern has 255 steps"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t level = (255u * step + steps / 2u) / steps; // 0 .. 255, rounded
      return {bayer16(x, y) >= level, x, y};
   }

private:
   uint64_t m_pixels;
};

// The order in which a dissolve takes the pixels.
enum class Order
{
   Random,    // a fixed shuffle
   CentreOut, // from the centre outward, on the frame's own proportions (an iris opening)
   EdgeIn,    // from the outside inward (an iris closing)
   Spiral,    // along a spiral from the top left corner, clockwise, to the centre
};

// A rank for every pixel (row-major): 0 goes first, width * height - 1 last. A permutation.
inline std::vector<uint32_t> makeRanks(Order order, uint32_t width, uint32_t height)
{
   const size_t count = static_cast<size_t>(width) * height;
   std::vector<uint32_t> ranks(count, 0);

   if (order == Order::Spiral)
   {
      // The usual spiral walk: the top row, down the right column, back along the bottom row,
      // up the left column, then the same one ring further in.
      int64_t top = 0, left = 0, bottom = static_cast<int64_t>(height) - 1,
              right = static_cast<int64_t>(width) - 1;
      uint32_t next = 0;
      auto at = [&](int64_t x, int64_t y) -> uint32_t& { return ranks[static_cast<size_t>(y) * width + static_cast<size_t>(x)]; };
      while (top <= bottom && left <= right)
      {
         for (int64_t x = left; x <= right; ++x)
            at(x, top) = next++;
         ++top;
         for (int64_t y = top; y <= bottom; ++y)
            at(right, y) = next++;
         --right;
         if (top <= bottom)
         {
            for (int64_t x = right; x >= left; --x)
               at(x, bottom) = next++;
            --bottom;
         }
         if (left <= right)
         {
            for (int64_t y = bottom; y >= top; --y)
               at(left, y) = next++;
            ++left;
         }
      }
      return ranks;
   }

   // The others sort the pixels by a key and break ties with a hash of the pixel's place, so
   // that pixels at the same distance do not go in a visible order.
   std::vector<uint64_t> key(count, 0);
   std::vector<uint64_t> tie(count, 0);
   uint64_t farthest = 0;
   for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
      {
         const size_t i = static_cast<size_t>(y) * width + x;
         tie[i] = mix64(i);
         if (order == Order::Random)
            continue;
         // The squared distance from the centre, with each axis in units of the frame's size,
         // times (width * height)^2, so that it is exact in integers: an ellipse that touches
         // all four edges, not a circle that hits the top and bottom first.
         const int64_t dx = (2 * static_cast<int64_t>(x) - (static_cast<int64_t>(width) - 1)) * height;
         const int64_t dy = (2 * static_cast<int64_t>(y) - (static_cast<int64_t>(height) - 1)) * width;
         key[i] = static_cast<uint64_t>(dx * dx + dy * dy);
         farthest = std::max(farthest, key[i]);
      }
   if (order == Order::EdgeIn)
      for (auto& k : key)
         k = farthest - k;

   std::vector<uint32_t> index(count);
   std::iota(index.begin(), index.end(), 0u);
   std::sort(index.begin(), index.end(), [&](uint32_t a, uint32_t b) {
      if (key[a] != key[b])
         return key[a] < key[b];
      if (tie[a] != tie[b])
         return tie[a] < tie[b];
      return a < b;
   });
   for (size_t rank = 0; rank < count; ++rank)
      ranks[index[rank]] = static_cast<uint32_t>(rank);
   return ranks;
}

// Dissolve with a given order. At step `step` of `steps` the first pixels of the order are
// black, so that the share grows evenly and the last step keeps one pixel in 256 (where the
// order ends: the corners for Centre-Out, the middle for Edge-In and Spiral).
class RankDissolve final : public Effect
{
public:
   RankDissolve(Order order, uint32_t width, uint32_t height)
      : m_width(width), m_ranks(makeRanks(order, width, height)),
        m_keep(std::max<uint64_t>(1, m_ranks.size() / 256))
   {
   }
   uint32_t maxSteps() const override
   {
      const uint64_t vanish = m_ranks.size() > m_keep ? m_ranks.size() - m_keep : 0;
      return static_cast<uint32_t>(std::min<uint64_t>(kMaxSteps, vanish));
   }
   const char* limitReason() const override { return "each step has to take at least one pixel"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint64_t vanish = m_ranks.size() - m_keep;
      const uint64_t threshold = (vanish * step + steps / 2u) / steps;
      return {m_ranks[static_cast<size_t>(y) * m_width + x] >= threshold, x, y};
   }

private:
   uint32_t m_width;
   std::vector<uint32_t> m_ranks;
   uint64_t m_keep;
};

// ---- Moving along an axis -------------------------------------------------------------------

enum class Direction
{
   Up,
   Down,
   Left,
   Right,
};

// Everything that moves leaves a thin strip of the picture behind at the end: 1/32 of the axis,
// at least one pixel. The pieces move in whole pixels, so the travel is also the most steps.
inline uint32_t tailOf(uint32_t extent) { return std::max(1u, extent / 32u); }
inline uint32_t travelOf(uint32_t extent)
{
   const uint32_t tail = tailOf(extent);
   return extent > tail ? extent - tail : 0u;
}
inline uint32_t offsetAt(uint32_t extent, uint32_t step, uint32_t steps)
{
   return static_cast<uint32_t>((static_cast<uint64_t>(travelOf(extent)) * step + steps / 2u) / steps);
}

inline bool isHorizontalMove(Direction d) { return d == Direction::Left || d == Direction::Right; }

// One pixel of a picture moved by `offset` in a direction, on a width x height frame.
inline Rule movedPixel(Direction d, uint32_t x, uint32_t y, uint32_t offset, uint32_t width, uint32_t height)
{
   switch (d)
   {
   case Direction::Left:
      return x + offset < width ? Rule{true, x + offset, y} : Rule{false, 0, 0};
   case Direction::Right:
      return x >= offset ? Rule{true, x - offset, y} : Rule{false, 0, 0};
   case Direction::Up:
      return y + offset < height ? Rule{true, x, y + offset} : Rule{false, 0, 0};
   case Direction::Down:
      return y >= offset ? Rule{true, x, y - offset} : Rule{false, 0, 0};
   }
   return {false, 0, 0};
}

// Slide: the whole picture slides off the screen in the direction.
class Slide final : public Effect
{
public:
   Slide(Direction direction, uint32_t width, uint32_t height)
      : m_direction(direction), m_width(width), m_height(height)
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, travelOf(extent())); }
   const char* limitReason() const override { return "the picture moves in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      return movedPixel(m_direction, x, y, offsetAt(extent(), step, steps), m_width, m_height);
   }

private:
   uint32_t extent() const { return isHorizontalMove(m_direction) ? m_width : m_height; }
   Direction m_direction;
   uint32_t m_width, m_height;
};

// Wipe: a black bar moves over the picture in the direction (Left: it comes in from the right
// edge and moves left). The picture itself stays where it is.
class Wipe final : public Effect
{
public:
   Wipe(Direction direction, uint32_t width, uint32_t height)
      : m_direction(direction), m_width(width), m_height(height)
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, travelOf(extent())); }
   const char* limitReason() const override { return "the bar moves in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t covered = offsetAt(extent(), step, steps);
      bool visible = true;
      switch (m_direction)
      {
      case Direction::Left: visible = x < m_width - covered; break;
      case Direction::Right: visible = x >= covered; break;
      case Direction::Up: visible = y < m_height - covered; break;
      case Direction::Down: visible = y >= covered; break;
      }
      return {visible, x, y};
   }

private:
   uint32_t extent() const { return isHorizontalMove(m_direction) ? m_width : m_height; }
   Direction m_direction;
   uint32_t m_width, m_height;
};

// Alternating Slide: every other line slides one way, the lines between slide the other way.
// Horizontal: the 1st, 3rd, 5th ... row slide left and the 2nd, 4th ... row slide right.
// Vertical: the 1st, 3rd ... column go up and the 2nd, 4th ... column go down.
class AlternatingSlide final : public Effect
{
public:
   AlternatingSlide(bool rows, uint32_t width, uint32_t height) : m_rows(rows), m_width(width), m_height(height) {}
   uint32_t maxSteps() const override { return std::min(kMaxSteps, travelOf(extent())); }
   const char* limitReason() const override { return "the lines move in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t offset = offsetAt(extent(), step, steps);
      // Counted as people count: the first row (y = 0) is an odd one.
      const bool odd = ((m_rows ? y : x) % 2u) == 0u;
      const Direction d = m_rows ? (odd ? Direction::Left : Direction::Right)
                                 : (odd ? Direction::Up : Direction::Down);
      return movedPixel(d, x, y, offset, m_width, m_height);
   }

private:
   uint32_t extent() const { return m_rows ? m_width : m_height; }
   bool m_rows;
   uint32_t m_width, m_height;
};

// Split: the picture is cut in the middle; the two halves move apart (vertical: a vertical cut,
// the left half to the left and the right half to the right; horizontal: the top half up and
// the bottom half down) until a thin strip of each is left.
inline uint32_t splitTravel(uint32_t extent)
{
   const uint32_t half = extent / 2u;
   const uint32_t tail = std::max(1u, half / 32u);
   return half > tail ? half - tail : 0u;
}

inline uint32_t splitShift(uint32_t extent, uint32_t step, uint32_t steps)
{
   return static_cast<uint32_t>((static_cast<uint64_t>(splitTravel(extent)) * step + steps / 2u) / steps);
}

// One axis of a split: the first half moves toward 0 and the second half toward the end by
// `shift`; between them is black. Returns the position the pixel shows, or false.
inline bool splitAxis(uint32_t pos, uint32_t extent, uint32_t shift, uint32_t& source)
{
   const uint32_t half = extent / 2u;
   if (pos + shift < half)
   {
      source = pos + shift;
      return true;
   }
   if (pos >= half + shift)
   {
      source = pos - shift;
      return true;
   }
   return false;
}

class Split final : public Effect
{
public:
   Split(bool vertical, uint32_t width, uint32_t height) : m_vertical(vertical), m_width(width), m_height(height) {}
   uint32_t maxSteps() const override { return std::min(kMaxSteps, splitTravel(extent())); }
   const char* limitReason() const override { return "the halves move in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      uint32_t source = 0;
      if (!splitAxis(m_vertical ? x : y, extent(), splitShift(extent(), step, steps), source))
         return {false, 0, 0};
      return m_vertical ? Rule{true, source, y} : Rule{true, x, source};
   }

private:
   uint32_t extent() const { return m_vertical ? m_width : m_height; }
   bool m_vertical;
   uint32_t m_width, m_height;
};

// ---- Blinds ---------------------------------------------------------------------------------

// Black bars at regular intervals that grow wider until only a thin slit of the picture is left
// in each interval. Horizontal: horizontal bars (stacked, as in window blinds). Vertical:
// vertical bars side by side. The bars grow in whole pixels, so the most steps is the interval
// less one.
class Blinds final : public Effect
{
public:
   Blinds(bool horizontal, uint32_t width, uint32_t height)
      : m_horizontal(horizontal), m_pitch(std::max(3u, horizontal ? height / 4u : width / 8u))
   {
   }
   uint32_t maxSteps() const override { return std::min(kMaxSteps, m_pitch - 1u); }
   const char* limitReason() const override { return "each bar grows in whole pixels"; }
   Rule rule(uint32_t x, uint32_t y, uint32_t step, uint32_t steps) const override
   {
      const uint32_t within = (m_horizontal ? y : x) % m_pitch;
      const uint32_t bar = ((m_pitch - 1u) * step + steps / 2u) / steps; // the bar's width
      const uint32_t start = (m_pitch - bar) / 2u;                      // centred in the interval
      return {!(within >= start && within < start + bar), x, y};
   }

private:
   bool m_horizontal;
   uint32_t m_pitch;
};

// ---- Fade -----------------------------------------------------------------------------------

// Fade: the picture gets darker (To Black) or brighter (To White) a little more at each step.
// The brightest color gets one unit away from black (or the darkest one unit from white) at the
// last step, so the last frame is almost black (almost white), never exactly. The geometry does
// not change.
class Fade final : public Effect
{
public:
   Fade(bool toWhite, const SourceInfo& source) : m_toWhite(toWhite), m_colorCount(std::max(2u, source.colorCount))
   {
      // The room a color has to fade in, in units of a 5-bit channel (green has 6 bits, so it
      // counts half): the brightest color's brightest channel for a fade to black, the darkest
      // color's darkest channel for a fade to white.
      uint32_t span = 0;
      for (uint16_t c : source.colors)
         span = std::max(span, spanOf(c));
      m_colorSpan = span;
      const uint32_t top = m_colorCount - 1u;
      m_shadeSpan = toWhite ? top - std::min(top, source.darkestShade) : std::min(top, source.brightestShade);

      // The colors that change first, for the quick check below.
      m_colors = source.colors;
      std::sort(m_colors.begin(), m_colors.end(), [&](uint16_t a, uint16_t b) { return spanOf(a) > spanOf(b); });
      m_maxSteps = computeMaxSteps();
   }

   uint32_t maxSteps() const override { return m_maxSteps; }
   const char* limitReason() const override { return "the picture only has so many brightness levels to fade through"; }
   bool changesColors() const override { return true; }

   uint16_t color(uint16_t c, uint32_t step, uint32_t steps) const override
   {
      const uint32_t f = factor(m_colorSpan, step, steps);
      const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
      return static_cast<uint16_t>((channel(r, 31, f) << 11) | (channel(g, 63, f) << 5) | channel(b, 31, f));
   }

   uint8_t shade(uint8_t s, uint32_t step, uint32_t steps) const override
   {
      return static_cast<uint8_t>(channel(s, m_colorCount - 1u, factor(m_shadeSpan, step, steps)));
   }

private:
   // How far a color is from the end of its fade, in 5-bit units: its brightest channel (to
   // black) or the darkest channel's distance from the top (to white).
   uint32_t spanOf(uint16_t c) const
   {
      const uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
      if (m_toWhite)
         return std::max({31u - r, (63u - g + 1u) / 2u, 31u - b});
      return std::max({r, (g + 1u) / 2u, b});
   }

   // 16.16 fixed point: 1.0 at step 0 down to 1 / span at the last step.
   static uint32_t factor(uint32_t span, uint32_t step, uint32_t steps)
   {
      if (span == 0)
         return 65536u;
      const uint64_t last = 65536u / span;
      const uint64_t drop = ((65536u - last) * step + steps / 2u) / steps;
      return static_cast<uint32_t>(65536u - drop);
   }

   // A value of a channel that goes from 0 to `top`, faded by the factor: toward 0 for a fade
   // to black, toward `top` for a fade to white.
   uint32_t channel(uint32_t value, uint32_t top, uint32_t f) const
   {
      if (m_toWhite)
         return top - (((top - value) * f + 32768u) >> 16);
      return (value * f + 32768u) >> 16;
   }

   uint32_t computeMaxSteps() const
   {
      if (m_colorSpan == 0 || m_colors.empty())
         return 0; // a picture that is already all black (white): nothing to fade
      for (uint32_t n = kMaxSteps; n >= 1; --n)
      {
         // The fade only goes one way, so frames differ from each other when each differs
         // from the one before it, and the first from the starting frame.
         bool distinct = true;
         for (uint32_t s = 0; s < n && distinct; ++s)
         {
            bool differs = false;
            for (uint16_t c : m_colors)
               if (color(c, s, n) != color(c, s + 1, n))
               {
                  differs = true;
                  break;
               }
            distinct = differs;
         }
         if (distinct)
            return n;
      }
      return 0;
   }

   bool m_toWhite;
   uint32_t m_colorCount;
   uint32_t m_colorSpan = 0;
   uint32_t m_shadeSpan = 0;
   std::vector<uint16_t> m_colors;
   uint32_t m_maxSteps = 0;
};

// ---- The list -------------------------------------------------------------------------------

// A choice a transition has (its direction, its type ...).
struct Option
{
   const char* id;    // kept in the plugin's settings
   const char* label; // shown in the window
};

struct Transition
{
   const char* id;           // kept in the plugin's settings
   const char* label;        // shown in the window
   const char* description;  // a sentence or two for the window
   const char* optionLabel;  // what the options are called ("Direction"); null without options
   const Option* options;
   size_t optionCount;
   std::unique_ptr<Effect> (*make)(const char* optionId, const SourceInfo& source);
};

inline bool same(const char* a, const char* b) { return std::string_view(a) == std::string_view(b); }

inline std::unique_ptr<Effect> makeDissolve(const char* option, const SourceInfo& s)
{
   if (same(option, "random")) return std::make_unique<RankDissolve>(Order::Random, s.width, s.height);
   if (same(option, "centre_out")) return std::make_unique<RankDissolve>(Order::CentreOut, s.width, s.height);
   if (same(option, "edge_in")) return std::make_unique<RankDissolve>(Order::EdgeIn, s.width, s.height);
   if (same(option, "spiral")) return std::make_unique<RankDissolve>(Order::Spiral, s.width, s.height);
   return std::make_unique<OrderlyDissolve>(s.width, s.height);
}

inline Direction directionOf(const char* option)
{
   if (same(option, "up")) return Direction::Up;
   if (same(option, "down")) return Direction::Down;
   if (same(option, "right")) return Direction::Right;
   return Direction::Left;
}

inline std::unique_ptr<Effect> makeSlide(const char* option, const SourceInfo& s)
{
   return std::make_unique<Slide>(directionOf(option), s.width, s.height);
}
inline std::unique_ptr<Effect> makeWipe(const char* option, const SourceInfo& s)
{
   return std::make_unique<Wipe>(directionOf(option), s.width, s.height);
}
inline std::unique_ptr<Effect> makeBlinds(const char* option, const SourceInfo& s)
{
   return std::make_unique<Blinds>(!same(option, "vertical"), s.width, s.height);
}
inline std::unique_ptr<Effect> makeFade(const char* option, const SourceInfo& s)
{
   return std::make_unique<Fade>(same(option, "white"), s);
}
inline std::unique_ptr<Effect> makeAlternatingSlide(const char* option, const SourceInfo& s)
{
   return std::make_unique<AlternatingSlide>(!same(option, "vertical"), s.width, s.height);
}
inline std::unique_ptr<Effect> makeSplitVertical(const char*, const SourceInfo& s)
{
   return std::make_unique<Split>(true, s.width, s.height);
}
inline std::unique_ptr<Effect> makeSplitHorizontal(const char*, const SourceInfo& s)
{
   return std::make_unique<Split>(false, s.width, s.height);
}

inline const Option kDissolveOptions[] = {
   {"orderly", "Orderly"}, {"random", "Random"}, {"centre_out", "Centre-Out"},
   {"edge_in", "Edge-In"}, {"spiral", "Spiral"},
};
inline const Option kDirectionOptions[] = {
   {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"},
};
inline const Option kOrientationOptions[] = {
   {"horizontal", "Horizontal"}, {"vertical", "Vertical"},
};
inline const Option kFadeOptions[] = {
   {"black", "To Black"}, {"white", "To White"},
};

#define TB_COUNT(a) (sizeof(a) / sizeof((a)[0]))

inline const Transition kTransitions[] = {
   {"dissolve", "Dissolve",
    "The picture dissolves to black. Orderly: pixels disappear in a regular pattern. Random: "
    "pixels disappear at random. Centre-Out: they disappear from the centre outward. Edge-In: "
    "they disappear from the outside edges inward. Spiral: they disappear along a spiral from "
    "the top left corner.",
    "Dissolve Type", kDissolveOptions, TB_COUNT(kDissolveOptions), &makeDissolve},
   {"slide", "Slide",
    "The whole picture slides off the screen in the direction you choose. A thin strip of it "
    "is left at the end.",
    "Direction", kDirectionOptions, TB_COUNT(kDirectionOptions), &makeSlide},
   {"wipe", "Wipe",
    "A black bar wipes over the picture in the direction you choose (Left: it comes in from "
    "the right edge and moves left). A thin strip of the picture is left at the end.",
    "Direction", kDirectionOptions, TB_COUNT(kDirectionOptions), &makeWipe},
   {"blinds", "Blinds",
    "Black bars appear at regular intervals and grow wider until only thin slits of the "
    "picture are left. Horizontal: horizontal bars, stacked. Vertical: vertical bars, side by "
    "side.",
    "Orientation", kOrientationOptions, TB_COUNT(kOrientationOptions), &makeBlinds},
   {"fade", "Fade",
    "The picture gets darker or brighter at each step. The last frame is almost black (To "
    "Black) or almost white (To White), but not quite. Color rotations of the new frames are "
    "removed, because they would not fade.",
    "Fade To", kFadeOptions, TB_COUNT(kFadeOptions), &makeFade},
   {"alternating_slide", "Alternating Slide",
    "Lines slide off in turns. Horizontal: the 1st, 3rd, 5th ... row slide left and the 2nd, "
    "4th, 6th ... row slide right. Vertical: the 1st, 3rd, 5th ... column go up and the 2nd, "
    "4th, 6th ... column go down. A thin strip of each line is left at the end.",
    "Orientation", kOrientationOptions, TB_COUNT(kOrientationOptions), &makeAlternatingSlide},
   {"split_vertical", "Split Vertical",
    "The picture is cut down the middle. The left half slides to the left and the right half "
    "to the right, until a thin strip of each is left at the edges.",
    nullptr, nullptr, 0, &makeSplitVertical},
   {"split_horizontal", "Split Horizontal",
    "The picture is cut across the middle. The top half slides up and the bottom half down, "
    "until a thin strip of each is left at the edges.",
    nullptr, nullptr, 0, &makeSplitHorizontal},
};
inline constexpr size_t kTransitionCount = 8;
static_assert(sizeof(kTransitions) / sizeof(kTransitions[0]) == kTransitionCount, "update kTransitionCount");

#undef TB_COUNT

inline const Transition* findTransition(const char* id)
{
   for (const Transition& transition : kTransitions)
      if (same(id, transition.id))
         return &transition;
   return nullptr;
}

// The option of a transition by id; the first one for an id it does not know; null without any.
inline const Option* findOption(const Transition& transition, const char* id)
{
   if (transition.optionCount == 0)
      return nullptr;
   for (size_t i = 0; i < transition.optionCount; ++i)
      if (same(id, transition.options[i].id))
         return &transition.options[i];
   return &transition.options[0];
}

} // namespace transitions
