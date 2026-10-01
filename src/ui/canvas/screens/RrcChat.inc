// Cardputer retains only bindings/revisions around its existing editor and
// HistoryWindow. RrcClient borrows the existing read-marker storage ticket.
void MessageView::rrcNotice(const char* text) {
    _sendNotice=text;_sendNoticeSince=millis();
}
handheld::rrc::Command MessageView::rrcCommand(handheld::rrc::Action action) const {
    handheld::rrc::Command command;command.action=action;
    command.generation=_backend?_backend->rrcStatus().generation:0;
    memcpy(command.hub,_rrcBinding.hub,16);memcpy(command.participant,_rrcBinding.participant,16);
    memcpy(command.room,_rrcBinding.room,sizeof command.room);return command;
}
bool MessageView::leaveRrcDraft() {
    if (!_rrcMode || !rrcWritable()) return true;
    if (!sameRrcIdentity(_rrcBinding,_rrcIdentity)) { _input.clear();return true; }
    if (_rrcLoading) { rrcNotice("Loading draft; try again");return false; }
    if (!_rrcLoaded || (_rrcSavedRevision==_input.revision() && !_rrcSaving)) return true;
    saveRrcDraft(true);rrcNotice("Saving draft; try again");return false;
}
bool MessageView::setRrcConversation(const handheld::rrc::Conversation& binding,const char* insert) {
    if (!_backend || binding.room[64] || (binding.room[0] && binding.privateNotice())) return false;
    handheld::storage::rrc::Context context;
    if (!_store || !_backend->rrcContext(binding.hub,binding.room,binding.privateNotice()?binding.participant:nullptr,context)) return false;
    const bool same=_rrcMode && sameRrcIdentity(_rrcBinding,_rrcIdentity) &&
        !memcmp(binding.hub,_rrcBinding.hub,16) && !memcmp(binding.participant,_rrcBinding.participant,16) &&
        !strcmp(binding.room,_rrcBinding.room);
    if (!same) {
        if (!leaveRrcDraft()) return false;
        if (!_rrcMode && !_input.getText().empty()) {
            if (_sendTicket.valid() && _retainedPeer!=_peerHex) { rrcNotice("Finish saving the current draft first");return false; }
            _retainedDraft=_input.getText();_retainedPeer=_peerHex;
            _retainedIdentity=_backend->destinationHashHex().c_str();
        }
        _history.close();
        _rrcMode=true;_rrcBinding=binding;memcpy(_rrcIdentity,context.local,16);++_rrcView;
        _peerHex.clear();_input.clear();rrcEdited();
        memset(_rrcInsert,0,sizeof _rrcInsert);
        _rrcLoaded=!rrcWritable();_rrcLoading=false;_rrcSavedRevision=_input.revision();
        _rrcStorageRevision=0;_rrcPreparedRevision=0;_rrcSendRequested=false;_rrcReadThrough=_rrcVisibleThrough=0;
        _rrcRetryAt=0;_rrcEmote=false;_rrcUncertain=false;_rrcResendConfirmed=false;
    }
    if (insert && *insert) {
        if (!_rrcLoaded) {
            const auto length=handheld::ui::rrc_input::textPrefix(insert,strlen(insert),sizeof _rrcInsert-1);
            memcpy(_rrcInsert,insert,length);_rrcInsert[length]=0;return true;
        }
        const auto length=handheld::ui::rrc_input::textPrefix(insert,strlen(insert),handheld::rrc::DraftCapacity-_input.getText().size());
        _input.appendText(insert,length);rrcEdited();
    }
    return true;
}
bool MessageView::sameRrcIdentity(const handheld::rrc::Conversation& binding,const uint8_t identity[16]) const {
    handheld::storage::rrc::Context context;
    return _backend && _backend->rrcContext(binding.hub,binding.room,binding.privateNotice()?binding.participant:nullptr,context) &&
        !memcmp(context.local,identity,16);
}
void MessageView::rrcEdited() {
    _rrcEditAt=millis();_rrcResendConfirmed=false;_rrcPreparedRevision=0;
}
void MessageView::enterRrcHistory() {
    if (!_backend || !_rrcMode || !_visible) return;
    handheld::storage::rrc::Context context;
    if (!_backend->rrcContext(_rrcBinding.hub,_rrcBinding.room,_rrcBinding.privateNotice()?_rrcBinding.participant:nullptr,context)) return;
    _history.open(context.conversation,1);_history.setScrollOffset(UINT32_MAX);
}
void MessageView::loadRrcDraft() {
    if (!_backend || !_draftReady || !_rrcMode || _rrcLoaded || _rrcLoading || int32_t(millis()-_rrcRetryAt)<0) return;
    const auto view=_rrcView;
    _rrcLoading=true;
    const auto id=_rrcClient.draft(_rrcBinding,[this,view](const handheld::canvas::RrcClient::Result& result,const handheld::rrc::DraftView* rows,size_t count) {
        if (_rrcView!=view || !_rrcMode) return;
        _rrcLoading=false;_rrcRetryAt=millis()+1000;
        if (!sameRrcIdentity(_rrcBinding,_rrcIdentity)) return;
        if (result.code!=handheld::rrc::Code::Ok || count!=1) { rrcNotice("Could not load draft; retrying");return; }
        _input.setText(rows[0].text,rows[0].length);rrcEdited();_rrcSavedRevision=_input.revision();_rrcLoaded=true;
        _rrcStorageRevision=rows[0].storageRevision;_rrcUncertain=rows[0].uncertain;
        if (_rrcInsert[0]) {
            const auto length=handheld::ui::rrc_input::textPrefix(_rrcInsert,strlen(_rrcInsert),handheld::rrc::DraftCapacity-_input.getText().size());
            _input.appendText(_rrcInsert,length);memset(_rrcInsert,0,sizeof _rrcInsert);rrcEdited();
        }
        _input.setActive(_visible && rrcWritable());
    });
    if (!id) {_rrcLoading=false;_rrcRetryAt=millis()+1000;}
}
void MessageView::saveRrcDraft(bool immediate, bool forSend) {
    if (!_backend || !_rrcMode || !rrcWritable() || !_rrcLoaded || _rrcSaving || _rrcClearPending ||
        !sameRrcIdentity(_rrcBinding,_rrcIdentity) || (!forSend && _rrcSavedRevision==_input.revision()) ||
        (!immediate && (uint32_t(millis()-_rrcEditAt)<500 || int32_t(millis()-_rrcRetryAt)<0))) return;
    const auto view=_rrcView;const auto revision=_input.revision();
    if (forSend && !_rrcStorageRevision) return;
    auto command=rrcCommand(handheld::rrc::Action::Draft);
    if (forSend) {command.flags=3;command.counter=_rrcStorageRevision;}
    _rrcSaving=true;
    const auto id=_rrcClient.command(command,
        reinterpret_cast<const uint8_t*>(_input.getText().data()),_input.getText().size(),[this,view,revision,forSend](const handheld::canvas::RrcClient::Result& result) {
            _rrcSaving=false;
            if (_rrcView!=view || !_rrcMode || !sameRrcIdentity(_rrcBinding,_rrcIdentity)) return;
            if (result.code==handheld::rrc::Code::Ok) { _rrcSavedRevision=revision;_rrcStorageRevision=result.total;_rrcPreparedRevision=forSend?revision:0; }
            else {
                if (forSend && result.code==handheld::rrc::Code::Stale) { _rrcSendRequested=false;rrcNotice("Saved draft changed; reopen before sending"); }
                else rrcNotice("Draft not saved; text kept");
            }
            _rrcRetryAt=result.code==handheld::rrc::Code::Ok?0:millis()+1000;
        });
    if (!id) {_rrcSaving=false;_rrcRetryAt=millis()+1000;}
}
void MessageView::clearConfirmedRrcDraft() {
    if (!_backend || !_rrcClearPending || _rrcClearing || int32_t(millis()-_rrcRetryAt)<0) return;
    if (!sameRrcIdentity(_rrcSendingBinding,_rrcSendIdentity)) {_rrcClearPending=false;return;}
    handheld::rrc::Command command;command.action=handheld::rrc::Action::Draft;
    command.generation=_backend->rrcStatus().generation;command.flags=1;command.counter=_rrcSentStorageRevision;
    memcpy(command.hub,_rrcSendingBinding.hub,16);memcpy(command.participant,_rrcSendingBinding.participant,16);
    memcpy(command.room,_rrcSendingBinding.room,sizeof command.room);
    _rrcClearing=true;
    const auto id=_rrcClient.command(command,nullptr,0,[this](const handheld::canvas::RrcClient::Result& result) {
        _rrcClearing=false;
        if (result.code==handheld::rrc::Code::Ok || result.code==handheld::rrc::Code::Stale) {
            _rrcClearPending=false;
            if (result.code==handheld::rrc::Code::Ok && _rrcMode && _rrcView==_rrcClearView &&
                _input.revision()==_rrcClearEditorRevision && sameRrcIdentity(_rrcBinding,_rrcIdentity)) {
                _rrcSavedRevision=_input.revision();_rrcStorageRevision=result.total;
            }
        } else _rrcRetryAt=millis()+2000;
    });
    if (!id) {_rrcClearing=false;_rrcRetryAt=millis()+1000;}
}
bool MessageView::pollRrc(bool allowAdmission) {
    if (!_backend) return false;
    const auto before=_input.revision();const auto notice=_sendNotice;
    const auto oldSend=_rrcSendId;
    _rrcClient.bind(_backend,_store,&_readTicket);_rrcClient.poll();
    const auto status=_backend->rrcStatus();
    const bool statusChanged=_rrcMode && _rrcObservedRevision!=status.revision;_rrcObservedRevision=status.revision;
    if (_rrcSendId && status.sendRevision==_rrcSendId && status.sendSettled) {
        const bool confirmed=status.sending==handheld::rrc::SendPhase::Confirmed || status.sending==handheld::rrc::SendPhase::Transmitted;
        if (confirmed && _rrcSentStorageRevision && sameRrcIdentity(_rrcSendingBinding,_rrcSendIdentity)) {
            _rrcClearPending=true;_rrcRetryAt=0;_rrcClearView=0;
            const bool sameStoredDraft=_rrcMode && _rrcLoaded && _rrcSavedRevision==_input.revision() &&
                _rrcStorageRevision==_rrcSentStorageRevision && !memcmp(_rrcBinding.hub,_rrcSendingBinding.hub,16) &&
                !memcmp(_rrcBinding.participant,_rrcSendingBinding.participant,16) && !strcmp(_rrcBinding.room,_rrcSendingBinding.room);
            if (sameStoredDraft) {
                _input.clear();rrcEdited();_rrcClearView=_rrcView;_rrcClearEditorRevision=_input.revision();
            }
        }
        if (_rrcMode && _rrcSentView==_rrcView && sameRrcIdentity(_rrcBinding,_rrcIdentity)) {
            _rrcUncertain=status.sending==handheld::rrc::SendPhase::Unconfirmed;
            rrcNotice(confirmed?status.sending==handheld::rrc::SendPhase::Confirmed?"Confirmed by hub":"Transmitted; no recipient receipt":
                _rrcUncertain?"Unconfirmed; draft kept":"Not sent; draft kept");
            _input.setActive(_visible && rrcWritable());
        }
        _rrcSendId=0;
    }
    if (allowAdmission) clearConfirmedRrcDraft();
    if (!_rrcMode) return statusChanged || oldSend!=_rrcSendId || notice!=_sendNotice;
    if (!sameRrcIdentity(_rrcBinding,_rrcIdentity)) {
        _input.clear();_peerHex.clear();memset(_rrcInsert,0,sizeof _rrcInsert);
        _rrcLoaded=false;_rrcMode=false;++_rrcView;_history.close();
        if (_visible && _backCb) _backCb();
        return true;
    }
    if (!allowAdmission) return statusChanged || oldSend!=_rrcSendId || notice!=_sendNotice || before!=_input.revision();
    if (_visible && _history.mode()==History::Mode::Closed) enterRrcHistory();
    if (_visible) loadRrcDraft();
    saveRrcDraft();
    if (_rrcSendRequested) {
        if (_input.revision()!=_rrcRequestedRevision) _rrcSendRequested=false;
        else if (!_rrcSaving && _rrcSavedRevision==_input.revision() && _rrcPreparedRevision==_input.revision()) {
            _rrcSendRequested=false;_rrcEmote=_rrcRequestedEmote;sendRrcMessage();_rrcEmote=false;
        } else if (int32_t(millis()-_rrcRetryAt)>=0) saveRrcDraft(true,_rrcSavedRevision==_input.revision() && _rrcStorageRevision);
    }
    if (_visible && _rrcVisibleThrough>_rrcReadThrough && !_rrcClient.pending()) {
        auto command=rrcCommand(handheld::rrc::Action::MarkRead);command.counter=_rrcVisibleThrough;
        const auto view=_rrcView,counter=command.counter;
        _rrcClient.command(command,nullptr,0,[this,view,counter](const handheld::canvas::RrcClient::Result& result) {
            if (view==_rrcView && result.code==handheld::rrc::Code::Ok) _rrcReadThrough=counter;
        });
    }
    return statusChanged || oldSend!=_rrcSendId || notice!=_sendNotice || before!=_input.revision();
}
void MessageView::sendRrcMessage() {
    if (!_backend || !_draftReady || !_rrcMode || !_rrcLoaded || !rrcWritable() || _input.getText().empty() || _rrcSendId || _rrcClearPending) return;
    if (_rrcUncertain && !_rrcResendConfirmed) {
        _rrcResendConfirmed=true;rrcNotice("Unconfirmed. Enter again resends");return;
    }
    const auto action=_rrcBinding.privateNotice()?handheld::rrc::Action::PrivateNotice:_rrcEmote?handheld::rrc::Action::Emote:handheld::rrc::Action::Message;
    const auto preview=handheld::ui::rrc_input::preview(_backend->rrcStatus(),_rrcBinding,action,
        reinterpret_cast<const uint8_t*>(_input.getText().data()),_input.getText().size());
    if (preview.code!=handheld::rrc::Code::Ok) {
        rrcNotice(_input.getText()[0]=='/'?"Use Advanced command in the channel menu":handheld::rrc::codeName(preview.code));return;
    }
    if (_rrcSaving || _rrcSavedRevision!=_input.revision() || _rrcPreparedRevision!=_input.revision()) {
        _rrcSendRequested=true;_rrcRequestedRevision=_input.revision();_rrcRequestedEmote=_rrcEmote;
        saveRrcDraft(true,_rrcSavedRevision==_input.revision() && _rrcStorageRevision);rrcNotice("Saving draft before sending...");return;
    }
    const auto view=_rrcView;
    _rrcSendId=UINT32_MAX;_rrcSentView=view;
    _rrcSendingBinding=_rrcBinding;_rrcSentStorageRevision=_rrcStorageRevision;memcpy(_rrcSendIdentity,_rrcIdentity,16);
    const auto id=_rrcClient.command(rrcCommand(action),reinterpret_cast<const uint8_t*>(_input.getText().data()),_input.getText().size(),
        [this,view](const handheld::canvas::RrcClient::Result& result) {
            if (result.code!=handheld::rrc::Code::Ok) {
                _rrcSendId=0;
                if (_rrcMode && _rrcView==view) rrcNotice("Not sent; draft kept");
            }
        });
    if (!id) {_rrcSendId=0;rrcNotice("Hub busy; draft kept");return;}
    if (!_rrcSendId) return;
    _rrcSendId=id;_rrcResendConfirmed=false;_rrcPreparedRevision=0;
    rrcNotice("Saving message...");
}
