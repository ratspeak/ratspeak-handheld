void LvMessageView::openMessageTools(size_t index) {
    if(_rrcMode || !boundWindow() || !_service || _messageActionPending) return;
    uint8_t local[16];const auto& s=_service->status();
    if(!handheld::storage::decodeHex(s.destination,strnlen(s.destination,sizeof s.destination),local,16)) return;
    if(_messageTools.begin(_service->historyWindow(),index,local)) showSendModeMenu();
}
void LvMessageView::activateMessageTool() {
    using Menu=handheld::ui::MessageMenu;
    const auto action=_messageTools.action(_messageTools.selected);
    if(action==Menu::Action::Back || action==Menu::Action::Cancel) {_messageTools.close();hideSendModeMenu();return;}
    if(action==Menu::Action::Retry) {
        auto* memo=(_ui?_ui->memoUi():nullptr);if(memo) memo->retryMessage(_messageTools.key.peer,_messageTools.key.counter,_messageTools.key.incoming);
        _messageTools.close();hideSendModeMenu();return;
    }
    if(action==Menu::Action::Delete) {_messageTools.confirm();showSendModeMenu();return;}
    if(action==Menu::Action::Read) {
        auto& window=_service->historyWindow();
        for(size_t i=0;i<window.spanCount();++i) {
            const auto* row=window.span(i);
            if(row->counter==_messageTools.key.counter && row->incoming()==_messageTools.key.incoming) {
                saveScroll();window.openFull(i);_scrollToEnd=false;break;
            }
        }
        _messageTools.close();hideSendModeMenu();refreshUI();return;
    }
    if(_messageActionPending || !_service) return;
    const bool deleting=action==Menu::Action::Confirm;
    if(!deleting && action!=Menu::Action::Copy) return;
    const auto serial=_messageTools.serial,identity=_service->status().generation;
    _messageActionPending=true;
    _messageTools.state=deleting?Menu::State::Deleting:Menu::State::Copying;_messageTools.selected=0;
    uint32_t id=0;
    if(deleting) {
        auto* memo=_ui?_ui->memoUi():nullptr;
        if(memo && memo->inlinePlayback() && memo->status().counter==_messageTools.key.counter &&
           memo->status().incoming==_messageTools.key.incoming && !memcmp(memo->peer(),_messageTools.key.peer,16)) memo->hide();
        id=_service->deleteRecord(_messageTools.key,_messageTools.revision,[this,serial,identity](const handheld::Result& result) {
            _messageActionPending=false;
            if(!_entered || !_screen || !_service || _service->status().generation!=identity || _messageTools.serial!=serial ||
               _rrcMode || memcmp(_service->historyWindow().peer(),_messageTools.key.peer,16)) return;
            if(result.outcome==handheld::Outcome::Ok) {
                _messageTools.close();hideSendModeMenu();_service->historyWindow().backToChat();_service->historyWindow().focusSpan(HistoryWindow::VisibleSpans);_service->historyWindow().refresh();
            } else {_messageTools.fail("Message not deleted");showSendModeMenu();}
        });
    } else {
        id=_service->copyRecord(_messageTools.key,[this,serial,identity](const handheld::Result& result,const handheld::storage::StoredRecordHeader& header,const uint8_t* bytes,size_t count) {
            _messageActionPending=false;
            if(!_entered || !_screen || !_service || _service->status().generation!=identity || _messageTools.serial!=serial ||
               _rrcMode || memcmp(_service->historyWindow().peer(),_messageTools.key.peer,16)) return;
            char copied[MAX_COMPOSER_CHARS+2];
            const bool valid=result.outcome==handheld::Outcome::Ok && !result.more && _messageTools.copy(header,bytes,count,copied,MAX_COMPOSER_CHARS+1);
            const auto old=_inputText.size(),size=valid?strlen(copied):0;
            if(valid && old+size+(old?1:0)<=MAX_COMPOSER_CHARS && _nextDraftRevision!=UINT64_MAX) {
                if(old) {memmove(copied+1,copied,size+1);copied[0]='\n';}
                try {_inputText.append(copied,size+(old?1:0));}
                catch(const std::bad_alloc&) {_messageTools.fail("Not enough memory");showSendModeMenu();return;}
                composerEdited();updateComposerText();updateComposerState();
                _messageTools.close();hideSendModeMenu();_service->historyWindow().backToChat();_service->historyWindow().focusSpan(HistoryWindow::VisibleSpans);updateHistoryFocus();
            } else {_messageTools.fail(valid?"Not enough composer space":"Cannot copy full message");showSendModeMenu();}
        });
    }
    if(!id) {_messageActionPending=false;_messageTools.fail("Device busy; try again");}
    showSendModeMenu();
}
