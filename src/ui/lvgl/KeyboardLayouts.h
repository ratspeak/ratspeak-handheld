#pragma once
// Add optional layouts here (and the matching font glyphs), then append them to
// layouts[]. Labels describe UTF-8 rows; the keyboard engine is language agnostic.
namespace TouchKeyboard {
struct Layout { const char* id; const char* name; const char* lower[3]; const char* upper[3]; };
inline constexpr Layout layouts[]={
 {"en","EN",{"qwertyuiop","asdfghjkl","zxcvbnm"},{"QWERTYUIOP","ASDFGHJKL","ZXCVBNM"}},
 {"ru","RU",{"йцукенгшщзхъ","фывапролджэ","ячсмитьбюё"},{"ЙЦУКЕНГШЩЗХЪ","ФЫВАПРОЛДЖЭ","ЯЧСМИТЬБЮЁ"}},
#ifdef CROW_KEYBOARD_EXTRA_LAYOUTS
 CROW_KEYBOARD_EXTRA_LAYOUTS
#endif
};
}
