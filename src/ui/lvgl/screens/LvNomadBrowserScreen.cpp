#include "LvNomadBrowserScreen.h"
#if defined(RSSENSECAPD1L)

#include "Theme.h"
#include "LvTheme.h"
#include "fonts/fonts.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr int kHeaderH = 32;
constexpr int kAddressBarH = 30;
// Was 64 for the literal "Reload" text label; shrunk to match the other
// icon-only buttons in this bar now that it's a single ↻ glyph.
constexpr int kReloadBtnW = 40;
constexpr int kCancelBtnW = 40;
// Back/Forward, left of the address label -- the conventional browser spot.
constexpr int kHistoryBtnW = 32;
constexpr int kFontBtnW = 48;
constexpr int kRawBtnW = 40;
constexpr int kAddressToggleBtnW = 40;
constexpr int kIdentifyBtnW = 36;
constexpr int kFavoriteBtnW = 36;
// Was 48 for the literal "Favs" text label; shrunk to match the other
// icon-only buttons now that it's a single ☰ glyph.
constexpr int kFavoritesListBtnW = 36;
// Largest first: _fontSizeIdx starts at 0 (see .h), so the body opens at the
// biggest size. Tapping the "Aa" button cycles forward and wraps.
constexpr const lv_font_t* kFontSizes[] = {
    &lv_font_rsdeck_16, &lv_font_rsdeck_14, &lv_font_rsdeck_12, &lv_font_rsdeck_10
};
constexpr size_t kFontSizeCount = sizeof(kFontSizes) / sizeof(kFontSizes[0]);
// Parallel to kFontSizes, same indexing -- for Micron `! bold runs.
constexpr const lv_font_t* kBoldFontSizes[] = {
    &lv_font_rsdeck_16_bold, &lv_font_rsdeck_14_bold, &lv_font_rsdeck_12_bold, &lv_font_rsdeck_10_bold
};

// --- Micron markup parsing (headings, colors, alignment, rules, links, form fields) ---
//
// Scoped to what's actually needed for a read-only-turned-interactive page viewer:
// italic (`*`) is lexed so it doesn't leak into visible text, but not rendered (no
// italic font asset, and LVGL 8.3 has no text-skew style); literal/quote blocks
// aren't handled at all (not seen in any real captured page this session).
// Unsupported-glyph codepoints (flag/emoji sequences, CJK, etc. -- anything outside
// the rsdeck fonts' Unicode ranges) are silently dropped at parse time rather than
// shown as tofu boxes -- see stripUnsupportedGlyphs(). Confirmed against real
// captured page source this session (headings, `F/`f colors, `c center,
// `[label`:path] links, `- rules, `< reset) -- see ratspeak-handheld TODO.md.
//
// Form fields (`<FLAGS|NAME|VALUE|PRECHECKED`LABEL>, and a link's optional 3rd
// backtick segment listing what to submit) were reverse-engineered from a REAL
// working implementation -- reticulum-meshchat's MicronParser.js/NomadNetworkPage.vue
// -- not guessed from one ambiguous captured example (see TODO.md's investigation).
struct MicronSegment {
    enum class Kind : uint8_t { Text, Link, Rule, Field } kind = Kind::Text;
    std::string text;      // display text (Text, Link); unused for Field (see fieldLabel)
    std::string destHex;   // Link only; empty = relative (same node, different page)
    std::string path;      // Link only
    // Link only: the tokens from the link's optional 3rd backtick segment (e.g.
    // "`[Apply`:/page/r.mu`s|n|a=1]" -> {"s","n","a=1"}) -- each token is either a
    // form field NAME (submit its current live value), "key=value" (a fixed
    // literal to include), or "*" (submit every field on the page). Empty means
    // this link carries no form data -- a plain navigation, same as before this
    // round.
    std::vector<std::string> submitFields;
    bool hasFg = false; uint8_t fgR = 0, fgG = 0, fgB = 0;
    bool hasBg = false; uint8_t bgR = 0, bgG = 0, bgB = 0;
    bool underline = false;
    bool bold = false;
    // Field only (Kind::Field):
    enum class FieldType : uint8_t { Text, Checkbox, Radio } fieldType = FieldType::Text;
    std::string fieldName;    // the key this field's value submits under
    std::string fieldValue;   // checkbox/radio: the value submitted when checked
    std::string fieldLabel;   // checkbox/radio: displayed label; text: initial/current value
    uint8_t fieldWidth = 24;  // text fields only, characters
    bool fieldMasked = false; // text fields only -- password-style (not rendered as such yet)
    bool fieldPrechecked = false; // checkbox/radio only
};
struct MicronLine {
    std::vector<MicronSegment> segments;
    int heading = 0;   // 0 = normal, 1..3 = heading level (applies to this line only)
    char align = 'l';  // persists across lines until changed by `c/`l/`r or `<
};

uint8_t hexNibbleValue(char c) {
    if (c >= '0' && c <= '9') return uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
    return 0;
}

// 3-hex-digit shorthand (CSS-style): each digit doubled, "2c5" -> R=0x22 G=0xcc B=0x55.
void expandHexColor3(const std::string& s, size_t at, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t d0 = hexNibbleValue(s[at]), d1 = hexNibbleValue(s[at + 1]), d2 = hexNibbleValue(s[at + 2]);
    r = uint8_t((d0 << 4) | d0); g = uint8_t((d1 << 4) | d1); b = uint8_t((d2 << 4) | d2);
}

// Per the user's confirmed choice ("hide them cleanly"): rather than special-case
// specific emoji/flag sequences, drop any codepoint outside the ranges the rsdeck
// fonts actually ship glyphs for (see fonts.h) -- covers flags, emoji, CJK, box-art
// outside our symbol ranges, etc. without ever showing a tofu box.
//
// History: this originally assumed Montserrat covered the whole requested
// 0x2580-0x25FF range -- wrong (it only had 13 of 96 Geometric Shapes
// codepoints, nothing in Block Elements), confirmed reproduced on real
// hardware as a string of "glyph dsc. not found for U+2588" tofu boxes
// rendering a repeater's battery meter. Fixed by pulling General
// Punctuation/Box Drawing/Block Elements+Geometric Shapes from DejaVu Sans
// instead (fonts.h) -- verified by parsing the generated font's actual
// cmap, not assumed: DejaVu gives complete coverage of those three ranges
// (every codepoint confirmed), so plain range checks are accurate there.
// Arrows (0x2190-0x21FF, reload icon) is likewise complete in DejaVu --
// confirmed the same way -- so it gets a plain range check too. Misc
// Symbols (0x2600-0x26FF, star icon) is NOT complete -- DejaVu is missing
// 67 of its 256 codepoints -- so only the two specific codepoints actually
// used and verified present (star filled/outline) are allowed; a blanket
// range here would reproduce the exact bug this function was already
// bitten by once.
bool codepointSupported(uint32_t cp) {
    return (cp >= 0x20 && cp <= 0x7F) || (cp >= 0xA0 && cp <= 0xFF) ||
           (cp >= 0x100 && cp <= 0x17F) || (cp >= 0x400 && cp <= 0x4FF) ||
           (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x2190 && cp <= 0x21FF) ||
           (cp >= 0x2500 && cp <= 0x25FF) || cp == 0x2605 || cp == 0x2606;
}

std::string stripUnsupportedGlyphs(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        const unsigned char c = (unsigned char)in[i];
        uint32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = c & 0x07; len = 4; }
        else { i++; continue; } // not a valid UTF-8 lead byte -- drop it
        bool valid = true;
        for (size_t k = 1; k < len; k++) {
            const unsigned char cc = (unsigned char)in[i + k];
            if ((cc & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!valid) { i++; continue; }
        if (codepointSupported(cp)) out.append(in, i, len);
        i += len;
    }
    return out;
}

