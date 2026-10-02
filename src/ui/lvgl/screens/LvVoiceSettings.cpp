#include "LvSettingsScreen.h"
void LvSettingsScreen::buildVoiceItems(int& idx) {
    auto& settings=_cfg->settings().voice;const int start=idx;
#ifdef RSM9
    _items.push_back({"Hardware",SettingType::READONLY,nullptr,nullptr,[](int) {return String("No audio hardware");}});++idx;
#else
    _items.push_back({"Playback volume",SettingType::INTEGER,[&settings] {return int(settings.volume);},[&settings](int value) {settings.volume=value;},[](int value) {return String(value)+"%";},0,100,10});++idx;
    _items.push_back({"Voice messages",SettingType::READONLY,nullptr,nullptr,[](int) {return String("Up to 15 seconds");}});++idx;
#endif
    _items.push_back({"Live calls",SettingType::READONLY,nullptr,nullptr,[](int) {return String("Unavailable");}});++idx;
    _categories.push_back({"Voice",start,idx-start,[&settings] {
#ifdef RSM9
        (void)settings;return String("No audio hardware");
#else
        return String(settings.volume)+"% volume";
#endif
    }});
}
