#include "protocol/RustRrcEngine.h"
#include "protocol/RustClock.h"
#include "protocol/RustEntropy.h"
#include "protocol/RustInterfacePump.h"
#include "storage/MessageStore.h"
#include "util/DisplayName.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

using namespace handheld::rrc;
namespace store = handheld::storage;
namespace disk = handheld::storage::rrc;
namespace {
void wipe(void* data, size_t length) { auto* p = static_cast<volatile uint8_t*>(data); while (length--) *p++ = 0; }
void text(char* out, size_t capacity, const uint8_t* bytes, size_t length) {
    if (!capacity) return;
    size_t n = 0;
    while (n < length && n < capacity - 1) {
        if (bytes[n] < 32 || bytes[n] == 127) { out[n] = bytes[n] == '\n' ? '\n' : ' '; ++n; continue; }
        const auto scalar = handheld::displayNameCodepoint(reinterpret_cast<const char*>(bytes + n), length - n);
        if (!scalar || scalar > capacity - 1 - n) break;
        memcpy(out + n, bytes + n, scalar); n += scalar;
    }
    out[n] = 0;
}
bool nonzero(const uint8_t id[16]) { uint8_t value = 0; for (size_t n = 0; n < 16; ++n) value |= id[n]; return value != 0; }
bool normalized(const uint8_t* input, size_t length, char* output, size_t capacity, bool nick = false) {
    size_t count = 0;
    if (rs_handheld_rrc_normalize(input, length, nick, reinterpret_cast<uint8_t*>(output), capacity - 1, &count) != RS_HANDHELD_OK) return false;
    output[count] = 0; return true;
}
bool messageKey(const rs_handheld_rrc_meta_t& meta, uint8_t file[16]) {
    uint8_t material[24]; memcpy(material, meta.source, 16); memcpy(material + 16, meta.id, 8);
    return rs_handheld_rrc_storage_key(2, material, sizeof material, file) == RS_HANDHELD_OK;
}
}