std::vector<MicronLine> parseMicron(const std::string& src) {
    std::vector<MicronLine> lines;
    const size_t n = src.size();
    size_t i = 0;
    char align = 'l';
    while (i < n) {
        // Comment/directive line: anything starting with '#' is fully consumed,
        // never rendered (matches both plain "# comment" and "#!directive" forms).
        if (src[i] == '#') {
            while (i < n && src[i] != '\n') i++;
            if (i < n) i++;
            continue;
        }
        MicronLine line; line.align = align;
        while (line.heading < 3 && i < n && src[i] == '>') { line.heading++; i++; }

        // A line that's just "<" (optional surrounding spaces) resets alignment to
        // left and produces no visible content.
        {
            size_t j = i;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == '<') {
                size_t k = j + 1;
                while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k >= n || src[k] == '\n') {
                    align = 'l';
                    i = (k < n) ? k + 1 : k;
                    continue;
                }
            }
        }
        // A line that's just "-" is a horizontal rule.
        {
            size_t j = i;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == '-') {
                size_t k = j + 1;
                while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k >= n || src[k] == '\n') {
                    MicronSegment rule; rule.kind = MicronSegment::Kind::Rule;
                    line.segments.push_back(std::move(rule));
                    lines.push_back(std::move(line));
                    i = (k < n) ? k + 1 : k;
                    continue;
                }
            }
        }

        bool hasFg = false, underline = false, bold = false, hasBg = false;
        uint8_t fgR = 0, fgG = 0, fgB = 0, bgR = 0, bgG = 0, bgB = 0;
        std::string acc;
        auto flush = [&]() {
            const std::string filtered = stripUnsupportedGlyphs(acc);
            acc.clear();
            if (filtered.empty()) return;
            MicronSegment seg; seg.text = filtered;
            seg.hasFg = hasFg; seg.fgR = fgR; seg.fgG = fgG; seg.fgB = fgB;
            seg.hasBg = hasBg; seg.bgR = bgR; seg.bgG = bgG; seg.bgB = bgB;
            seg.underline = underline; seg.bold = bold;
            line.segments.push_back(std::move(seg));
        };
        while (i < n && src[i] != '\n') {
            const char c = src[i];
            if (c == '`' && i + 1 < n) {
                const char code = src[i + 1];
                if ((code == 'F') && i + 4 < n &&
                    std::isxdigit((unsigned char)src[i + 2]) && std::isxdigit((unsigned char)src[i + 3]) &&
                    std::isxdigit((unsigned char)src[i + 4])) {
                    flush();
                    expandHexColor3(src, i + 2, fgR, fgG, fgB);
                    hasFg = true;
                    i += 5; continue;
                }
                if (code == 'B' && i + 4 < n &&
                    std::isxdigit((unsigned char)src[i + 2]) && std::isxdigit((unsigned char)src[i + 3]) &&
                    std::isxdigit((unsigned char)src[i + 4])) {
                    flush();
                    expandHexColor3(src, i + 2, bgR, bgG, bgB);
                    hasBg = true;
                    i += 5; continue;
                }
                if (code == 'f') { flush(); hasFg = false; i += 2; continue; }
                if (code == 'b') { flush(); hasBg = false; i += 2; continue; }
                if (code == 'a') { flush(); hasFg = false; hasBg = false; underline = false; bold = false; i += 2; continue; }
                if (code == '_') { flush(); underline = !underline; i += 2; continue; }
                if (code == '!') { flush(); bold = !bold; i += 2; continue; }
                if (code == '*') { flush(); i += 2; continue; } // italic: lexed, not rendered (no italic font asset)
                if (code == 'c' || code == 'l' || code == 'r') {
                    flush(); align = code; line.align = align; i += 2; continue;
                }
                if (code == '[') {
                    flush();
                    size_t j = i + 2;
                    std::string label, target;
                    // Label ends at the first bare backtick, full stop -- it's the
                    // delimiter for ANY `[label`target] link, same-node shorthand
                    // ("`:/page/x.mu") or not. This used to require the backtick be
                    // immediately followed by ':', which only matched the shorthand
                    // case; a cross-node link's target starts with a 32-hex-char
                    // destination hash instead (e.g. "`[Name`<hash>:/page/index.mu]",
                    // what nodes.mu's peer list generates), so that backtick was never
                    // recognised as the delimiter and the whole rest of the line --
                    // hash, path, closing bracket -- got swallowed into the label with
                    // no target at all. Confirmed on real hardware: Meshpoint's
                    // nodes.mu rendered every peer link as raw garbled text.
                    while (j < n && src[j] != '\n' && src[j] != '`')
                        label += src[j++];
                    if (j < n && src[j] == '`') {
                        j++;
                        // Stop at a bare backtick too, not just ']' -- that's the start of
                        // the optional 3rd (submit-fields) segment below, e.g.
                        // "`[next`:/page/r.mu`s=0|n=10|o=5]". Without this, target would
                        // swallow the field-suffix as part of the path.
                        while (j < n && src[j] != ']' && src[j] != '\n' && src[j] != '`') target += src[j++];
                    }
                    std::string fieldsStr;
                    if (j < n && src[j] == '`') {
                        j++;
                        while (j < n && src[j] != ']' && src[j] != '\n') fieldsStr += src[j++];
                    }
                    if (j < n && src[j] == ']') j++;
                    MicronSegment seg; seg.kind = MicronSegment::Kind::Link; seg.text = stripUnsupportedGlyphs(label);
                    if (target.size() > 33 && target[32] == ':' &&
                        std::all_of(target.begin(), target.begin() + 32,
                                    [](char ch) { return std::isxdigit((unsigned char)ch) != 0; })) {
                        seg.destHex = target.substr(0, 32);
                        seg.path = target.substr(33);
                    } else {
                        // Same-node shorthand ("`:/page/x.mu") -- the leading ':' is the
                        // shorthand's own sigil, not part of the actual request path. The
                        // old backtick+colon-delimiter parsing used to consume it along
                        // with the backtick; the bare-backtick fix above only consumes the
                        // backtick, so it has to be stripped here now instead. Missed this
                        // the first time -- confirmed on real hardware: a same-node link's
                        // follow-up request went out as "path=:/page/r.mu" (colon and all)
                        // and the server never responded to it.
                        seg.path = (!target.empty() && target[0] == ':') ? target.substr(1) : target;
                    }
                    // Split fieldsStr on '|' into individual tokens -- see submitFields'
                    // doc comment on MicronSegment for what each token means.
                    if (!fieldsStr.empty()) {
                        size_t start = 0;
                        while (true) {
                            const size_t pipe = fieldsStr.find('|', start);
                            if (pipe == std::string::npos) { seg.submitFields.push_back(fieldsStr.substr(start)); break; }
                            seg.submitFields.push_back(fieldsStr.substr(start, pipe - start));
                            start = pipe + 1;
                        }
                    }
                    line.segments.push_back(std::move(seg));
                    i = j; continue;
                }
                if (code == '<') {
                    // Form field: `<FLAGS|NAME|VALUE|PRECHECKED`LABEL> -- grammar
                    // reverse-engineered from reticulum-meshchat's MicronParser.js
                    // parseField(), not guessed. Opens with "`<", the declaration
                    // (FLAGS|NAME|...) ends at the NEXT backtick, and the whole tag
                    // closes with a plain '>' (not "`>" -- that only looks like one
                    // token when LABEL is empty, e.g. a checkbox with no trailing text).
                    size_t j = i + 2;
                    std::string decl;
                    while (j < n && src[j] != '\n' && src[j] != '`') decl += src[j++];
                    if (j >= n || src[j] != '`') { acc += c; i++; continue; } // malformed -- literal backtick
                    j++; // skip the declaration-terminating backtick
                    std::string label;
                    while (j < n && src[j] != '\n' && src[j] != '>') label += src[j++];
                    if (j >= n || src[j] != '>') { acc += c; i++; continue; } // malformed -- literal backtick
                    j++; // skip the closing '>'

                    std::vector<std::string> parts;
                    { std::string cur; for (char dc : decl) { if (dc == '|') { parts.push_back(cur); cur.clear(); } else cur += dc; } parts.push_back(cur); }
                    MicronSegment seg; seg.kind = MicronSegment::Kind::Field;
                    seg.fieldLabel = stripUnsupportedGlyphs(label);
                    std::string flags = parts.size() > 0 ? parts[0] : "";
                    seg.fieldName = parts.size() > 1 ? parts[1] : flags;
                    if (parts.size() > 2) seg.fieldValue = parts[2];
                    if (parts.size() > 3 && parts[3] == "*") seg.fieldPrechecked = true;
                    if (flags.find('^') != std::string::npos) { seg.fieldType = MicronSegment::FieldType::Radio; flags.erase(std::remove(flags.begin(), flags.end(), '^'), flags.end()); }
                    else if (flags.find('?') != std::string::npos) { seg.fieldType = MicronSegment::FieldType::Checkbox; flags.erase(std::remove(flags.begin(), flags.end(), '?'), flags.end()); }
                    else if (flags.find('!') != std::string::npos) { seg.fieldMasked = true; flags.erase(std::remove(flags.begin(), flags.end(), '!'), flags.end()); }
                    if (!flags.empty() && std::all_of(flags.begin(), flags.end(), [](char fc) { return std::isdigit((unsigned char)fc) != 0; })) {
                        const int w = std::atoi(flags.c_str());
                        if (w > 0) seg.fieldWidth = uint8_t(std::min(w, 256));
                    }
                    line.segments.push_back(std::move(seg));
                    i = j; continue;
                }
                // Unrecognized escape: treat the backtick as literal text.
            }
            acc += c; i++;
        }
        flush();
        lines.push_back(std::move(line));
        if (i < n && src[i] == '\n') i++;
    }
    return lines;
}

// LVGL label recolor ("#RRGGBB text#") escaping: a literal '#' in page content must
// be doubled, or it reads as a color-tag delimiter.
void appendRecolorSafe(std::string& out, const std::string& text) {
    for (char c : text) { if (c == '#') out += "##"; else out += c; }
}
char hexDigitChar(uint8_t v) { return v < 10 ? char('0' + v) : char('a' + v - 10); }
void appendHexByte(std::string& out, uint8_t v) { out += hexDigitChar(v >> 4); out += hexDigitChar(v & 0xF); }

