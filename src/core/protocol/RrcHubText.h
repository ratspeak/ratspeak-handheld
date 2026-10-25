#pragma once
#include "ratspeak_protocol.h"
#include <cstring>
#include <string_view>

namespace handheld::rrc::hubText {
// Recognized text dialect from the trusted Rust channel_hub implementation.
// Callers must separately authenticate the source and bind the envelope room.
// Unknown hub text remains readable, with no inferred permission or command.
inline bool invitation(const uint8_t* bytes,size_t length,char (&room)[65]) {
    std::string_view value(reinterpret_cast<const char*>(bytes),length);
    constexpr std::string_view prefix="You have been invited to join ";
    constexpr std::string_view keyed=". This invite allows joining without the key (+k).";
    if(value.substr(0,prefix.size())!=prefix) return false;
    value.remove_prefix(prefix.size());
    if(value.size()>=keyed.size() && value.substr(value.size()-keyed.size())==keyed) value.remove_suffix(keyed.size());
    else if(!value.empty() && value.back()=='.') value.remove_suffix(1);
    else return false;
    size_t count=0;
    if(value.empty() || value.size()>64 || rs_handheld_rrc_normalize(reinterpret_cast<const uint8_t*>(value.data()),value.size(),0,
        reinterpret_cast<uint8_t*>(room),64,&count)!=RS_HANDHELD_OK || count!=value.size() || std::memcmp(room,value.data(),count)) return false;
    room[count]=0;return true;
}
inline bool invitation(const uint8_t* packet,const rs_handheld_rrc_view_t& view,char (&room)[65]) {
    if(view.meta.kind!=21 || view.meta.has_destination || view.body_kind!=1 || !view.room.length ||
        !invitation(packet+view.text.offset,view.text.length,room)) return false;
    char canonical[65];size_t length=0;
    return rs_handheld_rrc_normalize(packet+view.room.offset,view.room.length,0,reinterpret_cast<uint8_t*>(canonical),64,&length)==RS_HANDHELD_OK &&
        length==std::strlen(room) && !std::memcmp(canonical,room,length);
}
inline bool source(const uint8_t identity[16],const uint8_t hub[16]) {
    uint8_t address[16];return rs_handheld_rrc_destination_for_identity(identity,address)==RS_HANDHELD_OK && !std::memcmp(address,hub,16);
}
}
