#pragma once

#include "protocol/ProtocolBackend.h"
#include "protocol/RrcPreferences.h"
#include "storage/MessageStore.h"
#include "ui/RrcCommandId.h"
#include "history/RrcHistory.h"
#include <functional>
#include <utility>

namespace handheld::canvas {
// Board-loop adapter. It borrows MessageView's existing read-marker ticket and
// uses the existing protocol and storage owners. There is no request queue,
// message-body retention, task, or independent history window.
class RrcClient {
public:
    struct Result { rrc::Code code = rrc::Code::Storage; uint32_t revision = 0, total = 0; };
    using Completion = std::function<void(const Result&)>;
    template<class Row> using Rows = std::function<void(const Result&, const Row*, size_t)>;
    void bind(ProtocolBackend* backend, MessageStore* store, storage::Ticket* ticket) {
        _backend=backend;_store=store;_ticket=ticket;
    }
    bool readingDraft() const { return bool(_read); }
    bool pending() const { return _preferenceId || _sendId || readingDraft(); }
    uint32_t command(rrc::Command command, const uint8_t* body, size_t length, Completion complete) {
        if (!_backend) return 0;
        const bool message=command.action==rrc::Action::Message || command.action==rrc::Action::Emote || command.action==rrc::Action::PrivateNotice;
        const bool preference=command.action==rrc::Action::Draft || command.action==rrc::Action::MarkRead ||
            command.action==rrc::Action::ClearHistory || command.action==rrc::Action::SaveHub ||
            command.action==rrc::Action::ForgetHub || command.action==rrc::Action::Nickname;
        if (!message && !preference) return 0;
        if ((message && _sendId) || (!message && _preferenceId)) return 0;
        command.revision=ui::nextCardRrcCommand();if (!command.revision) return 0;
        const auto code=_backend->rrcCommand(command,body,length);
        if (code!=rrc::Code::Ok) {if (complete) complete({code,command.revision,0});return command.revision;}
        if (message) {_sendId=command.revision;_send=std::move(complete);}
        else {_preferenceId=command.revision;_preference=std::move(complete);}
        return command.revision;
    }
    template<class Complete>
    uint32_t draft(const rrc::Conversation& binding,Complete complete) {
        return query(binding,0,[complete=std::move(complete)](const Result& result,const void* row,size_t count) {
            complete(result,static_cast<const rrc::DraftView*>(row),count);
        });
    }
    template<class Complete>
    uint32_t detail(const rrc::Conversation& binding,uint32_t counter,Complete complete) {
        if(!counter) return 0;
        return query(binding,counter,[complete=std::move(complete)](const Result& result,const void* row,size_t count) {
            complete(result,static_cast<const rrc::MessageDetail*>(row),count);
        });
    }
    void poll() {
        if (!_backend) return;
        const auto status=_backend->rrcStatus();
        if (_preferenceId && status.preferenceRevision==_preferenceId) {
            const Result result{status.preferenceResult,_preferenceId,status.preferenceRecordRevision};
            auto complete=std::move(_preference);_preference={};_preferenceId=0;if (complete) complete(result);
        }
        if (_sendId && status.sendRevision==_sendId && (status.sendSaved || status.sendSettled)) {
            const Result result{status.sendSaved?rrc::Code::Ok:rrc::Code::Storage,_sendId,0};
            auto complete=std::move(_send);_send={};_sendId=0;if (complete) complete(result);
        }
        if (!_read || !_store || !_ticket || !_ticket->valid()) return;
        storage::Result stored;if (!_store->peekResult(*_ticket,stored)) return;
        Result result;result.revision=_readId;
        storage::rrc::Record record;
        const bool valid=stored.outcome==storage::Outcome::Committed && stored.length>=storage::rrc::Header &&
            stored.length<=storage::rrc::Maximum && _store->readPayload(*_ticket,record.bytes,stored.length) &&
            record.valid(stored.length) && record.matches(_readContext);
        auto deliver=[&](const void* row) {
            if(!_store->releaseResult(*_ticket)) return;
            *_ticket={};auto complete=std::move(_read);_read={};_readId=_readCounter=0;
            complete(result,row,result.code==rrc::Code::Ok?1:0);
        };
        if(_readCounter) {
            rrc::MessageDetail detail;
            if(valid && record.counter()==_readCounter && history::rrc::detail(record,_readContext,detail)) result.code=rrc::Code::Ok;
            deliver(&detail);
        } else {
            rrc::DraftView draft;
            if(stored.outcome==storage::Outcome::Committed && !stored.length) result.code=rrc::Code::Ok;
            else if(valid && record.kind()==storage::rrc::Kind::Draft &&
                (record.status()==storage::rrc::Status::Received || record.status()==storage::rrc::Status::Unconfirmed) &&
                record.payloadLength()>=8 && !memcmp(record.payload(),"HRD1",4) &&
                rrc::validDraftText(record.payload()+8,record.payloadLength()-8)) {
                result.code=rrc::Code::Ok;draft.revision=storage::prepared::read32(record.payload()+4);
                draft.storageRevision=record.revision();draft.uncertain=record.status()==storage::rrc::Status::Unconfirmed;
                draft.length=record.payloadLength()-8;memcpy(draft.text,record.payload()+8,draft.length);
            }
            deliver(&draft);
        }
    }

private:
    ProtocolBackend* _backend=nullptr;
    MessageStore* _store=nullptr;
    storage::Ticket* _ticket=nullptr;
    storage::rrc::Context _readContext;
    Completion _preference,_send;
    using ReadCompletion=std::function<void(const Result&,const void*,size_t)>;
    ReadCompletion _read;
    uint32_t query(const rrc::Conversation& binding,uint32_t counter,ReadCompletion complete) {
        if(!_backend || !_store || !_ticket || _ticket->valid() || _read || !complete) return 0;
        storage::rrc::Context context;
        if(!_backend->rrcContext(binding.hub,binding.room,binding.privateNotice()?binding.participant:nullptr,context)) return 0;
        storage::rrc::Record record;
        storage::rrc::Record::make(record,context,counter?storage::rrc::Kind::Message:storage::rrc::Kind::Draft,nullptr,0);
        const auto id=ui::nextCardRrcCommand();if(!id) return 0;
        const auto submitted=_store->requestRrc(storage::Operation::RrcRead,record,nullptr,counter);
        if(!submitted.accepted()) return 0;
        *_ticket=submitted.ticket;_readContext=context;_read=std::move(complete);_readId=id;_readCounter=counter;return id;
    }
    uint32_t _readCounter=0;
    uint32_t _preferenceId=0,_sendId=0,_readId=0;
};
static_assert(sizeof(RrcClient)<=192,"Canvas RRC port retains only callbacks and one borrowed query binding");
}
