#include "NodesScreen.h"
#include "Theme.h"
#include <algorithm>

void NodesScreen::onEnter() {
    _showingActions = false;
    _peers.reset();
    const int bodyHeight = Theme::CONTENT_H - Theme::SECTION_HEADER_H - 2;
    _peers.configure(Theme::LIST_ROW_H, (bodyHeight / Theme::LIST_ROW_H) * Theme::LIST_ROW_H);
    refreshList();
}

void NodesScreen::onExit() {
    _showingActions = false;
}

// Helper: truncate name to maxChars respecting UTF-8 boundaries
static std::string truncUTF8(const std::string& name, size_t maxChars) {
    size_t chars = 0, i = 0;
    const uint8_t* p = (const uint8_t*)name.data();
    size_t sz = name.size();
    while (i < sz && chars < maxChars) {
        uint8_t c = p[i];
        size_t seqLen = 1;
        if ((c & 0xE0) == 0xC0) seqLen = 2;
        else if ((c & 0xF0) == 0xE0) seqLen = 3;
        else if ((c & 0xF8) == 0xF0) seqLen = 4;
        if (i + seqLen > sz) break;
        i += seqLen;
        chars++;
    }
    return name.substr(0, i);
}

// Helper: format a node line
static void formatNodeLine(char* line, size_t lineSize, const DiscoveredNode& node) {
    std::string displayName = truncUTF8(node.name.empty() ? node.hash.toHex().substr(0, 12) : node.name, 18);
    if (node.lastSeen == 0) {
        snprintf(line, lineSize, "%-20s saved", displayName.c_str());
        return;
    }
    unsigned long ago = (millis() - node.lastSeen) / 1000;
    if (ago < 60) {
        if (node.hops < 128)
            snprintf(line, lineSize, "%-20s %3lus %dhop", displayName.c_str(), ago, node.hops);
        else
            snprintf(line, lineSize, "%-20s %3lus", displayName.c_str(), ago);
    } else {
        if (node.hops < 128)
            snprintf(line, lineSize, "%-20s %3lum %dhop", displayName.c_str(), ago / 60, node.hops);
        else
            snprintf(line, lineSize, "%-20s %3lum", displayName.c_str(), ago / 60);
    }
}

// Helper: staleness color for a node
static uint16_t stalenessColor(const DiscoveredNode& node) {
    if (node.lastSeen == 0) return Theme::MUTED;
    unsigned long ageMs = millis() - node.lastSeen;
    if (ageMs > 1800000) return Theme::MUTED;
    if (ageMs > 300000) return Theme::BORDER + 0x0220;
    return 0;  // default
}

void NodesScreen::refreshList() {
    if (!_announces) return;

    _peers.sync(_announces->nodes());
    _lastRefresh = millis();
}

void NodesScreen::showActionMenu(int nodeIdx) {
    if (!_announces || nodeIdx < 0 || nodeIdx >= (int)_peers.total()) return;

    _selectedNodeHash = _peers.hash(nodeIdx);

    // Try to find live node — may have been evicted since list was built
    rs::Bytes hash;
    hash.assignHex(_selectedNodeHash.c_str());
    const DiscoveredNode* node = _announces->findNode(hash);

    if (node) {
        _selectedNodeName = node->name;
        _selectedNodeSaved = node->saved;
    } else {
        // Node was evicted — use name from the list item text, mark as not saved
        _selectedNodeName = _selectedNodeHash.substr(0, 12);
        // Try name cache as fallback
        std::string cached = _announces->lookupName(_selectedNodeHash);
        if (!cached.empty()) _selectedNodeName = cached;
        _selectedNodeSaved = false;
    }

    _actionList.clear();
    _actionList.addItem("Message");
    if (node) {
        if (_selectedNodeSaved) {
            _actionList.addItem("Remove Contact");
        } else {
            _actionList.addItem("Save Contact");
        }
    }
    _actionList.addItem("Back");
    _actionList.setSelected(0);

    _showingActions = true;
}

void NodesScreen::executeAction(int actionIdx) {
    const std::string& action = _actionList.getSelectedItem();

    if (action == "Message") {
        exitActionMenu();
        if (_selectCb) {
            _selectCb(_selectedNodeHash);
        }
    } else if (action == "Save Contact") {
        if (_saveCb) _saveCb(_selectedNodeHash, true);
        exitActionMenu();
        refreshList();
    } else if (action == "Remove Contact") {
        if (_saveCb) _saveCb(_selectedNodeHash, false);
        exitActionMenu();
        refreshList();
    } else {
        exitActionMenu();
    }
}

void NodesScreen::exitActionMenu() {
    _showingActions = false;
}

