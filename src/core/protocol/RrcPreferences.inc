// Included by RustRrcEngine.cpp. Application metadata uses the same ordered
// MessageStore credits as transcript IO; no metadata task or private queue.
void RustRrcEngine::loadPreferences(PreferenceStep step, const disk::Context& scope) {
    _preferences.step = step; _preferences.write = false;
    _preferences.room = UINT8_MAX;
    Record::make(_preferences.record, scope, disk::Kind::Preferences, nullptr, 0);
}
void RustRrcEngine::preferenceDone(Code result) {
    if (_preferences.step == PreferenceStep::ReadRoot && result != Code::Ok) {
        _preferences.ready = true; _preferences.corrupt = true;
    }
    if (_preferences.revision) {
        _status.preferenceRevision = _preferences.revision;
        _status.preferenceResult = result;
    }
    if (result != Code::Ok) {
        notice(result == Code::Storage ? "Preferences were not saved" : "Channel request failed");
        _joinRoom = UINT8_MAX; _joinConfirmed = false; _joinPreferenceLength = 0;
    }
    wipe(&_preferences.record, sizeof _preferences.record);
    _preferences.step = PreferenceStep::Idle; _preferences.ticket = {}; _preferences.revision = 0;
    _preferences.write = false; changed();
}
void RustRrcEngine::saveRoot(const SavedIndex& index, const char* nickname) {
    disk::Context scope; memcpy(scope.local, _d.identity, 16);
    uint8_t payload[431]; const auto n = encodeIndex(payload, index, nullptr, nickname ? nickname : _status.nickname);
    Record::make(_preferences.record, scope, disk::Kind::Preferences, payload, n);
    _preferences.step = PreferenceStep::WriteRoot; _preferences.write = true;
}
void RustRrcEngine::saveHubRooms() {
    disk::Context scope; context(_status.hub, nullptr, nullptr, scope);
    auto index = _savedRooms;
    if (_joinRoom < RoomCapacity && !index.add(_rooms[_joinRoom].view.key, SavedRoomCapacity)) {
        notice("Saved channel list full; history and key remain stored"); preferenceDone(Code::Full); return;
    }
    uint8_t payload[431]; const auto n = encodeIndex(payload, index, _status.name, nullptr);
    Record::make(_preferences.record, scope, disk::Kind::Preferences, payload, n);
    _preferences.step = PreferenceStep::WriteHub; _preferences.write = true;
}
void RustRrcEngine::joined(Room& room) {
    if (_joinRoom == size_t(&room - _rooms) && _joinPreferenceLength) _joinConfirmed = true;
}
RustRrcEngine::Code RustRrcEngine::startJoin(Room& room, const uint8_t* key, size_t length, bool rememberKey) {
    if (_joinRoom < RoomCapacity && _joinRoom != size_t(&room - _rooms)) return Code::Busy;
    uint8_t sealed[SealedKeyCapacity]; size_t sealedLength = 0;
    if (rememberKey && length) {
        uint8_t entropy[48]; RustEntropy::fill(entropy, sizeof entropy);
        const auto result = rs_handheld_rrc_seal_key(_d.ctx, _status.hub,
            reinterpret_cast<const uint8_t*>(room.view.name), strlen(room.view.name), key, length,
            entropy, sealed, sizeof sealed, &sealedLength);
        wipe(entropy, sizeof entropy);
        if (result != RS_HANDHELD_OK) return Code::Invalid;
    }
    const auto result = control(10, room.view.name, key, length, length ? 1 : 0);
    if (result != Code::Ok) return result;
    _joinPreferenceLength = uint16_t(roomPreference(_joinPreference, room.view.name, room.view.muted, sealed, sealedLength));
    _joinRoom = uint8_t(&room - _rooms); _joinConfirmed = false; _control.room = _joinRoom;
    _joinFromStored = false;
    room.wanted = true; room.view.needsKey = length != 0; room.view.phase = RoomPhase::Joining;
    room.deadline = now() + wait(8); changed(); return Code::Ok;
}
RustRrcEngine::Code RustRrcEngine::preferenceCommand(const Command& command, const uint8_t* body, size_t length) {
    if (!_preferences.ready || _preferences.step != PreferenceStep::Idle || _joinConfirmed) return Code::Busy;
    if (_preferences.corrupt) return Code::Storage;
    _preferences.action = command.action; _preferences.revision = command.revision;
    switch (command.action) {
    case Action::SaveHub: case Action::ForgetHub: {
        if (!nonzero(command.hub)) return Code::Invalid;
        auto candidate = _bookmarks;
        if (command.action == Action::SaveHub) {
            if (!candidate.add(command.hub, HubCapacity)) return Code::Full;
        } else candidate.remove(command.hub);
        saveRoot(candidate); return Code::Ok;
    }
    case Action::Nickname: {
        char nickname[33];
        if (!normalized(body, length, nickname, sizeof nickname, true) || strlen(nickname) > _status.nicknameLimit) return Code::Invalid;
        if (_control.length) return Code::Busy;
        saveRoot(_bookmarks, nickname); return Code::Ok;
    }
    case Action::Mute: {
        auto* room = findRoom(command.room); if (!room) return Code::NotJoined;
        disk::Context scope; context(_status.hub, room->view.name, nullptr, scope);
        loadPreferences(PreferenceStep::ReadMute, scope);
        _preferences.room = uint8_t(room - _rooms);
        // Desired mute state is the request's one-bit value, separate from the
        // published preference until the replacement commits.
        _preferences.record.bytes[6] = command.flags ? 1 : 0; _preferences.record.seal();
        return Code::Ok;
    }
    case Action::Draft: {
        if (length > DraftCapacity) return Code::TooLong;
        disk::Context scope;
        if (!context(command.hub, command.room, nonzero(command.participant) ? command.participant : nullptr, scope)) return Code::Invalid;
        uint8_t payload[DraftCapacity + 8]{}; memcpy(payload, "HRD1", 4);
        store::prepared::write32(payload + 4, command.revision);
        if (length) memcpy(payload + 8, body, length);
        Record::make(_preferences.record, scope, disk::Kind::Draft, payload, 8 + length);
        _preferences.step = PreferenceStep::WriteDraft; _preferences.write = true; return Code::Ok;
    }
    case Action::MarkRead: case Action::ClearHistory: {
        disk::Context scope;
        if (!context(command.hub, command.room, nonzero(command.participant) ? command.participant : nullptr, scope)) return Code::Invalid;
        if (command.action == Action::ClearHistory && _send.length && !memcmp(scope.conversation, _send.conversation, 16)) return Code::Busy;
        Record::make(_preferences.record, scope, disk::Kind::Message, nullptr, 0);
        _preferences.record.counter(command.revision); _preferences.record.seal();
        _preferences.step = command.action == Action::MarkRead ? PreferenceStep::MarkRead : PreferenceStep::ClearHistory;
        _preferences.write = false;
        const auto* room = findRoom(command.room); _preferences.room = room ? uint8_t(room - _rooms) : UINT8_MAX;
        return Code::Ok;
    }
    default: return Code::Invalid;
    }
}