uint64_t RustRrcEngine::now() const { return _d.clock ? _d.clock->nowMs() : 0; }
void RustRrcEngine::changed() { if (_status.revision != UINT32_MAX) ++_status.revision; }
void RustRrcEngine::notice(const char* value) {
    text(_status.notice, sizeof _status.notice, reinterpret_cast<const uint8_t*>(value), strlen(value)); changed();
}
uint32_t RustRrcEngine::wait(uint32_t packets) const {
    uint32_t radio = 0;
    if (_d.pump && _interface <= 6) radio = _d.pump->interfaceTxWaitMs(_interface, packets);
    else if (_d.pump) for (uint8_t i = 0; i <= 6; ++i) if (_d.pump->interfaceOnline(i))
        radio = std::max(radio, _d.pump->interfaceTxWaitMs(i, packets));
    return std::max(uint32_t(30000), radio > UINT32_MAX - 30000 ? UINT32_MAX : radio + 30000);
}
bool RustRrcEngine::live(Handle handle) const {
    return !_stopped && handle.valid() && handle.slot == _link.slot && handle.generation == _link.generation;
}
void RustRrcEngine::begin(const Deps& deps) {
    if (!drained()) return;
    const auto generation = _status.generation, sequence = _sequence;
    end(); _d = deps; _sequence = sequence; _status = {};
    if (!deps.ctx || !deps.clock || !deps.pump || !deps.links || !deps.store || !deps.identity || generation == UINT32_MAX) return;
    _status.generation = generation + 1; _stopped = false;
    memcpy(_status.nickname, "Handheld", 9); _d.links->setRrcSink(this); changed();
    disk::Context root; memcpy(root.local, _d.identity, 16);
    loadPreferences(PreferenceStep::ReadRoot, root);
}
void RustRrcEngine::stop() {
    if (_stopped) return;
    _stopped = true; disconnect();
}
bool RustRrcEngine::drained() const {
    if (_send.ticket.valid() || _send.statusDirty) return false;
    if (_preferences.step != PreferenceStep::Idle || _joinConfirmed || _invalidKeyRoom < RoomCapacity) return false;
    for (const auto& item : _receive) if (item.ticket.valid() || item.statusPending) return false;
    return true;
}
void RustRrcEngine::end() {
    if (_d.links) { stop(); _d.links->setRrcSink(nullptr); }
    // The runtime's lifecycle barrier consumes owned storage completions first.
    if (!drained()) return;
    const auto generation = _status.generation, sequence = _sequence;
    *this = RustRrcEngine{}; _status.generation = generation; _sequence = sequence;
}
void RustRrcEngine::wipeControl() { wipe(&_control, sizeof _control); _control = {}; }
void RustRrcEngine::disconnect() {
    const auto old = _link; _link = {};
    if (_d.links && old.valid()) _d.links->closeRrc(old);
    wipeControl(); _pong = {}; _status.phase = Phase::Disconnected;
    _status.directoryPending = false; _directoryDeadline = 0; _directoryWanted = false;
    _status.directoryStale = _status.directoryKnown;
    if (!_joinConfirmed) { _joinRoom = UINT8_MAX; _joinPreferenceLength = 0; }
    for (auto& room : _rooms) if (room.used) { room.wanted = false; room.view.phase = RoomPhase::Saved; }
    for (auto& person : _people) person = {};
    if (_send.length && !_send.confirmed) {
        _status.sending = _send.started ? SendPhase::Unconfirmed : SendPhase::NotSent;
        _send.status = _send.started ? disk::Status::Unconfirmed : disk::Status::Failed;
        _send.statusDirty = true;
    }
    changed();
}
void RustRrcEngine::recover(const char* reason) {
    if (_stopped || _status.phase == Phase::Disconnected) return;
    const auto old = _link; _link = {}; if (old.valid()) _d.links->closeRrc(old);
    wipeControl(); _pong = {}; _status.phase = Phase::Recovering;
    _status.directoryPending = false; _directoryDeadline = 0; _directoryWanted = false;
    _status.directoryStale = _status.directoryKnown;
    if (!_joinConfirmed) {_joinRoom=UINT8_MAX;_joinPreferenceLength=0;}
    if (_failures < 8) ++_failures;
    const uint32_t backoff = std::min(uint32_t(300000), uint32_t(2000) << (_failures - 1));
    uint8_t random[2]; RustEntropy::fill(random, sizeof random);
    const int32_t jitter = int32_t((uint32_t(random[0]) << 8 | random[1]) % (backoff / 5 + 1)) - int32_t(backoff / 10);
    _retryAt = now() + backoff + jitter; _pathRequested = false;
    for (auto& room : _rooms) if (room.used && room.wanted) {
        room.view.phase = room.view.needsKey && !room.view.keyRemembered ? RoomPhase::NeedsKey : RoomPhase::Recovering;
        room.view.membersComplete = false;
    }
    for (auto& person : _people) person = {};
    if (_send.length && !_send.confirmed) {
        _status.sending = _send.started ? SendPhase::Unconfirmed : SendPhase::NotSent;
        _send.status = _send.started ? disk::Status::Unconfirmed : disk::Status::Failed;
        _send.statusDirty = true;
    }
    notice(reason);
}
void RustRrcEngine::onRrcClosed(Handle handle) { if (live(handle)) recover("Connection lost; reconnecting"); }
RustRrcEngine::Hub* RustRrcEngine::findHub(const uint8_t address[16]) {
    for (auto& hub : _hubs) if (hub.used && !memcmp(hub.view.address, address, 16)) return &hub;
    return nullptr;
}
RustRrcEngine::Room* RustRrcEngine::findRoom(const char* name) {
    for (auto& room : _rooms) if (room.used && !strcmp(room.view.name, name)) return &room;
    return nullptr;
}
const RustRrcEngine::Room* RustRrcEngine::findRoom(const char* name) const {
    for (const auto& room : _rooms) if (room.used && !strcmp(room.view.name, name)) return &room;
    return nullptr;
}
void RustRrcEngine::learn(const rs_handheld_announce_event_t& event) {
    if (_stopped) return;
    uint8_t identity[16];
    if (rs_handheld_rrc_hub_identity(event.destination_hash, event.public_key, identity) != RS_HANDHELD_OK) return;
    Hub* hub = findHub(event.destination_hash);
    if (hub && memcmp(hub->publicKey, event.public_key, 64)) return;
    if (!hub) for (auto& candidate : _hubs) if (!candidate.used) { hub = &candidate; break; }
    if (!hub) for (auto& candidate : _hubs) {
        if (!memcmp(candidate.view.address, _status.hub, 16)) continue;
        if (!hub || candidate.seen < hub->seen) hub = &candidate;
    }
    if (!hub) return;
    const bool same = hub->used && !memcmp(hub->view.address, event.destination_hash, 16);
    const bool saved = (same && hub->view.saved) || _bookmarks.find(event.destination_hash) >= 0;
    *hub = {}; hub->used = true; hub->seen = now(); hub->view.saved = saved;
    memcpy(hub->publicKey, event.public_key, 64); memcpy(hub->view.address, event.destination_hash, 16);
    rs_handheld_rrc_span_t name{};
    if (rs_handheld_rrc_announce_name(event.app_data, event.app_data_len, &name) == RS_HANDHELD_OK)
        text(hub->view.name, sizeof hub->view.name, event.app_data + name.offset, name.length);
    if (!hub->view.name[0]) snprintf(hub->view.name, sizeof hub->view.name, "Hub %02x%02x%02x", event.destination_hash[0], event.destination_hash[1], event.destination_hash[2]);
    changed();
}
Status RustRrcEngine::status() const {
    auto value = _status;
    value.busy = _control.length || _send.ticket.valid() || _send.statusDirty || _send.length ||
        _preferences.step != PreferenceStep::Idle || _joinConfirmed || _invalidKeyRoom < RoomCapacity;
    return value;
}
size_t RustRrcEngine::hubs(HubView* output, size_t capacity) const {
    size_t used = 0; if (!output) return 0;
    for (const auto& hub : _hubs) {
        if (!hub.used || used >= capacity) continue;
        auto& row = output[used++]; row = hub.view;
        row.active = _status.phase != Phase::Disconnected && !memcmp(row.address, _status.hub, 16);
        row.ageSeconds = uint32_t(std::min(uint64_t(UINT32_MAX), (now() - hub.seen) / 1000));
        rs_handheld_route_t route{};
        if (rs_handheld_rns_route(_d.ctx, row.address, now(), &route) == RS_HANDHELD_OK && route.kind == RS_HANDHELD_ROUTE_DIRECT) {
            row.interface = route.interface_id; row.hops = route.hops;
            row.reachable = _d.pump->interfaceOnline(route.interface_id);
        }
    }
    for (size_t i = 0; i < _bookmarks.count && used < capacity; ++i) {
        bool listed = false; for (size_t j = 0; j < used; ++j) listed |= !memcmp(output[j].address, _bookmarks.keys[i], 16);
        if (listed) continue;
        auto& row = output[used++]; row = {}; memcpy(row.address, _bookmarks.keys[i], 16); row.saved = true;
        row.active = _status.phase != Phase::Disconnected && !memcmp(row.address, _status.hub, 16);
        snprintf(row.name, sizeof row.name, "Saved hub %02x%02x%02x", row.address[0], row.address[1], row.address[2]);
    }
    return used;
}
size_t RustRrcEngine::rooms(RoomView* output, size_t capacity) const {
    size_t used = 0; if (output) for (const auto& room : _rooms) if (room.used && used < capacity) output[used++] = room.view;
    return used;
}
size_t RustRrcEngine::channels(RoomView* output, size_t capacity, size_t offset) const {
    size_t used=0,position=0;
    for (const auto& room:_rooms) if (room.used) {
        if (position++<offset) continue;
        if (output && used<capacity) output[used++]=room.view;
    }
    DirectoryView listed;
    for (size_t n=0;_directory.at(n,listed);++n) {
        uint8_t key[16];
        if (findRoom(listed.name) || rs_handheld_rrc_storage_key(0,reinterpret_cast<const uint8_t*>(listed.name),strlen(listed.name),key)!=RS_HANDHELD_OK ||
            _savedRooms.find(key)>=0 || position++<offset) continue;
        if (output && used<capacity) {
            auto& row=output[used++];row={};strcpy(row.name,listed.name);strcpy(row.topic,listed.topic);
            memcpy(row.key,key,16);row.phase=RoomPhase::Available;
        }
    }
    return output ? used : position;
}
size_t RustRrcEngine::people(const char* name, PersonView* output, size_t capacity) const {
    const auto* room = name ? findRoom(name) : nullptr; const uint8_t mask = room ? uint8_t(1u << (room - _rooms)) : 0xff;
    size_t used = 0; if (output) for (const auto& person : _people) if ((person.rooms & mask) && used < capacity) output[used++] = person;
    return used;
}
size_t RustRrcEngine::directory(DirectoryView* output, size_t capacity, size_t offset) const {
    size_t count = 0;
    if (output) while (count < capacity && _directory.at(offset + count, output[count])) ++count;
    return count;
}
bool RustRrcEngine::context(const uint8_t hub[16], const char* room, const uint8_t* participant, disk::Context& out) const {
    if (!_d.identity || !nonzero(hub)) return false;
    out = {}; memcpy(out.local, _d.identity, 16); memcpy(out.hub, hub, 16);
    if (participant) return rs_handheld_rrc_storage_key(1, participant, 16, out.conversation) == RS_HANDHELD_OK;
    if (!room || !room[0]) return true; // hub notices and hub preferences
    char normalizedRoom[65]; if (!normalized(reinterpret_cast<const uint8_t*>(room), strnlen(room, 65), normalizedRoom, sizeof normalizedRoom)) return false;
    return rs_handheld_rrc_storage_key(0, reinterpret_cast<const uint8_t*>(normalizedRoom), strlen(normalizedRoom), out.conversation) == RS_HANDHELD_OK;
}
bool RustRrcEngine::known(const uint8_t file[16]) const {
    for (size_t n = 0; n < _recentCount; ++n) if (!memcmp(file, _recent[n], 16)) return true;
    return false;
}
void RustRrcEngine::remember(const uint8_t file[16]) {
    memcpy(_recent[_recentNext], file, 16); _recentNext = (_recentNext + 1) % DedupCapacity;
    if (_recentCount < DedupCapacity) ++_recentCount;
}
void RustRrcEngine::participant(const uint8_t identity[16], const uint8_t* nick, size_t length, uint8_t room, bool remove) {
    if (room >= RoomCapacity) return;
    PersonView* target = nullptr;
    for (auto& person : _people) if (person.rooms && !memcmp(person.identity, identity, 16)) { target = &person; break; }
    if (remove) { if (target) target->rooms &= uint8_t(~(1u << room)); return; }
    if (!target) for (auto& person : _people) if (!person.rooms) { target = &person; person = {}; memcpy(person.identity, identity, 16); break; }
    if (!target) { _rooms[room].view.membersComplete = false; return; }
    target->rooms |= uint8_t(1u << room);
    if (nick && length) text(target->nickname, sizeof target->nickname, nick, length);
}
bool RustRrcEngine::encode(uint64_t kind, const char* room, const uint8_t* body, size_t length,
    uint8_t bodyKind, const uint8_t* destination, uint8_t* packet, size_t& size, const uint8_t* messageId) const {
    rs_handheld_rrc_meta_t meta{}; meta.kind = kind; meta.timestamp_ms = RustClock::epochSecs() * 1000;
    if (!meta.timestamp_ms) meta.timestamp_ms = now();
    memcpy(meta.source, _d.identity, 16);
    if (messageId) memcpy(meta.id, messageId, 8); else RustEntropy::fill(meta.id, 8);
    if (destination) { meta.has_destination = 1; memcpy(meta.destination, destination, 16); }
    return rs_handheld_rrc_encode(&meta, reinterpret_cast<const uint8_t*>(room), room ? strlen(room) : 0,
        reinterpret_cast<const uint8_t*>(_status.nickname), strlen(_status.nickname) <= _status.nicknameLimit ? strlen(_status.nickname) : 0, body, length, bodyKind,
        packet, PacketCapacity, &size) == RS_HANDHELD_OK;
}
RustRrcEngine::Code RustRrcEngine::control(uint8_t kind, const char* room, const uint8_t* body, size_t length, uint8_t bodyKind) {
    if (_control.length || _sequence == UINT32_MAX) return Code::Busy;
    size_t count = 0;
    if (!encode(kind, room, body, length, bodyKind, nullptr, _control.packet, count)) return Code::TooLong;
    _control.length = count; _control.kind = kind; _control.born = now(); _control.token = ++_sequence;
    _control.waitMs = wait(8);
    auto* item = room ? findRoom(room) : nullptr; _control.room = item ? uint8_t(item - _rooms) : UINT8_MAX;
    changed(); return Code::Ok;
}

