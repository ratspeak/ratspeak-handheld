#pragma once
#include "protocol/RrcTypes.h"
#include "protocol/RrcPreferences.h"
#include "storage/RrcRecord.h"
#include "ratspeak_protocol.h"
#include <algorithm>
#include <cstring>

namespace handheld::history::rrc_catalog {
namespace disk = storage::rrc;
namespace model = handheld::rrc;
constexpr size_t Capacity = disk::SavedPageCapacity;

// One on-demand room detail shares the normal metadata read credit. Live
// observations are overlaid only for this exact active hub; persisted policy
// and remembered-key presence remain available while disconnected.
template<class Backend>
bool roomDetail(Backend& backend,const model::Conversation& binding,
                const storage::Result& result,const uint8_t* bytes,model::RoomView& row) {
    row={};disk::Context context;
    if(!binding.room[0] || binding.room[64] || binding.privateNotice() ||
        result.outcome!=storage::Outcome::Committed || result.error!=storage::Error::None ||
        !backend.rrcContext(binding.hub,binding.room,nullptr,context)) return false;
    uint8_t normalized[64];size_t length=0;const auto size=std::strlen(binding.room);
    if(rs_handheld_rrc_normalize(reinterpret_cast<const uint8_t*>(binding.room),size,0,normalized,sizeof normalized,&length)!=RS_HANDHELD_OK ||
        length!=size || std::memcmp(normalized,binding.room,size)) return false;
    std::memcpy(row.name,binding.room,sizeof row.name);std::memcpy(row.key,context.conversation,16);
    if(!std::memcmp(binding.hub,backend.rrcStatus().hub,16)) {
        model::RoomView rooms[model::RoomCapacity];const auto count=backend.rrcRooms(rooms,model::RoomCapacity);
        if(count>model::RoomCapacity) return false;
        for(size_t n=0;n<count;++n) if(!std::memcmp(rooms[n].key,row.key,16)) {row=rooms[n];break;}
    }
    if(result.length) {
        disk::Record record;if(result.length>sizeof record) return false;
        std::memcpy(record.bytes,bytes,result.length);
        if(!record.valid(result.length) || !record.matches(context) || record.kind()!=disk::Kind::Preferences ||
            !model::validRoomPreference(record.payload(),record.payloadLength()) ||
            std::strcmp(reinterpret_cast<const char*>(record.payload()+7),binding.room)) return false;
        row.notifications=model::Notifications(record.payload()[4]);
        row.muted=row.notifications==model::Notifications::Muted;
        row.keyRemembered=record.payloadLength()>model::RoomPreferenceHeader;
    }
    return true;
}

inline bool privateRows(const storage::Result& result,const uint8_t* bytes,model::PrivateView (&rows)[Capacity],size_t& count) {
    count=0;
    if(result.outcome!=storage::Outcome::Committed || result.error!=storage::Error::None ||
        result.length%sizeof(disk::PrivateNotice) || result.length>Capacity*sizeof(disk::PrivateNotice)) return false;
    uint32_t previous=UINT32_MAX;
    for(size_t n=0;n<result.length/sizeof(disk::PrivateNotice);++n) {
        disk::PrivateNotice row;std::memcpy(&row,bytes+n*sizeof row,sizeof row);uint8_t key[16];
        if(!row.counter || row.counter>=previous || rs_handheld_rrc_storage_key(1,row.participant,16,key)!=RS_HANDHELD_OK ||
            std::memcmp(key,row.key,16)) return false;
        std::memcpy(rows[n].identity,row.participant,16);rows[n].counter=row.counter;rows[n].unread=row.unread;
        previous=row.counter;++count;
    }
    return true;
}

// The query and result live in the existing worker credit. Only the visible
// page is projected; saved room names/keys are never hydrated into a new cache.
template<class Backend>
bool prepare(Backend& backend, const uint8_t hub[16], bool merged, size_t offset,
             disk::Record& record, uint32_t& cursor) {
    disk::Context context;
    if (offset>UINT32_MAX || !backend.rrcContext(hub,nullptr,nullptr,context)) return false;
    const auto status=backend.rrcStatus();
    if (merged && std::memcmp(status.hub,hub,16)) return false;
    const size_t live=merged ? backend.rrcChannels(nullptr,0) : 0;
    const size_t prefix=offset<live ? std::min(Capacity,live-offset) : 0;
    cursor=uint32_t(offset>live ? offset-live : 0);
    uint8_t payload[8+model::RoomCapacity*16]{};std::memcpy(payload,"HQS1",4);
    payload[4]=uint8_t(Capacity-prefix);
    if (merged) {
        model::RoomView rooms[model::RoomCapacity];
        const auto count=backend.rrcRooms(rooms,model::RoomCapacity);
        if (count>model::RoomCapacity) return false;
        payload[5]=uint8_t(count);
        for (size_t n=0;n<count;++n) std::memcpy(payload+8+n*16,rooms[n].key,16);
    }
    return disk::Record::make(record,context,disk::Kind::Preferences,payload,8+size_t(payload[5])*16);
}

template<class Backend>
bool project(Backend& backend, const uint8_t hub[16], bool merged, size_t offset,
             const storage::Result& result, const uint8_t* bytes,
             model::RoomView (&rows)[Capacity], size_t& count, bool& more) {
    count=0;more=false;
    if (result.outcome!=storage::Outcome::Committed || result.error!=storage::Error::None ||
        result.length%sizeof(disk::SavedRoom) || result.length>Capacity*sizeof(disk::SavedRoom)) return false;
    if (merged) {
        if (std::memcmp(backend.rrcStatus().hub,hub,16)) return false;
        count=backend.rrcChannels(rows,Capacity,offset);
        if (count>Capacity) return false;
        more=backend.rrcChannels(nullptr,0)>offset+count;
    }
    const size_t saved=result.length/sizeof(disk::SavedRoom);
    if (saved>Capacity-count) return false;
    for (size_t n=0;n<saved;++n) {
        disk::SavedRoom savedRoom;std::memcpy(&savedRoom,bytes+n*sizeof savedRoom,sizeof savedRoom);
        if (!savedRoom.name[0] || savedRoom.name[64] || savedRoom.notifications>2) return false;
        uint8_t normalized[64],key[16];size_t length=0;
        const auto size=std::strlen(savedRoom.name);
        if (rs_handheld_rrc_normalize(reinterpret_cast<const uint8_t*>(savedRoom.name),size,0,
            normalized,sizeof normalized,&length)!=RS_HANDHELD_OK || length!=size ||
            std::memcmp(normalized,savedRoom.name,size) ||
            rs_handheld_rrc_storage_key(0,normalized,length,key)!=RS_HANDHELD_OK ||
            std::memcmp(key,savedRoom.key,16)) return false;
        auto& row=rows[count++];row={};std::memcpy(row.key,key,16);std::memcpy(row.name,savedRoom.name,sizeof row.name);
        row.notifications=model::Notifications(savedRoom.notifications);row.muted=row.notifications==model::Notifications::Muted;row.keyRemembered=savedRoom.keyRemembered;
        row.phase=model::RoomPhase::Saved;
    }
    more |= result.more;return true;
}
}
