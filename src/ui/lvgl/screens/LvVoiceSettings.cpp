#include "LvSettingsScreen.h"
void LvSettingsScreen::buildVoiceItems(int& idx) {
    auto& settings=_cfg->settings().voice;const int start=idx;
#ifdef RSM9
    _items.push_back({"Hardware",SettingType::READONLY,nullptr,nullptr,[](int) {return String("No audio hardware");}});++idx;
#else
    _items.push_back({"Live voice",SettingType::TOGGLE,[&settings] {return int(settings.enabled);},[&settings](int value) {settings.enabled=value!=0;},[](int value) {return String(value?"ON":"OFF");}});++idx;
    _items.push_back({"Incoming requests",SettingType::ENUM_CHOICE,[&settings] {return settings.contactsOnly?0:1;},[&settings](int value) {settings.contactsOnly=value==0;},nullptr,0,1,1,{"Contacts only","Anyone"}});++idx;
    _items.push_back({"Connection",SettingType::ENUM_CHOICE,[&settings] {return int(settings.route);},[&settings](int value) {settings.route=handheld::voice::Route(value);},nullptr,0,2,1,{"AUTO","WiFi / TCP","LoRa"}});++idx;
    _items.push_back({"Voice volume",SettingType::INTEGER,[&settings] {return int(settings.volume);},[&settings](int value) {settings.volume=value;},[](int value) {return String(value)+"%";},0,100,10});++idx;
#endif
    _categories.push_back({"Voice",start,idx-start,[&settings] {
#ifdef RSM9
        (void)settings;return String("Unavailable");
#else
        return settings.enabled?String("ON"):String("OFF");
#endif
    }});
}
