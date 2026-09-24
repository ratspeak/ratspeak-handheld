#include "LvMessagesScreen.h"
#include "Theme.h"
#include "LvTheme.h"
#include "LvInput.h"
#include "LxmFaceAvatar.h"
#include "fonts/fonts.h"
#include "reticulum/MessageStatusDetail.h"
#include "util/PerfTrace.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <limits>

namespace {
constexpr int AvatarSize = 32;
constexpr int TextX = 54;
constexpr int NavigationHeight = 30;
constexpr int NavigationSpace = NavigationHeight + 6;
constexpr int RowHeight = (Theme::CONTENT_H - 19 - NavigationSpace) / 2;
void drawNavigation(lv_event_t* event) {
    auto* button=lv_event_get_target(event);
    const auto index=reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button));
    lv_area_t area;lv_obj_get_coords(button,&area);
    const lv_coord_t cx=(area.x1+area.x2)/2,cy=(area.y1+area.y2)/2;
    const bool doubleArrow=index==0 || index==3;
    lv_draw_line_dsc_t stroke;lv_draw_line_dsc_init(&stroke);
    stroke.width=2;stroke.round_start=1;stroke.round_end=1;
    stroke.color=lv_color_hex(lv_obj_has_state(button,LV_STATE_DISABLED)?Theme::TEXT_MUTED:Theme::TEXT_PRIMARY);
    // Geometric icons keep the visible strokes centered independently of font
    // bearings. The complete button, including the icon, remains the hit area.
    for (int i=0;i<(doubleArrow?2:1);++i) {
        const lv_coord_t x=cx-(doubleArrow?7:3)+i*8;
        const lv_coord_t outer=index<2?x+6:x,tip=index<2?x:x+6;
        const lv_point_t points[]={{outer,static_cast<lv_coord_t>(cy-6)},
                                  {tip,cy},{outer,static_cast<lv_coord_t>(cy+6)}};
        lv_draw_line(lv_event_get_draw_ctx(event),&stroke,&points[0],&points[1]);
        lv_draw_line(lv_event_get_draw_ctx(event),&stroke,&points[1],&points[2]);
    }
}
void peerText(const uint8_t* peer, char (&out)[33]) {
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i=0;i<16;++i) { out[2*i]=hex[peer[i]>>4]; out[2*i+1]=hex[peer[i]&15]; }
    out[32]=0;
}
const char* statusText(uint8_t status) {
    switch (static_cast<LXMFStatus>(status)) {
        case LXMFStatus::QUEUED: return "QUEUED";
        case LXMFStatus::SENDING: return "SENDING";
        case LXMFStatus::SENT: return "SENT";
        case LXMFStatus::DELIVERED: return "DELIVERED";
        case LXMFStatus::FAILED: return "FAILED";
        case LXMFStatus::UNCONFIRMED: return "UNCONFIRMED";
        default: return "";
    }
}
lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int x, int y, int width) {
    auto* result=lv_label_create(parent);
    lv_obj_set_style_text_font(result,font,0);
    lv_obj_set_style_text_color(result,lv_color_hex(color),0);
    lv_obj_set_pos(result,x,y);lv_obj_set_width(result,width);
    lv_label_set_long_mode(result,LV_LABEL_LONG_CLIP);
    return result;
}
lv_obj_t* createEmptyState(lv_obj_t* parent) {
    // Match Contacts' centered icon/title/hint composition.
    auto* box=lv_obj_create(parent);
    lv_obj_set_size(box,252,94);lv_obj_center(box);
    lv_obj_set_style_bg_opa(box,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(box,0,0);lv_obj_set_style_pad_all(box,0,0);
    lv_obj_clear_flag(box,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);

    auto* bubble=lv_obj_create(box);
    lv_obj_set_pos(bubble,108,7);lv_obj_set_size(bubble,36,28);
    lv_obj_set_style_radius(bubble,6,0);lv_obj_set_style_bg_opa(bubble,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(bubble,2,0);
    lv_obj_set_style_border_color(bubble,lv_color_hex(Theme::PRIMARY),0);
    lv_obj_set_style_pad_all(bubble,0,0);
    lv_obj_clear_flag(bubble,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    static const lv_point_t tailPoints[]={{0,0},{0,8},{8,0}};
    auto* tail=lv_line_create(box);lv_line_set_points(tail,tailPoints,3);
    lv_obj_set_pos(tail,116,34);lv_obj_set_style_line_width(tail,2,0);
    lv_obj_set_style_line_color(tail,lv_color_hex(Theme::PRIMARY),0);
    for (int i=0;i<3;++i) {
        auto* dot=lv_obj_create(box);lv_obj_set_pos(dot,117+7*i,20);lv_obj_set_size(dot,3,3);
        lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_border_width(dot,0,0);
        lv_obj_set_style_bg_color(dot,lv_color_hex(Theme::BORDER_ACTIVE),0);
        lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);lv_obj_set_style_pad_all(dot,0,0);
        lv_obj_clear_flag(dot,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    }
    auto* title=label(box,&lv_font_rsdeck_14,Theme::TEXT_SECONDARY,0,49,252);
    lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text_static(title,"No conversations");
    auto* hint=label(box,&lv_font_rsdeck_10,Theme::TEXT_MUTED,0,70,252);
    lv_obj_set_style_text_align(hint,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text_static(hint,"Your chats appear here");
    return box;
}
}

bool LvMessagesScreen::bound() const {
    if (!_active || !_screen || !_service) return false;
    const auto& window=_service->conversationWindow();
    return window.visible() && window.statusReady() && _boundRevision==window.revision() &&
        _boundIdentity==window.identityGeneration() && _boundIdentity==_service->status().generation;
}
void LvMessagesScreen::notice(const char* text) {
    if (_ui) _ui->lvStatusBar().showToast(text,1500);
}
void LvMessagesScreen::createUI(lv_obj_t* parent) {
    _screen=parent;
    lv_obj_set_layout(parent,0);lv_obj_clear_flag(parent,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent,0,0);lv_obj_set_style_bg_color(parent,lv_color_hex(Theme::BG),0);
    _caption=label(parent,&lv_font_rsdeck_10,Theme::TEXT_SECONDARY,8,2,Theme::CONTENT_W-16);
    _list=lv_obj_create(parent);
    lv_obj_set_pos(_list,0,19);lv_obj_set_size(_list,Theme::CONTENT_W,Theme::CONTENT_H-51);
    lv_obj_add_style(_list,LvTheme::styleList(),0);
    lv_obj_add_style(_list,LvTheme::styleScrollbar(),LV_PART_SCROLLBAR);
    lv_obj_set_layout(_list,LV_LAYOUT_FLEX);lv_obj_set_flex_flow(_list,LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(_list,[](lv_event_t* event) {
        auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(event));
        // Programmatic layout/restore must never turn an anchored view into a
        // following view. Pointer, wheel and keyboard scrolling have an indev.
        if (lv_indev_get_act()) self->reportViewport();
    },LV_EVENT_SCROLL,this);
    _empty=label(parent,&lv_font_rsdeck_14,Theme::TEXT_SECONDARY,12,55,Theme::CONTENT_W-24);
    lv_obj_set_style_text_align(_empty,LV_TEXT_ALIGN_CENTER,0);
    _emptyState=createEmptyState(parent);
    _update=lv_btn_create(parent);
    lv_obj_set_pos(_update,Theme::CONTENT_W-116,0);lv_obj_set_size(_update,112,18);
    lv_obj_add_style(_update,LvTheme::styleListBtn(),0);
    lv_obj_add_style(_update,LvTheme::styleListBtnFocused(),LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(_update,0,0);
    auto* updateText=label(_update,&lv_font_rsdeck_10,Theme::TEXT_PRIMARY,0,0,112);
    lv_obj_set_style_text_align(updateText,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(updateText);
    lv_obj_add_event_cb(_update,[](lv_event_t* event) {
        static_cast<LvMessagesScreen*>(lv_event_get_user_data(event))->applyUpdate();
    },LV_EVENT_CLICKED,this);
    lv_group_add_obj(LvInput::group(),_update);
    for (size_t i=0;i<4;++i) {
        auto* button=lv_btn_create(parent);_navigation[i]=button;
        lv_obj_set_pos(button,4+i*(Theme::CONTENT_W-8)/4,Theme::CONTENT_H-NavigationHeight-3);
        lv_obj_set_size(button,(Theme::CONTENT_W-8)/4-3,NavigationHeight);
        lv_obj_add_style(button,LvTheme::styleListBtn(),0);
        lv_obj_add_style(button,LvTheme::styleListBtnFocused(),LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(button,0,0);lv_obj_set_user_data(button,reinterpret_cast<void*>(i));
        lv_obj_add_event_cb(button,drawNavigation,LV_EVENT_DRAW_MAIN,nullptr);
        lv_obj_add_event_cb(button,[](lv_event_t* event) {
            auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(event));
            self->navigate(static_cast<Navigation>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(event)))));
        },LV_EVENT_CLICKED,this);
        lv_group_add_obj(LvInput::group(),button);
    }
    updateCaptions();
}
void LvMessagesScreen::onEnter() {
    _active=true;
    _loadStarted=millis();_loadPending=true;
#if HAS_TOUCH
    _focusActive=false;
#endif
    hideActionMenu();
    if (_service) _service->watchConversations();
    refreshUI();
}
void LvMessagesScreen::detachRows() {
    _binding=true;
    if (_screen && _list) lv_obj_clean(_list);
    for (auto& row:_rows) row={};
    _rowCount=0;_boundRevision=0;_boundIdentity=0;_namesResolved=0;_nameFailed=false;
    _binding=false;
}
void LvMessagesScreen::onExit() {
    if (bound()) reportViewport();
    _active=false;hideActionMenu();detachRows();
    if (_service) {
        auto& window=_service->conversationWindow();
        _service->closeConversations();
        window.acknowledgePublication(window.revision());
    }
}
void LvMessagesScreen::destroyUI() {
    onExit();_list=nullptr;_caption=nullptr;_empty=nullptr;_update=nullptr;_emptyState=nullptr;
    for (auto& button:_navigation) button=nullptr;
    LvScreen::destroyUI();
}
void LvMessagesScreen::refreshUI() {
    if (!_screen || !_service) return;
    auto& window=_service->conversationWindow();
    if (!_active || !window.visible() || window.identityGeneration()!=_service->status().generation) {
        hideActionMenu();detachRows();
        window.acknowledgePublication(window.revision());
    } else if (window.statusReady() && (_boundRevision!=window.revision() || _boundIdentity!=window.identityGeneration())) {
        hideActionMenu();bindRows();
    }
    if (bound() && _statusRevision!=window.statusRevision()) updateStatuses();
    if (bound() && _nodeRevision!=(_am?_am->revision():0)) updateNames();
    if (bound() && !window.loading() && _lpState==LP_NONE) pollName();
    updateCaptions();
    if (_loadPending && bound() && !window.loading()) {
        _loadPending=false;
        const auto elapsed=PerfTrace::elapsedMs(_loadStarted);
        if (PerfTrace::shouldLog(elapsed,RSDECK_PERF_UI_TRACE_MS))
            Serial.printf("[PERF] Chats page=%u rows=%u ready in %lums\n",unsigned(_boundPage),unsigned(_rowCount),elapsed);
    }
}
void LvMessagesScreen::bindRows() {
    auto& window=_service->conversationWindow();
    detachRows();_binding=true;
    _boundRevision=window.revision();_boundIdentity=window.identityGeneration();
    _boundPage=window.pageNumber();
    _nodeRevision=_am?_am->revision():0;
    _rowCount=static_cast<uint8_t>(window.count());
    for (size_t i=0;i<_rowCount;++i) {
        const auto& value=*window.row(i);auto& widgets=_rows[i];
        char peer[33];peerText(value.peer,peer);
        widgets.row=lv_obj_create(_list);
        auto* row=widgets.row;
        lv_obj_set_size(row,Theme::CONTENT_W,RowHeight);
        lv_obj_add_style(row,LvTheme::styleListBtn(),0);
        lv_obj_add_style(row,LvTheme::styleListBtnFocused(),LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(row,0,0);lv_obj_clear_flag(row,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row,lv_color_hex(value.unreadCount?Theme::PRIMARY_SUBTLE:Theme::BG),0);
        lv_obj_set_user_data(row,reinterpret_cast<void*>(i));
        lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);
        // The first child is the existing unread/pending/contact status rail.
        auto* rail=lv_obj_create(row);
        lv_obj_set_pos(rail,0,0);lv_obj_set_size(rail,4,RowHeight);
        lv_obj_set_style_border_width(rail,0,0);lv_obj_set_style_radius(rail,0,0);
        lv_obj_set_style_pad_all(rail,0,0);lv_obj_set_style_bg_opa(rail,LV_OPA_COVER,0);
        lv_obj_clear_flag(rail,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row,[](lv_event_t* event) {
            auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(event));
            self->openRow(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(event))));
        },LV_EVENT_CLICKED,this);
        lv_obj_add_event_cb(row,[](lv_event_t* event) {
            auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(event));
            self->showActionMenu(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(event))));
        },LV_EVENT_LONG_PRESSED,this);
        lv_obj_add_event_cb(row,[](lv_event_t* event) {
            auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(event));
            if (self->_binding || !self->bound()) return;
            auto* target=lv_event_get_target(event);
            self->_service->conversationWindow().select(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)));
            lv_obj_scroll_to_view(target,LV_ANIM_OFF);self->reportViewport();
        },LV_EVENT_FOCUSED,this);
        lv_group_add_obj(LvInput::group(),row);
        // LVGL owns each bounded avatar buffer until its canvas is deleted.
        if (auto* pixels=lv_mem_alloc(LxmFaceAvatar::bufferSize(AvatarSize))) {
            auto avatar=LxmFaceAvatar::create(row,12,16,AvatarSize,pixels,Theme::PRIMARY_SUBTLE,
                                              value.unreadCount?Theme::PRIMARY:Theme::BORDER);
            lv_obj_add_event_cb(avatar.canvas,[](lv_event_t* event) { lv_mem_free(lv_event_get_user_data(event)); },LV_EVENT_DELETE,pixels);
            LxmFaceAvatar::render(avatar.canvas,String(peer));
        }
        widgets.name=label(row,&lv_font_rsdeck_14,Theme::TEXT_PRIMARY,TextX,4,Theme::CONTENT_W-TextX-58);
        lv_label_set_long_mode(widgets.name,LV_LABEL_LONG_DOT);
        const auto* node=_am?_am->findNodeByHex(peer):nullptr;
        if (node && !node->name.empty()) {
            lv_label_set_text(widgets.name,node->name.c_str());_namesResolved|=uint64_t{1}<<i;
        } else { char fallback[13];std::memcpy(fallback,peer,12);fallback[12]=0;lv_label_set_text(widgets.name,fallback); }
        if (value.timestamp>1700000000 && value.timestamp<static_cast<double>(std::numeric_limits<time_t>::max())) {
            const time_t stamp=static_cast<time_t>(value.timestamp);struct tm result;
            if (localtime_r(&stamp,&result)) {
                char clock[8];snprintf(clock,sizeof(clock),"%02d:%02d",result.tm_hour,result.tm_min);
                auto* time=label(row,&lv_font_rsdeck_10,Theme::TEXT_MUTED,Theme::CONTENT_W-47,6,43);
                lv_label_set_text(time,clock);
            }
        }
        widgets.preview=label(row,&lv_font_rsdeck_12,Theme::TEXT_SECONDARY,TextX,25,Theme::CONTENT_W-TextX-8);
        // CLIP never mutates borrowed bytes; DOT is forbidden for bank pointers.
        lv_label_set_text_static(widgets.preview,(value.flags&Row::Unavailable)?"Message unavailable":
            value.previewLength?value.preview:"No preview");
        if (value.flags&Row::PreviewTruncated) {
            lv_obj_set_width(widgets.preview,Theme::CONTENT_W-TextX-23);
            auto* more=label(row,&lv_font_rsdeck_12,Theme::TEXT_MUTED,Theme::CONTENT_W-19,25,15);
            lv_label_set_text_static(more,"...");
        }
        widgets.status=label(row,&lv_font_rsdeck_10,Theme::TEXT_MUTED,TextX,46,Theme::CONTENT_W-TextX-8);
    }
    updateStatuses();
    lv_obj_update_layout(_list);
    const auto selected=window.selectedIndex();
    if (selected<_rowCount) lv_group_focus_obj(_rows[selected].row);
    lv_obj_scroll_to_y(_list,window.scrollOffset(),LV_ANIM_OFF);
