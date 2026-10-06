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
        if(_voiceClose) _voiceClose();
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
    const auto beforeMenu=_rrcTools.state;
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
    if (!_rrcMode) return beforeMenu!=_rrcTools.state || statusChanged || oldSend!=_rrcSendId || notice!=_sendNotice;
    if (!sameRrcIdentity(_rrcBinding,_rrcIdentity)) {
        _input.clear();_peerHex.clear();memset(_rrcInsert,0,sizeof _rrcInsert);
        _rrcLoaded=false;_rrcMode=false;++_rrcView;_history.close();
        if (_visible && _backCb) _backCb();
        return true;
    }
    if (!allowAdmission) return beforeMenu!=_rrcTools.state || statusChanged || oldSend!=_rrcSendId || notice!=_sendNotice || before!=_input.revision();
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
    return beforeMenu!=_rrcTools.state || statusChanged || oldSend!=_rrcSendId || notice!=_sendNotice || before!=_input.revision();
}
void MessageView::sendRrcMessage() {
    if(!_backend || !sameRrcIdentity(_rrcBinding,_rrcIdentity)) return;
    if (!_backend || !_draftReady || !_rrcMode || !_rrcLoaded || !rrcWritable() || _input.getText().empty() || _rrcSendId || _rrcClearPending) return;
    const auto& live=_backend->rrcStatus();
    if(memcmp(live.hub,_rrcBinding.hub,16) || live.phase!=handheld::rrc::Phase::Online) {showRrcGuidance(RrcTools::State::Connect);return;}
    if (_rrcUncertain && !_rrcResendConfirmed) {
        _rrcResendConfirmed=true;rrcNotice("Unconfirmed. Enter again resends");return;
    }
    const auto action=_rrcBinding.privateNotice()?handheld::rrc::Action::PrivateNotice:_rrcEmote?handheld::rrc::Action::Emote:handheld::rrc::Action::Message;
    const auto preview=handheld::ui::rrc_input::preview(_backend->rrcStatus(),_rrcBinding,action,
        reinterpret_cast<const uint8_t*>(_input.getText().data()),_input.getText().size());
    if (preview.code!=handheld::rrc::Code::Ok) {
        if(preview.code==handheld::rrc::Code::Unsupported && _rrcEmote && !_rrcBinding.privateNotice()) {showRrcGuidance(RrcTools::State::Plain);return;}
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
                if (_rrcMode && _rrcView==view) {
                    const auto code=result.code;
                    if(code==handheld::rrc::Code::NotJoined) showRrcGuidance(RrcTools::State::Join);
                    else if(code==handheld::rrc::Code::Offline || code==handheld::rrc::Code::Stale) showRrcGuidance(RrcTools::State::Connect);
                    else rrcNotice(handheld::rrc::codeName(code==handheld::rrc::Code::Ok?handheld::rrc::Code::Storage:code));
                }
            }
        });
    if (!id) {_rrcSendId=0;rrcNotice("Hub busy; draft kept");return;}
    if (!_rrcSendId) return;
    _rrcSendId=id;_rrcResendConfirmed=false;_rrcPreparedRevision=0;
    rrcNotice("Saving message...");
}

