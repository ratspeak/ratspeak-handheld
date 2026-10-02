#include "SettingsScreen.h"
void SettingsScreen::buildVoiceMenu() {
    _list.clear();const auto& s=_candidate.settings().voice;
    char volume[32];snprintf(volume,sizeof volume,"Playback volume: %u%%",s.volume);
    _list.addItem(volume);_list.addItem("Clips: up to 15 seconds");
    _list.addItem("Live calls: unavailable");_list.addItem("< Back");
}
void SettingsScreen::activateVoiceRow(int row) {
    if(!_config || _config->settingsPending() || _radioApplyPending) return;
    if(row==3) {_subMenu=MENU_MAIN;buildMainMenu();return;}
    if(row!=0) return;
    auto& s=_candidate.settings().voice;s.volume=s.volume>=100?0:s.volume+10;
    applyAndSave();buildVoiceMenu();_list.setSelected(row);
}