RustRrcEngine::Code RustRrcEngine::command(const Command& command, const uint8_t* body, size_t length) {
    if (_stopped) return Code::Offline;
    if (command.generation != _status.generation) return Code::Stale;
    if (length && !body) return Code::Invalid;
    auto preference = [&] {
        const auto result = preferenceCommand(command, body, length);
        if (result != Code::Ok && _preferences.step == PreferenceStep::Idle) _preferences.revision = 0;
        return result;
    };
    if (command.action == Action::SaveHub || command.action == Action::ForgetHub || command.action == Action::Nickname || command.action == Action::Draft ||
        command.action == Action::MarkRead || command.action == Action::ClearHistory || command.action == Action::Mute ||
        command.action == Action::ForgetKey || command.action == Action::ForgetRoom)
        return preference();
    if (command.action == Action::Connect) {
        if (!nonzero(command.hub)) return Code::Invalid;
        if (!drained() || _send.length || !_preferences.ready) return Code::Busy;
        disconnect();
        if (_status.generation == UINT32_MAX) return Code::Full;
        ++_status.generation; memcpy(_status.hub, command.hub, 16); memset(_status.identity, 0, 16);
        memset(_rooms, 0, sizeof _rooms); memset(_people, 0, sizeof _people);
        _recentCount = _recentNext = 0; _status.phase = Phase::Finding; _status.notice[0] = 0;
        _directory = {}; _status.directoryPartial = false;
        _status.directoryKnown = _status.directoryStale = _status.directoryFailed = false; _status.directoryCount = 0;
        _savedRooms = {};
        disk::Context scope; context(_status.hub, nullptr, nullptr, scope);
        loadPreferences(PreferenceStep::ReadHub, scope);
        _status.capabilities = 0; _status.bodyLimit = 350; _status.roomLimit = 64; _status.nicknameLimit = 32; _status.roomsLimit = RoomCapacity;
        _failures = 0; _interface = UINT8_MAX; _pathRequested = false; _deadline = now() + wait();
        if (const auto* hub = findHub(command.hub)) memcpy(_status.name, hub->view.name, sizeof _status.name);
        else snprintf(_status.name, sizeof _status.name, "Hub %02x%02x%02x", command.hub[0], command.hub[1], command.hub[2]);
        changed(); return Code::Ok;
    }
    if (memcmp(command.hub, _status.hub, 16)) return Code::Stale;
    if (command.action == Action::Disconnect) { disconnect(); return Code::Ok; }
    if (command.action == Action::Retry) { if (_status.phase == Phase::Recovering) _retryAt = now(); return Code::Ok; }
    if (_preferences.step != PreferenceStep::Idle) return Code::Busy;
    if (_status.phase != Phase::Online) return Code::Offline;
    char room[65]{};
    if (command.room[0] && !normalized(reinterpret_cast<const uint8_t*>(command.room), strnlen(command.room, 65), room, sizeof room)) return Code::Invalid;
    if (strlen(room) > _status.roomLimit) return Code::TooLong;
    auto* item = room[0] ? findRoom(room) : nullptr;
    switch (command.action) {
    case Action::Join: {
        if (!room[0] || length > KeyCapacity || (length && memchr(body, 0, length))) return Code::Invalid;
        size_t joined = 0; for (const auto& row : _rooms) if (row.used && row.wanted) ++joined;
        if (item && (item->view.phase == RoomPhase::Joined || item->view.phase == RoomPhase::Joining)) return Code::Ok;
        if ((!item || !item->wanted) && joined >= _status.roomsLimit) return Code::Full;
        if (!item) for (auto& row : _rooms) if (!row.used || !row.wanted) {
            bool owned=false;for(const auto& receive:_receive)
                owned|=receive.room==size_t(&row-_rooms) && (receive.ticket.valid() || receive.statusPending || receive.proofPending);
            if(!owned) {item=&row;break;}
        }
        if (!item) return Code::Full;
        if (_control.length || _joinRoom < RoomCapacity) return Code::Busy;
        if (!item->used || strcmp(item->view.name, room)) { *item = {}; item->used = true; strcpy(item->view.name, room);
            rs_handheld_rrc_storage_key(0, reinterpret_cast<const uint8_t*>(room), strlen(room), item->view.key); }
        if (length) return startJoin(*item, body, length, command.flags & 1);
        disk::Context scope; context(_status.hub, room, nullptr, scope);
        loadPreferences(PreferenceStep::ReadJoin, scope); _preferences.room = uint8_t(item - _rooms);
        _preferences.revision = command.revision; _preferences.action = Action::Join;
        item->view.phase = RoomPhase::Joining; item->deadline = now() + wait(8); changed(); return Code::Ok;
    }
    case Action::Leave: {
        if (!item) return Code::NotJoined;
        const auto result = control(12, room, nullptr, 0, 0); if (result != Code::Ok) return result;
        item->wanted = false; item->view.phase = RoomPhase::Leaving; item->deadline = now() + wait(); changed(); return Code::Ok;
    }
    case Action::Message: case Action::PrivateNotice: case Action::Emote: return send(command, body, length);
    case Action::Directory: {
        if (_status.directoryPending) return Code::Busy;
        const auto result = control(20, nullptr, reinterpret_cast<const uint8_t*>("/list"), 5);
        if (result == Code::Ok) { _directoryWanted = false; _status.directoryPending = true;
            _status.directoryFailed = false; _status.directoryStale = _status.directoryKnown; _directoryDeadline = now() + wait(8); } return result;
    }
    case Action::Who: case Action::Topic: {
        if (!item || item->view.phase != RoomPhase::Joined) return Code::NotJoined;
        const char* value = command.action == Action::Who ? "/who" : "/topic";
        return control(20, room, reinterpret_cast<const uint8_t*>(value), strlen(value));
    }
    case Action::Advanced:
        if (!length || body[0] != '/' || length > _status.bodyLimit) return Code::Invalid;
        return control(20, room[0] ? room : nullptr, body, length);
    case Action::Mute:
        return preference();
    default: return Code::Unsupported;
    }
}