void MessageView::openRrcTools(size_t index) {
    if(!_rrcMode || !_visible || !_history.visible()) return;
    const auto* row=_history.span(index);if(!row || row->unavailable()) return;
    _history.focusSpan(index);_rrcTools.begin(row->counter);_input.setActive(false);
    readRrcTool(RrcTools::Action::Read,true);
}
void MessageView::closeRrcTools() {
    if(_rrcTools.state==RrcTools::State::Identity || _rrcTools.state==RrcTools::State::Copy) {
        _rrcTools.state=RrcTools::State::Menu;_rrcTools.selected=0;return;
    }
    _rrcTools.close();_input.setActive(_history.focusedSpan()>=_history.spanCount() && rrcWritable());
}
void MessageView::renderRrcTools(M5Canvas& canvas) {
    canvas.fillRect(0,Theme::CONTENT_Y,Theme::CONTENT_W,Theme::CONTENT_H,Theme::BG);
    Theme::useSmallFont(canvas);canvas.setTextColor(Theme::ACCENT);
    char heading[64];snprintf(heading,sizeof heading,"%s %u/%u",_rrcTools.title(),unsigned(_rrcTools.selected+1),unsigned(_rrcTools.count()));
    canvas.drawString(heading,3,Theme::CONTENT_Y+2);
    const size_t first=_rrcTools.selected>=4?_rrcTools.selected-3:0;
    for(size_t n=first;n<_rrcTools.count() && n<first+4;++n) {
        char label[64];_rrcTools.label(n,label,sizeof label);
        const int y=Theme::CONTENT_Y+19+int(n-first)*17;
        if(n==_rrcTools.selected) canvas.fillRect(0,y,Theme::CONTENT_W,17,Theme::SELECTION_BG);
        canvas.setTextColor(n==_rrcTools.selected?Theme::ACCENT:Theme::TEXT_PRIMARY);canvas.drawString(label,4,y+1);
    }
}
void MessageView::activateRrcTool() {
    using Action=RrcTools::Action;using State=RrcTools::State;
    if(_rrcTools.state==State::Connect || _rrcTools.state==State::Join || _rrcTools.state==State::Plain) {activateRrcGuidance();return;}
    if(_rrcTools.state==State::Loading) {closeRrcTools();return;}
    if(_rrcTools.state==State::Failed) {
        if(_rrcTools.selected) closeRrcTools();else {_rrcTools.state=State::Loading;readRrcTool(Action::Read,true);}return;
    }
    if(_rrcTools.state==State::Identity) {if(_rrcTools.selected==4) closeRrcTools();return;}
    if(_rrcTools.state==State::Copy) {
        if(_rrcTools.selected==1) readRrcTool(Action::Copy);
        else if(_rrcTools.selected==2) closeRrcTools();return;
    }
    if(_rrcTools.state==State::Invitation && _rrcTools.selected==2) return;
    const auto action=_rrcTools.action();
    if(action==Action::Back) {closeRrcTools();return;}
    if(action==Action::Identity) {_rrcTools.state=State::Identity;_rrcTools.selected=0;return;}
    if(action==Action::Copy) {_rrcTools.state=State::Copy;_rrcTools.selected=1;return;}
    if(action==Action::Read) {
        for(size_t n=0;n<_history.spanCount();++n) if(_history.span(n)->counter==_rrcTools.counter) {
            if(_history.openFull(n)) _rrcTools.close();return;
        }
        rrcNotice("Message moved; reopen its actions");closeRrcTools();return;
    }
    readRrcTool(action);
}
void MessageView::readRrcTool(RrcTools::Action action,bool metadata) {
    if(!_backend || !_visible || !_rrcMode || !_rrcTools.visible()) return;
    if(!metadata && (action==RrcTools::Action::Private || action==RrcTools::Action::Direct) && !leaveRrcDraft()) return;
    const auto view=_rrcView,revision=_rrcTools.revision,counter=_rrcTools.counter;
    _rrcTools.state=RrcTools::State::Loading;
    const auto id=_rrcClient.detail(_rrcBinding,counter,[this,view,revision,counter,action,metadata](const handheld::canvas::RrcClient::Result& result,const handheld::rrc::MessageDetail* rows,size_t count) {
        if(!_visible || !_rrcMode || _rrcView!=view || _rrcTools.revision!=revision || !_rrcTools.visible() ||
            !sameRrcIdentity(_rrcBinding,_rrcIdentity)) return;
        if(result.code!=handheld::rrc::Code::Ok || count!=1 || rows[0].counter!=counter) {_rrcTools.state=RrcTools::State::Failed;return;}
        const auto& row=rows[0];
        if(metadata) {_rrcTools.accept(row);return;}
        _rrcTools.state=RrcTools::State::Menu;
        if(!row.kind) {rrcNotice("This is a local history marker");return;}
        if(action==RrcTools::Action::AcceptInvite) {
            char room[65];
            if(!row.invitation || !handheld::rrc::hubText::invitation(reinterpret_cast<const uint8_t*>(row.text),row.length,room)) {
                rrcNotice("This is not a recognized hub invitation");return;
            }
            const auto& live=_backend->rrcStatus();
            if(memcmp(live.hub,_rrcBinding.hub,16) || live.phase!=handheld::rrc::Phase::Online) {
                showRrcGuidance(RrcTools::State::Connect);return;
            }
            if(memcmp(row.source,live.identity,16)) {rrcNotice("Hub identity changed; reopen the invitation");return;}
            auto command=rrcCommand(handheld::rrc::Action::Join);memset(command.participant,0,16);strcpy(command.room,room);
            command.revision=handheld::ui::nextCardRrcCommand();
            const auto code=_backend->rrcCommand(command,nullptr,0);
            if(code==handheld::rrc::Code::Ok) {closeRrcTools();rrcNotice("Joining invited channel");}
            else rrcNotice(handheld::rrc::codeName(code));return;
        }

        if(action==RrcTools::Action::Private || action==RrcTools::Action::Direct) {
            if(action==RrcTools::Action::Private && !(_backend->rrcStatus().capabilities&4)) {rrcNotice("Hub does not support private notices");return;}
            if(!leaveRrcDraft()) return;
            if(action==RrcTools::Action::Private) {
                auto binding=_rrcBinding;binding.room[0]=0;memcpy(binding.participant,row.source,16);
                onExit();if(setRrcConversation(binding)) onEnter();
            } else {
                uint8_t destination[16];char address[33];
                if(rs_handheld_lxmf_destination_for_identity(row.source,destination)!=RS_HANDHELD_OK) return;
                RrcTools::hex(destination,16,address);onExit();if(setPeerHex(address)) onEnter();
            }
            return;
        }
        if(!rrcWritable() || !_rrcLoaded) {rrcNotice("Open a channel or private composer first");return;}
        const auto available=handheld::rrc::DraftCapacity-_input.getText().size();
        char insertion[handheld::rrc::DraftCapacity+1]{};size_t length=0;bool shortened=false;
        if(action==RrcTools::Action::Quote) length=RrcTools::quote(row,insertion,available+1,shortened);
        else if(action==RrcTools::Action::Mention) {char token[36];length=RrcTools::mention(row,token);if(length<=available) memcpy(insertion,token,length+1);else length=0;}
        else if(action==RrcTools::Action::Copy && row.length<=available) {length=row.length;memcpy(insertion,row.text,length);}
        if(!length) {rrcNotice("Draft has no room; text kept");return;}
        _input.appendText(insertion,length);rrcEdited();_rrcTools.close();_history.backToChat();
        _history.focusSpan(History::VisibleSpans);_input.setActive(true);
        rrcNotice(shortened?"Quote shortened to fit draft":"Added to draft; review before sending");
    });
    if(!id) {_rrcTools.state=RrcTools::State::Failed;rrcNotice("History busy; try again");}
}