void NodesScreen::render(M5Canvas& canvas) {
    if (!_showingActions && millis() - _lastRefresh >= 1000) refreshList();

    int y = Theme::CONTENT_Y;

    if (_showingActions) {
        const int headerH = Theme::SECTION_HEADER_H;
        canvas.fillRect(0, y, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
        canvas.fillRect(0, y + 2, 3, headerH - 4, Theme::PRIMARY);
        Theme::useUiFont(canvas);
        canvas.setTextColor(Theme::TEXT_PRIMARY);
        {
            char truncName[80];
            size_t chars = 0, i = 0;
            const uint8_t* p = (const uint8_t*)_selectedNodeName.data();
            size_t sz = _selectedNodeName.size();
            while (i < sz && chars < 26 && i < sizeof(truncName) - 1) {
                uint8_t c = p[i];
                size_t seqLen = 1;
                if ((c & 0xE0) == 0xC0) seqLen = 2;
                else if ((c & 0xF0) == 0xE0) seqLen = 3;
                else if ((c & 0xF8) == 0xF0) seqLen = 4;
                if (i + seqLen > sz || i + seqLen > sizeof(truncName) - 1) break;
                i += seqLen;
                chars++;
            }
            memcpy(truncName, _selectedNodeName.data(), i);
            truncName[i] = '\0';
            canvas.drawString(truncName, 8, y + 2);
        }
        y += headerH + 2;
        canvas.drawFastHLine(0, y, Theme::SCREEN_W, Theme::DIVIDER);
        y += 2;

        _actionList.render(canvas, 0, y, Theme::SCREEN_W, Theme::CONTENT_H - (y - Theme::CONTENT_Y));
    } else {
        const int headerH = Theme::SECTION_HEADER_H;
        canvas.fillRect(0, y, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
        canvas.fillRect(0, y + 2, 3, headerH - 4, Theme::ACCENT);
        Theme::useUiFont(canvas);
        canvas.setTextColor(Theme::ACCENT);
        canvas.setCursor(8, y + 2);
        canvas.printf("%u peers", unsigned(_peers.total()));

        canvas.drawFastHLine(0, y + headerH, Theme::SCREEN_W, Theme::DIVIDER);
        y += headerH + 2;

        if (_peers.total() == 0) {
            Theme::useSmallFont(canvas);
            canvas.setTextColor(Theme::MUTED);
            canvas.setCursor(4, y + 10);
            canvas.print("No peers discovered yet.");
            canvas.setCursor(4, y + 22);
            canvas.print("Waiting for announces...");
        } else {
            // Only the viewport is formatted; the shared order stores IDs,
            // never a second 50-row cache of display text.
            const size_t begin = _peers.visibleBegin(), end = _peers.visibleEnd();
            const int bodyHeight = Theme::CONTENT_H - (y - Theme::CONTENT_Y);
            for (size_t i = begin; i < end; ++i) {
                const auto* node = _announces->findNodeByHex(_peers.hash(i));
                char line[128];
                if (node) formatNodeLine(line, sizeof(line), *node);
                else snprintf(line, sizeof(line), "%.12s unavailable", _peers.hash(i).c_str());
                const auto color = node ? (node->saved ? Theme::ACCENT : stalenessColor(*node)) : Theme::MUTED;
                const int rowY = y + int(i - begin) * Theme::LIST_ROW_H;
                if (i + 1 < end && i != _peers.selected())
                    canvas.drawFastHLine(8, rowY + Theme::LIST_ROW_H - 1, Theme::CONTENT_W - 12, Theme::DIVIDER);
                ScrollList::renderRow(canvas, line, 0, rowY, Theme::CONTENT_W, i == _peers.selected(), color);
            }
            if (_peers.maxScroll() > 0) {
                const int barH = std::max(6, bodyHeight * int(end - begin) / int(_peers.total()));
                const int barY = y + _peers.scrollY() * (bodyHeight - barH) / _peers.maxScroll();
                canvas.fillRoundRect(Theme::CONTENT_W - 3, y, 2, bodyHeight, 1, Theme::DIVIDER);
                canvas.fillRoundRect(Theme::CONTENT_W - 4, barY, 3, barH, 1, Theme::PRIMARY_MUTED);
            }
        }
    }
    Theme::useSmallFont(canvas);
}

bool NodesScreen::handleKey(const KeyEvent& event) {
    if (event.repeat && (event.backspace || event.forwardDelete)) return true;
    if (_showingActions) {
        // ESC or Delete exits action menu
        if (event.escape || event.backspace) {
            exitActionMenu();
            return true;
        }
        if (event.navUp()) {
            _actionList.scrollUp();
            return true;
        }
        if (event.navDown()) {
            _actionList.scrollDown();
            return true;
        }
        if (event.enter) {
            executeAction(_actionList.getSelectedIndex());
            return true;
        }
        return true;
    }

    if (event.navUp() || event.navDown()) {
        _peers.move(event.navUp() ? -1 : 1);
        _peers.ensureSelectedVisible();
        return true;
    }
    if (event.enter) {
        if (_peers.total()) showActionMenu(int(_peers.selected()));
        return true;
    }
    return false;
}