RustRrcEngine::Code RustRrcEngine::send(const Command& command, const uint8_t* body, size_t length) {
    if (_send.length || _send.ticket.valid() || _send.statusDirty || _sequence == UINT32_MAX) return Code::Busy;
    if (!length || length > _status.bodyLimit || memchr(body, 0, length)) return Code::TooLong;
    const bool pm = command.action == Action::PrivateNotice, action = command.action == Action::Emote;
    if ((pm && !(_status.capabilities & 4)) || (action && !(_status.capabilities & 2))) return Code::Unsupported;
    if (pm && !nonzero(command.participant)) return Code::Invalid;
    const auto* room = pm ? nullptr : findRoom(command.room);
    if (!pm && (!room || room->view.phase != RoomPhase::Joined)) return Code::NotJoined;
    if (now() - _rateWindow >= 60000) { _rateWindow = now(); _rateCount = 0; }
    if (_rateLimit && _rateCount >= _rateLimit) return Code::Busy;
    if (!pm && body[0] == '/' && !action) return Code::Invalid; // explicit Advanced command mode
    size_t count = 0;
    if (!encode(pm ? 21 : action ? 22 : 20, pm ? nullptr : room->view.name, body, length, 1,
                pm ? command.participant : nullptr, _send.packet, count)) return Code::TooLong;
    rs_handheld_rrc_view_t view{};
    if (rs_handheld_rrc_decode(_send.packet, count, &view) != RS_HANDHELD_OK || !messageKey(view.meta, _send.file)) return Code::Invalid;
    disk::Context scope; if (!context(_status.hub, pm ? nullptr : room->view.name, pm ? command.participant : nullptr, scope)) return Code::Invalid;
    Record record; Record::make(record, scope, disk::Kind::Message, _send.packet, count, disk::Status::Pending, now());
    const auto submitted = _d.store->requestRrc(store::Operation::RrcAppend, record, _send.file,0,0,
        store::HistoryDirection::Before,pm ? command.participant : nullptr,pm?_observation:room->observation);
    if (!submitted.accepted()) { wipe(&_send, sizeof _send); _send = {}; return Code::Storage; }
    _send.ticket = submitted.ticket; _send.length = count; _send.token = ++_sequence;
    _send.born = now(); _send.deadline = now() + wait(8); memcpy(_send.conversation, scope.conversation, 16);
    _send.waitMs = wait(8);
    _status.sending = SendPhase::Saving; _status.sendRevision = command.revision;
    _status.sendSaved = false; _status.sendSettled = false; ++_rateCount; changed(); return Code::Ok;
}

