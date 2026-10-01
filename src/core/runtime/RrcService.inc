// Included by DeviceService.cpp inside handheld. RRC shares the existing
// mailbox payload arena, serialized reader, storage worker and query ticket.
namespace {
bool rrcPreferenceAction(rrc::Action action) {
    return action == rrc::Action::SaveHub || action == rrc::Action::ForgetHub || action == rrc::Action::Nickname ||
        action == rrc::Action::ForgetRoom || action == rrc::Action::ForgetKey || action == rrc::Action::Mute || action == rrc::Action::Draft || action == rrc::Action::MarkRead || action == rrc::Action::ClearHistory;
}
bool rrcMessageAction(rrc::Action action) {
    return action == rrc::Action::Message || action == rrc::Action::Emote || action == rrc::Action::PrivateNotice;
}
}
void DeviceService::pollRrcCommands() {
    if (!_rrcCommands) return;
    const auto status = _backend.rrcStatus();
    for (uint8_t slot = 0; slot < ServiceMailbox::NormalSlots; ++slot) {
        if (!(_rrcCommands & (uint16_t(1) << slot))) continue;
        rrc::Command command; const auto& request = _mailbox.request(slot);
        if (!_mailbox.read(slot, &command, sizeof command)) continue; // Keep ownership on invariant failure.
        rrc::Code code;
        if (rrcPreferenceAction(command.action)) {
            if (status.preferenceRevision != request.id) continue;
            code = status.preferenceResult;
        } else {
            if (status.sendRevision != request.id || (!status.sendSaved && !status.sendSettled)) continue;
            code = status.sendSaved ? rrc::Code::Ok : rrc::Code::Storage;
        }
        Result result; result.outcome = code == rrc::Code::Ok ? Outcome::Ok : Outcome::Failed;
        if (rrcPreferenceAction(command.action)) result.total = status.preferenceRecordRevision;
        result.next = uint32_t(code); result.revision = request.id;
        strlcpy(result.detail, rrc::codeName(code), sizeof result.detail);
        _rrcCommands &= ~(uint16_t(1) << slot); _mailbox.complete(slot, result);
    }
}
bool DeviceService::readRrcContext(uint8_t slot, storage::rrc::Context& context) const {
    rrc::Conversation binding;
    if (_mailbox.length(slot) != sizeof binding || !_mailbox.read(slot, &binding, sizeof binding) ||
        binding.room[64] || (binding.room[0] && binding.privateNotice())) return false;
    return _backend.rrcContext(binding.hub, binding.room, binding.privateNotice() ? binding.participant : nullptr, context);
}

void DeviceService::executeRrc(uint8_t slot) {
    const auto& request = _mailbox.request(slot);
    const auto status = _backend.rrcStatus();
    Result result; result.outcome = Outcome::Ok; result.revision = status.generation;
    auto publish = [&](const void* rows, size_t count, size_t width) {
        if (count * width > _mailbox.capacity(slot) || !_mailbox.write(slot, rows, count * width)) {
            complete(slot, Outcome::Invalid); return;
        }
        result.total = count; result.length = count * width; _mailbox.complete(slot, result);
    };
    if (request.operation == Operation::RrcCommand) {
        rrc::Command command;
        const auto length = _mailbox.length(slot);
        if (length < sizeof command || length > sizeof command + rrc::PacketCapacity) { complete(slot, Outcome::Invalid); return; }
        memcpy(&command, _scratch, sizeof command);
        command.revision = request.id; // Unique for this mailbox lifetime, independent of draft edits/read counter.
        const bool valid = !command.room[64] && command.action <= rrc::Action::ForgetKey && command.flags <= (command.action==rrc::Action::Draft?3:command.action==rrc::Action::Mute?2:1);
        const auto code = valid ? _backend.rrcCommand(command,
            reinterpret_cast<const uint8_t*>(_scratch) + sizeof command, length - sizeof command) : rrc::Code::Invalid;
        volatile char* clear = _scratch; for (size_t n = 0; n < length; ++n) clear[n] = 0;
        if (code == rrc::Code::Ok && (rrcPreferenceAction(command.action) || rrcMessageAction(command.action))) {
            _rrcCommands |= uint16_t(1) << slot;
            pollRrcCommands(); return; // Result owns durable completion, not merely queue admission.
        }
        result.outcome = code == rrc::Code::Ok ? Outcome::Ok : code == rrc::Code::Stale ? Outcome::Stale : Outcome::Failed;
        result.next = uint32_t(code); result.revision = command.revision;
        strlcpy(result.detail, rrc::codeName(code), sizeof result.detail);
        _mailbox.complete(slot, result); return; // admission, never a delivery receipt
    }
    if (request.operation == Operation::RrcContext) {
        storage::rrc::Context context;
        if (!readRrcContext(slot, context)) { complete(slot, Outcome::Invalid); return; }
        publish(context.conversation, 1, sizeof context.conversation); return;
    }
    if (request.operation != Operation::RrcHubs && request.revision != status.generation) {
        complete(slot, Outcome::Stale); return;
    }
    if (request.operation == Operation::RrcHubs) {
        rrc::HubView rows[rrc::HubViewCapacity]{};
        const size_t count = _backend.rrcHubs(rows, rrc::HubViewCapacity);
        if (count > rrc::HubViewCapacity) { complete(slot, Outcome::Invalid); return; }
        publish(rows, count, sizeof rows[0]);
    } else if (request.operation == Operation::RrcRooms) {
        rrc::RoomView rows[rrc::RoomCapacity]{};
        const size_t count = _backend.rrcRooms(rows, rrc::RoomCapacity);
        if (count > rrc::RoomCapacity) { complete(slot, Outcome::Invalid); return; }
        publish(rows, count, sizeof rows[0]);
    } else if (request.operation == Operation::RrcPeople) {
        char room[65]{};
        if (_mailbox.length(slot) >= sizeof room || !_mailbox.read(slot, room, _mailbox.length(slot))) { complete(slot, Outcome::Invalid); return; }
        rrc::PersonView rows[rrc::PeopleCapacity]{};
        const size_t count = _backend.rrcPeople(room, rows, rrc::PeopleCapacity);
        if (count > rrc::PeopleCapacity) { complete(slot, Outcome::Invalid); return; }
        publish(rows, count, sizeof rows[0]);
    } else if (request.operation == Operation::RrcDirectory) {
        rrc::DirectoryView rows[8]{};
        const size_t count = _backend.rrcDirectory(rows, 8, request.offset);
        if (count > 8) { complete(slot, Outcome::Invalid); return; }
        rrc::DirectoryView following;
        result.more = _backend.rrcDirectory(&following, 1, request.offset + count) != 0;
        result.next = request.offset + count;
        publish(rows, count, sizeof rows[0]);
    } else complete(slot, Outcome::Invalid);
}

