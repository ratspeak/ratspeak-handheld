#include "TouchKeyboard.h"
#ifdef RSCROWPANEL
#include "KeyboardLayouts.h"
#include "input/Utf8.h"
#include "fonts/fonts.h"
#include "Theme.h"
#include "LvPrivacy.h"
#include <cstring>
#include <utility>
namespace TouchKeyboard {
namespace {
lv_obj_t *overlay=nullptr,*editor=nullptr,*keys=nullptr,*hint=nullptr;
Accept accepted;
std::function<bool(size_t)> choiceAccepted;
size_t limit=0,language=0;
uint32_t generation=0;
bool shift=false,symbols=false;
std::function<bool(uint8_t)> persistLanguage;
constexpr size_t languageCount=sizeof(layouts)/sizeof(layouts[0]);
void layout();
// The firmware disables LVGL's default theme. Top-layer windows must supply
// their own text color, and backgrounds remain transparent without an opacity.
lv_obj_t* createOverlay() {
 auto* obj=lv_obj_create(lv_layer_top());
 lv_obj_set_size(obj,800,480);lv_obj_set_pos(obj,0,0);
 lv_obj_clear_flag(obj,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_style_pad_all(obj,8,0);lv_obj_set_style_radius(obj,0,0);
 lv_obj_set_style_bg_color(obj,lv_color_hex(Theme::BG),0);
 lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);
 lv_obj_set_style_text_color(obj,lv_color_hex(Theme::TEXT_PRIMARY),0);
 lv_obj_set_style_text_font(obj,&lv_font_rsdeck_14,0);
 return obj;
}
void button(lv_obj_t* parent,const char* text,int x,int y,int w,int h,lv_event_cb_t cb) {
 auto* b=lv_btn_create(parent); lv_obj_set_pos(b,x,y); lv_obj_set_size(b,w,h);
 lv_obj_set_style_bg_color(b,lv_color_hex(Theme::BG_ELEVATED),0);
 lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(Theme::PRIMARY_MUTED),LV_STATE_PRESSED);
 lv_obj_set_style_radius(b,5,0);
 lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,nullptr);
 auto* label=lv_label_create(b); lv_label_set_text(label,text);
 lv_obj_set_style_text_font(label,&lv_font_rsdeck_14,0); lv_obj_center(label);
}
void key(lv_event_t* e) {
 const char* text=lv_label_get_text(lv_obj_get_child(lv_event_get_target(e),0));
 if(!strcmp(text,"Shift")) { shift=!shift;layout();return; }
 if(!strcmp(text,"Lang")) { const size_t next=(language+1)%languageCount;
  if(persistLanguage && persistLanguage(uint8_t(next))) {language=next;symbols=false;layout();}
  else lv_label_set_text(hint,"Settings busy. Try again.");
  return; }
 if(!strcmp(text,"123") || !strcmp(text,"ABC")) { symbols=!symbols;layout();return; }
 if(!strcmp(text,"Backspace")) { lv_textarea_del_char(editor);return; }
 if(!strcmp(text,"Left")) { lv_textarea_cursor_left(editor);return; }
 if(!strcmp(text,"Right")) { lv_textarea_cursor_right(editor);return; }
 if(!strcmp(text,"Space")) text=" ";
 if(strlen(lv_textarea_get_text(editor))+strlen(text)>limit) {
  lv_label_set_text(hint,"Text limit reached");return;
 }
 lv_textarea_add_text(editor,text);
}
void layout() {
 lv_obj_clean(keys);
 const char* numberRows[]={"1234567890","@#%&*()-_+=",R"keys(.,:;!?/\"')keys"};
 const auto& l=layouts[language];
 for(int row=0;row<3;++row) {
  const char* chars=symbols?numberRows[row]:(shift?l.upper[row]:l.lower[row]);
  size_t count=0;
  for(const unsigned char* s=reinterpret_cast<const unsigned char*>(chars);*s;++s) if((*s&0xc0)!=0x80) ++count;
  if(!count) continue;
  const int width=776/count;
  int col=0;
  while(*chars) {
   char symbol[5]{}; size_t n=1;
   while(chars[n] && (static_cast<unsigned char>(chars[n])&0xc0)==0x80 && n<4) ++n;
   memcpy(symbol,chars,n);chars+=n;
   button(keys,symbol,col++*width,row*56,width-4,50,key);
  }
 }
 button(keys,"Shift",0,168,95,48,key);
 button(keys,symbols?"ABC":"123",99,168,65,48,key);
 button(keys,"Lang",168,168,65,48,key);
 button(keys,"Space",237,168,275,48,key);
 button(keys,"Left",516,168,60,48,key);
 button(keys,"Right",580,168,60,48,key);
 button(keys,"Backspace",644,168,132,48,key);
 lv_label_set_text_fmt(hint,"%s | %s",l.name,shift?"Uppercase":"Lowercase");
}
void textareaClick(lv_event_t* e) {
 auto* target=lv_event_get_target(e);
 if(overlay || lv_obj_has_state(target,LV_STATE_DISABLED)) return;
 const bool password=lv_textarea_get_password_mode(target);
 const uint32_t maxLength=lv_textarea_get_max_length(target);
 show("Edit text",lv_textarea_get_text(target),maxLength?maxLength:400,
  [target](const std::string& text) {
   if(!lv_obj_is_valid(target)) return false;
   lv_textarea_set_text(target,text.c_str());
   lv_event_send(target,LV_EVENT_READY,nullptr);
   return true;
  },password);
}
}
void configure(uint8_t initial,std::function<bool(uint8_t)> save) {
 language=initial<languageCount?initial:0;persistLanguage=std::move(save);
}
bool visible() { return overlay; }
void close() {
 ++generation;accepted={};choiceAccepted={};
 if(overlay) { auto* old=overlay;overlay=editor=keys=hint=nullptr;lv_obj_del(old); }
}
void show(const char* title,const char* initial,size_t maxBytes,Accept accept,bool password) {
 close();
 accepted=std::move(accept);limit=maxBytes;shift=symbols=false;
 overlay=createOverlay();
 auto* label=lv_label_create(overlay);lv_label_set_text(label,title);lv_obj_set_pos(label,4,0);
 editor=lv_textarea_create(overlay);lv_obj_set_pos(editor,0,28);lv_obj_set_size(editor,776,130);
 lv_obj_set_style_text_font(editor,&lv_font_rsdeck_14,0);
 lv_obj_set_style_bg_color(editor,lv_color_hex(Theme::BG_ELEVATED),0);
 lv_obj_set_style_bg_opa(editor,LV_OPA_COVER,0);
 lv_obj_set_style_pad_all(editor,6,0);
 lv_obj_set_style_border_color(editor,lv_color_hex(Theme::BORDER),0);
 lv_obj_set_style_border_width(editor,1,0);
 lv_obj_set_style_border_color(editor,lv_color_hex(Theme::ACCENT),LV_PART_CURSOR);
 lv_obj_set_style_border_width(editor,2,LV_PART_CURSOR);
 lv_obj_set_style_border_side(editor,LV_BORDER_SIDE_LEFT,LV_PART_CURSOR);
 lv_textarea_set_max_length(editor,maxBytes);
 lv_textarea_set_password_mode(editor,password);
 if(password) LvPrivacy::markSensitive(editor);
 lv_textarea_set_text(editor,handheld::utf8::prefix(initial?initial:"",limit).c_str());
 lv_textarea_set_cursor_pos(editor,LV_TEXTAREA_CURSOR_LAST);
 hint=lv_label_create(overlay);lv_obj_set_pos(hint,4,164);
 keys=lv_obj_create(overlay);lv_obj_set_pos(keys,0,190);lv_obj_set_size(keys,780,220);
 lv_obj_set_style_pad_all(keys,0,0);lv_obj_set_style_border_width(keys,0,0);
 lv_obj_clear_flag(keys,LV_OBJ_FLAG_SCROLLABLE);
 button(overlay,"Cancel",0,416,150,44,[](lv_event_t*){close();});
 button(overlay,"Done",626,416,150,44,[](lv_event_t*) {
  std::string text=lv_textarea_get_text(editor);
  auto callback=accepted;const auto current=generation;
  const bool ok=callback && callback(text);
  if(generation==current && ok) close();
  else if(generation==current) lv_label_set_text(hint,"Unable to apply value. Check input and retry.");
 });
 layout();
}
namespace {
void choiceClick(lv_event_t* event) {
 const auto current=generation;
 auto callback=choiceAccepted;
 const size_t index=reinterpret_cast<size_t>(lv_event_get_user_data(event));
 if(callback && callback(index) && generation==current) close();
}
}
void choose(const char* title,const std::vector<std::string>& values,size_t selected,std::function<bool(size_t)> accept) {
 close();choiceAccepted=std::move(accept);
 overlay=createOverlay();
 auto* label=lv_label_create(overlay);lv_label_set_text(label,title);
 auto* list=lv_obj_create(overlay);lv_obj_set_pos(list,0,30);lv_obj_set_size(list,776,370);
 lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);
 for(size_t i=0;i<values.size();++i) {
  auto* b=lv_btn_create(list);lv_obj_set_size(b,740,48);
  lv_obj_set_style_bg_color(b,lv_color_hex(i==selected?Theme::PRIMARY_MUTED:Theme::BG_ELEVATED),0);
  lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
  lv_obj_set_style_bg_color(b,lv_color_hex(Theme::PRIMARY_MUTED),LV_STATE_PRESSED);
  lv_obj_add_event_cb(b,choiceClick,LV_EVENT_CLICKED,reinterpret_cast<void*>(i));
  auto* text=lv_label_create(b);lv_label_set_text(text,values[i].c_str());lv_obj_center(text);
 }
 button(overlay,"Cancel",0,416,150,44,[](lv_event_t*){close();});
}
void attach(lv_obj_t* root) {
 if(!root || root==overlay) return;
 if(lv_obj_check_type(root,&lv_textarea_class)) {
  if(!lv_obj_get_event_user_data(root,textareaClick))
   lv_obj_add_event_cb(root,textareaClick,LV_EVENT_CLICKED,root);
 }
 const uint32_t n=lv_obj_get_child_cnt(root);
 for(uint32_t i=0;i<n;++i) attach(lv_obj_get_child(root,i));
}
}
#endif
