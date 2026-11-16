// license:GPLv3+
//
// The little pictures of the transitions.
//
// A picture is a small grey rectangle (16 x 4 cells, the shape of a DMD) that shows a
// transition halfway: white is the picture, black is what the transition has taken away. It is
// made by running the transition itself on a tiny picture (white with a grey disc, so that
// movement shows), so it cannot disagree with what the transition does.
//
// The editor's plugin windows have no picture field, but a note is a text label that draws
// rich text, so a picture is drawn as colored blocks of spaces. The window shows these only
// while the "Show pictures" setting is on.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Transitions.h"

namespace preview {

struct Icon
{
   uint32_t width = 0;
   uint32_t height = 0;
   std::vector<uint8_t> gray; // row-major, 0 black .. 255 white
};

inline uint8_t grayOf(uint16_t c)
{
   const uint32_t r = ((c >> 11) & 31u) * 255u / 31u;
   const uint32_t g = ((c >> 5) & 63u) * 255u / 63u;
   const uint32_t b = (c & 31u) * 255u / 31u;
   return static_cast<uint8_t>((r * 30u + g * 59u + b * 11u) / 100u);
}

inline Icon makeIcon(const transitions::Transition& transition, const transitions::Option* option,
                     uint32_t width = 16, uint32_t height = 6)
{
   const bool fadeToWhite = transitions::same(transition.id, "fade") && option &&
                            transitions::same(option->id, "white");
   // The picture to start from: white with a grey disc, or dark with a darker disc to fade up.
   const uint16_t background = fadeToWhite ? 0x2965 : 0xFFFF;
   const uint16_t disc = fadeToWhite ? 0x10A2 : 0x8410;
   std::vector<uint16_t> picture(static_cast<size_t>(width) * height, background);
   const double cx = (width - 1) / 2.0, cy = (height - 1) / 2.0, radius = height * 0.4;
   for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
         if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius)
            picture[static_cast<size_t>(y) * width + x] = disc;

   transitions::SourceInfo info;
   info.width = width;
   info.height = height;
   info.colorCount = 4;
   info.brightestShade = 3;
   info.darkestShade = 0;
   info.colors = {background, disc};
   const auto effect = transition.make(option ? option->id : "", info);

   Icon icon;
   icon.width = width;
   icon.height = height;
   icon.gray.assign(picture.size(), 0);
   const uint32_t most = effect->maxSteps();
   if (effect->changesColors())
   {
      // A fade does not move anything: show it as it goes, from the picture (left) to the last
      // step (right).
      const uint32_t steps = std::max(1u, most);
      for (uint32_t y = 0; y < height; ++y)
         for (uint32_t x = 0; x < width; ++x)
         {
            const uint32_t step = (steps * x + (width - 1) / 2u) / std::max(1u, width - 1);
            icon.gray[static_cast<size_t>(y) * width + x] =
               grayOf(effect->color(picture[static_cast<size_t>(y) * width + x], step, steps));
         }
      return icon;
   }
   const uint32_t steps = std::max(1u, std::min(8u, most));
   const uint32_t step = (steps + 1u) / 2u; // halfway
   for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
      {
         const transitions::Rule rule = effect->rule(x, y, step, steps);
         if (rule.visible)
            icon.gray[static_cast<size_t>(y) * width + x] =
               grayOf(picture[static_cast<size_t>(rule.y) * width + rule.x]);
      }
   return icon;
}

inline std::string escapeHtml(const std::string& text)
{
   std::string out;
   for (char c : text)
      switch (c)
      {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += c;
      }
   return out;
}

// The picture as rich text: a row of spans per line, each run of one grey a span with that
// background and two spaces per cell (so that a cell is about square), in a table that gives
// it a thin border.
inline std::string iconHtml(const Icon& icon)
{
   static const char* hex = "0123456789abcdef";
   std::string html = "<table border=\"1\" cellspacing=\"0\" cellpadding=\"0\"><tr><td>";
   for (uint32_t y = 0; y < icon.height; ++y)
   {
      uint32_t x = 0;
      while (x < icon.width)
      {
         const uint8_t gray = icon.gray[static_cast<size_t>(y) * icon.width + x];
         uint32_t end = x;
         while (end < icon.width && icon.gray[static_cast<size_t>(y) * icon.width + end] == gray)
            ++end;
         html += "<span style=\"background-color:#";
         for (int i = 0; i < 3; ++i)
         {
            html += hex[gray >> 4];
            html += hex[gray & 15];
         }
         html += ";\">";
         for (uint32_t i = x; i < end; ++i)
            html += "&nbsp;&nbsp;";
         html += "</span>";
         x = end;
      }
      if (y + 1 < icon.height)
         html += "<br>";
   }
   html += "</td></tr></table>";
   return html;
}

struct GalleryItem
{
   std::string label;
   Icon icon;
};

// A paragraph of text and a table of labelled pictures, `columns` to a row. Starts with a tag,
// which is what makes the label draw it as rich text.
inline std::string galleryHtml(const std::string& text, const std::vector<GalleryItem>& items,
                               size_t columns = 4)
{
   std::string html = "<p>" + escapeHtml(text) + "</p><table cellspacing=\"6\">";
   for (size_t i = 0; i < items.size(); ++i)
   {
      if (i % columns == 0)
         html += "<tr>";
      html += "<td valign=\"top\"><b>" + escapeHtml(items[i].label) + "</b><br>" + iconHtml(items[i].icon) + "</td>";
      if (i % columns == columns - 1 || i + 1 == items.size())
         html += "</tr>";
   }
   html += "</table>";
   return html;
}

} // namespace preview