void RustRrcEngine::pollPreferences() {
    if (!_d.store) return;
    if (_preferences.ticket.valid()) {
        store::Result result;
        if (!_d.store->peekResult(_preferences.ticket, result)) return;
        const auto step = _preferences.step;
        const auto originalContext = _preferences.record.context();
        const bool wantedMute = _preferences.record.flags() != 0;
        bool good = result.outcome == store::Outcome::Committed;
        if (good && result.length && step != PreferenceStep::ReadUnread) good = result.length >= disk::Header && result.length <= sizeof(Record) &&
            _d.store->readPayload(_preferences.ticket, _preferences.record.bytes, result.length) &&
            _preferences.record.valid(result.length) && _preferences.record.matches(originalContext);
        _d.store->releaseResult(_preferences.ticket); _preferences.ticket = {};
        if (!good) {
            if (step == PreferenceStep::ReadRoot) { _preferences.ready = true; _preferences.corrupt = true; }
            if (step == PreferenceStep::ReadJoin && _preferences.room < RoomCapacity) _rooms[_preferences.room].view.phase = RoomPhase::Error;
            preferenceDone(Code::Storage); return;
        }
        if (step == PreferenceStep::ReadUnread) {
            if (_preferences.room < RoomCapacity) {
                auto& room = _rooms[_preferences.room];
                if (room.view.revision != _preferences.roomRevision) return; // retry a superseded count
                room.view.unread = uint32_t(std::max(int32_t(0), result.unreadDelta)); room.view.unreadKnown = true;
            }
            preferenceDone(Code::Ok);
        } else if (step == PreferenceStep::MarkRead || step == PreferenceStep::ClearHistory) {
            if (_preferences.room < RoomCapacity) {
                ++_rooms[_preferences.room].view.revision;
                Record::make(_preferences.record, originalContext, disk::Kind::Message, nullptr, 0);
                _preferences.step = PreferenceStep::ReadUnread; _preferences.write = false;
            } else preferenceDone(Code::Ok);
        } else if (step == PreferenceStep::ReadRoot || step == PreferenceStep::WriteRoot) {
            SavedIndex index; char nickname[33]{};
            if (result.length && !decodeIndex(_preferences.record.payload(), _preferences.record.payloadLength(), index, nullptr, nickname)) {
                _preferences.ready = true; _preferences.corrupt = true; preferenceDone(Code::Storage); return;
            }
            _bookmarks = index; _preferences.ready = true;
            if (nickname[0]) {
                char checked[33];
                if (normalized(reinterpret_cast<const uint8_t*>(nickname), strlen(nickname), checked, sizeof checked, true)) {
                    _nicknameDirty = strcmp(_status.nickname, checked) != 0;
                    strcpy(_status.nickname, checked);
                }
            }
            for (auto& hub : _hubs) if (hub.used) hub.view.saved = _bookmarks.find(hub.view.address) >= 0;
            preferenceDone(Code::Ok);
        } else if (step == PreferenceStep::ReadHub || step == PreferenceStep::WriteHub) {
            SavedIndex index;
            if (result.length && !decodeIndex(_preferences.record.payload(), _preferences.record.payloadLength(), index, nullptr, nullptr)) { preferenceDone(Code::Storage); return; }
            _savedRooms = index;
            _joinRoom = UINT8_MAX; _joinConfirmed = false; _joinPreferenceLength = 0;
            if (step == PreferenceStep::WriteHub && _preferences.room < RoomCapacity) {
                disk::Context scope; context(_status.hub, _rooms[_preferences.room].view.name, nullptr, scope);
                Record::make(_preferences.record, scope, disk::Kind::Message, nullptr, 0);
                _preferences.step = PreferenceStep::ReadUnread; _preferences.write = false;
            } else preferenceDone(Code::Ok);
        } else if (step == PreferenceStep::ReadJoin || step == PreferenceStep::ReadMute || step == PreferenceStep::ReadForgetKey) {
            if (_preferences.room >= RoomCapacity) { preferenceDone(Code::Stale); return; }
            auto& room = _rooms[_preferences.room];
            const auto* payload = _preferences.record.payload(); const auto length = _preferences.record.payloadLength();
            if (result.length && (!validRoomPreference(payload, length) || strcmp(reinterpret_cast<const char*>(payload + 7), room.view.name))) {
                preferenceDone(Code::Storage); return;
            }
            if (step == PreferenceStep::ReadMute || step == PreferenceStep::ReadForgetKey) {
                uint8_t replacement[431];
                const bool forgetting = step == PreferenceStep::ReadForgetKey;
                const auto size = roomPreference(replacement, room.view.name, forgetting ? room.view.muted : wantedMute,
                    result.length && !forgetting ? payload + RoomPreferenceHeader : nullptr,
                    result.length && !forgetting ? length - RoomPreferenceHeader : 0);
                Record::make(_preferences.record, originalContext, disk::Kind::Preferences, replacement, size);
                _preferences.step = PreferenceStep::WriteRoom; _preferences.write = true;
            } else {
                if (_stopped || _status.phase != Phase::Online) { preferenceDone(Code::Offline); return; }
                uint8_t key[KeyCapacity]{}; size_t keyLength = 0;
                const size_t sealedLength = result.length ? length - RoomPreferenceHeader : 0;
                if (sealedLength && rs_handheld_rrc_unseal_key(_d.ctx, _status.hub,
                    reinterpret_cast<const uint8_t*>(room.view.name), strlen(room.view.name), payload + RoomPreferenceHeader,
                    sealedLength, key, sizeof key, &keyLength) != RS_HANDHELD_OK) {
                    room.view.phase = RoomPhase::NeedsKey; room.view.keyRemembered = false;
                    preferenceDone(Code::Storage); return;
                }
                if (result.length) room.view.muted = payload[4] != 0;
                room.view.keyRemembered = sealedLength != 0;
                const auto admitted = startJoin(room, key, keyLength, sealedLength != 0);
                _joinFromStored = admitted == Code::Ok && sealedLength != 0;
                wipe(key, sizeof key); preferenceDone(admitted);
            }
        } else if (step == PreferenceStep::WriteRoom) {
            if (_preferences.room < RoomCapacity) {
                auto& room = _rooms[_preferences.room];
                room.view.muted = _preferences.record.payload()[4] != 0;
                room.view.keyRemembered = _preferences.record.payloadLength() > RoomPreferenceHeader;
            }
            if (_joinConfirmed) saveHubRooms(); else preferenceDone(Code::Ok);
        } else preferenceDone(Code::Ok);
    }
    if (_preferences.step == PreferenceStep::Idle && _joinConfirmed && _joinRoom < RoomCapacity) {
        const auto& room = _rooms[_joinRoom]; disk::Context scope;
        context(_status.hub, room.view.name, nullptr, scope);
        Record::make(_preferences.record, scope, disk::Kind::Preferences, _joinPreference, _joinPreferenceLength);
        _preferences.step = PreferenceStep::WriteRoom; _preferences.write = true; _preferences.room = _joinRoom;
    }
    if (_preferences.step == PreferenceStep::Idle && _invalidKeyRoom < RoomCapacity) {
        const auto slot = _invalidKeyRoom; _invalidKeyRoom = UINT8_MAX;
        disk::Context scope; context(_status.hub, _rooms[slot].view.name, nullptr, scope);
        loadPreferences(PreferenceStep::ReadForgetKey, scope); _preferences.room = slot;
    }
    if (_preferences.step != PreferenceStep::Idle && !_preferences.ticket.valid()) {
        auto operation = _preferences.write ? store::Operation::RrcWrite : store::Operation::RrcRead;
        if (_preferences.step == PreferenceStep::ReadUnread) {
            operation = store::Operation::RrcPage;
            if (_preferences.room < RoomCapacity) _preferences.roomRevision = _rooms[_preferences.room].view.revision;
        } else if (_preferences.step == PreferenceStep::MarkRead) operation = store::Operation::RrcMarkRead;
        else if (_preferences.step == PreferenceStep::ClearHistory) operation = store::Operation::RrcClear;
        const auto submitted = _d.store->requestRrc(operation, _preferences.record, nullptr,
            operation == store::Operation::RrcMarkRead ? _preferences.record.counter() : 0);
        if (submitted.accepted()) _preferences.ticket = submitted.ticket;
        else if (submitted.rejection != store::Rejection::Busy) preferenceDone(Code::Storage);
    }
}