storage::Submission DeviceService::submitRrcRead(uint8_t slot) {
    storage::rrc::Context context;
    if (!readRrcContext(slot, context)) return {{}, storage::Rejection::Invalid};
    const auto& request = _mailbox.request(slot);
    if(request.operation==Operation::RrcInbox) {
        for(auto byte:context.conversation) if(byte) return {{},storage::Rejection::Invalid};
        if(request.revision!=_backend.rrcStatus().generation) return {{},storage::Rejection::Fenced};
        storage::rrc::Record record;
        storage::rrc::Record::make(record,context,storage::rrc::Kind::Preferences,reinterpret_cast<const uint8_t*>("HQN1"),4);
        return _messages.requestRrc(storage::Operation::RrcPrivateInbox,record,nullptr,request.offset,0,request.historyDirection);
    }
    if (rrcCatalog(request.operation)) {
        for (auto byte:context.conversation) if (byte) return {{},storage::Rejection::Invalid};
        const auto status=_backend.rrcStatus();
        if (request.revision!=status.generation || (request.operation==Operation::RrcChannels && request.statusRevision!=status.revision))
            return {{},storage::Rejection::Fenced};
        storage::rrc::Record record;uint32_t cursor=0;
        if (!history::rrc_catalog::prepare(_backend,context.hub,request.operation==Operation::RrcChannels,request.offset,record,cursor))
            return {{},storage::Rejection::Invalid};
        return _messages.requestRrc(storage::Operation::RrcSavedRooms,record,nullptr,cursor);
    }
    if (request.operation == Operation::RrcDraft || request.operation==Operation::RrcRoom) {
        storage::rrc::Record record;
        storage::rrc::Record::make(record, context, request.operation==Operation::RrcRoom?storage::rrc::Kind::Preferences:storage::rrc::Kind::Draft, nullptr, 0);
        return _messages.requestRrc(storage::Operation::RrcRead, record);
    }
    return history::rrc::submit(_messages, context, rrcHistoryQuery(request, context));
}

