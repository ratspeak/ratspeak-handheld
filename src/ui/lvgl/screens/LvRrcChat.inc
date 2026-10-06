// RRC uses the foreground editor and ServiceClient's one HistoryWindow. Only
// bindings/revisions survive callbacks; no message body is cached here.
void LvMessageView::rrcNotice(const char* text) {
    if (_ui) _ui->lvStatusBar().showToast(text,1800);
}
handheld::rrc::Command LvMessageView::rrcCommand(handheld::rrc::Action action) const {
    handheld::rrc::Command command;command.action=action;
    command.generation=_service?_service->status().rrc.generation:0;
    memcpy(command.hub,_rrcBinding.hub,16);memcpy(command.participant,_rrcBinding.participant,16);
    memcpy(command.room,_rrcBinding.room,sizeof command.room);return command;
}
bool LvMessageView::leaveRrcDraft() {
    if (!_rrcMode || !rrcWritable()) return true;
    if (_rrcIdentity!=(_service?_service->status().generation:0)) { _inputText.clear();return true; }
    if (_rrcLoading) { rrcNotice("Loading draft; try again");return false; }
    if (!_rrcLoaded || (_rrcSavedRevision==_draftRevision && !_rrcSaving)) return true;
    saveRrcDraft(true);rrcNotice("Saving draft; try again");return false;
}
bool LvMessageView::setRrcConversation(const handheld::rrc::Conversation& binding,const char* insert) {
    if (!_service || binding.room[64] || (binding.room[0] && binding.privateNotice())) return false;
    const bool same=_rrcMode && _rrcIdentity==_service->status().generation &&
        !memcmp(binding.hub,_rrcBinding.hub,16) && !memcmp(binding.participant,_rrcBinding.participant,16) &&
        !strcmp(binding.room,_rrcBinding.room);
    if (!same) {
        if (!leaveRrcDraft()) return false;
        if (!_rrcMode && !_inputText.empty()) {
            if (_sendPending && _retainedDraftPeer!=_peerHex) { rrcNotice("Finish saving the current draft first");return false; }
            _retainedDraft=_inputText;_retainedDraftPeer=_peerHex;_retainedDraftRevision=_draftRevision;
            _retainedDraftIdentity=_service->status().generation;
        }
        if(_ui) _ui->closeVoiceConversation();
        clearMessages();_service->closeHistory();
        _rrcMode=true;_rrcBinding=binding;_rrcIdentity=_service->status().generation;++_rrcView;
        _peerHex.clear();_inputText.clear();composerEdited();
        memset(_rrcInsert,0,sizeof _rrcInsert);
        _rrcLoaded=!rrcWritable();_rrcLoading=false;_rrcSavedRevision=_draftRevision;
        _rrcStorageRevision=0;_rrcPreparedRevision=0;_rrcSendRequested=false;
        _rrcRetryAt=0;_rrcEmote=false;_rrcUncertain=false;_rrcResendConfirmed=false;
    }
    if (insert && *insert) {
        if (!_rrcLoaded) {
            const auto length=handheld::ui::rrc_input::textPrefix(insert,strlen(insert),sizeof _rrcInsert-1);
            memcpy(_rrcInsert,insert,length);_rrcInsert[length]=0;return true;
        }
        const auto length=handheld::ui::rrc_input::textPrefix(insert,strlen(insert),handheld::rrc::DraftCapacity-_inputText.size());
        _inputText.append(insert,length);composerEdited();
    }
    return true;
}
void LvMessageView::enterRrcHistory() {
    if (!_service || !_rrcMode || !_entered) return;
    const auto view=_rrcView,identity=_rrcIdentity;
    const auto id=_service->watchRrcHistory(_rrcBinding,[this,view,identity](const handheld::Result& result) {
        if (!_rrcMode || _rrcView!=view || _service->status().generation!=identity) return;
        if (result.outcome==handheld::Outcome::Ok) {
            constexpr char hex[]="0123456789abcdef";char text[33]{};
            const auto* key=_service->historyWindow().peer();
            for (size_t n=0;n<16;++n) {text[2*n]=hex[key[n]>>4];text[2*n+1]=hex[key[n]&15];}
            _peerHex=text;
        } else rrcNotice("Could not open hub history");
        _rrcRetryAt=result.outcome==handheld::Outcome::Ok?0:millis()+1000;
    });
    if (!id) _rrcRetryAt=millis()+1000;
}
void LvMessageView::loadRrcDraft() {
    if (!_service || !_rrcMode || _rrcLoaded || _rrcLoading || int32_t(millis()-_rrcRetryAt)<0) return;
    const auto view=_rrcView,identity=_rrcIdentity;
    _rrcLoading=true;
    const auto id=_service->requestRrcDraft(_rrcBinding,[this,view,identity](const handheld::Result& result,const handheld::rrc::DraftView* rows,size_t count) {
        if (_rrcView!=view || !_rrcMode) return;
        _rrcLoading=false;_rrcRetryAt=millis()+1000;
        if (_service->status().generation!=identity) return;
        if (result.outcome!=handheld::Outcome::Ok || count!=1) { rrcNotice("Could not load draft; retrying");return; }
        _inputText.assign(rows[0].text,rows[0].length);composerEdited();_rrcSavedRevision=_draftRevision;_rrcLoaded=true;
        _rrcStorageRevision=rows[0].storageRevision;_rrcUncertain=rows[0].uncertain;
        if (_rrcInsert[0]) {
            const auto length=handheld::ui::rrc_input::textPrefix(_rrcInsert,strlen(_rrcInsert),handheld::rrc::DraftCapacity-_inputText.size());
            _inputText.append(_rrcInsert,length);memset(_rrcInsert,0,sizeof _rrcInsert);composerEdited();
        }
        if (_screen) {updateComposerText();updateComposerState();}
    });
    if (!id) {_rrcLoading=false;_rrcRetryAt=millis()+1000;}
}
void LvMessageView::saveRrcDraft(bool immediate, bool forSend) {
    if (!_service || !_rrcMode || !rrcWritable() || !_rrcLoaded || _rrcSaving || _rrcClearPending ||
        _rrcIdentity!=_service->status().generation || (!forSend && _rrcSavedRevision==_draftRevision) ||
        (!immediate && (uint32_t(millis()-_rrcEditAt)<500 || int32_t(millis()-_rrcRetryAt)<0))) return;
    const auto view=_rrcView,identity=_rrcIdentity;const auto revision=_draftRevision;
    if (forSend && !_rrcStorageRevision) return;
    auto command=rrcCommand(handheld::rrc::Action::Draft);
    if (forSend) {command.flags=3;command.counter=_rrcStorageRevision;}
    _rrcSaving=true;
    const auto id=_service->rrcCommand(command,
        reinterpret_cast<const uint8_t*>(_inputText.data()),_inputText.size(),[this,view,identity,revision,forSend](const handheld::Result& result) {
            _rrcSaving=false;
            if (_rrcView!=view || !_rrcMode || _service->status().generation!=identity) return;
            if (result.outcome==handheld::Outcome::Ok) { _rrcSavedRevision=revision;_rrcStorageRevision=result.total;_rrcPreparedRevision=forSend?revision:0; }
            else {
                if (forSend && result.next==uint32_t(handheld::rrc::Code::Stale)) { _rrcSendRequested=false;rrcNotice("Saved draft changed; reopen before sending"); }
                else rrcNotice("Draft not saved; text kept");
            }
            _rrcRetryAt=result.outcome==handheld::Outcome::Ok?0:millis()+1000;
        });
    if (!id) {_rrcSaving=false;_rrcRetryAt=millis()+1000;}
}
void LvMessageView::clearConfirmedRrcDraft() {
    if (!_service || !_rrcClearPending || _rrcClearing || int32_t(millis()-_rrcRetryAt)<0) return;
    if (_rrcSendIdentity!=_service->status().generation) {_rrcClearPending=false;return;}
    handheld::rrc::Command command;command.action=handheld::rrc::Action::Draft;
    command.generation=_service->status().rrc.generation;command.flags=1;command.counter=_rrcSentStorageRevision;
    memcpy(command.hub,_rrcSendingBinding.hub,16);memcpy(command.participant,_rrcSendingBinding.participant,16);
    memcpy(command.room,_rrcSendingBinding.room,sizeof command.room);
    _rrcClearing=true;
    const auto id=_service->rrcCommand(command,nullptr,0,[this](const handheld::Result& result) {
        _rrcClearing=false;
        if (result.outcome==handheld::Outcome::Ok || result.next==uint32_t(handheld::rrc::Code::Stale)) {
            _rrcClearPending=false;
            if (result.outcome==handheld::Outcome::Ok && _rrcMode && _rrcView==_rrcClearView &&
                _draftRevision==_rrcClearEditorRevision && _rrcIdentity==_service->status().generation) {
                _rrcSavedRevision=_draftRevision;_rrcStorageRevision=result.total;
            }
        } else _rrcRetryAt=millis()+2000;
    },true);
    if (!id) {_rrcClearing=false;_rrcRetryAt=millis()+1000;}
}
void LvMessageView::pollRrc() {
    if (!_service) return;
    const auto& status=_service->status().rrc;
    if (_rrcSendId && status.sendRevision==_rrcSendId && status.sendSettled) {
        const bool confirmed=status.sending==handheld::rrc::SendPhase::Confirmed || status.sending==handheld::rrc::SendPhase::Transmitted;
        if (confirmed && _rrcSentStorageRevision && _rrcSendIdentity==_service->status().generation) {
            _rrcClearPending=true;_rrcRetryAt=0;_rrcClearView=0;
            const bool sameStoredDraft=_rrcMode && _rrcLoaded && _rrcSavedRevision==_draftRevision &&
                _rrcStorageRevision==_rrcSentStorageRevision && !memcmp(_rrcBinding.hub,_rrcSendingBinding.hub,16) &&
                !memcmp(_rrcBinding.participant,_rrcSendingBinding.participant,16) && !strcmp(_rrcBinding.room,_rrcSendingBinding.room);
            if (sameStoredDraft) {
                _inputText.clear();composerEdited();_rrcClearView=_rrcView;_rrcClearEditorRevision=_draftRevision;
            }
        }
        if (_rrcMode && _rrcSentView==_rrcView && _rrcIdentity==_service->status().generation) {
            _rrcUncertain=status.sending==handheld::rrc::SendPhase::Unconfirmed;
            rrcNotice(confirmed?status.sending==handheld::rrc::SendPhase::Confirmed?"Confirmed by hub":"Transmitted; no recipient receipt":
                _rrcUncertain?"Unconfirmed; draft kept":"Not sent; draft kept");
            if (_screen) {updateComposerText();updateComposerState();}
        }
        _rrcSendId=0;
    }
    clearConfirmedRrcDraft();
    if (!_rrcMode) return;
    if (_rrcIdentity!=_service->status().generation) {
        _inputText.clear();_peerHex.clear();memset(_rrcInsert,0,sizeof _rrcInsert);
        _rrcLoaded=false;_rrcMode=false;++_rrcView;clearMessages();
        if (_entered && _onBack) _onBack();
        return;
    }
    if (_entered && !_service->rrcHistory() && int32_t(millis()-_rrcRetryAt)>=0) enterRrcHistory();
    if (_entered) loadRrcDraft();
    saveRrcDraft();
    if (_rrcSendRequested) {
        if (_draftRevision!=_rrcRequestedRevision) _rrcSendRequested=false;
        else if (!_rrcSaving && _rrcSavedRevision==_draftRevision && _rrcPreparedRevision==_draftRevision) {
            _rrcSendRequested=false;_rrcEmote=_rrcRequestedEmote;sendRrcMessage();_rrcEmote=false;
        } else if (int32_t(millis()-_rrcRetryAt)>=0) saveRrcDraft(true,_rrcSavedRevision==_draftRevision && _rrcStorageRevision);
    }
}
void LvMessageView::sendRrcMessage() {
    if(!_service || _rrcIdentity!=_service->status().generation) return;
    if (!_service || !_rrcMode || !_rrcLoaded || !rrcWritable() || _inputText.empty() || _rrcSendId || _rrcClearPending) return;
    const auto& live=_service->status().rrc;
    if(memcmp(live.hub,_rrcBinding.hub,16) || live.phase!=handheld::rrc::Phase::Online) {showRrcGuidance(RrcTools::State::Connect);return;}
    if (_rrcUncertain && !_rrcResendConfirmed) {
        _rrcResendConfirmed=true;rrcNotice("May duplicate an unconfirmed send. Press Send again.");return;
    }
    const auto action=_rrcBinding.privateNotice()?handheld::rrc::Action::PrivateNotice:_rrcEmote?handheld::rrc::Action::Emote:handheld::rrc::Action::Message;
    const auto preview=handheld::ui::rrc_input::preview(_service->status().rrc,_rrcBinding,action,
        reinterpret_cast<const uint8_t*>(_inputText.data()),_inputText.size());
    if (preview.code!=handheld::rrc::Code::Ok) {
        if(preview.code==handheld::rrc::Code::Unsupported && _rrcEmote && !_rrcBinding.privateNotice()) {showRrcGuidance(RrcTools::State::Plain);return;}
        rrcNotice(_inputText[0]=='/'?"Use Advanced command in the channel menu":handheld::rrc::codeName(preview.code));return;
    }
    if (_rrcSaving || _rrcSavedRevision!=_draftRevision || _rrcPreparedRevision!=_draftRevision) {
        _rrcSendRequested=true;_rrcRequestedRevision=_draftRevision;_rrcRequestedEmote=_rrcEmote;
        saveRrcDraft(true,_rrcSavedRevision==_draftRevision && _rrcStorageRevision);rrcNotice("Saving draft before sending...");return;
    }
    const auto view=_rrcView;
    _rrcSendId=UINT32_MAX;_rrcSentView=view;_rrcSentDraftRevision=_draftRevision;
    _rrcSendingBinding=_rrcBinding;_rrcSentStorageRevision=_rrcStorageRevision;_rrcSendIdentity=_rrcIdentity;
    const auto id=_service->rrcCommand(rrcCommand(action),reinterpret_cast<const uint8_t*>(_inputText.data()),_inputText.size(),
        [this,view](const handheld::Result& result) {
            if (result.outcome!=handheld::Outcome::Ok) {
                _rrcSendId=0;
                if (_rrcMode && _rrcView==view) {
                    const auto code=handheld::rrc::Code(result.next);
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

void LvMessageView::openRrcTools(size_t index) {
    if(!_rrcMode || !_entered || !boundWindow()) return;
    auto& window=_service->historyWindow();const auto* row=window.span(index);
    if(!row || row->unavailable()) return;
    saveScroll();window.focusSpan(index);_rrcTools.begin(row->counter);
    showSendModeMenu();readRrcTool(RrcTools::Action::Read,true);
}
void LvMessageView::closeRrcTools() {
    if(_rrcTools.state==RrcTools::State::Identity || _rrcTools.state==RrcTools::State::Copy) {
        _rrcTools.state=RrcTools::State::Menu;_rrcTools.selected=0;showSendModeMenu();return;
    }
    _rrcTools.close();hideSendModeMenu();updateHistoryFocus();
}
void LvMessageView::activateRrcTool() {
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
    if(action==Action::Identity || action==Action::Copy) {
        _rrcTools.state=action==Action::Identity?State::Identity:State::Copy;
        _rrcTools.selected=action==Action::Copy?1:0;showSendModeMenu();return;
    }
    if(action==Action::Read) {
        auto& window=_service->historyWindow();
        for(size_t n=0;n<window.spanCount();++n) if(window.span(n)->counter==_rrcTools.counter) {
            if(window.openFull(n)) {_rrcTools.close();hideSendModeMenu();_scrollToEnd=false;refreshUI();}return;
        }
        rrcNotice("Message moved; reopen its actions");closeRrcTools();return;
    }
    readRrcTool(action);
}
void LvMessageView::readRrcTool(RrcTools::Action action,bool metadata) {
    if(!_service || !_entered || !_rrcMode || !_rrcTools.visible()) return;
    if(!metadata && (action==RrcTools::Action::Private || action==RrcTools::Action::Direct) && !leaveRrcDraft()) return;
    const auto view=_rrcView,revision=_rrcTools.revision,counter=_rrcTools.counter;
    _rrcTools.state=RrcTools::State::Loading;showSendModeMenu();
    const auto id=_service->requestRrcDetail(_rrcBinding,_service->historyWindow().peer(),counter,
        [this,view,revision,counter,action,metadata](const handheld::Result& result,const handheld::rrc::MessageDetail* rows,size_t count) {
        if(!_entered || !_screen || !_rrcMode || _rrcView!=view || _rrcTools.revision!=revision || !_rrcTools.visible() ||
            _rrcIdentity!=_service->status().generation) return;
        if(result.outcome!=handheld::Outcome::Ok || count!=1 || rows[0].counter!=counter) {_rrcTools.state=RrcTools::State::Failed;showSendModeMenu();return;}
        const auto& row=rows[0];
        if(metadata) {_rrcTools.accept(row);showSendModeMenu();return;}
        _rrcTools.state=RrcTools::State::Menu;showSendModeMenu();
        if(!row.kind) {rrcNotice("This is a local history marker");return;}
        if(action==RrcTools::Action::AcceptInvite) {
            char room[65];
            if(!row.invitation || !handheld::rrc::hubText::invitation(reinterpret_cast<const uint8_t*>(row.text),row.length,room)) {
                rrcNotice("This is not a recognized hub invitation");return;
            }
            const auto& live=_service->status().rrc;
            if(memcmp(live.hub,_rrcBinding.hub,16) || live.phase!=handheld::rrc::Phase::Online) {
                showRrcGuidance(RrcTools::State::Connect);return;
            }
            if(memcmp(row.source,live.identity,16)) {rrcNotice("Hub identity changed; reopen the invitation");return;}
            auto command=rrcCommand(handheld::rrc::Action::Join);memset(command.participant,0,16);strcpy(command.room,room);
            _rrcTools.state=RrcTools::State::Loading;showSendModeMenu();
            const auto id=_service->rrcCommand(command,nullptr,0,[this,view,revision](const handheld::Result& result) {
                if(!_entered || !_screen || !_rrcMode || _rrcView!=view || _rrcTools.revision!=revision || !_rrcTools.visible() ||
                    _rrcIdentity!=_service->status().generation) return;
                if(result.outcome==handheld::Outcome::Ok) {closeRrcTools();rrcNotice("Joining invited channel");}
                else {_rrcTools.state=RrcTools::State::Invitation;showSendModeMenu();const auto code=handheld::rrc::Code(result.next);
                    rrcNotice(handheld::rrc::codeName(code==handheld::rrc::Code::Ok?handheld::rrc::Code::Storage:code));}
            });
            if(!id) {_rrcTools.state=RrcTools::State::Invitation;showSendModeMenu();rrcNotice("Hub busy; try again");}return;
        }

        if(action==RrcTools::Action::Private || action==RrcTools::Action::Direct) {
            if(action==RrcTools::Action::Private && !(_service->status().rrc.capabilities&4)) {rrcNotice("Hub does not support private notices");return;}
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
        const auto available=handheld::rrc::DraftCapacity-_inputText.size();
        char insertion[handheld::rrc::DraftCapacity+1]{};size_t length=0;bool shortened=false;
        if(action==RrcTools::Action::Quote) length=RrcTools::quote(row,insertion,available+1,shortened);
        else if(action==RrcTools::Action::Mention) {char token[36];length=RrcTools::mention(row,token);if(length<=available) memcpy(insertion,token,length+1);else length=0;}
        else if(action==RrcTools::Action::Copy && row.length<=available) {length=row.length;memcpy(insertion,row.text,length);}
        if(!length) {rrcNotice("Draft has no room; text kept");return;}
        _inputText.append(insertion,length);composerEdited();_rrcTools.close();hideSendModeMenu();
        auto& window=_service->historyWindow();window.backToChat();window.focusSpan(HistoryWindow::VisibleSpans);
        updateComposerText();updateComposerState();updateHistoryFocus();
        rrcNotice(shortened?"Quote shortened to fit draft":"Added to draft; review before sending");
    });
    if(!id) {_rrcTools.state=RrcTools::State::Failed;showSendModeMenu();rrcNotice("History busy; try again");}
}

void LvMessageView::showRrcGuidance(RrcTools::State state) {
    _rrcSendRequested=false;_rrcTools.begin(0);_rrcTools.state=state;
    const auto& live=_service->status().rrc;
    _rrcTools.kind=live.phase!=handheld::rrc::Phase::Disconnected && memcmp(live.hub,_rrcBinding.hub,16);
    memcpy(_rrcTools.source,_rrcBinding.hub,16);_rrcTools.selected=state==RrcTools::State::Connect?3:1;
    showSendModeMenu();
}
void LvMessageView::activateRrcGuidance() {
    if(!_service || _rrcIdentity!=_service->status().generation) return;
    using State=RrcTools::State;const auto state=_rrcTools.state;const auto selected=_rrcTools.selected;
    if((state==State::Connect && selected==4) || (state==State::Join && selected==3) || (state==State::Plain && selected==2)) {closeRrcTools();return;}
    if(state==State::Plain) {
        if(selected==1) {closeRrcTools();_rrcEmote=false;sendRrcMessage();}return;
    }
    if(state==State::Join && selected==2) {
        if(!leaveRrcDraft()) return;_rrcTools.close();hideSendModeMenu();if(_onBack) _onBack();return;
    }
    if((state==State::Connect && selected!=3) || (state==State::Join && selected!=1)) return;
    auto action=handheld::rrc::Action::Join;
    if(state==State::Connect) {
        const auto& live=_service->status().rrc;const bool same=!memcmp(live.hub,_rrcBinding.hub,16);
        if(same && live.phase!=handheld::rrc::Phase::Disconnected && live.phase!=handheld::rrc::Phase::Recovering) {
            closeRrcTools();rrcNotice(live.phase==handheld::rrc::Phase::Online?"Connected; send when ready":"Connection in progress; draft kept");return;
        }
        action=same && live.phase==handheld::rrc::Phase::Recovering?handheld::rrc::Action::Retry:handheld::rrc::Action::Connect;
    }
    auto command=rrcCommand(action);memset(command.participant,0,16);
    const auto view=_rrcView,revision=_rrcTools.revision;
    _rrcTools.state=State::Loading;showSendModeMenu();
    const auto id=_service->rrcCommand(command,nullptr,0,[this,view,revision,state,action](const handheld::Result& result) {
        if(!_entered || !_screen || !_rrcMode || _rrcView!=view || _rrcTools.revision!=revision || !_rrcTools.visible() || _rrcIdentity!=_service->status().generation) return;
        if(result.outcome==handheld::Outcome::Ok) {closeRrcTools();rrcNotice(action==handheld::rrc::Action::Join?"Joining; send when ready":"Connecting; draft kept");}
        else {_rrcTools.state=state;showSendModeMenu();const auto code=handheld::rrc::Code(result.next);rrcNotice(handheld::rrc::codeName(code==handheld::rrc::Code::Ok?handheld::rrc::Code::Storage:code));}
    });
    if(!id) {_rrcTools.state=state;showSendModeMenu();rrcNotice("Hub busy; draft kept");}
}
