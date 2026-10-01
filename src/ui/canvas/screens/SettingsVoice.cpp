#include "SettingsScreen.h"
void SettingsScreen::buildVoiceMenu() {
    _list.clear();const auto& s=_candidate.settings().voice;
    _list.addItem(s.enabled?"Live voice: ON":"Live voice: OFF");
    _list.addItem(s.contactsOnly?"Incoming: contacts only":"Incoming: anyone");
    _list.addItem(s.route==handheld::voice::Route::Auto?"Connection: AUTO":s.route==handheld::voice::Route::IpOnly?"Connection: WiFi/TCP":"Connection: LoRa");
    char volume[32];snprintf(volume,sizeof volume,"Voice volume: %u%%",s.volume);_list.addItem(volume);_list.addItem("< Back");
}
void SettingsScreen::activateVoiceRow(int row) {
    if(!_config || _config->settingsPending() || _radioApplyPending) return;
    auto& s=_candidate.settings().voice;
    if(row==4) {_subMenu=MENU_MAIN;buildMainMenu();return;}
    if(row==0) s.enabled=!s.enabled;
    else if(row==1) s.contactsOnly=!s.contactsOnly;
    else if(row==2) s.route=handheld::voice::Route((uint8_t(s.route)+1)%3);
    else if(row==3) s.volume=s.volume>=100?0:s.volume+10;
    else return;
    applyAndSave();buildVoiceMenu();_list.setSelected(row);
}