#if HAS_TOUCH
    if (!_focusActive) {
        auto* focused=lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_clear_state(focused,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY);
    }
#endif
    _binding=false;
    window.acknowledgePublication(_boundRevision);
}
void LvMessagesScreen::updateStatuses() {
    if (!_service) return;
    const auto& window=_service->conversationWindow();
    for (size_t i=0;i<_rowCount;++i) {
        const auto* value=window.row(i);if (!value) continue;
        char text[128]={};const char* detail=nullptr;
        if (value->flags&Row::Unavailable) detail="Read failed; tap Retry";
        else if (value->flags&Row::StatusUnavailable) detail="Status unavailable";
        else detail=messageStatusDetail(static_cast<LXMFStatus>(value->status),value->flags&Row::StatusPending,
            value->error,value->flags&Row::TxSuppressed);
        const char* state=(value->flags&Row::HasOutgoing)?statusText(value->status):"";
        if (detail) snprintf(text,sizeof(text),"%s%s%s",state,*state?" · ":"",detail);
        else if (value->unreadCount) snprintf(text,sizeof(text),"%u new%s%s",unsigned(value->unreadCount),*state?" · ":"",state);
        else snprintf(text,sizeof(text),"%s",state);
        lv_label_set_text(_rows[i].status,text);
        const auto color=(value->flags&(Row::Unavailable|Row::StatusUnavailable))?Theme::WARNING_CLR:
            value->status==static_cast<uint8_t>(LXMFStatus::FAILED)?Theme::ERROR_CLR:
            value->status==static_cast<uint8_t>(LXMFStatus::DELIVERED)?Theme::SUCCESS:Theme::TEXT_MUTED;
        lv_obj_set_style_text_color(_rows[i].status,lv_color_hex(color),0);
        char peer[33];peerText(value->peer,peer);
        const auto* node=_am?_am->findNodeByHex(peer):nullptr;
        const auto rail=(value->flags&Row::Unavailable)?Theme::WARNING_CLR:
            value->status==static_cast<uint8_t>(LXMFStatus::FAILED)?Theme::ERROR_CLR:
            value->pendingCount?Theme::WARNING_CLR:value->unreadCount?Theme::PRIMARY:
            node&&node->saved?Theme::TEXT_SECONDARY:node?Theme::TEXT_MUTED:Theme::BORDER;
        lv_obj_set_style_bg_color(lv_obj_get_child(_rows[i].row,0),lv_color_hex(rail),0);
    }
    _statusRevision=window.statusRevision();
}
void LvMessagesScreen::updateCaptions() {
    if (!_screen || !_caption) return;
    const auto* window=_service?&_service->conversationWindow():nullptr;
    char text[100];
    if (!window || !_active) snprintf(text,sizeof(text),"Conversations closed");
    else snprintf(text,sizeof(text),"Page %u",unsigned(_rowCount?_boundPage:window->pageNumber()));
    lv_label_set_text(_caption,text);
    const bool emptyReady=window && _active && !_rowCount && window->state()==Window::State::Ready &&
        !window->loading() && window->statusReady();
    if (emptyReady) lv_obj_clear_flag(_emptyState,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(_emptyState,LV_OBJ_FLAG_HIDDEN);
    if (!_rowCount && !emptyReady) {
        const char* empty=!window || !_active?"":
            window->state()==Window::State::Retrying || window->state()==Window::State::Exhausted?"Unable to read conversations":
            window->loading() || !window->statusReady()?"Loading conversations...":"No conversations";
        lv_label_set_text(_empty,empty);lv_obj_clear_flag(_empty,LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(_empty,LV_OBJ_FLAG_HIDDEN);
    bool retry=_nameFailed;
    if (window && bound()) for (size_t i=0;i<window->count();++i)
        retry|=bool(window->row(i)->flags&(Row::Unavailable|Row::StatusUnavailable));
    const bool update=window && _active && window->state()!=Window::State::Exhausted &&
        (retry || !window->freshnessAvailable());
    lv_label_set_text_static(lv_obj_get_child(_update,0),retry?"Retry":"Check updates");
    if (update) lv_obj_clear_flag(_update,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(_update,LV_OBJ_FLAG_HIDDEN);
    for (size_t i=0;i<4;++i) {
        const bool enabled=window && _active && !window->loading() && (i<2?window->canPrevious():window->canNext());
        if (enabled) lv_obj_clear_state(_navigation[i],LV_STATE_DISABLED);
        else lv_obj_add_state(_navigation[i],LV_STATE_DISABLED);
    }
    // Keep paging controls visible at a stable position, including empty lists.
    // Recovery stays in the header; both previews always fit above the arrows.
    const int top=19;
    _binding=true;
    const int height=Theme::CONTENT_H-top-NavigationSpace;
    if (lv_obj_get_y(_list)!=top || lv_obj_get_height(_list)!=height) {
        lv_obj_set_y(_list,top);lv_obj_set_height(_list,height);lv_obj_update_layout(_list);
    }
    auto* focus=lv_group_get_focused(LvInput::group());
    if (focus && (lv_obj_has_flag(focus,LV_OBJ_FLAG_HIDDEN) || lv_obj_has_state(focus,LV_STATE_DISABLED))) {
        if (_rowCount) {
            const auto selected=window?window->selectedIndex():RowCount;
            lv_group_focus_obj(_rows[selected<_rowCount?selected:0].row);
        }
        else if (update) lv_group_focus_obj(_update);
        else lv_obj_clear_state(focus,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY);
    }
    _binding=false;
}
void LvMessagesScreen::reportViewport() {
    if (_binding || !bound()) return;
    auto& window=_service->conversationWindow();
    const auto offset=std::max<int>(0,lv_obj_get_scroll_y(_list));
    window.setScrollOffset(offset);
}
void LvMessagesScreen::navigate(Navigation navigation) {
    if (!_active || !_service || _lpState!=LP_NONE) return;
    auto& window=_service->conversationWindow();
    if (bound()) reportViewport();
    _loadStarted=millis();_loadPending=true;
    switch (navigation) {
        case Navigation::Previous:window.previous();break;
        case Navigation::Next:window.nextPage();break;
        case Navigation::First:if (window.canPrevious()) window.first();break;
        case Navigation::Last:window.last();break;
    }
    refreshUI();
}
void LvMessagesScreen::applyUpdate() {
    if (!_active || !_service || _lpState!=LP_NONE || lv_obj_has_flag(_update,LV_OBJ_FLAG_HIDDEN)) return;
    auto& window=_service->conversationWindow();
    if (bound()) reportViewport();
    _loadStarted=millis();_loadPending=true;
    window.refresh();
    _namesResolved=0;_nameFailed=false;
    refreshUI();
}
void LvMessagesScreen::openRow(size_t index) {
    if (!bound() || _lpState!=LP_NONE || index>=_rowCount || !_onOpen) return;
    auto& window=_service->conversationWindow();window.select(index);reportViewport();
    char peer[33];peerText(window.row(index)->peer,peer);
    _onOpen(std::string(peer));
}
int LvMessagesScreen::focusedIndex() const {
    auto* focus=lv_group_get_focused(LvInput::group());
    for (size_t i=0;i<_rowCount;++i) if (_rows[i].row==focus) return static_cast<int>(i);
    return -1;
}
void LvMessagesScreen::pollName() {
    if (_nameRequest || !bound()) return;
    auto& window=_service->conversationWindow();
    for (size_t i=0;i<_rowCount;++i) {
        const uint64_t bit=uint64_t{1}<<i;if (_namesResolved&bit) continue;
        const auto* row=window.row(i);char peer[33];peerText(row->peer,peer);
        const uint32_t revision=_boundRevision,identity=_boundIdentity,nodes=_nodeRevision;
        const uint32_t advertisedNodes=_service->status().nodeRevision;
        // The callback's fixed peer value belongs to the single admitted query.
        std::array<uint8_t,16> expected;std::memcpy(expected.data(),row->peer,16);
        _nameRequest=_service->requestPeerName(peer,[this,i,revision,identity,nodes,advertisedNodes,expected](const handheld::Result& result,const char* name) {
            _nameRequest=0;
            if (!bound() || _boundRevision!=revision || _boundIdentity!=identity ||
                _service->status().nodeRevision!=advertisedNodes || (_am?_am->revision():0)!=nodes || i>=_rowCount ||
                std::memcmp(_service->conversationWindow().row(i)->peer,expected.data(),16)) return;
            _namesResolved|=uint64_t{1}<<i;
            if (result.outcome==handheld::Outcome::Ok && name && *name) lv_label_set_text(_rows[i].name,name);
            if (result.outcome!=handheld::Outcome::Ok) _nameFailed=true;
        });
        // Admission pressure waits for explicit Retry; never spin per frame.
        if (!_nameRequest) { _namesResolved|=bit;_nameFailed=true; }
        return;
    }
}
void LvMessagesScreen::updateNames() {
    _nodeRevision=_am?_am->revision():0;_namesResolved=0;_nameFailed=false;
    if (!_am) return;
    const auto& window=_service->conversationWindow();
    for (size_t i=0;i<_rowCount;++i) {
        char peer[33];peerText(window.row(i)->peer,peer);
        const auto* node=_am->findNodeByHex(peer);
        if (node && !node->name.empty()) {
            lv_label_set_text(_rows[i].name,node->name.c_str());_namesResolved|=uint64_t{1}<<i;
        }
    }
    updateStatuses(); // Contact status also contributes to the existing rail.
}
void LvMessagesScreen::showActionMenu(size_t index) {
    if (!bound() || index>=_rowCount || _actionRequest) return;
    auto& window=_service->conversationWindow();window.select(index);
    std::memcpy(_actionPeer,window.row(index)->peer,16);_actionIdentity=_boundIdentity;
    _focusActive=true;_lpState=LP_MENU;_menuIdx=0;rebuildActionOverlay();
}
void LvMessagesScreen::hideActionMenu() {
    if (_actionOverlay) lv_obj_del(_actionOverlay);
    _actionOverlay=nullptr;for (auto& row:_actionRows) row=nullptr;
    _lpState=LP_NONE;_menuIdx=0;_actionIdentity=0;std::memset(_actionPeer,0,16);
}
void LvMessagesScreen::rebuildActionOverlay() {
    if (_actionOverlay) lv_obj_del(_actionOverlay);
    _actionOverlay=lv_obj_create(lv_layer_top());lv_obj_set_size(_actionOverlay,244,124);lv_obj_center(_actionOverlay);
    lv_obj_add_style(_actionOverlay,LvTheme::styleModal(),0);lv_obj_set_style_pad_all(_actionOverlay,6,0);
    lv_obj_clear_flag(_actionOverlay,LV_OBJ_FLAG_SCROLLABLE);
    const bool confirm=_lpState==LP_CONFIRM_DELETE;
    auto* title=label(_actionOverlay,&lv_font_rsdeck_12,Theme::ACCENT,3,0,220);
    lv_label_set_text_static(title,confirm?"DELETE CHAT?":"CHAT ACTION");
    const char* labels[]={confirm?"Delete Chat":"Add Friend",confirm?"Cancel":"Delete Chat","Cancel"};
    for (size_t i=0;i<(confirm?2u:3u);++i) {
        auto* row=lv_btn_create(_actionOverlay);_actionRows[i]=row;
        lv_obj_set_pos(row,3,23+28*i);lv_obj_set_size(row,220,24);
        lv_obj_add_style(row,LvTheme::styleListBtn(),0);
        lv_obj_set_style_bg_color(row,lv_color_hex(i==_menuIdx?Theme::BG_HOVER:Theme::BG),0);
        lv_obj_set_style_pad_all(row,0,0);lv_obj_set_user_data(row,reinterpret_cast<void*>(i));
        auto* text=label(row,&lv_font_rsdeck_12,Theme::TEXT_PRIMARY,0,0,210);
        lv_label_set_text_static(text,labels[i]);lv_obj_center(text);
        lv_obj_add_event_cb(row,[](lv_event_t* event) {
            auto* self=static_cast<LvMessagesScreen*>(lv_event_get_user_data(event));
            self->_menuIdx=reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(event)));
            KeyEvent enter{};enter.enter=true;self->handleKey(enter);
        },LV_EVENT_CLICKED,this);
    }
}
void LvMessagesScreen::performAction(handheld::Operation operation) {
    if (!_service || _actionRequest || _actionIdentity!=_service->status().generation || !_actionIdentity) {
        hideActionMenu();return;
    }
    char peer[33];peerText(_actionPeer,peer);const uint32_t identity=_actionIdentity;
    hideActionMenu();
    _actionRequest=_service->action(operation,peer,"",0,[this,identity](const handheld::Result& result) {
        _actionRequest=0;
        if (!_service || identity!=_service->status().generation) return;
        if (result.outcome==handheld::Outcome::Ok) {
            _service->conversationWindow().refresh();
            if (_active && _screen) notice("Saved");
        } else if (_active && _screen) notice("Action failed; retry");
    });
    if (!_actionRequest) notice("Busy; try again");
}
bool LvMessagesScreen::handleLongPress() {
    if (!bound()) return false;
#if HAS_TOUCH
    if (!_focusActive) return false;
#endif
    const int index=focusedIndex();if (index<0) return false;
    showActionMenu(index);return true;
}
bool LvMessagesScreen::handleKey(const KeyEvent& event) {
    if (!_service || !_active) return false;
    if (_lpState!=LP_NONE) {
        if (_actionIdentity!=_service->status().generation) {hideActionMenu();return true;}
        const bool confirm=_lpState==LP_CONFIRM_DELETE;const unsigned count=confirm?2:3;
        if (event.up || event.down) {_menuIdx=(_menuIdx+count+(event.down?1:-1))%count;rebuildActionOverlay();return true;}
        if (event.enter || event.character=='\n' || event.character=='\r') {
            if (confirm) {if (_menuIdx==0) performAction(handheld::Operation::DeleteConversation);else hideActionMenu();}
            else if (_menuIdx==0) performAction(handheld::Operation::SaveContact);
            else if (_menuIdx==1) {_lpState=LP_CONFIRM_DELETE;_menuIdx=0;rebuildActionOverlay();}
            else hideActionMenu();
            return true;
        }
        if (!event.repeat && (event.del || event.character==8 || event.character==0x1b)) hideActionMenu();
        return true;
    }
#if HAS_TOUCH
    if (!_focusActive && (event.up || event.down || event.enter)) {
        _focusActive=true;auto* focused=lv_group_get_focused(LvInput::group());
        if (focused) lv_obj_add_state(focused,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY);
        return true;
    }
#endif
    return false;
}
