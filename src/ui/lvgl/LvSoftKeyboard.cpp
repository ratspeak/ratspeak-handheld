#include "LvSoftKeyboard.h"
#if HAS_SOFT_KEYBOARD
#include <lvgl.h>
#include <cstring>
#include "Theme.h"
#include "UIManager.h"

namespace LvSoftKeyboard {
namespace {

enum class Layout : uint8_t { Lower, Upper, Symbols, More, Adjust };

#define KB_ROW "\n"
const char* lowerMap[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", KB_ROW,
    "a", "s", "d", "f", "g", "h", "j", "k", "l", KB_ROW,
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, KB_ROW,
    "123", ",", " ", ".", LV_SYMBOL_NEW_LINE, LV_SYMBOL_KEYBOARD, ""};
const char* upperMap[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", KB_ROW,
    "A", "S", "D", "F", "G", "H", "J", "K", "L", KB_ROW,
    LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, KB_ROW,
    "123", ",", " ", ".", LV_SYMBOL_NEW_LINE, LV_SYMBOL_KEYBOARD, ""};
const char* symbolMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", KB_ROW,
    "@", "#", "$", "%", "&", "*", "-", "+", "(", ")", KB_ROW,
    "#+=", "!", "\"", "'", ":", ";", "/", "?", LV_SYMBOL_BACKSPACE, KB_ROW,
    "abc", ",", " ", ".", LV_SYMBOL_NEW_LINE, LV_SYMBOL_KEYBOARD, ""};
const char* moreMap[] = {
    "[", "]", "{", "}", "<", ">", "^", "~", "`", "|", KB_ROW,
    "\\", "_", "=", "@", "#", "$", "%", "&", "*", "+", KB_ROW,
    "123", "!", "?", "'", "\"", ":", ";", "/", LV_SYMBOL_BACKSPACE, KB_ROW,
    "abc", ",", " ", ".", LV_SYMBOL_NEW_LINE, LV_SYMBOL_KEYBOARD, ""};
// Settings values and frequencies are edited with arrows, digits and OK.
const char* adjustMap[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", KB_ROW,
    LV_SYMBOL_CLOSE, LV_SYMBOL_LEFT, LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_RIGHT,
    LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""};
#undef KB_ROW

constexpr lv_btnmatrix_ctrl_t Key = LV_BTNMATRIX_CTRL_NO_REPEAT | 2;
constexpr lv_btnmatrix_ctrl_t Wide = LV_BTNMATRIX_CTRL_NO_REPEAT | 3;
constexpr lv_btnmatrix_ctrl_t Space = LV_BTNMATRIX_CTRL_NO_REPEAT | 6;
constexpr lv_btnmatrix_ctrl_t Delete = 3;  // Backspace repeats while held.

const lv_btnmatrix_ctrl_t lettersCtrl[] = {
    Key, Key, Key, Key, Key, Key, Key, Key, Key, Key,
    Key, Key, Key, Key, Key, Key, Key, Key, Key,
    Wide, Key, Key, Key, Key, Key, Key, Key, Delete,
    Wide, Key, Space, Key, Wide, Wide};
const lv_btnmatrix_ctrl_t symbolsCtrl[] = {
    Key, Key, Key, Key, Key, Key, Key, Key, Key, Key,
    Key, Key, Key, Key, Key, Key, Key, Key, Key, Key,
    Wide, Key, Key, Key, Key, Key, Key, Key, Delete,
    Wide, Key, Space, Key, Wide, Wide};
const lv_btnmatrix_ctrl_t adjustCtrl[] = {
    Key, Key, Key, Key, Key, Key, Key, Key, Key, Key,
    Key, Key, Key, Key, Key, Delete - 1, Wide};

constexpr lv_coord_t TextHeight = 116;
constexpr lv_coord_t AdjustHeight = 62;
constexpr size_t QueueSize = 8;

lv_obj_t* s_kb = nullptr;
Layout s_layout = Layout::Lower;
TextInputMode s_mode = TextInputMode::None;
Theme::Scheme s_scheme = Theme::Scheme::DARK;
lv_obj_t* s_panRoot = nullptr;
bool s_dismissed = false, s_visible = false, s_down = false;
KeyEvent s_queue[QueueSize];
uint8_t s_head = 0, s_count = 0;

void push(const KeyEvent& event) {
    if (s_count == QueueSize) return;
    s_queue[(s_head + s_count) % QueueSize] = event;
    ++s_count;
}

void applyTheme() {
    s_scheme = Theme::scheme();
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(Theme::BG_SURFACE), 0);
    lv_obj_set_style_border_color(s_kb, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(Theme::BG_ELEVATED), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(Theme::TEXT_PRIMARY), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(Theme::PRIMARY_MUTED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(Theme::PRIMARY_SUBTLE), LV_PART_ITEMS | LV_STATE_CHECKED);
}

void setLayout(Layout layout) {
    s_layout = layout;
    switch (layout) {
    case Layout::Lower:
        lv_btnmatrix_set_map(s_kb, lowerMap);
        lv_btnmatrix_set_ctrl_map(s_kb, lettersCtrl);
        break;
    case Layout::Upper:
        lv_btnmatrix_set_map(s_kb, upperMap);
        lv_btnmatrix_set_ctrl_map(s_kb, lettersCtrl);
        lv_btnmatrix_set_btn_ctrl(s_kb, 19, LV_BTNMATRIX_CTRL_CHECKED);  // Shift
        break;
    case Layout::Symbols:
        lv_btnmatrix_set_map(s_kb, symbolMap);
        lv_btnmatrix_set_ctrl_map(s_kb, symbolsCtrl);
        break;
    case Layout::More:
        lv_btnmatrix_set_map(s_kb, moreMap);
        lv_btnmatrix_set_ctrl_map(s_kb, symbolsCtrl);
        break;
    case Layout::Adjust:
        lv_btnmatrix_set_map(s_kb, adjustMap);
        lv_btnmatrix_set_ctrl_map(s_kb, adjustCtrl);
        break;
    }
    lv_obj_set_height(s_kb, layout == Layout::Adjust ? AdjustHeight : TextHeight);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void press(const char* label, bool repeat) {
    if (!label) return;
    const bool deleteKey = strcmp(label, LV_SYMBOL_BACKSPACE) == 0;
    if (repeat && !deleteKey) return;
    KeyEvent event{};
    if (deleteKey) {
        event.del = true; event.character = 0x08; event.repeat = repeat;
    } else if (strcmp(label, LV_SYMBOL_NEW_LINE) == 0 || strcmp(label, LV_SYMBOL_OK) == 0) {
        event.enter = true; event.character = '\n';
    } else if (strcmp(label, LV_SYMBOL_CLOSE) == 0) {
        event.character = 0x1b;
    } else if (strcmp(label, LV_SYMBOL_KEYBOARD) == 0) {
        s_dismissed = true;
        return;
    } else if (strcmp(label, "123") == 0) {
        setLayout(Layout::Symbols); return;
    } else if (strcmp(label, "#+=") == 0) {
        setLayout(Layout::More); return;
    } else if (strcmp(label, "abc") == 0) {
        setLayout(Layout::Lower); return;
    } else if (s_layout == Layout::Adjust && strcmp(label, LV_SYMBOL_LEFT) == 0) {
        event.left = true;
    } else if (s_layout == Layout::Adjust && strcmp(label, LV_SYMBOL_RIGHT) == 0) {
        event.right = true;
    } else if (s_layout == Layout::Adjust && strcmp(label, LV_SYMBOL_UP) == 0) {
        event.up = true;
    } else if (s_layout == Layout::Adjust && strcmp(label, LV_SYMBOL_DOWN) == 0) {
        event.down = true;
    } else if (strcmp(label, LV_SYMBOL_UP) == 0) {
        setLayout(s_layout == Layout::Upper ? Layout::Lower : Layout::Upper);
        return;
    } else if (label[0] && !label[1]) {
        event.character = label[0];
        event.space = label[0] == ' ';
        // Shift applies to one letter.
        if (s_layout == Layout::Upper) setLayout(Layout::Lower);
    } else {
        return;
    }
    push(event);
}

void onEvent(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    // LVGL sends VALUE_CHANGED from its own PRESSED handler, before this
    // callback sees PRESSED, so a VALUE_CHANGED while down is a repeat.
    if (code == LV_EVENT_PRESSED) {
        s_down = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_down = false;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t* kb = lv_event_get_target(e);
        press(lv_btnmatrix_get_btn_text(kb, lv_btnmatrix_get_selected_btn(kb)), s_down);
    }
}

// The direct child of the screen or top layer that holds the editor.
lv_obj_t* panRootOf(lv_obj_t* anchor) {
    lv_obj_t* root = anchor;
    while (root) {
        lv_obj_t* parent = lv_obj_get_parent(root);
        if (!parent) return nullptr;
        if (parent == lv_scr_act() || parent == lv_layer_top()) return root;
        root = parent;
    }
    return nullptr;
}

void pan(lv_obj_t* anchor) {
    lv_obj_t* root = anchor ? panRootOf(anchor) : nullptr;
    if (root != s_panRoot) {
        if (s_panRoot && lv_obj_is_valid(s_panRoot)) lv_obj_set_style_translate_y(s_panRoot, 0, 0);
        s_panRoot = root;
    }
    if (!root) return;
    lv_obj_update_layout(root);
    lv_area_t area;
    lv_obj_get_coords(anchor, &area);
    // Coordinates include the translation already applied to the root.
    const lv_coord_t applied = lv_obj_get_style_translate_y(root, 0);
    const lv_coord_t top = area.y1 - applied, bottom = area.y2 - applied;
    const lv_coord_t keyboardTop = LV_VER_RES -
        (s_layout == Layout::Adjust ? AdjustHeight : TextHeight);
    lv_coord_t shift = bottom + 3 - keyboardTop;
    if (shift > top) shift = top;
    if (shift < 0) shift = 0;
    if (-shift != applied) lv_obj_set_style_translate_y(root, -shift, 0);
}

void setVisible(bool visible) {
    if (visible == s_visible) return;
    s_visible = visible;
    if (visible) {
        lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_kb);
    } else {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        s_down = false;
    }
}

} // namespace