void RustRrcEngine::onRrcTransmit(Handle handle, uint32_t token, bool started) {
    if (!live(handle)) return;
    if (_send.length && token == _send.token) {
        _send.admitted = true; _send.started = started;
        rs_handheld_rrc_view_t view{}; rs_handheld_rrc_decode(_send.packet, _send.length, &view);
        const bool pm = view.meta.has_destination;
        _status.sending = started ? (pm ? SendPhase::Transmitted : SendPhase::Awaiting) : SendPhase::NotSent;
        _send.status = started ? (pm ? disk::Status::Transmitted : disk::Status::Unconfirmed) : disk::Status::Failed;
        _send.statusDirty = true; _send.deadline = now() + wait(8); changed(); return;
    }
    if (_pong.length && token == _pong.token) { _pong = {}; return; }
    if (_control.length && token == _control.token) {
        const auto kind = _control.kind, room = _control.room;
        wipeControl();
        if (!started) { notice("Command was not transmitted"); if (room < RoomCapacity) _rooms[room].view.phase = RoomPhase::Error; }
        else if (kind == 1) { _nicknameDirty = false; _status.phase = Phase::Greeting; _deadline = now() + wait(6); changed(); }
    }
}

void RustRrcEngine::pollSend() {
    if (!_d.store) return;
    if (_send.ticket.valid()) {
        store::Result result;
        if (!_d.store->peekResult(_send.ticket, result)) return;
        Record saved;
        const bool haveSaved = _send.statusRead && result.outcome == store::Outcome::Committed &&
            result.length >= disk::Header && result.length <= sizeof saved &&
            _d.store->readPayload(_send.ticket, saved.bytes, result.length) && saved.valid(result.length);
        _d.store->releaseResult(_send.ticket); _send.ticket = {};
        if (result.outcome != store::Outcome::Committed) {
            if (!_send.statusWrite) { _status.sending = SendPhase::NotSent; _status.sendSettled = true; wipe(&_send, sizeof _send); _send = {}; }
            else { _send.statusDirty = true; _send.statusRead = result.error == store::Error::Stale; }
            notice("Storage failed; draft retained"); return;
        }
        if (_send.statusRead) {
            _send.statusRead = false;
            if (!haveSaved) { notice("Send record unavailable; draft retained"); return; }
            if (saved.status() == disk::Status::Confirmed) {
                _send.status = disk::Status::Confirmed; _send.statusDirty = false; _status.sending = SendPhase::Confirmed;
            }
        }
        _send.counter = result.key.counter; _send.revision = result.revision;
        _status.sendSaved = true;
        if (_send.statusWrite) { _send.statusWrite = false; }
        else if (_status.phase == Phase::Online && !_stopped) _status.sending = SendPhase::Sending;
        else { _send.status = disk::Status::Failed; _send.statusDirty = true; _status.sending = SendPhase::NotSent; }
        changed();
    }
    if (!_send.length) return;
    if (_send.confirmed) { _send.status = disk::Status::Confirmed; _send.statusDirty = true; _send.confirmed = false; _status.sending = SendPhase::Confirmed; }
    if (_send.statusDirty && _send.counter) {
        disk::Context scope; context(_status.hub, nullptr, nullptr, scope); memcpy(scope.conversation, _send.conversation, 16);
        Record record; Record::make(record, scope, disk::Kind::Message, nullptr, 0, _send.status);
        const auto submitted = _d.store->requestRrc(_send.statusRead ? store::Operation::RrcRead : store::Operation::RrcStatus, record, _send.file, _send.counter);
        if (submitted.accepted()) { _send.ticket = submitted.ticket; _send.statusWrite = true; _send.statusDirty = false; }
        return;
    }
    if (_send.statusWrite || _send.statusDirty) return;
    if (_status.sending == SendPhase::Sending && !_send.admitted && !_stopped && _status.phase == Phase::Online) {
        const auto token = _send.token;
        // The receipt hook may run inline. Install all ownership before offer.
        if (_d.links->sendRrc(_link, _send.packet, _send.length, token, _send.born, _send.waitMs) && _send.token == token) _send.admitted = true;
    }
    if (now() >= _send.deadline && (_status.sending == SendPhase::Sending || _status.sending == SendPhase::Awaiting)) {
        _status.sending = _send.started ? SendPhase::Unconfirmed : SendPhase::NotSent;
        _send.status = _send.started ? disk::Status::Unconfirmed : disk::Status::Failed; _send.statusDirty = true; changed(); return;
    }
    if (_status.sending == SendPhase::Confirmed || _status.sending == SendPhase::Transmitted ||
        _status.sending == SendPhase::Unconfirmed || _status.sending == SendPhase::NotSent) {
        wipe(&_send, sizeof _send); _send = {}; _status.sendSettled = true; changed();
    }
}

