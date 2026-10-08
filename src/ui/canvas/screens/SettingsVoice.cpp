#include "SettingsScreen.h"
#include "ui/VoiceSettingsText.h"
void SettingsScreen::buildVoiceMenu() {
    _list.clear();const auto& s=_candidate.settings().voice;
    char volume[32];snprintf(volume,sizeof volume,"Playback volume: %u%%",s.volume);
    _list.addItem(volume);
    char limit[32];snprintf(limit,sizeof limit,"Max voice messages: %u",s.maxMessages);_list.addItem(limit);
    _list.addItem(handheld::ui::voice_settings::InternalLimit);
    _list.addItem(handheld::ui::voice_settings::Expiry);
    _list.addItem(std::string("SD card: ")+handheld::ui::voice_settings::SdLimit);
    _list.addItem("Clips: up to 15 seconds");
    _list.addItem("Live calls: unavailable");_list.addItem("< Back");
}
void SettingsScreen::activateVoiceRow(int row) {
    if(!_config || _config->settingsPending() || _radioApplyPending) return;
    if(row==7) {_subMenu=MENU_MAIN;buildMainMenu();return;}
    if(row==1) {startEditing(row,std::to_string(signed(_candidate.settings().voice.maxMessages)));_editLabel="Max voice messages: 1-50";_editInput.setMaxLength(2);return;}
    if(row!=0) return;
    auto& s=_candidate.settings().voice;s.volume=s.volume>=100?0:s.volume+10;
    applyAndSave();buildVoiceMenu();_list.setSelected(row);
}
