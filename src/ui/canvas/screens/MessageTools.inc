void MessageView::openMessageTools(size_t index) {
    if(_rrcMode || !_store || !_backend || _actionTicket.valid()) return;
    uint8_t local[16];const auto identity=_backend->destinationHashHex();
    if(handheld::storage::decodeHex(identity.c_str(),identity.length(),local,16)) _messageTools.begin(_history,index,local);
}
void MessageView::activateMessageTool() {
    using Menu=handheld::ui::MessageMenu;
    const auto action=_messageTools.action(_messageTools.selected);
    if(action==Menu::Action::Back || action==Menu::Action::Cancel) {_messageTools.close();return;}
    if(action==Menu::Action::Retry) {
        auto* memo=_memo;if(memo) memo->retryMessage(_messageTools.key.peer,_messageTools.key.counter,_messageTools.key.incoming);
        _messageTools.close();return;
    }
    if(action==Menu::Action::Delete) {_messageTools.confirm();return;}
    if(action==Menu::Action::Read) {
        for(size_t i=0;i<_history.spanCount();++i) {
            const auto* row=_history.span(i);
            if(row->counter==_messageTools.key.counter && row->incoming()==_messageTools.key.incoming) {
                _history.openFull(i);break;
            }
        }
        _messageTools.close();return;
    }
    if(_actionTicket.valid() || !_store || !_backend) return;
    const bool deleting=action==Menu::Action::Confirm;
    if(!deleting && action!=Menu::Action::Copy) return;
    if(deleting) {
        if(!_backend->lxmfBeginRecordDelete(_messageTools.key)) {_messageTools.fail("Deletion busy; try again");return;}
        if(_memo && _memo->inlinePlayback() && _memo->status().counter==_messageTools.key.counter &&
           _memo->status().incoming==_messageTools.key.incoming && !memcmp(_memo->peer(),_messageTools.key.peer,16)) _memo->hide();
    }
    const auto submitted=deleting?_store->requestDeleteRecord(_messageTools.key,_messageTools.local,_messageTools.revision):
        _store->requestRecord(_messageTools.key,0,512);
    if(!submitted.accepted()) {
        if(deleting) {handheld::storage::Result failed;_backend->lxmfFinishRecordDelete(_messageTools.key,failed);}
        _messageTools.fail("Storage busy; try again");return;
    }
    _actionTicket=submitted.ticket;_actionSerial=_messageTools.serial;_actionDelete=deleting;_actionSettled=false;
    _messageTools.state=deleting?Menu::State::Deleting:Menu::State::Copying;_messageTools.selected=0;
}
bool MessageView::pollMessageAction() {
    if(!_actionTicket.valid() || !_store) return false;
    if(_actionSettled) {
        if(_store->releaseResult(_actionTicket)) {_actionTicket={};_actionSettled=false;}
        return true;
    }
    handheld::storage::Result result;if(!_store->peekResult(_actionTicket,result)) return false;
    const bool same=_visible && !_rrcMode && _messageTools.serial==_actionSerial &&
        !memcmp(_history.peer(),_messageTools.key.peer,16);
    if(_actionDelete) {
        if(_backend) _backend->lxmfFinishRecordDelete(_messageTools.key,result);
        if(same) {
            if(result.outcome==handheld::storage::Outcome::Committed) {
                _messageTools.close();_history.backToChat();_history.focusSpan(History::VisibleSpans);_history.refresh();_input.setActive(true);
            } else _messageTools.fail("Message not deleted");
        }
    } else if(same) {
        handheld::storage::StoredRecordHeader header;uint8_t bytes[408];char copied[402];
        bool valid=result.outcome==handheld::storage::Outcome::Committed && !result.more && result.length>=sizeof header &&
            result.length<=sizeof header+sizeof bytes && _store->readPayload(_actionTicket,&header,sizeof header);
        const size_t count=valid?result.length-sizeof header:0;
        valid=valid && _store->readPayload(_actionTicket,bytes,count,sizeof header) && _messageTools.copy(header,bytes,count,copied,401);
        const auto old=_input.getText().size();const auto size=valid?strlen(copied):0;
        if(valid && old+size+(old?1:0)<=DraftLength && _draftReady && _input.revision()!=UINT64_MAX) {
            if(old) {memmove(copied+1,copied,size+1);copied[0]='\n';}
            _input.appendText(copied,size+(old?1:0));_submissionEdited=true;
            if(_peerHex==_retainedPeer) _retainedDraft=_input.getText();
            _messageTools.close();_history.backToChat();_history.focusSpan(History::VisibleSpans);_input.setActive(true);
        } else _messageTools.fail(valid?"Not enough composer space":"Cannot copy full message");
    }
    _actionSettled=true;
    if(_store->releaseResult(_actionTicket)) {_actionTicket={};_actionSettled=false;}
    return true;
}
void MessageView::renderMessageTools(M5Canvas& canvas) {
    using Menu=handheld::ui::MessageMenu;
    const int y=Theme::CONTENT_Y;
    canvas.fillRect(0,y,Theme::CONTENT_W,Theme::CONTENT_H,Theme::BG);Theme::useSmallFont(canvas);
    canvas.setTextColor(Theme::ACCENT);canvas.drawString(_messageTools.title(),6,y+4);
    canvas.setTextColor(Theme::TEXT_SECONDARY);canvas.drawString(_messageTools.hint(),6,y+18);
    const bool confirm=_messageTools.state==Menu::State::Confirm;
    const int step=_messageTools.count()>4?12:15;
    for(unsigned n=0;n<_messageTools.count();++n) {
        const int x=confirm?6+int(n)*116:6,top=y+34+(confirm?0:int(n)*step),width=confirm?110:228;
        const bool selected=n==_messageTools.selected;
        canvas.fillRoundRect(x,top,width,confirm?25:step-1,2,selected?Theme::SELECTION_BG:Theme::BG_SURFACE);
        canvas.setTextColor(confirm && !n?Theme::ERROR:selected?Theme::PRIMARY:Theme::TEXT_PRIMARY);
        canvas.drawString(_messageTools.label(n),x+4,top+(confirm?8:3));
    }
}