void RustRrcEngine::applyRoomControl(const uint8_t* data, size_t length, const rs_handheld_rrc_view_t& view, Room& room) {
    size_t count = 0;
    if (rs_handheld_rrc_member(data, length, 0, nullptr, &count) != RS_HANDHELD_OK) return;
    const uint8_t slot = uint8_t(&room - _rooms); bool includesSelf = false;
    for (size_t n = 0; n < count; ++n) { uint8_t identity[16]; rs_handheld_rrc_member(data, length, n, identity, &count); includesSelf |= !memcmp(identity, _d.identity, 16); }
    if (view.meta.kind == 11) {
        if (!room.wanted || room.view.phase == RoomPhase::Leaving) return;
        const bool confirming = room.view.phase == RoomPhase::Joining || (room.view.phase == RoomPhase::Error && includesSelf);
        if (room.view.phase == RoomPhase::Error && !confirming) return;
        const bool replace = confirming || (includesSelf && !(room.view.phase == RoomPhase::Joined && count == 1));
        if (replace) for (auto& person : _people) person.rooms &= uint8_t(~(1u << slot));
        if (replace) room.view.membersComplete = includesSelf;
        else if (count > 1) room.view.membersComplete = false;
        room.view.phase = RoomPhase::Joined;
        joined(room);
        for (size_t n = 0; n < count; ++n) { uint8_t identity[16]; rs_handheld_rrc_member(data, length, n, identity, &count);
            participant(identity, count == 1 ? data + view.nickname.offset : nullptr, count == 1 ? view.nickname.length : 0, slot); }
        if (confirming) participant(_d.identity, reinterpret_cast<const uint8_t*>(_status.nickname), strlen(_status.nickname), slot);
    } else {
        for (size_t n = 0; n < count; ++n) { uint8_t identity[16]; rs_handheld_rrc_member(data, length, n, identity, &count); participant(identity, nullptr, 0, slot, true); }
        if (includesSelf || (room.view.phase == RoomPhase::Leaving && !count)) {
            room.wanted = false; room.view.phase = RoomPhase::Saved; room.view.membersComplete = false;
            for (auto& person : _people) person.rooms &= uint8_t(~(1u << slot));
        }
    }
    ++room.view.revision; changed();
}

bool RustRrcEngine::roomStatus(Room& room, const uint8_t* bytes, size_t length) {
    // Exact rrcd status grammar, and only called after hub-source validation and
    // durable admission. A peer quoting this text cannot confirm a JOIN.
    char prefix[73]; snprintf(prefix, sizeof prefix, "room %s: ", room.view.name);
    std::string_view value(reinterpret_cast<const char*>(bytes), length);
    if (value.substr(0, strlen(prefix)) != prefix) return false;
    value.remove_prefix(strlen(prefix)); const auto mode = value.find("; mode=");
    if (mode == value.npos) return false;
    const auto registration = Directory::trim(value.substr(0, mode));
    if (registration != "registered" && registration != "unregistered") return false;
    value.remove_prefix(mode + 7); const auto topic = value.find("; topic=");
    if (topic == value.npos) return false;
    const auto modes = Directory::trim(value.substr(0, topic)); const auto description = Directory::trim(value.substr(topic + 8));
    text(room.view.modes, sizeof room.view.modes, reinterpret_cast<const uint8_t*>(modes.data()), modes.size());
    text(room.view.topic, sizeof room.view.topic, reinterpret_cast<const uint8_t*>(description.data()), description == "(none)" ? 0 : description.size());
    room.view.registered = registration == "registered" ? 1 : 2;
    if (room.wanted && (room.view.phase == RoomPhase::Joining || room.view.phase == RoomPhase::Error)) {
        room.view.phase = RoomPhase::Joined; room.view.membersComplete = false;
        participant(_d.identity, reinterpret_cast<const uint8_t*>(_status.nickname), strlen(_status.nickname), uint8_t(&room - _rooms));
        joined(room);
    }
    ++room.view.revision; changed(); return true;
}

void RustRrcEngine::onRrcPacket(Handle handle, const uint8_t* data, size_t length, const uint8_t packetHash[32]) {
    if (!live(handle)) return;
    rs_handheld_rrc_view_t view{}; if (rs_handheld_rrc_decode(data, length, &view) != RS_HANDHELD_OK) return;
    const auto kind = view.meta.kind;
    const bool hub = !memcmp(view.meta.source, _status.identity, 16);
    if ((kind == 2 || kind == 11 || kind == 13 || kind == 30 || kind == 40 || kind == 50) && !hub) return;
    if (view.meta.has_destination && (view.room.length || kind != 21 || memcmp(view.meta.destination, _d.identity, 16))) return;
    uint8_t file[16]; if (!messageKey(view.meta, file)) return;
    if (known(file)) { _d.links->proveRrc(handle, packetHash); return; }
    if (kind == 2) {
        if (_status.phase != Phase::Greeting) return;
        rs_handheld_rrc_welcome_t welcome{}; if (rs_handheld_rrc_welcome(data, length, &welcome) != RS_HANDHELD_OK) return;
        if (welcome.name.length) text(_status.name, sizeof _status.name, data + welcome.name.offset, welcome.name.length);
        _status.capabilities = welcome.capabilities & ~1u; // Resource is never negotiated
        if (welcome.limits_present & 1) _status.nicknameLimit = uint8_t(std::min(uint32_t(32), welcome.limits[0]));
        if (welcome.limits_present & 2) _status.roomLimit = uint16_t(std::min(uint32_t(64), welcome.limits[1]));
        if (welcome.limits_present & 4) _status.bodyLimit = uint16_t(std::min(uint32_t(350), welcome.limits[2]));
        if (welcome.limits_present & 8) _status.roomsLimit = uint8_t(std::min(uint32_t(RoomCapacity), welcome.limits[3]));
        _rateLimit = welcome.limits_present & 16 ? welcome.limits[4] : 0;
        _status.phase = Phase::Online; _onlineAt = now(); _directoryWanted = true; notice("Connected");
    } else if (kind == 30) {
        if (_pong.length || _sequence == UINT32_MAX) return;
        if (!encode(31, nullptr, data + view.body.offset, view.body.length, view.body.length ? 2 : 0,
                    nullptr, _pong.packet, _pong.length)) return;
        _pong.born = now(); _pong.token = ++_sequence; _pong.kind = 31;
        _pong.waitMs = wait(4);
    } else if (kind == 11 || kind == 13) {
        char name[65]; if (!normalized(data + view.room.offset, view.room.length, name, sizeof name)) return;
        auto* room = findRoom(name); if (!room) return; applyRoomControl(data, length, view, *room);
    } else if (kind == 20 || kind == 21 || kind == 22 || kind == 40) {
        if ((_status.phase != Phase::Online && kind != 40) || view.body_kind != 1) return;
        char name[65]{}; Room* room = nullptr;
        if (view.room.length) {
            if (!normalized(data + view.room.offset, view.room.length, name, sizeof name)) return;
            room = findRoom(name); if (!room || !room->wanted) return;
        } else if (!hub && !view.meta.has_destination) return;
        Receive* pending = nullptr;
        for (auto& item : _receive) {
            if ((item.ticket.valid() || item.proofPending || item.statusPending) && !memcmp(item.file, file, 16)) return;
            if (!item.ticket.valid() && !item.proofPending && !item.statusPending) pending = &item;
        }
        if (!pending) { ++_status.dropped; notice("Busy: some hub messages were not saved"); return; }
        disk::Context scope;
        if (!context(_status.hub, room ? room->view.name : nullptr, view.meta.has_destination ? view.meta.source : nullptr, scope)) return;
        const bool ours = !memcmp(view.meta.source, _d.identity, 16) && room && (kind == 20 || kind == 22);
        Record record; Record::make(record, scope, disk::Kind::Message, data, length, ours ? disk::Status::Confirmed : disk::Status::Received, now());
        const auto submitted = _d.store->requestRrc(store::Operation::RrcAppend, record, file,0,0,
            store::HistoryDirection::Before,view.meta.has_destination ? view.meta.source : nullptr,room?room->observation:_observation);
        if (!submitted.accepted()) { ++_status.dropped; notice("Storage busy: message not saved"); return; }
        *pending = {}; pending->ticket = submitted.ticket; pending->link = handle; pending->confirming = ours;
        pending->room = room ? uint8_t(room - _rooms) : UINT8_MAX;
        memcpy(pending->hash, packetHash, 32); memcpy(pending->file, file, 16);
        if (room) participant(view.meta.source, data + view.nickname.offset, view.nickname.length, pending->room);
        return; // durability before proof or visible receipt
    } else return;
    remember(file); _d.links->proveRrc(handle, packetHash);
}