// Encodes {name: value} pairs as a msgpack fixmap of fixstr/str8 strings -- the
// exact wire shape RNS.Link.request(path, data=dict) sends for a NomadNet form
// submission, confirmed via reticulum-meshchat's meshchat.py (NomadnetPageDownloader
// -> link.request(..., data=combined_data)), not guessed. Deliberately NOT a
// general-purpose msgpack encoder: real NomadNet forms are a handful of short
// fields, so this caps at 15 entries (fixmap's own ceiling) and 255-byte strings
// (str8's ceiling) and just refuses anything bigger rather than growing more
// msgpack type support nothing here would ever exercise.
bool encodeFieldDataMsgpack(const std::vector<std::pair<std::string, std::string>>& fields,
                             std::vector<uint8_t>& out) {
    if (fields.size() > 15) return false;
    out.clear();
    auto putStr = [&](const std::string& s) -> bool {
        if (s.size() <= 31) out.push_back(uint8_t(0xA0 | s.size()));
        else if (s.size() <= 255) { out.push_back(0xD9); out.push_back(uint8_t(s.size())); }
        else return false;
        out.insert(out.end(), s.begin(), s.end());
        return true;
    };
    out.push_back(uint8_t(0x80 | fields.size()));
    for (const auto& kv : fields) {
        if (!putStr(kv.first) || !putStr(kv.second)) { out.clear(); return false; }
    }
    return true;
}

} // namespace

