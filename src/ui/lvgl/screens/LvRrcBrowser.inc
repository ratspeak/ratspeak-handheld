// Included by LvMessagesScreen.cpp; widgets share the existing Messages screen.
void LvMessagesScreen::clearRrc() {
    if (_rrcText) {
        auto* text = const_cast<char*>(lv_textarea_get_text(_rrcText));
        const auto length = text ? std::strlen(text) : 0;
        volatile char* bytes = text; for (size_t n=0;n<length;++n) bytes[n]=0;
    }
    if (_rrcPanel) lv_obj_del(_rrcPanel);
    _rrcPanel=nullptr;_rrcText=nullptr;_rrcRemember=nullptr;
    for (auto& row:_rrcRows) row=nullptr;
    _rrcRendered=0;_rrcFormView=0;
}
void LvMessagesScreen::switchFamily(bool direct) {
    if (!_active || !_service || _rrc.navigation.direct()==direct) return;
    hideActionMenu();detachRows();clearRrc();
    _rrc.navigation.direct(direct);_rrc.refresh();
    if (direct) {
        _service->watchConversations();
        for (auto* button:_navigation) lv_obj_clear_flag(button,LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_caption,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(_list,LV_OBJ_FLAG_HIDDEN);
    } else {
        _service->closeConversations();
        for (auto* button:_navigation) lv_obj_add_flag(button,LV_OBJ_FLAG_HIDDEN);
        for (auto* object:{_caption,_list,_empty,_emptyState,_update}) lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
    }
    refreshUI();
}
void LvMessagesScreen::pollRrc() {
    if (!_service) return;
    _rrc.observe(_service->status().rrc,_service->status().generation);
    if (!_rrc.needsRows() || int32_t(millis()-_rrcRetryAt)<0) return;
    const auto view=_rrc.beginRows(), identity=_service->status().generation, session=_service->status().rrc.generation;
    const auto page=_rrc.navigation.page();
    auto completion=[this,view,identity,session,page](const handheld::Result& result,const auto* rows,size_t count) {
        const bool current=_active && !_rrc.navigation.direct() && identity==_service->status().generation &&
            session==_service->status().rrc.generation && page==_rrc.navigation.page();
        if (!current || result.outcome==handheld::Outcome::Stale || result.outcome==handheld::Outcome::NotReady) _rrc.cancelRows(view);
        else if (_rrc.completeRows(view,result.outcome==handheld::Outcome::Ok))
            _rrc.rows(rows,count,result.more,page==handheld::ui::RrcNavigation::Page::Directory || page==handheld::ui::RrcNavigation::Page::Channels || page==handheld::ui::RrcNavigation::Page::SavedRooms);
        _rrcRetryAt=millis()+500;
    };
    uint32_t request=0;
    using Page=handheld::ui::RrcNavigation::Page;
    switch (page) {
    case Page::RoomInfo: case Page::Notifications: request=_service->requestRrcRoom(_rrc.navigation.conversation(),completion);break;
    case Page::Hubs: case Page::HubInfo: request=_service->requestRrcHubs(completion);break;
    case Page::Channels: request=_service->requestRrcChannels(_rrc.navigation.offset(),completion);break;
    case Page::People:
        if (!_rrc.navigation.conversationCurrent()) { _rrc.completeRows(view,false);return; }
        request=_service->requestRrcPeople(_rrc.navigation.conversation().room,completion);break;
    case Page::Directory: request=_service->requestRrcDirectory(_rrc.navigation.offset(),completion);break;
    case Page::SavedRooms: request=_service->requestRrcSavedRooms(_rrc.navigation.conversation().hub,_rrc.navigation.offset(),completion);break;
    case Page::Inbox: request=_service->requestRrcInbox(_rrc.navigation.conversation().hub,uint32_t(_rrc.navigation.offset()),
        _rrc.navigation.after()?handheld::storage::HistoryDirection::After:handheld::storage::HistoryDirection::Before,completion);break;
    default: break;
    }
    if (!request) { _rrc.cancelRows(view);_rrcRetryAt=millis()+500; }
}
void LvMessagesScreen::renderRrc() {
    if (!_screen || !_active) return;
    // Status updates may refresh the surrounding lists, never erase a form.
    if (_rrc.form() && _rrcPanel && _rrcFormView==_rrc.navigation.revision()) return;
    if (_rrcPanel && _rrcRendered==_rrc.revision()) return;
    const auto selected=_rrc.navigation.selected();
    _binding=true;
    clearRrc();
    for (auto* button:_navigation) lv_obj_add_flag(button,LV_OBJ_FLAG_HIDDEN);
    for (auto* object:{_caption,_list,_empty,_emptyState,_update}) lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
    _rrcPanel=lv_obj_create(_screen);
    lv_obj_set_pos(_rrcPanel,0,FamilyHeight);lv_obj_set_size(_rrcPanel,Theme::CONTENT_W,Theme::CONTENT_H-FamilyHeight);
    lv_obj_set_style_pad_all(_rrcPanel,4,0);lv_obj_set_style_pad_row(_rrcPanel,3,0);
    lv_obj_set_style_bg_color(_rrcPanel,lv_color_hex(Theme::BG),0);lv_obj_set_style_border_width(_rrcPanel,0,0);
    lv_obj_set_flex_flow(_rrcPanel,LV_FLEX_FLOW_COLUMN);lv_obj_add_style(_rrcPanel,LvTheme::styleScrollbar(),LV_PART_SCROLLBAR);
    if (_rrc.navigation.page()!=handheld::ui::RrcNavigation::Page::Channels) {
        auto* title=label(_rrcPanel,&lv_font_rsdeck_12,Theme::TEXT_SECONDARY,0,0,Theme::CONTENT_W-16);
        lv_label_set_text(title,_rrc.heading());lv_obj_set_height(title,lv_font_get_line_height(&lv_font_rsdeck_12));
    }
    auto button=[this](const char* text,size_t index) {
        auto* item=lv_btn_create(_rrcPanel);lv_obj_set_size(item,Theme::CONTENT_W-12,29);
        lv_obj_add_style(item,LvTheme::styleListBtn(),0);lv_obj_add_style(item,LvTheme::styleListBtnFocused(),LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(item,4,0);lv_obj_clear_flag(item,LV_OBJ_FLAG_SCROLLABLE);
        auto* caption=label(item,&lv_font_rsdeck_12,Theme::TEXT_PRIMARY,0,0,Theme::CONTENT_W-24);
        lv_obj_set_height(caption,lv_font_get_line_height(&lv_font_rsdeck_12));lv_label_set_long_mode(caption,LV_LABEL_LONG_DOT);lv_label_set_text(caption,text);
        lv_obj_set_user_data(item,reinterpret_cast<void*>(index));lv_group_add_obj(LvInput::group(),item);return item;
    };
    if (_rrc.form()) {
        _rrcFormView=_rrc.navigation.revision();
        _rrcText=lv_textarea_create(_rrcPanel);lv_obj_set_size(_rrcText,Theme::CONTENT_W-12,32);
        lv_textarea_set_one_line(_rrcText,true);lv_textarea_set_max_length(_rrcText,_rrc.formLimit());
        lv_obj_add_style(_rrcText,LvTheme::styleTextarea(),0);lv_obj_add_style(_rrcText,LvTheme::styleTextareaFocused(),LV_STATE_FOCUSED);
        lv_obj_set_style_text_font(_rrcText,&lv_font_rsdeck_12,0);
        lv_obj_add_event_cb(_rrcText,[](lv_event_t* event) {
            auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(event));
            const auto* inserted=static_cast<const char*>(lv_event_get_param(event));
            if (inserted && std::strlen(lv_textarea_get_text(self->_rrcText))+std::strlen(inserted)>self->_rrc.formLimit())
                lv_textarea_set_insert_replace(self->_rrcText,"");
        },LV_EVENT_INSERT,this);
        lv_group_add_obj(LvInput::group(),_rrcText);
        if (_rrc.navigation.page()==handheld::ui::RrcNavigation::Page::RoomKey) {
            // Keep one owned plaintext buffer; password mode makes an additional
            // untracked copy in LVGL. The whole form is wiped on submit/exit.
            _rrcRemember=button("Remember after joining: OFF",0);
            lv_obj_add_flag(_rrcRemember,LV_OBJ_FLAG_CHECKABLE);
            lv_obj_add_event_cb(_rrcRemember,[](lv_event_t* event) {
                auto* button=lv_event_get_target(event);
                lv_label_set_text(lv_obj_get_child(button,0),lv_obj_has_state(button,LV_STATE_CHECKED)?
                    "Remember after joining: ON":"Remember after joining: OFF");
            },LV_EVENT_VALUE_CHANGED,nullptr);
        }
        auto* submit=button(_rrc.formAction(),0);
        lv_obj_add_event_cb(submit,[](lv_event_t* e) { static_cast<LvMessagesScreen*>(lv_event_get_user_data(e))->submitRrc(); },LV_EVENT_CLICKED,this);
        auto* cancel=button("Back",1);
        lv_obj_add_event_cb(cancel,[](lv_event_t* e) {
            auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(e));self->_rrc.back();self->renderRrc();
        },LV_EVENT_CLICKED,this);
        lv_group_focus_obj(_rrcText);
    } else if (!_rrc.ready()) {
        auto* loading=label(_rrcPanel,&lv_font_rsdeck_12,Theme::TEXT_MUTED,0,0,Theme::CONTENT_W-16);
        lv_label_set_text(loading,"Loading...");
        auto* back=button("Back",0);
        lv_obj_add_event_cb(back,[](lv_event_t* event) {
            auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(event));self->_rrc.back();self->renderRrc();
        },LV_EVENT_CLICKED,this);
    } else {
        for (size_t n=0;n<_rrc.count() && n<sizeof _rrcRows/sizeof _rrcRows[0];++n) {
            char text[112];_rrc.label(n,_service->status().rrc,text,sizeof text);
            auto* item=_rrcRows[n]=button(text,n);
            lv_obj_add_event_cb(item,[](lv_event_t* e) {
                auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(e));
                self->_rrc.activate(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(e))),self->_service->status().rrc);
                if (self->_active) self->renderRrc();
            },LV_EVENT_CLICKED,this);
            lv_obj_add_event_cb(item,[](lv_event_t* e) {
                auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(e));
                if (self->_binding) return;
                self->_rrc.navigation.select(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(e))));
                lv_obj_scroll_to_view(lv_event_get_target(e),LV_ANIM_OFF);
            },LV_EVENT_FOCUSED,this);
            if (!n && _rrc.navigation.page()==handheld::ui::RrcNavigation::Page::Channels) {
                auto* divider=lv_obj_create(_rrcPanel);lv_obj_set_size(divider,Theme::CONTENT_W-16,1);
                lv_obj_set_style_pad_all(divider,0,0);lv_obj_set_style_border_width(divider,0,0);
                lv_obj_set_style_bg_color(divider,lv_color_hex(Theme::DIVIDER),0);
                lv_obj_set_style_bg_opa(divider,LV_OPA_COVER,0);lv_obj_set_style_radius(divider,0,0);
                lv_obj_clear_flag(divider,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
            }
        }
        if (selected<_rrc.count() && selected<sizeof _rrcRows/sizeof _rrcRows[0] && _rrcRows[selected]) lv_group_focus_obj(_rrcRows[selected]);
    }
    _binding=false;
    _rrcRendered=_rrc.revision();
}
void LvMessagesScreen::submitRrc() {
    if (!_rrcText || !_service) return;
    const char* text=lv_textarea_get_text(_rrcText);
    if (_rrc.submit(text,std::strlen(text),_service->status().rrc,_rrcRemember && lv_obj_has_state(_rrcRemember,LV_STATE_CHECKED))) renderRrc();
}
bool LvMessagesScreen::handleRrcKey(const KeyEvent& event) {
    if (_rrc.form()) {
        if (event.character==0x1b && !event.repeat) { _rrc.back();renderRrc();return true; }
        if (event.enter) {
            if (event.repeat) return true;
            // Buttons keep their native Enter action, including the key-memory
            // toggle and Back. Enter in the editor remains the submit shortcut.
            if (lv_group_get_focused(LvInput::group())!=_rrcText) return false;
            submitRrc();return true;
        }
        if (_rrcText && (event.del || event.character==8)) { lv_textarea_del_char(_rrcText);return true; }
        if (_rrcText && event.character>=0x20 && event.character<=0x7e) { char text[]={event.character,0};lv_textarea_add_text(_rrcText,text);return true; }
        return false;
    }
    if (!event.repeat && (event.del || event.character==8 || event.character==0x1b)) { _rrc.back();renderRrc();return true; }
    return false; // Native focus/touch dispatches the same indexed action.
}
