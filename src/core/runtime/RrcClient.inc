// Included by ServiceClient.cpp inside handheld. Values borrow only the copied
// callback payload. There is one native history owner for both message families.
template<class Row, size_t Capacity>
uint32_t ServiceClient::rrcQuery(Request request, const void* payload, size_t length, RrcCompletion<Row> completion) {
    if (!completion) return 0;
    request.revision = _status.rrc.generation;
    return submit(request, payload, length, Capacity * sizeof(Row),
        [completion = std::move(completion)](const Result& result, const char* data) {
            Result checked = result; Row rows[Capacity]{}; size_t count = 0;
            if (result.outcome == Outcome::Ok) {
                if (result.length % sizeof(Row) || result.length > sizeof rows) checked.outcome = Outcome::Invalid;
                else { count = result.length / sizeof(Row); memcpy(rows, data, result.length); }
            }
            completion(checked, rows, count);
        });
}
uint32_t ServiceClient::requestRrcHubs(RrcCompletion<rrc::HubView> completion) {
    Request request; request.operation = Operation::RrcHubs;
    return rrcQuery<rrc::HubView, rrc::HubViewCapacity>(request, nullptr, 0, std::move(completion));
}
uint32_t ServiceClient::requestRrcRooms(RrcCompletion<rrc::RoomView> completion) {
    Request request; request.operation = Operation::RrcRooms;
    return rrcQuery<rrc::RoomView, rrc::RoomCapacity>(request, nullptr, 0, std::move(completion));
}
uint32_t ServiceClient::requestRrcPeople(const char* room, RrcCompletion<rrc::PersonView> completion) {
    const size_t length = room ? strnlen(room, 65) : 0;
    if (length > 64) return 0;
    Request request; request.operation = Operation::RrcPeople;
    return rrcQuery<rrc::PersonView, rrc::PeopleCapacity>(request, room, length, std::move(completion));
}
uint32_t ServiceClient::requestRrcDirectory(size_t offset, RrcCompletion<rrc::DirectoryView> completion) {
    if (offset > UINT32_MAX) return 0;
    Request request; request.operation = Operation::RrcDirectory; request.offset = offset;
    return rrcQuery<rrc::DirectoryView, 8>(request, nullptr, 0, std::move(completion));
}
uint32_t ServiceClient::rrcCommand(const rrc::Command& command, const uint8_t* body, size_t length, Completion completion) {
    if (length > rrc::PacketCapacity || (length && !body) || command.room[64] || command.action > rrc::Action::MarkRead || command.flags > 1) return 0;
    uint8_t payload[sizeof command + rrc::PacketCapacity]; memcpy(payload, &command, sizeof command);
    if (length) memcpy(payload + sizeof command, body, length);
    Request request; request.operation = Operation::RrcCommand;
    const auto id = submit(request, payload, sizeof command + length, 0,
        [this, completion = std::move(completion)](const Result& result, const char*) {
            if (result.outcome != Outcome::Ok) tell(result.detail[0] ? result.detail : "Hub action failed");
            if (completion) completion(result);
        });
    volatile uint8_t* clear = payload; for (size_t n = 0; n < sizeof command + length; ++n) clear[n] = 0;
    return id;
}
uint32_t ServiceClient::requestRrcDraft(const rrc::Conversation& binding, RrcCompletion<rrc::DraftView> completion) {
    Request request; request.operation = Operation::RrcDraft;
    return rrcQuery<rrc::DraftView, 1>(request, &binding, sizeof binding, std::move(completion));
}
uint32_t ServiceClient::requestRrcDetail(const rrc::Conversation& binding, const uint8_t key[16], uint32_t counter,
                                       RrcCompletion<rrc::MessageDetail> completion) {
    if (!key || !counter) return 0;
    Request request; request.operation = Operation::RrcDetail; request.argument = counter;
    storage::encodeHex(key, 16, request.peer);
    return rrcQuery<rrc::MessageDetail, 1>(request, &binding, sizeof binding, std::move(completion));
}
uint32_t ServiceClient::watchRrcHistory(const rrc::Conversation& binding, Completion completion) {
    closeHistory();
    if (_rrcViewRevision == UINT32_MAX || binding.room[64] || (binding.room[0] && binding.privateNotice())) return 0;
    _rrcHistory = true; _rrcConversation = binding;
    const auto revision = _rrcViewRevision;
    Request request; request.operation = Operation::RrcContext;
    const auto id = submit(request, &binding, sizeof binding, 16,
        [this, revision, completion = std::move(completion)](const Result& result, const char* data) {
            if (revision != _rrcViewRevision || !_rrcHistory) return;
            Result checked = result;
            if (result.outcome == Outcome::Ok && result.length == 16) {
                _history.open(reinterpret_cast<const uint8_t*>(data), _status.generation);
                _history.observeStatusRevision(_status.rrc.revision);
                _history.observeHistoryRevision(_status.rrc.revision);
            } else { checked.outcome = Outcome::Failed; _rrcHistory = false; }
            if (completion) completion(checked);
        });
    if (!id) _rrcHistory = false;
    return id;
}