void LvNomadBrowserScreen::createUI(lv_obj_t* parent) {
    _screen = parent;
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);

    // --- Header: close (X), title, font-size cycle ---
    _header = lv_obj_create(parent);
    lv_obj_set_size(_header, lv_pct(100), kHeaderH);
    lv_obj_set_style_bg_color(_header, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(_header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_header, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_header, 1, 0);
    lv_obj_set_style_border_side(_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_clear_flag(_header, LV_OBJ_FLAG_SCROLLABLE);

    // Text-size cycle: tap to step through the sizes in kFontSizes (wraps).
    // Session-only -- not persisted, see _fontSizeIdx's doc comment.
    _btnFontSize = lv_btn_create(_header);
    lv_group_remove_obj(_btnFontSize);
    lv_obj_set_size(_btnFontSize, kFontBtnW, kHeaderH);
    lv_obj_align(_btnFontSize, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(_btnFontSize, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnFontSize, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnFontSize, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->cycleFontSize();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* fontLbl = lv_label_create(_btnFontSize);
    lv_obj_set_style_text_font(fontLbl, &lv_font_rsdeck_14, 0);
    lv_label_set_text(fontLbl, "Aa");
    lv_obj_center(fontLbl);

    // Raw-Micron-source toggle: flips the body between the rendered page and the
    // plain fetched text, no re-fetch needed -- see toggleRawView()/_pageSource.
    _btnRawToggle = lv_btn_create(_header);
    lv_group_remove_obj(_btnRawToggle);
    lv_obj_set_size(_btnRawToggle, kRawBtnW, kHeaderH);
    lv_obj_align(_btnRawToggle, LV_ALIGN_RIGHT_MID, -kFontBtnW, 0);
    lv_obj_add_style(_btnRawToggle, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnRawToggle, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnRawToggle, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->toggleRawView();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* rawLbl = lv_label_create(_btnRawToggle);
    lv_obj_set_style_text_font(rawLbl, &lv_font_rsdeck_14, 0);
    // _showRaw is sticky across reopens (see .h) -- reflect it on this fresh label.
    lv_obj_set_style_text_color(rawLbl, lv_color_hex(_showRaw ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
    lv_label_set_text(rawLbl, "{}");
    lv_obj_center(rawLbl);

    // Identify-on-request toggle: off by default (see TODO.md) -- every Nomad
    // request otherwise sends a Link-Identify frame unconditionally, an extra
    // full LoRa transmission on literally every page load (NomadBrowseClient
    // tears down and re-establishes a fresh Link each time, so this isn't
    // amortized across a browsing session), for a server identity most nodes
    // (meshpoint included -- checked, remote_identity is never read) never
    // even use. Matches real NomadNet's own "identify on connect" setting,
    // which is itself opt-in there too, not forced. Toggling ON costs
    // whatever a real gated/personalized node needs it for.
    _btnIdentify = lv_btn_create(_header);
    lv_group_remove_obj(_btnIdentify);
    lv_obj_set_size(_btnIdentify, kIdentifyBtnW, kHeaderH);
    lv_obj_align(_btnIdentify, LV_ALIGN_RIGHT_MID, -(kFontBtnW + kRawBtnW), 0);
    lv_obj_add_style(_btnIdentify, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnIdentify, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnIdentify, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->toggleIdentify();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* idLbl = lv_label_create(_btnIdentify);
    lv_obj_set_style_text_font(idLbl, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(idLbl, lv_color_hex(_identifyOnRequest ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
    lv_label_set_text(idLbl, "ID");
    lv_obj_center(idLbl);

    // Address-bar show/hide toggle ("globe"): lives in the header, not the address
    // bar itself, so it's still reachable once the bar is hidden.
    _btnAddressToggle = lv_btn_create(_header);
    lv_group_remove_obj(_btnAddressToggle);
    lv_obj_set_size(_btnAddressToggle, kAddressToggleBtnW, kHeaderH);
    lv_obj_align(_btnAddressToggle, LV_ALIGN_RIGHT_MID, -(kFontBtnW + kRawBtnW + kIdentifyBtnW), 0);
    lv_obj_add_style(_btnAddressToggle, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnAddressToggle, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnAddressToggle, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->toggleAddressBar();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* globeLbl = lv_label_create(_btnAddressToggle);
    lv_obj_set_style_text_font(globeLbl, &lv_font_rsdeck_14, 0);
    lv_label_set_text(globeLbl, "@");
    lv_obj_center(globeLbl);

    // Favorite-current-node toggle -- like a normal browser's address-bar
    // bookmark star, except there's no address bar visible by default here,
    // so it lives in the header instead. Real filled/outline star glyph
    // (U+2605/U+2606, see fonts.h) plus color both reflect the current
    // state; toggleFavoriteCurrent() sets it directly rather than through
    // updateFavoriteButton(), since right after a toggle NodeView's cached
    // copy is still stale.
    _btnFavorite = lv_btn_create(_header);
    lv_group_remove_obj(_btnFavorite);
    lv_obj_set_size(_btnFavorite, kFavoriteBtnW, kHeaderH);
    lv_obj_align(_btnFavorite, LV_ALIGN_RIGHT_MID, -(kFontBtnW + kRawBtnW + kIdentifyBtnW + kAddressToggleBtnW), 0);
    lv_obj_add_style(_btnFavorite, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnFavorite, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnFavorite, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->toggleFavoriteCurrent();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* favLbl = lv_label_create(_btnFavorite);
    lv_obj_set_style_text_font(favLbl, &lv_font_rsdeck_16, 0);
    lv_label_set_text(favLbl, "☆"); // updateFavoriteButton() sets the real state right after creation
    lv_obj_center(favLbl);

    // Favorites list -- jumps straight into any favorited node's default
    // page. See openFavoritesList().
    _btnFavoritesList = lv_btn_create(_header);
    lv_group_remove_obj(_btnFavoritesList);
    lv_obj_set_size(_btnFavoritesList, kFavoritesListBtnW, kHeaderH);
    lv_obj_align(_btnFavoritesList, LV_ALIGN_RIGHT_MID,
                -(kFontBtnW + kRawBtnW + kIdentifyBtnW + kAddressToggleBtnW + kFavoriteBtnW), 0);
    lv_obj_add_style(_btnFavoritesList, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnFavoritesList, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnFavoritesList, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->openFavoritesList();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* favsLbl = lv_label_create(_btnFavoritesList);
    // U+2630 "trigram for heaven" -- repurposed as a hamburger/list icon,
    // not a semantic match, but reads clearly as "open a list" at a glance.
    lv_obj_set_style_text_font(favsLbl, &lv_font_rsdeck_16, 0);
    lv_label_set_text(favsLbl, "☰");
    lv_obj_center(favsLbl);

    _lblTitle = lv_label_create(_header);
    lv_obj_set_style_text_font(_lblTitle, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(_lblTitle, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_long_mode(_lblTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_lblTitle, Theme::CONTENT_W - kFontBtnW - kRawBtnW - kIdentifyBtnW - kAddressToggleBtnW
                      - kFavoriteBtnW - kFavoritesListBtnW - 16);
    lv_label_set_text(_lblTitle, "Nomad Browser");
    lv_obj_align(_lblTitle, LV_ALIGN_LEFT_MID, 8, 0);

    // --- Address bar: current page + reload ---
    _addressBar = lv_obj_create(parent);
    lv_obj_set_size(_addressBar, lv_pct(100), kAddressBarH);
    lv_obj_set_style_bg_color(_addressBar, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(_addressBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_addressBar, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_addressBar, 1, 0);
    lv_obj_set_style_border_side(_addressBar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(_addressBar, 0, 0);
    lv_obj_set_style_radius(_addressBar, 0, 0);
    lv_obj_clear_flag(_addressBar, LV_OBJ_FLAG_SCROLLABLE);

    _btnBack = lv_btn_create(_addressBar);
    lv_group_remove_obj(_btnBack);
    lv_obj_set_size(_btnBack, kHistoryBtnW, kAddressBarH);
    lv_obj_align(_btnBack, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_style(_btnBack, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnBack, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnBack, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->goBack();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* backLbl = lv_label_create(_btnBack);
    lv_obj_set_style_text_font(backLbl, &lv_font_rsdeck_16, 0);
    lv_label_set_text(backLbl, "\xE2\x86\x90"); // U+2190 LEFTWARDS ARROW
    lv_obj_center(backLbl);

    _btnForward = lv_btn_create(_addressBar);
    lv_group_remove_obj(_btnForward);
    lv_obj_set_size(_btnForward, kHistoryBtnW, kAddressBarH);
    lv_obj_align(_btnForward, LV_ALIGN_LEFT_MID, kHistoryBtnW, 0);
    lv_obj_add_style(_btnForward, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnForward, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnForward, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->goForward();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* forwardLbl = lv_label_create(_btnForward);
    lv_obj_set_style_text_font(forwardLbl, &lv_font_rsdeck_16, 0);
    lv_label_set_text(forwardLbl, "\xE2\x86\x92"); // U+2192 RIGHTWARDS ARROW
    lv_obj_center(forwardLbl);
    updateHistoryButtons();

    _btnReload = lv_btn_create(_addressBar);
    lv_group_remove_obj(_btnReload);
    lv_obj_set_size(_btnReload, kReloadBtnW, kAddressBarH);
    lv_obj_align(_btnReload, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(_btnReload, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnReload, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnReload, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->startBrowse();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* reloadLbl = lv_label_create(_btnReload);
    lv_obj_set_style_text_font(reloadLbl, &lv_font_rsdeck_16, 0);
    lv_label_set_text(reloadLbl, "↻");
    lv_obj_center(reloadLbl);

    // Stop an in-progress load -- only shown while one is actually in flight,
    // see setCancelVisible()/cancelBrowse().
    _btnCancel = lv_btn_create(_addressBar);
    lv_group_remove_obj(_btnCancel);
    lv_obj_set_size(_btnCancel, kCancelBtnW, kAddressBarH);
    lv_obj_align(_btnCancel, LV_ALIGN_RIGHT_MID, -kReloadBtnW, 0);
    lv_obj_add_style(_btnCancel, LvTheme::styleBtn(), 0);
    lv_obj_add_style(_btnCancel, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btnCancel, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->cancelBrowse();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* cancelLbl = lv_label_create(_btnCancel);
    // Font 16, not 10 -- a single glyph has room to spare in this button,
    // and at 10px the square read as a barely-visible dot.
    lv_obj_set_style_text_font(cancelLbl, &lv_font_rsdeck_16, 0);
    // U+25A0 BLACK SQUARE -- the standard media-player "Stop" glyph, and one
    // of the 13 real icon-like codepoints this font actually has (confirmed
    // via its cmap, see TODO.md) -- not a guess, not a tofu box.
    lv_label_set_text(cancelLbl, "\xE2\x96\xA0");
    lv_obj_center(cancelLbl);
    lv_obj_add_flag(_btnCancel, LV_OBJ_FLAG_HIDDEN);

    _lblAddress = lv_label_create(_addressBar);
    lv_obj_set_style_text_font(_lblAddress, &lv_font_rsdeck_10, 0);
    lv_obj_set_style_text_color(_lblAddress, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    lv_label_set_long_mode(_lblAddress, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_lblAddress, Theme::CONTENT_W - kReloadBtnW - kCancelBtnW - 2 * kHistoryBtnW - 16);
    lv_obj_align(_lblAddress, LV_ALIGN_LEFT_MID, 2 * kHistoryBtnW + 8, 0);
    // Tap to edit -- like clicking a normal browser's address bar. No-ops
    // while already editing (guards against re-prefilling over an in-progress
    // edit) -- see startUrlInput()'s doc comment for what it pre-fills with.
    lv_obj_add_flag(_lblAddress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_lblAddress, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        if (!self->_urlInputActive) self->startUrlInput();
    }, LV_EVENT_CLICKED, this);
    // _addressBarHidden is sticky across reopens (see .h) -- re-apply it now that
    // the bar widget exists again.
    if (_addressBarHidden) lv_obj_add_flag(_addressBar, LV_OBJ_FLAG_HIDDEN);

    // --- Body: page content (dynamic -- see renderMicron()) ---
    _body = lv_obj_create(parent);
    lv_obj_set_width(_body, lv_pct(100));
    lv_obj_set_flex_grow(_body, 1);
    lv_obj_set_style_bg_color(_body, lv_color_hex(Theme::BG), 0);
    lv_obj_set_style_bg_opa(_body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_body, 0, 0);
    lv_obj_set_style_pad_all(_body, 8, 0);
    lv_obj_set_style_pad_row(_body, 4, 0);
    lv_obj_set_style_radius(_body, 0, 0);
    lv_obj_set_layout(_body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_body, LV_DIR_VER);
    lv_obj_add_style(_body, LvTheme::styleScrollbar(), LV_PART_SCROLLBAR);

    _logText = lv_label_create(_body);
    lv_obj_set_style_text_color(_logText, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_label_set_long_mode(_logText, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_logText, lv_pct(100));
    lv_label_set_text(_logText, "");
    applyFontSize();

    // --- Favorites-list overlay (hidden until openFavoritesList()) ---
    constexpr int kFavOverlayW = 300, kFavOverlayH = 260, kFavOverlayInnerW = kFavOverlayW - 24;
    _favoritesOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_favoritesOverlay, kFavOverlayW, kFavOverlayH);
    lv_obj_center(_favoritesOverlay);
    lv_obj_set_style_bg_color(_favoritesOverlay, lv_color_hex(Theme::BG_ELEVATED), 0);
    lv_obj_set_style_bg_opa(_favoritesOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_favoritesOverlay, lv_color_hex(Theme::BORDER), 0);
    lv_obj_set_style_border_width(_favoritesOverlay, 1, 0);
    lv_obj_set_style_radius(_favoritesOverlay, 6, 0);
    lv_obj_set_style_pad_all(_favoritesOverlay, 12, 0);
    lv_obj_clear_flag(_favoritesOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_favoritesOverlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* favTitle = lv_label_create(_favoritesOverlay);
    lv_obj_set_style_text_font(favTitle, &lv_font_rsdeck_14, 0);
    lv_obj_set_style_text_color(favTitle, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(favTitle, "Favorites");
    lv_obj_set_pos(favTitle, 0, 0);

    lv_obj_t* favClose = lv_btn_create(_favoritesOverlay);
    lv_group_remove_obj(favClose);
    lv_obj_set_size(favClose, 28, 28);
    lv_obj_align(favClose, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_add_style(favClose, LvTheme::styleBtn(), 0);
    lv_obj_add_style(favClose, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(favClose, [](lv_event_t* e) {
        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
        self->closeFavoritesList();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* favCloseLbl = lv_label_create(favClose);
    lv_obj_set_style_text_font(favCloseLbl, &lv_font_rsdeck_14, 0);
    lv_label_set_text(favCloseLbl, "X");
    lv_obj_center(favCloseLbl);

    _favoritesList = lv_obj_create(_favoritesOverlay);
    lv_obj_set_size(_favoritesList, kFavOverlayInnerW, kFavOverlayH - 40);
    lv_obj_set_pos(_favoritesList, 0, 34);
    lv_obj_set_style_bg_opa(_favoritesList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_favoritesList, 0, 0);
    lv_obj_set_style_pad_all(_favoritesList, 0, 0);
    lv_obj_set_style_pad_row(_favoritesList, 6, 0);
    lv_obj_set_layout(_favoritesList, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_favoritesList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_favoritesList, LV_DIR_VER);
    lv_obj_add_style(_favoritesList, LvTheme::styleScrollbar(), LV_PART_SCROLLBAR);
}

void LvNomadBrowserScreen::destroyUI() {
    LvScreen::destroyUI();
    _links.clear();
    _favoriteRows.clear();
    _checkboxRefs.clear();
    _textFieldRefs.clear();
    // Not sticky (unlike _showRaw/_addressBarHidden) -- leaving mid-typing
    // and coming back should not re-pop the keyboard with stale text.
    _urlInputActive = false;
    _urlInputText.clear();
    _editingFieldActive = false;
    _editingFieldName.clear();
    _editingFieldText.clear();
    _pendingFieldData.clear();
    _header = _lblTitle = _btnFontSize = _btnRawToggle = _btnIdentify = _btnAddressToggle = _btnFavorite =
        _btnFavoritesList = _addressBar = _lblAddress = _btnBack = _btnForward = _btnReload = _btnCancel =
        _body = _logText = _favoritesOverlay = _favoritesList = nullptr;
}

void LvNomadBrowserScreen::onEnter() {
    // openHome() (the Nomad tab) leaves _peerHex empty; openFor() (Peers'
    // per-contact action, or a favorites-row tap) always sets it first.
    if (_peerHex.empty()) showHome();
    else startBrowse();
}

bool LvNomadBrowserScreen::handleKey(const KeyEvent& event) {
    if (!_urlInputActive && !_editingFieldActive) return false;
    // Same input-accumulation shape as LvNodesScreen's nickname input: Esc
    // cancels, Enter submits, Backspace/Del removes one character, printable
    // ASCII appends. Routes to whichever of the two text-edit modes is active
    // -- only one is ever active at a time (see _editingFieldActive's doc
    // comment on why these stay separate state machines).
    std::string& text = _editingFieldActive ? _editingFieldText : _urlInputText;
    constexpr size_t maxLen = 48;
    if (event.character == 0x1B) {
        if (_editingFieldActive) cancelFieldInput(); else cancelUrlInput();
        return true;
    }
    if (event.enter || event.character == '\n' || event.character == '\r') {
        if (_editingFieldActive) submitFieldInput(); else submitUrlInput();
        return true;
    }
    if ((event.del || event.character == '\b') && !event.repeat) {
        if (!text.empty()) { text.pop_back(); if (_editingFieldActive) updateFieldInputDisplay(); else updateUrlInputDisplay(); }
        return true;
    }
    if (event.character >= 0x20 && event.character <= 0x7E && text.size() < maxLen) {
        text += (char)event.character;
        if (_editingFieldActive) updateFieldInputDisplay(); else updateUrlInputDisplay();
    }
    return true;
}

void LvNomadBrowserScreen::openFor(const std::string& peerHex, const std::string& displayName) {
    _peerHex = peerHex;
    _displayName = displayName;
    _path = "/page/index.mu";
    _needsHistoryPush = true;
}

void LvNomadBrowserScreen::navigateTo(const std::string& path) {
    _path = path;
    _needsHistoryPush = true;
    startBrowse();
}

void LvNomadBrowserScreen::pushHistory() {
    if (_historyIndex >= 0 && _historyIndex < (int)_history.size()) {
        const auto& top = _history[_historyIndex];
        if (top.peerHex == _peerHex && top.path == _path) { updateHistoryButtons(); return; }
    }
    // Abandon any forward entries -- same rule a real browser follows when
    // you navigate somewhere new after having gone Back.
    if (_historyIndex + 1 < (int)_history.size()) _history.resize(_historyIndex + 1);
    _history.push_back({_peerHex, _displayName, _path});
    _historyIndex = (int)_history.size() - 1;
    updateHistoryButtons();
}

void LvNomadBrowserScreen::goBack() {
    if (_historyIndex <= 0) return;
    --_historyIndex;
    restoreHistoryEntry();
}

void LvNomadBrowserScreen::goForward() {
    if (_historyIndex < 0 || _historyIndex + 1 >= (int)_history.size()) return;
    ++_historyIndex;
    restoreHistoryEntry();
}

void LvNomadBrowserScreen::restoreHistoryEntry() {
    const auto& e = _history[_historyIndex];
    _peerHex = e.peerHex; _displayName = e.displayName; _path = e.path;
    updateHistoryButtons();
    // _needsHistoryPush is never set here -- landing on a stored entry must
    // not push a new one, or Back/Forward would corrupt their own stack.
    if (_peerHex.empty()) showHome(); else startBrowse();
}

void LvNomadBrowserScreen::updateHistoryButtons() {
    const bool canBack = _historyIndex > 0;
    const bool canForward = _historyIndex >= 0 && _historyIndex + 1 < (int)_history.size();
    if (_btnBack) {
        lv_obj_t* lbl = lv_obj_get_child(_btnBack, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(canBack ? Theme::TEXT_PRIMARY : Theme::TEXT_MUTED), 0);
    }
    if (_btnForward) {
        lv_obj_t* lbl = lv_obj_get_child(_btnForward, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(canForward ? Theme::TEXT_PRIMARY : Theme::TEXT_MUTED), 0);
    }
}

void LvNomadBrowserScreen::startBrowse() {
    // _requestId != 0 means a request was actually submitted; _done==false
    // means it hasn't settled yet -- i.e. one is genuinely in flight (not
    // just the fresh-screen default state, where both _requestId and _done
    // start at 0/false with nothing ever having been submitted).
    if (_requestId && !_done) {
        // The device only allows one browse at a time (executeNomad()'s
        // admission guard rejects a second with "A browse is already in
        // progress"). Cancel it first; the retry happens inside its own
        // completion callback below, once Outcome::Cancelled confirms the
        // device-side slot is free again.
        _pendingRestart = true;
        cancelBrowse();
        return;
    }
    if (_needsHistoryPush) { _needsHistoryPush = false; pushHistory(); }
    _pendingRestart = false;
    const uint32_t generation = ++_generation;
    _lastPhase = 0xFF; _lastAttempt = 0; _done = false; _destHashShown = false;
    _pageSource.clear();
    // A genuinely NEW fetch -- not a re-render of the same page (font-size change,
    // raw/rendered toggle, a field edit's own renderPage() call) -- so form state
    // resets here, not inside renderMicron() itself (which runs on every one of
    // those re-renders too, and must NOT wipe out a just-submitted field edit).
    _formFieldValues.clear();
    _checkboxRefs.clear();
    _textFieldRefs.clear();
    _log = "Connecting...";
    updateFavoriteButton();
    if (_lblAddress) {
        // Placeholder until the real nomadnetwork.node hash is known -- see pollProgress().
        const std::string address = (_displayName.empty() ? _peerHex : _displayName) + ":" + _path;
        lv_label_set_text(_lblAddress, address.c_str());
    }
    if (_body) {
        lv_obj_clean(_body);
        _links.clear();
        _logText = lv_label_create(_body);
        lv_obj_set_style_text_color(_logText, lv_color_hex(Theme::TEXT_PRIMARY), 0);
        lv_label_set_long_mode(_logText, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_logText, lv_pct(100));
        applyFontSize();
        lv_label_set_text(_logText, _log.c_str());
    }
    setCancelVisible(false);
    if (!_service) {
        if (_logText) lv_label_set_text(_logText, "Not available");
        return;
    }
    const std::string hex = _peerHex;
    const std::string path = _path;
    const uint8_t* fieldDataPtr = _pendingFieldData.empty() ? nullptr : _pendingFieldData.data();
    const size_t fieldDataLen = _pendingFieldData.size();
    _requestId = _service->requestNomadBrowse(hex, path,
        [this, generation](const handheld::Result& result, const char* data) {
            if (generation != _generation) return; // superseded by a later openFor()/navigateTo()
            _done = true;
            setCancelVisible(false);
            if (_pendingRestart) {
                // Reload (or a link/favorite) was tapped while this request was
                // still in flight -- it's been cancelled (or, rarely, raced to a
                // natural finish first; either way the slot is free now), so
                // the retry this was waiting on can go ahead for real.
                _pendingRestart = false;
                startBrowse();
                return;
            }
            if (result.outcome == handheld::Outcome::Ok) {
                _pageSource.assign(data, result.length);
                renderPage();
            } else if (_logText) {
                std::string msg;
                if (result.outcome == handheld::Outcome::Cancelled) msg = "Cancelled.";
                else {
                    msg = "Could not load the page: ";
                    msg += result.detail[0] ? result.detail : "unknown error";
                }
                lv_label_set_text(_logText, msg.c_str());
            }
        }, fieldDataPtr, fieldDataLen, _identifyOnRequest);
    _pendingFieldData.clear(); // consumed -- already copied into the mailbox body by submit()
    if (_requestId) setCancelVisible(true);
    else {
        _done = true;
        if (_logText) lv_label_set_text(_logText, "Could not start the browse (device busy)");
    }
}

void LvNomadBrowserScreen::appendLog(const std::string& line) {
    if (!_log.empty()) _log += '\n';
    _log += line;
    if (_logText) lv_label_set_text(_logText, _log.c_str());
    if (_body) lv_obj_scroll_to_y(_body, LV_COORD_MAX, LV_ANIM_OFF);
}

void LvNomadBrowserScreen::pollProgress() {
    const auto& st = _service->status();
    if (st.nomadRevision != _requestId) return; // stale snapshot, not this browse yet
    // Swap the address bar's placeholder (contact name) for the real nomadnetwork.node hash
    // as soon as the backend has computed it -- independent of the phase-change dedup below,
    // since the hash can become known on a tick where the phase itself doesn't change.
    if (!_destHashShown && st.nomadDestHex[0] && _lblAddress) {
        _destHashShown = true;
        const std::string address = std::string(st.nomadDestHex) + ":" + _path;
        lv_label_set_text(_lblAddress, address.c_str());
    }
    if (st.nomadStatus == _lastPhase && st.nomadAttempt == _lastAttempt) return;
    _lastPhase = st.nomadStatus; _lastAttempt = st.nomadAttempt;
    // Mirrors handheld::NomadBrowseClient::Status ordinals -- same cross-task raw-uint8_t
    // convention ProtocolBackend.h's "0 == NomadBrowseClient::Status::Idle" comment
    // documents: Idle=0, ResolvingPath=1, Connecting=2, Requesting=3, Done=4, Failed=5.
    // Done/Failed are left alone here: the requestNomadBrowse() completion callback
    // replaces the body with the rendered page or the error once the mailbox settles.
    switch (_lastPhase) {
        case 1: appendLog("Resolving path to node..."); break;
        case 2: appendLog("Connecting (link handshake)..."); break;
        case 3:
            if (_lastAttempt > 1) {
                // "3" here must match handheld::NomadBrowseClient::kMaxAttempts -- this UI
                // layer only sees the raw attempt number over the cross-task Status
                // snapshot, not the backend class itself, so it can't read the constant.
                char buf[48];
                snprintf(buf, sizeof buf, "No response -- retrying (%u/3)...", unsigned(_lastAttempt));
                appendLog(buf);
            } else {
                appendLog("Requesting " + _path + "...");
            }
            break;
        default: break;
    }
}

void LvNomadBrowserScreen::refreshUI() {
    if (_done || !_service || !_requestId) return;
    pollProgress();
}

void LvNomadBrowserScreen::cycleFontSize() {
    _fontSizeIdx = (_fontSizeIdx + 1) % kFontSizeCount;
    applyFontSize();
    // A page is already rendered (not just the loading/error _logText) -- rebuild
    // it at the new size instead of leaving it stuck at whatever size it first
    // rendered at.
    if (!_pageSource.empty()) renderPage();
}

void LvNomadBrowserScreen::applyFontSize() {
    if (_logText) lv_obj_set_style_text_font(_logText, kFontSizes[_fontSizeIdx], 0);
}

void LvNomadBrowserScreen::renderPage() {
    if (_showRaw) showRawText(_pageSource);
    else renderMicron(_pageSource);
}

void LvNomadBrowserScreen::toggleRawView() {
    _showRaw = !_showRaw;
    if (_btnRawToggle) {
        lv_obj_t* lbl = lv_obj_get_child(_btnRawToggle, 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
            lv_color_hex(_showRaw ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
    }
    if (!_pageSource.empty()) renderPage();
}

void LvNomadBrowserScreen::toggleIdentify() {
    _identifyOnRequest = !_identifyOnRequest;
    if (_btnIdentify) {
        lv_obj_t* lbl = lv_obj_get_child(_btnIdentify, 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
            lv_color_hex(_identifyOnRequest ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
    }
    // Takes effect on the next request -- not worth forcing a reload just for
    // a toggle flip, same restraint as _showRaw/_addressBarHidden.
}

void LvNomadBrowserScreen::toggleAddressBar() {
    _addressBarHidden = !_addressBarHidden;
    if (_addressBar) {
        if (_addressBarHidden) lv_obj_add_flag(_addressBar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(_addressBar, LV_OBJ_FLAG_HIDDEN);
    }
}

void LvNomadBrowserScreen::setCancelVisible(bool visible) {
    if (!_btnCancel) return;
    if (visible) lv_obj_clear_flag(_btnCancel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(_btnCancel, LV_OBJ_FLAG_HIDDEN);
}

void LvNomadBrowserScreen::cancelBrowse() {
    if (!_requestId || _done || !_service) return;
    _service->action(handheld::Operation::CancelNomadBrowse);
}

void LvNomadBrowserScreen::toggleFavoriteCurrent() {
    if (!_service || _peerHex.empty() || !_btnFavorite) return;
    const DiscoveredNode* node = _service->nodes.findNodeByHex(_peerHex);
    const bool newState = !(node && node->nomadFavorite);
    _service->nodes.setContactFavorite(_peerHex, newState);
    lv_obj_t* lbl = lv_obj_get_child(_btnFavorite, 0);
    if (lbl) {
        lv_obj_set_style_text_color(lbl, lv_color_hex(newState ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
        lv_label_set_text(lbl, newState ? "★" : "☆");
    }
}

void LvNomadBrowserScreen::updateFavoriteButton() {
    if (!_btnFavorite || !_service) return;
    const DiscoveredNode* node = _peerHex.empty() ? nullptr : _service->nodes.findNodeByHex(_peerHex);
    const bool fav = node && node->nomadFavorite;
    lv_obj_t* lbl = lv_obj_get_child(_btnFavorite, 0);
    if (lbl) {
        lv_obj_set_style_text_color(lbl, lv_color_hex(fav ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
        lv_label_set_text(lbl, fav ? "★" : "☆");
    }
}

// Shared by the "Favs" overlay and the Nomad tab's home/landing state (see
// showHome()) -- same rows, same tap-to-browse behavior, just a different
// container. closeFavoritesList() is safe to call even when the overlay was
// never shown (home state): it's just an idempotent hide.
void LvNomadBrowserScreen::populateFavoriteRows(lv_obj_t* container) {
    lv_obj_clean(container);
    _favoriteRows.clear();
    if (!_service) return;
    for (const auto& node : _service->nodes.nodes()) {
        if (!node.nomadFavorite) continue;
        const std::string hex = node.hash.toHex();
        _favoriteRows.push_back({hex, node.name.empty() ? hex.substr(0, 12) : node.name});
    }
    if (_favoriteRows.empty()) {
        lv_obj_t* empty = lv_label_create(container);
        lv_obj_set_style_text_font(empty, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, lv_pct(100));
        lv_label_set_text(empty, "No favorites yet. Tap \xE2\x98\x86 while browsing a node to add one.");
        return;
    }
    for (size_t i = 0; i < _favoriteRows.size(); ++i) {
        lv_obj_t* btn = lv_obj_create(container);
        lv_obj_set_size(btn, lv_pct(100), 36);
        lv_obj_set_style_bg_color(btn, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(i));
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
            const size_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(e)));
            if (idx >= self->_favoriteRows.size()) return;
            const auto row = self->_favoriteRows[idx]; // copy -- closeFavoritesList() clears the vector
            self->closeFavoritesList();
            // No display name -- startBrowse()'s address-bar placeholder falls back
            // to _peerHex (the hash) when _displayName is empty, matching this
            // screen's hash-first address convention instead of showing the
            // favorite's friendly name.
            self->openFor(row.hex, "");
            self->startBrowse();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, lv_pct(90));
        lv_label_set_text(lbl, _favoriteRows[i].name.c_str());
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    }
}

void LvNomadBrowserScreen::openFavoritesList() {
    if (!_favoritesOverlay || !_favoritesList) return;
    populateFavoriteRows(_favoritesList);
    lv_obj_clear_flag(_favoritesOverlay, LV_OBJ_FLAG_HIDDEN);
}

void LvNomadBrowserScreen::closeFavoritesList() {
    if (_favoritesOverlay) lv_obj_add_flag(_favoritesOverlay, LV_OBJ_FLAG_HIDDEN);
}

void LvNomadBrowserScreen::openHome() {
    _peerHex.clear();
    _displayName.clear();
    _path = "/page/index.mu";
    pushHistory();
}

void LvNomadBrowserScreen::showHome() {
    ++_generation; // supersede any in-flight browse's completion callback
    _done = true;
    setCancelVisible(false);
    _lastPhase = 0xFF; _lastAttempt = 0; _destHashShown = false;
    _pageSource.clear();
    _formFieldValues.clear();
    _checkboxRefs.clear();
    _textFieldRefs.clear();
    if (_lblAddress) lv_label_set_text(_lblAddress, "");
    updateFavoriteButton();
    if (_body) {
        lv_obj_clean(_body);
        _logText = nullptr;
        _links.clear();
        populateFavoriteRows(_body);

        // Escape hatch: open the browser with no page loaded at all -- lays the
        // groundwork for a future manual destination-hash/path entry feature
        // (not built yet, see openBlankBrowser()'s doc comment) without
        // forcing every visit here through a favorite or Peers.
        lv_obj_t* openAnyway = lv_btn_create(_body);
        lv_obj_set_size(openAnyway, lv_pct(100), 36);
        lv_obj_set_style_bg_color(openAnyway, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(openAnyway, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(openAnyway, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(openAnyway, 1, 0);
        lv_obj_set_style_radius(openAnyway, 4, 0);
        lv_obj_add_event_cb(openAnyway, [](lv_event_t* e) {
            auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
            self->openBlankBrowser();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* openAnywayLbl = lv_label_create(openAnyway);
        lv_obj_set_style_text_font(openAnywayLbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(openAnywayLbl, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_label_set_text(openAnywayLbl, "Open Browser (no page)");
        lv_obj_center(openAnywayLbl);
    }
}

void LvNomadBrowserScreen::openBlankBrowser() {
    _peerHex.clear();
    _displayName.clear();
    _path.clear(); // nothing to fetch yet -- see this method's .h doc comment
    _pageSource.clear();
    _formFieldValues.clear();
    _checkboxRefs.clear();
    _textFieldRefs.clear();
    ++_generation;
    _done = true;
    _urlInputActive = false;
    _urlInputText.clear();
    _editingFieldActive = false;
    _editingFieldText.clear();
    setCancelVisible(false);
    updateFavoriteButton();
    // Force the address bar visible (not just un-hidden if it happened to
    // already be shown) -- it's where the "Enter URL..." row below puts
    // what you type, so it should be the obvious focus of this state.
    _addressBarHidden = false;
    if (_addressBar) lv_obj_clear_flag(_addressBar, LV_OBJ_FLAG_HIDDEN);
    if (_lblAddress) {
        lv_obj_set_style_text_color(_lblAddress, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_label_set_text(_lblAddress, "No URL yet");
    }
    if (_body) {
        lv_obj_clean(_body);
        _links.clear();
        _logText = lv_label_create(_body);
        lv_obj_set_style_text_color(_logText, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_label_set_long_mode(_logText, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_logText, lv_pct(100));
        applyFontSize();
        lv_label_set_text(_logText,
            "No page loaded. Manual entry resolves against contacts you "
            "already know -- not an arbitrary shared URL yet.");

        lv_obj_t* enterUrl = lv_btn_create(_body);
        lv_obj_set_size(enterUrl, lv_pct(100), 36);
        lv_obj_set_style_bg_color(enterUrl, lv_color_hex(Theme::BG_SURFACE), 0);
        lv_obj_set_style_bg_opa(enterUrl, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(enterUrl, lv_color_hex(Theme::BORDER), 0);
        lv_obj_set_style_border_width(enterUrl, 1, 0);
        lv_obj_set_style_radius(enterUrl, 4, 0);
        lv_obj_add_event_cb(enterUrl, [](lv_event_t* e) {
            auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
            self->startUrlInput();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* enterUrlLbl = lv_label_create(enterUrl);
        lv_obj_set_style_text_font(enterUrlLbl, &lv_font_rsdeck_12, 0);
        lv_obj_set_style_text_color(enterUrlLbl, lv_color_hex(Theme::PRIMARY), 0);
        lv_label_set_text(enterUrlLbl, "Enter URL...");
        lv_obj_center(enterUrlLbl);
    }
}

void LvNomadBrowserScreen::startUrlInput() {
    _urlInputActive = true;
    _addressBeforeEdit = _lblAddress ? lv_label_get_text(_lblAddress) : "";
    // Pre-fill from _peerHex, not _addressBeforeEdit -- only _peerHex is a
    // valid NodeView lookup key (see this method's .h doc comment).
    _urlInputText = _peerHex.empty() ? "" : (_peerHex + ":" + _path);
    updateUrlInputDisplay();
    if (_body) {
        lv_obj_clean(_body);
        _links.clear();
        _logText = lv_label_create(_body);
        lv_obj_set_style_text_color(_logText, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_label_set_long_mode(_logText, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_logText, lv_pct(100));
        applyFontSize();
        lv_label_set_text(_logText,
            "Type a 32-character node hash (optionally \":path\"), then "
            "press Enter/OK on the keyboard. Must be a contact you already "
            "know -- see Peers.");

        // The keyboard's own "Close" button only dismisses the keyboard
        // widget (LvVirtualKeyboard::dismiss()) -- it doesn't tell this
        // screen input was cancelled, so Esc alone (handleKey()'s 0x1B
        // branch) would leave a touch-only user stuck with no way out.
        // This is the real, always-reachable cancel, matching
        // LvNodesScreen's nickname-input "Cancel" button exactly.
        lv_obj_t* cancel = lv_btn_create(_body);
        lv_group_remove_obj(cancel);
        lv_obj_set_size(cancel, 100, 32);
        lv_obj_add_style(cancel, LvTheme::styleBtn(), 0);
        lv_obj_add_style(cancel, LvTheme::styleBtnPressed(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(cancel, [](lv_event_t* e) {
            auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
            self->cancelUrlInput();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* cancelLbl = lv_label_create(cancel);
        lv_obj_set_style_text_font(cancelLbl, &lv_font_rsdeck_12, 0);
        lv_label_set_text(cancelLbl, "Cancel");
        lv_obj_center(cancelLbl);
    }
}

void LvNomadBrowserScreen::updateUrlInputDisplay() {
    if (!_lblAddress) return;
    lv_obj_set_style_text_color(_lblAddress, lv_color_hex(Theme::ACCENT), 0);
    lv_label_set_text(_lblAddress, (_urlInputText + "_").c_str());
}

void LvNomadBrowserScreen::cancelUrlInput() {
    if (_peerHex.empty()) {
        openBlankBrowser(); // clears _urlInputActive/_urlInputText and rebuilds the hint + button
        return;
    }
    // Editing an already-loaded page's address -- restore exactly what was
    // there (not a re-derived guess) and re-render the page already fetched,
    // no re-fetch.
    _urlInputActive = false;
    _urlInputText.clear();
    if (_lblAddress) {
        lv_obj_set_style_text_color(_lblAddress, lv_color_hex(Theme::TEXT_SECONDARY), 0);
        lv_label_set_text(_lblAddress, _addressBeforeEdit.c_str());
    }
    renderPage();
}

void LvNomadBrowserScreen::submitUrlInput() {
    const std::string input = _urlInputText;
    std::string hash, path;
    const bool hashIsHex32 = [&](const std::string& s) {
        return s.size() == 32 && std::all_of(s.begin(), s.end(),
            [](char c) { return std::isxdigit((unsigned char)c) != 0; });
    }(input.substr(0, std::min<size_t>(input.size(), 32)));
    if (input.size() >= 33 && input[32] == ':' && hashIsHex32) {
        hash = input.substr(0, 32);
        path = input.substr(33);
        if (path.empty()) path = "/page/index.mu";
    } else if (input.size() == 32 && hashIsHex32) {
        hash = input;
        path = "/page/index.mu";
    } else {
        if (_logText) lv_label_set_text(_logText,
            "Invalid format -- need a 32-character hex hash, optionally \":path\". Try again or Esc to cancel.");
        return; // stay in input mode so they can fix it
    }
    const DiscoveredNode* node = _service ? _service->nodes.findNodeByHex(hash) : nullptr;
    if (!node) {
        if (_logText) lv_label_set_text(_logText,
            "Unknown contact -- that hash isn't in your Peers list yet. Try again or Esc to cancel.");
        return;
    }
    _urlInputActive = false;
    if (_lblAddress) lv_obj_set_style_text_color(_lblAddress, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    // No display name -- see the favorites-list click handler's identical
    // comment: this screen's address bar shows the hash, not a friendly name.
    openFor(hash, "");
    _path = path; // openFor() defaults to index.mu; override with the typed path
    startBrowse();
}

std::string LvNomadBrowserScreen::getFormFieldValue(const std::string& name) const {
    for (const auto& kv : _formFieldValues) if (kv.first == name) return kv.second;
    return "";
}

void LvNomadBrowserScreen::setFormFieldValue(const std::string& name, const std::string& value) {
    for (auto& kv : _formFieldValues) if (kv.first == name) { kv.second = value; return; }
    _formFieldValues.push_back({name, value});
}

void LvNomadBrowserScreen::startFieldInput(const std::string& name, const std::string& currentText) {
    if (_urlInputActive) return; // shouldn't overlap, but never fight over the keyboard
    _editingFieldActive = true;
    _editingFieldName = name;
    _editingFieldText = currentText;
    // Reuse the address bar as the live-typed display, same as URL entry --
    // simplest way to show the buffer without adding a second floating input
    // widget, and the address bar is hidden-by-default/out of the way of the
    // actual field being edited in the page body below it.
    if (!_addressBar) return;
    _addressBarHidden = false;
    lv_obj_clear_flag(_addressBar, LV_OBJ_FLAG_HIDDEN);
    updateFieldInputDisplay();
}

void LvNomadBrowserScreen::updateFieldInputDisplay() {
    if (!_lblAddress) return;
    lv_obj_set_style_text_color(_lblAddress, lv_color_hex(Theme::ACCENT), 0);
    const std::string shown = "Editing \"" + _editingFieldName + "\": " + _editingFieldText + "_";
    lv_label_set_text(_lblAddress, shown.c_str());
}

void LvNomadBrowserScreen::submitFieldInput() {
    setFormFieldValue(_editingFieldName, _editingFieldText);
    _editingFieldActive = false;
    if (_lblAddress) lv_obj_set_style_text_color(_lblAddress, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    renderPage(); // rebuild the body so the field widget shows its new value
}

void LvNomadBrowserScreen::cancelFieldInput() {
    _editingFieldActive = false;
    if (_lblAddress) lv_obj_set_style_text_color(_lblAddress, lv_color_hex(Theme::TEXT_SECONDARY), 0);
    renderPage(); // discard the edit, restore whatever was actually committed
}

bool LvNomadBrowserScreen::buildPendingFieldData(const std::vector<std::string>& submitFields) {
    if (submitFields.empty()) return false;
    std::vector<std::pair<std::string, std::string>> toSubmit;
    for (const auto& token : submitFields) {
        if (token == "*") {
            toSubmit = _formFieldValues; // every field on the page, as-is
            break;
        }
        const size_t eq = token.find('=');
        if (eq != std::string::npos) {
            // A fixed literal baked into the link by the page itself (e.g. pagination
            // state like "s=0"), not a live field lookup.
            toSubmit.push_back({token.substr(0, eq), token.substr(eq + 1)});
            continue;
        }
        // A named field -- include its CURRENT live value if we have one. Per
        // reticulum-meshchat's onNodePageUrlClick(): a name with no matching input
        // just contributes nothing, not an error.
        for (const auto& kv : _formFieldValues) if (kv.first == token) { toSubmit.push_back(kv); break; }
    }
    if (toSubmit.empty()) return false;
    return encodeFieldDataMsgpack(toSubmit, _pendingFieldData);
}

void LvNomadBrowserScreen::showRawText(const std::string& source) {
    if (!_body) return;
    lv_obj_clean(_body);
    _logText = nullptr;
    _links.clear();
    lv_obj_t* lbl = lv_label_create(_body);
    lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(lbl, kFontSizes[_fontSizeIdx], 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_text(lbl, source.c_str());
}

void LvNomadBrowserScreen::renderMicron(const std::string& source) {
    if (!_body) return;
    lv_obj_clean(_body);
    _logText = nullptr;
    _links.clear();
    _checkboxRefs.clear(); // rebuilt below -- NOT _formFieldValues, which must survive
                            // a re-render (font change, field edit) -- see startBrowse().
    _textFieldRefs.clear();
    const auto lines = parseMicron(source);
    for (const auto& line : lines) {
        if (line.segments.size() == 1 && line.segments[0].kind == MicronSegment::Kind::Rule) {
            lv_obj_t* hr = lv_obj_create(_body);
            lv_obj_set_size(hr, lv_pct(100), 1);
            lv_obj_set_style_bg_color(hr, lv_color_hex(Theme::BORDER), 0);
            lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(hr, 0, 0);
            lv_obj_set_style_pad_all(hr, 0, 0);
            lv_obj_clear_flag(hr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            continue;
        }
        const bool hasLink = std::any_of(line.segments.begin(), line.segments.end(),
            [](const MicronSegment& s) { return s.kind == MicronSegment::Kind::Link; });
        // A plain line where every run shares the same style still fits the single-
        // recolored-label fast path (foreground color rides LVGL's inline "#hex text#"
        // tags); anything needing its own widget -- links, bold, underline, a per-run
        // background, or a form field -- forces the flex-row-of-widgets path below.
        const bool needsRunWidgets = hasLink || std::any_of(line.segments.begin(), line.segments.end(),
            [](const MicronSegment& s) { return s.bold || s.underline || s.hasBg || s.kind == MicronSegment::Kind::Field; });
        const size_t fontIdx = (line.heading > 0 && _fontSizeIdx > 0) ? _fontSizeIdx - 1 : _fontSizeIdx;
        const lv_font_t* font = kFontSizes[fontIdx];
        const lv_text_align_t textAlign = line.align == 'c' ? LV_TEXT_ALIGN_CENTER
                                         : line.align == 'r' ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;

        if (!needsRunWidgets) {
            // Common case: one recolored label carries the whole line -- LVGL's
            // "#RRGGBB text#" inline tags give per-run foreground color without
            // needing a separate widget per run.
            lv_obj_t* lbl = lv_label_create(_body);
            lv_label_set_recolor(lbl, true);
            lv_obj_set_style_text_font(lbl, font, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(line.heading > 0 ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(lbl, lv_pct(100));
            lv_obj_set_style_text_align(lbl, textAlign, 0);
            std::string markup;
            for (const auto& seg : line.segments) {
                if (seg.hasFg) {
                    markup += '#'; appendHexByte(markup, seg.fgR); appendHexByte(markup, seg.fgG);
                    appendHexByte(markup, seg.fgB); markup += ' ';
                    appendRecolorSafe(markup, seg.text);
                    markup += '#';
                } else {
                    appendRecolorSafe(markup, seg.text);
                }
            }
            lv_label_set_text(lbl, markup.c_str());
            continue;
        }

        // A line needing per-run styling (links, bold, underline, or background): a
        // wrapping flex row so each run is its own widget -- recolor tags alone can't
        // make part of a label independently clickable, bold, underlined, or bg-filled.
        lv_obj_t* row = lv_obj_create(_body);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(row,
            line.align == 'c' ? LV_FLEX_ALIGN_CENTER : line.align == 'r' ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        for (const auto& seg : line.segments) {
            if (seg.kind == MicronSegment::Kind::Link) {
                lv_obj_t* lbl = lv_label_create(row);
                lv_obj_set_style_text_font(lbl, seg.bold ? kBoldFontSizes[fontIdx] : font, 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(Theme::PRIMARY), 0);
                lv_obj_set_style_text_decor(lbl, LV_TEXT_DECOR_UNDERLINE, 0);
                if (seg.hasBg) {
                    lv_obj_set_style_bg_color(lbl, lv_color_make(seg.bgR, seg.bgG, seg.bgB), 0);
                    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
                }
                lv_label_set_text(lbl, seg.text.c_str());
                lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
                _links.push_back({seg.destHex, seg.path, seg.submitFields});
                lv_obj_set_user_data(lbl, reinterpret_cast<void*>(_links.size() - 1));
                lv_obj_add_event_cb(lbl, [](lv_event_t* e) {
                    auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
                    const size_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(e)));
                    if (idx >= self->_links.size()) return;
                    const auto link = self->_links[idx]; // copy -- navigateTo()/startBrowse() rebuild _links
                    if (link.destHex.empty()) {
                        // buildPendingFieldData() is a no-op (and _pendingFieldData stays
                        // untouched) for a plain link with no submitFields -- this is
                        // exactly the same navigation as before forms existed.
                        self->buildPendingFieldData(link.submitFields);
                        self->navigateTo(link.path);
                    } else {
                        // Cross-node links need a "browse by raw destination hash" entry
                        // point NomadBrowseClient doesn't have yet (it only derives a
                        // hash from a saved contact's public key) -- a clearly scoped
                        // follow-up, not a silent failure.
                        Serial.printf("[NOMAD] absolute link tap ignored (not yet supported): %s:%s\n",
                                      link.destHex.c_str(), link.path.c_str());
                    }
                }, LV_EVENT_CLICKED, this);
            } else if (seg.kind == MicronSegment::Kind::Field) {
                // Seed _formFieldValues from this field's parsed default THE FIRST TIME
                // it's encountered -- on every later re-render (font change, a committed
                // edit, raw/rendered toggle) it's already present and this is a no-op, so
                // the live value (not the page's original default) is what shows. Checked
                // by actual membership, NOT by whether the current value is empty -- an
                // unchecked checkbox's live value IS "", which must not be confused with
                // "never seeded" (that would silently re-check it on every re-render).
                bool alreadySeeded = false;
                for (const auto& kv : _formFieldValues) if (kv.first == seg.fieldName) { alreadySeeded = true; break; }
                if (!alreadySeeded) {
                    if (seg.fieldType == MicronSegment::FieldType::Text) {
                        setFormFieldValue(seg.fieldName, seg.fieldLabel);
                    } else {
                        setFormFieldValue(seg.fieldName, seg.fieldPrechecked ? seg.fieldValue : "");
                    }
                }
                if (seg.fieldType == MicronSegment::FieldType::Text) {
                    lv_obj_t* box = lv_obj_create(row);
                    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_bg_color(box, lv_color_hex(Theme::BG_SURFACE), 0);
                    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_color(box, lv_color_hex(Theme::BORDER), 0);
                    lv_obj_set_style_border_width(box, 1, 0);
                    lv_obj_set_style_radius(box, 3, 0);
                    lv_obj_set_style_pad_hor(box, 4, 0);
                    lv_obj_set_style_pad_ver(box, 1, 0);
                    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_t* fieldLbl = lv_label_create(box);
                    lv_obj_set_style_text_font(fieldLbl, font, 0);
                    lv_obj_set_style_text_color(fieldLbl, lv_color_hex(Theme::TEXT_PRIMARY), 0);
                    std::string shown = getFormFieldValue(seg.fieldName);
                    if (seg.fieldMasked) shown = std::string(shown.size(), '*');
                    if (shown.empty()) shown = " ";
                    lv_label_set_text(fieldLbl, shown.c_str());
                    _textFieldRefs.push_back(seg.fieldName);
                    lv_obj_set_user_data(box, reinterpret_cast<void*>(_textFieldRefs.size() - 1));
                    lv_obj_add_event_cb(box, [](lv_event_t* e) {
                        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
                        const size_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target(e)));
                        if (idx >= self->_textFieldRefs.size()) return;
                        const std::string& name = self->_textFieldRefs[idx];
                        self->startFieldInput(name, self->getFormFieldValue(name));
                    }, LV_EVENT_CLICKED, this);
                } else {
                    lv_obj_t* cb = lv_checkbox_create(row);
                    lv_obj_set_style_text_font(cb, font, 0);
                    lv_checkbox_set_text(cb, seg.fieldLabel.empty() ? " " : seg.fieldLabel.c_str());
                    if (getFormFieldValue(seg.fieldName) == seg.fieldValue && !seg.fieldValue.empty())
                        lv_obj_add_state(cb, LV_STATE_CHECKED);
                    _checkboxRefs.push_back({seg.fieldName, seg.fieldValue});
                    lv_obj_set_user_data(cb, reinterpret_cast<void*>(_checkboxRefs.size() - 1));
                    lv_obj_add_event_cb(cb, [](lv_event_t* e) {
                        auto* self = static_cast<LvNomadBrowserScreen*>(lv_event_get_user_data(e));
                        lv_obj_t* target = lv_event_get_target(e);
                        const size_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));
                        if (idx >= self->_checkboxRefs.size()) return;
                        const auto& ref = self->_checkboxRefs[idx];
                        const bool checked = lv_obj_has_state(target, LV_STATE_CHECKED);
                        self->setFormFieldValue(ref.name, checked ? ref.value : "");
                    }, LV_EVENT_VALUE_CHANGED, this);
                }
            } else {
                lv_obj_t* lbl = lv_label_create(row);
                lv_label_set_recolor(lbl, true);
                lv_obj_set_style_text_font(lbl, seg.bold ? kBoldFontSizes[fontIdx] : font, 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(line.heading > 0 ? Theme::ACCENT : Theme::TEXT_PRIMARY), 0);
                if (seg.underline) lv_obj_set_style_text_decor(lbl, LV_TEXT_DECOR_UNDERLINE, 0);
                if (seg.hasBg) {
                    lv_obj_set_style_bg_color(lbl, lv_color_make(seg.bgR, seg.bgG, seg.bgB), 0);
                    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
                }
                std::string markup;
                if (seg.hasFg) {
                    markup += '#'; appendHexByte(markup, seg.fgR); appendHexByte(markup, seg.fgG);
                    appendHexByte(markup, seg.fgB); markup += ' ';
                    appendRecolorSafe(markup, seg.text);
                    markup += '#';
                } else {
                    appendRecolorSafe(markup, seg.text);
                }
                lv_label_set_text(lbl, markup.c_str());
            }
        }
    }
}

#endif
