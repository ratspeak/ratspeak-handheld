#pragma once
#include "config/BoardConfig.h"
#ifdef RSCROWPANEL
#include <lvgl.h>
#include <functional>
#include <string>
#include <vector>
namespace TouchKeyboard {
void configure(uint8_t initial,std::function<bool(uint8_t)> saveLanguage);
using Accept=std::function<bool(const std::string&)>;
void show(const char* title,const char* initial,size_t maxBytes,Accept accept,bool password=false);
void choose(const char* title,const std::vector<std::string>& values,size_t selected,std::function<bool(size_t)> accept);
void close();
void attach(lv_obj_t* root);
bool visible();
}
#else
namespace TouchKeyboard { inline void close() {} inline void attach(void*) {} inline bool visible() {return false;} }
#endif
