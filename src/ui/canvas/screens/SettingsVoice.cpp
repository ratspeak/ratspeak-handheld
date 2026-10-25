#include "SettingsScreen.h"
void SettingsScreen::buildVoiceMenu() {
    clearRows();if(!_config) return;const auto& s=settingsView().voice;
    char volume[32];snprintf(volume,sizeof volume,"Playback volume: %u%%",s.volume);
    addRow(volume, RowAction::Voice, 0);
    char limit[32];snprintf(limit,sizeof limit,"Max voice messages: %u",s.maxMessages);addRow(limit, RowAction::Voice, 1);
    addRow("< Back", RowAction::Back);
}
void SettingsScreen::activateVoiceRow(int row) {
    if(!_config || _config->settingsPending() || _radioApplyPending || (row!=0 && row!=1)) return;
    const auto& s=settingsView().voice;
    startEditing(row,std::to_string(unsigned(row==0?s.volume:s.maxMessages)));
    if(!_editing) return;
    _editLabel=row==0?"Playback volume: 0-100%":"Max voice messages: 1-50";
    _editInput.setMaxLength(row==0?3:2);
}