void RustRrcEngine::alert(const Record& record,const rs_handheld_rrc_view_t& view,uint8_t slot) {
    if(record.status()!=disk::Status::Received || !memcmp(view.meta.source,_d.identity,16) ||
        (view.meta.kind!=20 && view.meta.kind!=21 && view.meta.kind!=22)) return;
    const auto context=record.context();
    if(memcmp(context.local,_d.identity,16) || memcmp(context.hub,_status.hub,16)) return;
    const auto* room=slot<RoomCapacity && !memcmp(_rooms[slot].view.key,context.conversation,16)?&_rooms[slot]:nullptr;
    if(!room && !(view.meta.kind==21 && !view.room.length && view.meta.has_destination &&
        !memcmp(view.meta.destination,_d.identity,16))) return; // roomless hub output stays quiet
    if(room && room->view.notifications==Notifications::Muted) return;
    if(room && room->view.notifications==Notifications::Mentions) {
        uint8_t mentioned=0;
        if((view.meta.kind!=20 && view.meta.kind!=22) || rs_handheld_rrc_mentions(record.payload()+view.text.offset,view.text.length,
            reinterpret_cast<const uint8_t*>(_status.nickname),strlen(_status.nickname),_d.identity,&mentioned)!=RS_HANDHELD_OK || !mentioned) return;
    }
    // Commit-time coalescing never queues a replay burst or changes raw unread.
    if(now()<_nextAlertAt || _status.alertRevision==UINT32_MAX) return;
    _nextAlertAt=now()+5000;++_status.alertRevision;
}

void RustRrcEngine::pollReceive() {
    if (!_d.store) return;
    for (auto& item : _receive) {
        if (item.ticket.valid()) {
            store::Result result; if (!_d.store->peekResult(item.ticket, result)) continue;
            Record record;
            const bool ok = result.outcome == store::Outcome::Committed && result.length <= sizeof record &&
                result.length >= disk::Header && _d.store->readPayload(item.ticket, record.bytes, result.length) && record.valid(result.length);
            _d.store->releaseResult(item.ticket); item.ticket = {};
            if (!ok) { item = {}; ++_status.dropped; notice("Message storage failed"); continue; }
            item.counter = result.key.counter; item.revision = result.revision;
            if (item.confirming && record.status() != disk::Status::Confirmed) item.statusPending = true;
            else {
                item.statusPending = false; item.proofPending = true; remember(item.file);
                if (item.confirming && _send.length && !memcmp(item.file, _send.file, 16)) _send.confirmed = true;
                if (!result.duplicate && item.room < RoomCapacity && !memcmp(record.context().conversation,_rooms[item.room].view.key,16)) { auto& room = _rooms[item.room];
                    if (record.status() == disk::Status::Received && room.view.unread != UINT32_MAX) ++room.view.unread;
                    ++room.view.revision;
                }
                rs_handheld_rrc_view_t view{};
                const bool decoded=rs_handheld_rrc_decode(record.payload(),record.payloadLength(),&view)==RS_HANDHELD_OK;
                if(!result.duplicate && decoded && live(item.link)) alert(record,view,item.room);
                // A durable completion from a lost Link may update history,
                // but cannot apply control observations to its replacement.
                if (!result.duplicate && decoded && live(item.link) &&
                    !memcmp(view.meta.source, _status.identity, 16) && view.body_kind == 1) {
                    text(_status.notice, sizeof _status.notice, record.payload() + view.text.offset, view.text.length);
                    if (view.meta.kind == 21 && !view.room.length && !view.meta.has_destination) {
                        const auto result = _directory.apply(record.payload() + view.text.offset, view.text.length, _status.roomLimit);
                        if (result == Directory::Result::Applied) {
                            _status.directoryPending = false; _status.directoryPartial = _directory.omitted() != 0;
                            _status.directoryKnown = true; _status.directoryStale = _status.directoryFailed = false; _status.directoryCount = _directory.count();
                            notice(_status.directoryPartial ? "Partial channel list; join by name is available" : "Channel list updated");
                        } else if (result == Directory::Result::Invalid && _status.directoryPending) {
                            _status.directoryPending = false; _status.directoryFailed = true; notice("Hub channel list was invalid");
                        }
                    }
                    if (view.meta.kind == 21 && item.room < RoomCapacity && roomStatus(_rooms[item.room], record.payload() + view.text.offset, view.text.length))
                        notice("Channel details updated");
                    if (view.meta.kind == 40) {
                        if (item.room < RoomCapacity) {
                            auto& room = _rooms[item.room];
                            if (room.view.phase == RoomPhase::Joining) {
                                const bool badKey = !strcmp(_status.notice, "bad key (+k)");
                                room.view.phase = badKey ? RoomPhase::NeedsKey : RoomPhase::Error;
                                room.wanted = false;
                                if (_joinRoom == item.room && !_joinConfirmed) {
                                    if (badKey && _joinFromStored) { room.view.keyRemembered = false; _invalidKeyRoom = item.room; }
                                    _joinRoom = UINT8_MAX; _joinPreferenceLength = 0;
                                }
                            }
                        } else if (!strcmp(_status.notice, "banned")) disconnect();
                        else if (_status.phase == Phase::Greeting) recover("Hub rejected the greeting");
                        _status.directoryPending = false;
                    }
                }
                changed();
            }
        }
        if (item.statusPending) {
            disk::Context scope; context(_status.hub, item.room < RoomCapacity ? _rooms[item.room].view.name : nullptr, nullptr, scope);
            Record record; Record::make(record, scope, disk::Kind::Message, nullptr, 0, disk::Status::Confirmed);
            const auto submitted = _d.store->requestRrc(store::Operation::RrcStatus, record, item.file, item.counter);
            if (submitted.accepted()) { item.ticket = submitted.ticket; item.statusPending = false; }
        }
        if (item.proofPending && (!live(item.link) || _d.links->proveRrc(item.link, item.hash))) item = {};
    }
}

