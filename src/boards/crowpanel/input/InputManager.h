#pragma once
#include "hal/Keyboard.h"
#include "hal/Power.h"
#include "hal/TouchInput.h"
class InputManager {
public:
 void begin(Keyboard*, TouchInput* touch) { _touch=touch; }
 void setPowerMgr(Power*) {}
 void update(bool = true) {
   _activity=false;
   if (!_touch) return;
   _touch->update();
   _activity=_touch->isTouched();
 }
 bool hasKeyEvent() const { return false; }
 const KeyEvent& getKeyEvent() const { static const KeyEvent empty{}; return empty; }
 bool hadActivity() const { return _activity; }
 bool hadStrongActivity() const { return _activity; }
 bool hadLongPress() const { return false; }
private:
 TouchInput* _touch=nullptr;
 bool _activity=false;
};