void init() {
    s_kb = lv_btnmatrix_create(lv_layer_top());
    // Touch-only: keep the keypad group's focus on screen content.
    if (lv_obj_get_group(s_kb)) lv_group_remove_obj(s_kb);
    lv_obj_set_width(s_kb, LV_HOR_RES);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_16, 0);
    lv_obj_set_style_radius(s_kb, 0, 0);
    // No LVGL theme is active, so btnmatrix defaults to transparent; the
    // keyboard must hide whatever it covers, including the tab bar.
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 1, 0);
    lv_obj_set_style_border_side(s_kb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(s_kb, 3, 0);
    lv_obj_set_style_pad_row(s_kb, 3, 0);
    lv_obj_set_style_pad_column(s_kb, 3, 0);
    lv_obj_set_style_radius(s_kb, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_kb, onEvent, LV_EVENT_ALL, nullptr);
    applyTheme();
    setLayout(Layout::Lower);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

void update(LvScreen* screen) {
    if (!s_kb) return;
    const TextInputRequest request = screen ? screen->textInput() : TextInputRequest{};
    if (Theme::scheme() != s_scheme) applyTheme();
    if (request.mode != s_mode) {
        // A new editor clears an earlier dismissal and starts on its layout.
        s_mode = request.mode;
        s_dismissed = false;
        s_head = s_count = 0;
        if (s_mode == TextInputMode::Adjust) setLayout(Layout::Adjust);
        else if (s_mode == TextInputMode::Text) setLayout(Layout::Lower);
    }
    const bool visible = request.mode != TextInputMode::None && request.anchor && !s_dismissed;
    setVisible(visible);
    pan(visible ? request.anchor : nullptr);
}

void show() { s_dismissed = false; }

bool take(KeyEvent& event) {
    if (!s_count) return false;
    event = s_queue[s_head];
    s_head = (s_head + 1) % QueueSize;
    --s_count;
    return true;
}

bool visible() { return s_visible; }

}  // namespace LvSoftKeyboard
#endif