void RustRrcEngine::loop() {
    pollReceive(); pollSend(); pollPreferences();
    if (_stopped || !_d.links || _status.phase == Phase::Disconnected) return;
    const auto time = now();
    if (_status.phase == Phase::Recovering && time >= _retryAt) {
        _status.phase = Phase::Finding; _deadline = time + wait(); _pathRequested = false; changed();
    }
    if (_status.phase == Phase::Finding && _preferences.step != PreferenceStep::ReadHub) {
        rs_handheld_route_t route{}; uint8_t key[64], hop = 0, next[16]; int32_t has = 0, hasNext = 0;
        if (rs_handheld_rns_route(_d.ctx, _status.hub, time, &route) == RS_HANDHELD_OK && route.kind == RS_HANDHELD_ROUTE_DIRECT &&
            _d.pump->interfaceOnline(route.interface_id) && rs_handheld_rns_path_info(_d.ctx, _status.hub, time, &has, &hop, next, &hasNext, key) == RS_HANDHELD_OK && has &&
            rs_handheld_rrc_hub_identity(_status.hub, key, _status.identity) == RS_HANDHELD_OK) {
            _interface = route.interface_id;
            _d.links->openRrc(_status.hub, key, route, _link);
            if (_link.valid()) { RustEntropy::fill(_observation,sizeof _observation);_status.phase = Phase::Connecting; _deadline = time + wait(8); changed(); }
        } else if (!_pathRequested) {
            uint8_t tag[16]; RustEntropy::fill(tag, sizeof tag);
            _pathRequested = rs_handheld_rns_request_path(_d.ctx, _status.hub, tag, 0, time) == RS_HANDHELD_OK;
        }
    }
    if (_status.phase == Phase::Connecting && _d.links->rrcActive(_link)) {
        _status.phase = Phase::Identifying; _deadline = time + wait(4); changed();
        _identifyBorn = time;
    }
    if (_status.phase == Phase::Identifying) {
        if (_d.links->rrcIdentified(_link)) {
            static constexpr uint8_t Version[] = "2.2.4";
            if (!_control.length) control(1, nullptr, Version, sizeof Version - 1, 3);
        } else _d.links->identifyRrc(_link, _identifyBorn, uint32_t(_deadline - _identifyBorn));
    }
    auto offer = [&](Control& item) {
        if (item.length && time - item.born >= item.waitMs) {
            if (&item == &_control) { wipeControl(); notice("Command transmission timed out"); }
            else item = {};
            return;
        }
        if (!item.length || item.admitted) return;
        const auto token = item.token;
        if (_d.links->sendRrc(_link, item.packet, item.length, token, item.born, item.waitMs) && item.token == token) item.admitted = true;
    };
    if (_d.links->rrcIdentified(_link)) { offer(_pong); if (!_pong.length) offer(_control); }
    if (_status.phase != Phase::Online && _status.phase != Phase::Recovering && time >= _deadline) { recover("Hub unavailable; reconnecting"); return; }
    if (_status.phase != Phase::Online) return;
    if (_nicknameDirty && !_control.length) {
        static constexpr uint8_t Version[] = "2.2.4";
        control(1, nullptr, Version, sizeof Version - 1, 3);
    }
    if (_failures && time - _onlineAt >= 120000) _failures = 0;
    if (_status.directoryPending && time >= _directoryDeadline) { _status.directoryPending = false; _status.directoryFailed = true; notice("Channel list timed out; join by name is available"); }
    for (auto& room : _rooms) {
        if (!room.used) continue;
        if (room.view.phase == RoomPhase::Recovering && !_control.length && _preferences.step == PreferenceStep::Idle && _joinRoom == UINT8_MAX) {
            disk::Context scope; context(_status.hub, room.view.name, nullptr, scope);
            loadPreferences(PreferenceStep::ReadJoin, scope); _preferences.room = uint8_t(&room - _rooms);
            room.view.phase = RoomPhase::Joining; room.deadline = time + wait(8); changed();
        }
        if ((room.view.phase == RoomPhase::Joining || room.view.phase == RoomPhase::Leaving) && time >= room.deadline) {
            room.view.phase = RoomPhase::Error; notice("Channel request timed out");
            if (_joinRoom == size_t(&room - _rooms) && !_joinConfirmed) { _joinRoom = UINT8_MAX; _joinPreferenceLength = 0; }
        }
    }
    // One optional directory request after each authenticated welcome, behind
    // greeting/nickname traffic, room rejoin and user sends. UI navigation never
    // schedules background refreshes, and an unsupported dialect is not polled.
    bool joining=false;
    for (const auto& room:_rooms) joining |= room.used &&
        (room.view.phase==RoomPhase::Recovering || room.view.phase==RoomPhase::Joining || room.view.phase==RoomPhase::Leaving);
    if (_directoryWanted && !joining && !_control.length && !_send.length && _preferences.step==PreferenceStep::Idle && !_joinConfirmed) {
        Command request;request.action=Action::Directory;request.generation=_status.generation;memcpy(request.hub,_status.hub,16);
        const auto result=command(request,nullptr,0);
        if (result!=Code::Ok && result!=Code::Busy) {
            _directoryWanted=false;_status.directoryFailed=true;notice("Channel list unavailable; join by name is available");
        }
    }
}

#include "RrcPreferences.inc"