Result DeviceService::projectRrcRead(const storage::Result& stored) {
    const auto slot = _querySlot; const auto& request = _mailbox.request(slot);
    Result result; result.storageError = stored.error; result.outcome = Outcome::Failed;
    storage::rrc::Context context;
    if (!readRrcContext(slot, context) || stored.length > storage::Budget::SmallPayload ||
        !_messages.readPayload(_queryTicket, _scratch, stored.length)) {
        result.storageError = storage::Error::InvalidRecord; return result;
    }
    if (stored.outcome != storage::Outcome::Committed) return result;
    auto invalid = [&]() { result.storageError = storage::Error::InvalidRecord; return result; };
    if(request.operation==Operation::RrcInbox) {
        if(request.revision!=_backend.rrcStatus().generation) {result.outcome=Outcome::Stale;result.storageError=storage::Error::Stale;return result;}
        rrc::PrivateView rows[history::rrc_catalog::Capacity];size_t count=0;
        if(!history::rrc_catalog::privateRows(stored,reinterpret_cast<const uint8_t*>(_scratch),rows,count) ||
            !_mailbox.write(slot,rows,count*sizeof rows[0])) return invalid();
        result.length=count*sizeof rows[0];result.more=stored.more;result.next=stored.key.counter;result.total=stored.total;
        result.outcome=Outcome::Ok;return result;
    }
    if (rrcCatalog(request.operation)) {
        const auto status=_backend.rrcStatus();
        if (request.revision!=status.generation || (request.operation==Operation::RrcChannels && request.statusRevision!=status.revision)) {
            result.outcome=Outcome::Stale;result.storageError=storage::Error::Stale;return result;
        }
        rrc::RoomView rows[history::rrc_catalog::Capacity];size_t count=0;bool more=false;
        if (!history::rrc_catalog::project(_backend,context.hub,request.operation==Operation::RrcChannels,request.offset,
            stored,reinterpret_cast<const uint8_t*>(_scratch),rows,count,more) || !_mailbox.write(slot,rows,count*sizeof rows[0])) return invalid();
        result.length=count*sizeof rows[0];result.more=more;result.next=request.offset+count;
        result.total=count;result.outcome=Outcome::Ok;return result;
    }
    if(request.operation==Operation::RrcRoom) {
        if(request.revision!=_backend.rrcStatus().generation) {result.outcome=Outcome::Stale;result.storageError=storage::Error::Stale;return result;}
        rrc::Conversation binding;rrc::RoomView room;
        if(!_mailbox.read(slot,&binding,sizeof binding) || !history::rrc_catalog::roomDetail(_backend,binding,stored,
            reinterpret_cast<const uint8_t*>(_scratch),room) || !_mailbox.write(slot,&room,sizeof room)) return invalid();
        result.length=sizeof room;result.total=1;result.outcome=Outcome::Ok;return result;
    }
    if (request.operation == Operation::RrcDraft || request.operation == Operation::RrcDetail) {
        storage::rrc::Record record;
        if (stored.length) {
            memcpy(record.bytes, _scratch, stored.length);
            if (!record.valid(stored.length) || !record.matches(context)) return invalid();
        }
        if (request.operation == Operation::RrcDraft) {
            rrc::DraftView draft;
            if (stored.length) {
                const auto length = record.payloadLength();
                if (record.kind() != storage::rrc::Kind::Draft ||
                    (record.status()!=storage::rrc::Status::Received && record.status()!=storage::rrc::Status::Unconfirmed) || length < 8 || length > 8 + rrc::DraftCapacity ||
                    memcmp(record.payload(), "HRD1", 4) || !rrc::validDraftText(record.payload() + 8, length - 8)) return invalid();
                draft.revision = storage::prepared::read32(record.payload() + 4); draft.length = length - 8;
                draft.storageRevision = record.revision(); draft.uncertain = record.status()==storage::rrc::Status::Unconfirmed;
                memcpy(draft.text, record.payload() + 8, draft.length);
            }
            if (!_mailbox.write(slot, &draft, sizeof draft)) return invalid();
            result.length = sizeof draft;
        } else {
            rs_handheld_rrc_view_t view{};
            if (!stored.length || !history::rrc::decode(record, context, view) || record.counter() != request.argument) return invalid();
            rrc::MessageDetail detail;
            memcpy(detail.source, view.meta.source, 16); detail.counter = record.counter();
            detail.timestamp = history::rrc::timestamp(view.meta.timestamp_ms);
            detail.kind = uint8_t(view.meta.kind); detail.status = history::rrc::displayedStatus(record.status());
            memcpy(detail.nickname, record.payload() + view.nickname.offset, std::min(size_t(32), size_t(view.nickname.length)));
            detail.length = view.text.length; memcpy(detail.text, record.payload() + view.text.offset, detail.length);
            if (!_mailbox.write(slot, &detail, sizeof detail)) return invalid();
            result.length = sizeof detail;
        }
        result.outcome = Outcome::Ok; return result;
    }
    auto projected = stored;
    history::rrc::project(context, rrcHistoryQuery(request, context), projected,
        reinterpret_cast<uint8_t*>(_scratch), sizeof _scratch, _backend.rrcStatus().revision);
    result.outcome = projected.outcome == storage::Outcome::Committed ? Outcome::Ok : Outcome::Failed;
    result.storageError = projected.error; result.key = projected.key;
    result.revision = projected.revision; result.statusRevision = projected.revision;
    result.total = projected.total; result.more = projected.more; result.next = projected.nextOffset;
    if (!_mailbox.write(slot, _scratch, projected.length)) return invalid();
    result.length = projected.length; return result;
}