void MessageView::showRrcGuidance(RrcTools::State state) {
    _rrcSendRequested=false;_rrcTools.begin(0);_rrcTools.state=state;
    const auto& live=_backend->rrcStatus();
    _rrcTools.kind=live.phase!=handheld::rrc::Phase::Disconnected && memcmp(live.hub,_rrcBinding.hub,16);
    memcpy(_rrcTools.source,_rrcBinding.hub,16);_rrcTools.selected=state==RrcTools::State::Connect?3:1;
    _input.setActive(false);
}
void MessageView::activateRrcGuidance() {
    if(!_backend || !sameRrcIdentity(_rrcBinding,_rrcIdentity)) return;
    using State=RrcTools::State;const auto state=_rrcTools.state;const auto selected=_rrcTools.selected;
    if((state==State::Connect && selected==4) || (state==State::Join && selected==3) || (state==State::Plain && selected==2)) {closeRrcTools();return;}
    if(state==State::Plain) {
        if(selected==1) {closeRrcTools();_rrcEmote=false;sendRrcMessage();}return;
    }
    if(state==State::Join && selected==2) {
        if(!leaveRrcDraft()) return;_rrcTools.close();if(_backCb) _backCb();return;
    }
    if((state==State::Connect && selected!=3) || (state==State::Join && selected!=1)) return;
    auto action=handheld::rrc::Action::Join;
    if(state==State::Connect) {
        const auto& live=_backend->rrcStatus();const bool same=!memcmp(live.hub,_rrcBinding.hub,16);
        if(same && live.phase!=handheld::rrc::Phase::Disconnected && live.phase!=handheld::rrc::Phase::Recovering) {
            closeRrcTools();rrcNotice(live.phase==handheld::rrc::Phase::Online?"Connected; send when ready":"Connection in progress; draft kept");return;
        }
        action=same && live.phase==handheld::rrc::Phase::Recovering?handheld::rrc::Action::Retry:handheld::rrc::Action::Connect;
    }
    auto command=rrcCommand(action);memset(command.participant,0,16);
    command.revision=handheld::ui::nextCardRrcCommand();
    const auto code=_backend->rrcCommand(command,nullptr,0);
    if(code==handheld::rrc::Code::Ok) {closeRrcTools();rrcNotice(action==handheld::rrc::Action::Join?"Joining; send when ready":"Connecting; draft kept");}
    else rrcNotice(handheld::rrc::codeName(code==handheld::rrc::Code::Ok?handheld::rrc::Code::Storage:code));
}
