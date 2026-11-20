// license:GPLv3+
//
// The plugin's texts in the author's language: English, German and French, the languages the
// editor has. English is the text in the source; Translations.h holds German and French for it.
// An unknown language, or a text without a translation, is shown in English.
#pragma once

#include <initializer_list>
#include <string>
#include <unordered_map>

namespace i18n {

struct Entry
{
   const char* english;
   const char* german;
   const char* french;
};

} // namespace i18n

#include "Translations.h" // kEntries: the texts, English with German and French

namespace i18n {

enum class Language
{
   English,
   German,
   French
};

inline Language& current()
{
   static Language language = Language::English;
   return language;
}

// Takes the editor's language code ("en", "de", "fr"); anything else is English.
inline void setLanguage(const char* code)
{
   const std::string c = code ? code : "";
   current() = c == "de" ? Language::German : c == "fr" ? Language::French : Language::English;
}

inline const Entry* find(const char* english)
{
   static const std::unordered_map<std::string, const Entry*> index = [] {
      std::unordered_map<std::string, const Entry*> m;
      for (size_t i = 0; i < kEntryCount; ++i)
         m[kEntries[i].english] = &kEntries[i];
      return m;
   }();
   const auto it = index.find(english ? english : "");
   return it == index.end() ? nullptr : it->second;
}

// The text in the current language. The pointer stays valid (it is the argument or a literal
// of the table), so it can be handed to the editor.
inline const char* tr(const char* english)
{
   if (current() == Language::English)
      return english;
   const Entry* e = find(english);
   if (!e)
      return english;
   const char* text = current() == Language::German ? e->german : e->french;
   return text && *text ? text : english;
}

// A text with %1, %2 ... in it, filled in.
inline std::string fmt(const char* english, std::initializer_list<std::string> arguments)
{
   std::string text = tr(english);
   std::string out;
   for (size_t i = 0; i < text.size(); ++i)
   {
      if (text[i] == '%' && i + 1 < text.size() && text[i + 1] >= '1' && text[i + 1] <= '9')
      {
         const size_t n = static_cast<size_t>(text[i + 1] - '1');
         if (n < arguments.size())
            out += *(arguments.begin() + n);
         ++i;
      }
      else
         out += text[i];
   }
   return out;
}

} // namespace i18n
