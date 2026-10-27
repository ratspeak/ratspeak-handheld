#pragma once
#include "hal/TouchInput.h"
#include "input/KeyEvent.h"
namespace crowpanel {
// Recovery runs before the normal LVGL event loop. Require a fresh touch for
// every choice, particularly the second, destructive confirmation.
template<class Display> class RecoveryInput {
public:
 explicit RecoveryInput(Display& display):_display(display) { _touch.begin(); }
 void discardPending() {
  _waitRelease=true;_hasEvent=false;
  const char* labels[]={"Restart","Erase","Review","Back","Confirm"};
  _display.setTextSize(2);_display.setTextColor(0xffff,0x0000);
  for(int i=0;i<5;++i) {
   _display.drawRect(8+i*158,410,150,60,0xffff);
   _display.setCursor(18+i*158,430);_display.print(labels[i]);
  }
 }
 void update() {
  _hasEvent=false;_touch.update();
  if(!_touch.isTouched()) { _waitRelease=false;return; }
  if(_waitRelease) return;
  _waitRelease=true;
  if(_touch.y()<410 || _touch.y()>=470 || _touch.x()<8) return;
  const int choice=(_touch.x()-8)/158;
  if(choice<0 || choice>4) return;
  _event={};
  if(choice==4) _event.enter=true;
  else _event.character="xerb"[choice];
  _hasEvent=true;
 }
 bool hasEvent() const { return _hasEvent; }
 const KeyEvent& getEvent() const { return _event; }
private:
 Display& _display;
 TouchInput _touch;
 KeyEvent _event{};
 bool _waitRelease=true,_hasEvent=false;
};
}
