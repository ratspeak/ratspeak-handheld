#pragma once
#include <cstddef>
#include <string>
namespace handheld::utf8 {
// Strings here are valid UTF-8 from LVGL. Limits are bytes, matching LXMF drafts.
inline void eraseLast(std::string& text) {
 if(text.empty()) return;
 size_t i=text.size()-1;
 while(i && (static_cast<unsigned char>(text[i])&0xc0)==0x80) --i;
 text.resize(i);
}
inline std::string prefix(const std::string& text,size_t bytes) {
 if(text.size()<=bytes) return text;
 while(bytes && (static_cast<unsigned char>(text[bytes])&0xc0)==0x80) --bytes;
 return text.substr(0,bytes);
}
}
