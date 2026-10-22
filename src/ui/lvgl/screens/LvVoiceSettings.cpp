#include "LvSettingsScreen.h"
void LvSettingsScreen::buildVoiceItems(int& idx) {
    auto& settings=_cfg->settings().voice;const int start=idx;
    // RSM9 has a notification buzzer (HAS_AUDIO is true for it) but no
    // microphone/speaker for actual voice messages — this category checks
    // for that specific capability, not HAS_AUDIO, so D1L (genuinely no
    // audio hardware at all) needs to be named here too, not swapped onto
    // HAS_AUDIO (that would incorrectly show M9 a real playback slider).
#if !defined(RSM9) && !defined(RSSENSECAPD1L)
    _items.push_back({"Playback volume",SettingType::INTEGER,[&settings] {return int(settings.volume);},[&settings](int value) {settings.volume=value;},[](int value) {return String(value)+"%";},0,100,10});_items.back().id = SettingId::PlaybackVolume;++idx;
#endif
    _items.push_back({"Max voice messages",SettingType::INTEGER,[&settings] {return int(settings.maxMessages);},[&settings](int value) {settings.maxMessages=value;},nullptr,1,50,1});_items.back().id = SettingId::MaxVoiceMessages;++idx;
    _categories.push_back({"Voice Messages",start,idx-start,[&settings] {
#if defined(RSM9) || defined(RSSENSECAPD1L)
        (void)settings;return String("No audio hardware");
#else
        return String(settings.volume)+"% volume";
#endif
    }});
    _categories.back().id = SettingsCategoryId::Voice;
}
