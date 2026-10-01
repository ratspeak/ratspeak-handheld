#include "LvSettingsScreen.h"
void LvSettingsScreen::buildVoiceItems(int& idx) {
    auto& settings=_cfg->settings().voice;const int start=idx;
#ifdef RSM9
    _items.push_back({"Hardware",SettingType::READONLY,nullptr,nullptr,[](int) {return String("No microphone or speaker");}});++idx;
#else
    _items.push_back({"Live voice",SettingType::TOGGLE,[&settings] {return int(settings.enabled);},[&settings](int value) {settings.enabled=value!=0;}});++idx;
    _items.push_back({"Contacts only",SettingType::TOGGLE,[&settings] {return int(settings.contactsOnly);},[&settings](int value) {settings.contactsOnly=value!=0;}});++idx;
    _items.push_back({"Connection",SettingType::ENUM_CHOICE,[&settings] {return int(settings.route);},[&settings](int value) {settings.route=handheld::voice::Route(value);},nullptr,0,2,1,{"AUTO","WiFi / TCP","LoRa"}});++idx;
    _items.push_back({"Voice volume",SettingType::INTEGER,[&settings] {return int(settings.volume);},[&settings](int value) {settings.volume=value;},nullptr,0,100,10});++idx;
#endif
    _categories.push_back({"Voice",start,idx-start,[&settings] {return settings.enabled?String("ON"):String("OFF");}});
}
