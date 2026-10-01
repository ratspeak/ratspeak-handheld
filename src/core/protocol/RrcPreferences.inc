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
        _status.preferenceRecordRevision = result == Code::Ok ? _preferences.record.revision() : 0;
    }
    if (result != Code::Ok && !(_preferences.action == Action::Draft && (_preferences.record.flags() & 1) && result == Code::Stale)) {
        notice(result == Code::Storage ? "Preferences were not saved" : "Channel request failed");
    }
    if (result != Code::Ok && (_preferences.step == PreferenceStep::ReadJoin || _preferences.step == PreferenceStep::ReadJoinedPolicy ||
        _preferences.step == PreferenceStep::WriteRoom || _preferences.step == PreferenceStep::WriteHub)) {
        _joinRoom = UINT8_MAX; _joinConfirmed = false; _joinPreferenceLength = 0;
    }
    wipe(&_preferences.record, sizeof _preferences.record);
    memset(_preferences.participant,0,sizeof _preferences.participant);
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
    _joinPreferenceLength = uint16_t(roomPreference(_joinPreference, room.view.name, uint8_t(room.view.notifications), sealed, sealedLength));
    _joinRoom = uint8_t(&room - _rooms); _joinConfirmed = false; _control.room = _joinRoom;
    _joinFromStored = false;
    RustEntropy::fill(room.observation,sizeof room.observation);room.observationPending=true;
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
    case Action::Mute: case Action::ForgetKey: case Action::ForgetRoom: {
        char name[65]{}; disk::Context scope;
        if (nonzero(command.participant) || !normalized(reinterpret_cast<const uint8_t*>(command.room),
            strnlen(command.room,65),name,sizeof name) || !name[0] || strcmp(name,command.room) ||
            !context(command.hub,name,nullptr,scope) || (command.action==Action::Mute && command.flags>2)) return Code::Invalid;
        // A pending JOIN owns the shared room-preference replacement. Keep
        // policy/forget writes separate until its confirmation or failure.
        if (_joinRoom<RoomCapacity) return Code::Busy;
        if (command.action==Action::ForgetRoom) {
            memcpy(_preferences.participant,scope.conversation,16);
            memset(scope.conversation,0,16);loadPreferences(PreferenceStep::ReadForgetRoom,scope);
        } else {
            loadPreferences(command.action==Action::Mute?PreferenceStep::ReadMute:PreferenceStep::ReadForgetKey,scope);
            // Retain the request name/policy in the existing record until its
            // read completes. No room slot or network session is required.
            uint8_t payload[RoomPreferenceHeader];
            const auto size=roomPreference(payload,name,command.action==Action::Mute?command.flags:0,nullptr,0);
            Record::make(_preferences.record,scope,disk::Kind::Preferences,payload,size);
            if (!memcmp(command.hub,_status.hub,16)) {
                const auto* room=findRoom(name);if(room) _preferences.room=uint8_t(room-_rooms);
            }
        }
        return Code::Ok;
    }
    case Action::Draft: {
        if (command.room[0] && nonzero(command.participant)) return Code::Invalid;
        if (length > DraftCapacity) return Code::TooLong;
        if (!validDraftText(body,length)) return Code::Invalid;
        if (command.flags>3 || ((command.flags&1) && !command.counter) ||
            ((command.flags&2) && (!(command.flags&1) || !length))) return Code::Invalid;
        disk::Context scope;
        if (!context(command.hub, command.room, nonzero(command.participant) ? command.participant : nullptr, scope)) return Code::Invalid;
        memcpy(_preferences.participant,command.participant,16);
        uint8_t payload[DraftCapacity + 8]{}; memcpy(payload, "HRD1", 4);
        store::prepared::write32(payload + 4, command.revision);
        if (length) memcpy(payload + 8, body, length);
        // Persist uncertainty before the UI can submit a message. A crash in the
        // gap may conservatively warn about an unsent draft, but never silently
        // offers a possibly transmitted body as a fresh send after restart.
        Record::make(_preferences.record, scope, disk::Kind::Draft, payload, 8 + length,
            command.flags&2 ? disk::Status::Unconfirmed : disk::Status::Received);
        if (command.flags & 1) { _preferences.record.flags(1); _preferences.record.revision(command.counter); _preferences.record.seal(); }
        _preferences.step = PreferenceStep::WriteDraft; _preferences.write = true; return Code::Ok;
    }
    case Action::MarkRead: case Action::ClearHistory: {
        disk::Context scope;
        if (!context(command.hub, command.room, nonzero(command.participant) ? command.participant : nullptr, scope)) return Code::Invalid;
        if (command.action == Action::ClearHistory && _send.length && !memcmp(scope.conversation, _send.conversation, 16)) return Code::Busy;
        Record::make(_preferences.record, scope, disk::Kind::Message, nullptr, 0);
        _preferences.record.counter(command.counter); _preferences.record.seal();
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
        const auto wantedNotifications=_preferences.record.payload()[4];
        char requestedRoom[65]{};
        if ((step==PreferenceStep::ReadMute || step==PreferenceStep::ReadForgetKey) &&
            _preferences.record.payloadLength()>=RoomPreferenceHeader)
            memcpy(requestedRoom,_preferences.record.payload()+7,sizeof requestedRoom);
        bool good = result.outcome == store::Outcome::Committed;
        if (good && result.length && step != PreferenceStep::ReadUnread) good = result.length >= disk::Header && result.length <= sizeof(Record) &&
            _d.store->readPayload(_preferences.ticket, _preferences.record.bytes, result.length) &&
            _preferences.record.valid(result.length) && _preferences.record.matches(originalContext);
        _d.store->releaseResult(_preferences.ticket); _preferences.ticket = {};
        if (!good) {
            if(step==PreferenceStep::Observe && _preferences.room<RoomCapacity &&
                !memcmp(_rooms[_preferences.room].observation,_preferences.participant,16))
                _rooms[_preferences.room].observationPending=false;
            if (step == PreferenceStep::ReadRoot) { _preferences.ready = true; _preferences.corrupt = true; }
            if (step == PreferenceStep::ReadJoin && _preferences.room < RoomCapacity) _rooms[_preferences.room].view.phase = RoomPhase::Error;
            preferenceDone(result.error == store::Error::Stale ? Code::Stale : Code::Storage); return;
        }
        if (step == PreferenceStep::Observe) {
            if(_preferences.room<RoomCapacity) {
                auto& room=_rooms[_preferences.room];
                if(!memcmp(room.observation,_preferences.participant,16) &&
                    !memcmp(room.view.key,originalContext.conversation,16) && !memcmp(_status.hub,originalContext.hub,16)) {
                    room.observationPending=false;++room.view.revision;
                }
            }
            preferenceDone(Code::Ok);
        } else if (step == PreferenceStep::ReadUnread) {
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
        } else if(step==PreferenceStep::ReadJoinedPolicy) {
            if(!_joinConfirmed || _joinRoom>=RoomCapacity || _preferences.room!=_joinRoom) {preferenceDone(Code::Stale);return;}
            const auto* payload=_preferences.record.payload();
            if(result.length && (!validRoomPreference(payload,_preferences.record.payloadLength()) ||
                strcmp(reinterpret_cast<const char*>(payload+7),_rooms[_joinRoom].view.name))) {preferenceDone(Code::Storage);return;}
            // Supplying a replacement key after restart must not reset an
            // existing notification policy to the fresh live-room default.
            if(result.length) _joinPreference[4]=payload[4];
            Record::make(_preferences.record,originalContext,disk::Kind::Preferences,_joinPreference,_joinPreferenceLength);
            _preferences.step=PreferenceStep::WriteRoom;_preferences.write=true;
        } else if (step==PreferenceStep::ReadForgetRoom || step==PreferenceStep::WriteForgetRoom) {
            SavedIndex index;char name[33]{},nickname[33]{};
            if(result.length && !decodeIndex(_preferences.record.payload(),_preferences.record.payloadLength(),index,name,nickname)) {
                preferenceDone(Code::Storage);return;
            }
            if(step==PreferenceStep::ReadForgetRoom) {
                index.remove(_preferences.participant);uint8_t payload[431];
                const auto length=encodeIndex(payload,index,name,nickname);
                Record::make(_preferences.record,originalContext,disk::Kind::Preferences,payload,length);
                _preferences.step=PreferenceStep::WriteForgetRoom;_preferences.write=true;
            } else {
                if(!memcmp(originalContext.hub,_status.hub,16)) _savedRooms=index;
                preferenceDone(Code::Ok);
            }
        } else if (step == PreferenceStep::ReadJoin || step == PreferenceStep::ReadMute || step == PreferenceStep::ReadForgetKey) {
            auto* item=_preferences.room<RoomCapacity ? &_rooms[_preferences.room] : nullptr;
            if(step==PreferenceStep::ReadJoin && !item) {preferenceDone(Code::Stale);return;}
            const char* name=step==PreferenceStep::ReadJoin ? item->view.name : requestedRoom;
            const auto* payload = _preferences.record.payload(); const auto length = _preferences.record.payloadLength();
            if (!name[0] || (result.length && (!validRoomPreference(payload, length) || strcmp(reinterpret_cast<const char*>(payload + 7), name)))) {
                preferenceDone(Code::Storage); return;
            }
            if (step == PreferenceStep::ReadMute || step == PreferenceStep::ReadForgetKey) {
                uint8_t replacement[431];
                const bool forgetting = step == PreferenceStep::ReadForgetKey;
                const auto size = roomPreference(replacement, name, forgetting ? (result.length?payload[4]:0) : wantedNotifications,
                    result.length && !forgetting ? payload + RoomPreferenceHeader : nullptr,
                    result.length && !forgetting ? length - RoomPreferenceHeader : 0);
                Record::make(_preferences.record, originalContext, disk::Kind::Preferences, replacement, size);
                _preferences.step = PreferenceStep::WriteRoom; _preferences.write = true;
            } else {
                auto& room=*item;
                if (_stopped || _status.phase != Phase::Online) { preferenceDone(Code::Offline); return; }
                uint8_t key[KeyCapacity]{}; size_t keyLength = 0;
                const size_t sealedLength = result.length ? length - RoomPreferenceHeader : 0;
                if (sealedLength && rs_handheld_rrc_unseal_key(_d.ctx, _status.hub,
                    reinterpret_cast<const uint8_t*>(room.view.name), strlen(room.view.name), payload + RoomPreferenceHeader,
                    sealedLength, key, sizeof key, &keyLength) != RS_HANDHELD_OK) {
                    room.view.phase = RoomPhase::NeedsKey; room.view.keyRemembered = false;
                    preferenceDone(Code::Storage); return;
                }
                if (result.length) {room.view.notifications=Notifications(payload[4]);room.view.muted=room.view.notifications==Notifications::Muted;}
                room.view.keyRemembered = sealedLength != 0;
                const auto admitted = startJoin(room, key, keyLength, sealedLength != 0);
                _joinFromStored = admitted == Code::Ok && sealedLength != 0;
                wipe(key, sizeof key); preferenceDone(admitted);
            }
        } else if (step == PreferenceStep::WriteRoom) {
            if (_preferences.room < RoomCapacity) {
                auto& room = _rooms[_preferences.room];
                room.view.notifications=Notifications(_preferences.record.payload()[4]);room.view.muted=room.view.notifications==Notifications::Muted;
                room.view.keyRemembered = _preferences.record.payloadLength() > RoomPreferenceHeader;
            }
            if (_joinConfirmed) saveHubRooms(); else preferenceDone(Code::Ok);
        } else preferenceDone(Code::Ok);
    }
    if (_preferences.step == PreferenceStep::Idle && _joinConfirmed && _joinRoom < RoomCapacity) {
        const auto& room = _rooms[_joinRoom]; disk::Context scope;
        context(_status.hub, room.view.name, nullptr, scope);
        loadPreferences(PreferenceStep::ReadJoinedPolicy,scope);_preferences.room=_joinRoom;
    }
    if (_preferences.step == PreferenceStep::Idle && _invalidKeyRoom < RoomCapacity) {
        const auto slot = _invalidKeyRoom; _invalidKeyRoom = UINT8_MAX;
        disk::Context scope; context(_status.hub, _rooms[slot].view.name, nullptr, scope);
        loadPreferences(PreferenceStep::ReadForgetKey, scope); _preferences.room = slot;
        uint8_t payload[RoomPreferenceHeader];
        const auto size=roomPreference(payload,_rooms[slot].view.name,0,nullptr,0);
        Record::make(_preferences.record,scope,disk::Kind::Preferences,payload,size);
    }
    if(!_stopped && _status.phase==Phase::Online && _preferences.step==PreferenceStep::Idle) {
        for(auto& room:_rooms) if(room.used && room.view.phase==RoomPhase::Joined && room.observationPending) {
            disk::Context scope;context(_status.hub,room.view.name,nullptr,scope);
            Record::make(_preferences.record,scope,disk::Kind::Observation,nullptr,0);
            _preferences.step=PreferenceStep::Observe;_preferences.room=uint8_t(&room-_rooms);
            memcpy(_preferences.participant,room.observation,16);break;
        }
    }
    if (_preferences.step != PreferenceStep::Idle && !_preferences.ticket.valid()) {
        auto operation = _preferences.write ? store::Operation::RrcWrite : store::Operation::RrcRead;
        if (_preferences.step == PreferenceStep::ReadUnread) {
            operation = store::Operation::RrcPage;
            if (_preferences.room < RoomCapacity) _preferences.roomRevision = _rooms[_preferences.room].view.revision;
        } else if (_preferences.step == PreferenceStep::MarkRead) operation = store::Operation::RrcMarkRead;
        else if (_preferences.step == PreferenceStep::ClearHistory) operation = store::Operation::RrcClear;
        else if (_preferences.step == PreferenceStep::Observe) operation = store::Operation::RrcObserve;
        const auto submitted = _d.store->requestRrc(operation, _preferences.record, nullptr,
            operation == store::Operation::RrcMarkRead ? _preferences.record.counter() : 0,
            _preferences.step == PreferenceStep::WriteDraft && (_preferences.record.flags() & 1) ? _preferences.record.revision() : 0,
            store::HistoryDirection::Before,_preferences.step==PreferenceStep::WriteDraft && nonzero(_preferences.participant) ? _preferences.participant : nullptr,
            _preferences.step==PreferenceStep::Observe?_preferences.participant:nullptr);
        if (submitted.accepted()) _preferences.ticket = submitted.ticket;
        else if (submitted.rejection != store::Rejection::Busy) preferenceDone(Code::Storage);
    }
}
