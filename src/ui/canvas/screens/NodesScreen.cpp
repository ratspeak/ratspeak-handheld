#include "NodesScreen.h"
#include "Theme.h"
#include "PageNavigation.h"
#include <algorithm>

void NodesScreen::onEnter() {
    _pages.reset(); _pageFocus = -1; _nodeHashes.clear(); _list.clear();
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

    std::string prevSelected;
    int oldIdx = _list.getSelectedIndex();
    if (oldIdx >= 0 && oldIdx < (int)_nodeHashes.size()) {
        prevSelected = _nodeHashes[oldIdx];
    }

    _pages.sync(_announces->nodes(), prevSelected);
    std::vector<std::string> hashes;
    for (size_t i = 0; i < _pages.count(); ++i) hashes.push_back(_pages.hash(i));
    const bool changed = hashes != _nodeHashes;
    if (changed) _list.clear();
    _nodeHashes = std::move(hashes);
    int selected = 0;
    for (size_t i = 0; i < _pages.count(); ++i) {
        const auto& node = _announces->nodes()[_pages.sourceIndex(i)];
        char line[128]; formatNodeLine(line, sizeof(line), node);
        const auto color = node.saved ? Theme::ACCENT : stalenessColor(node);
        if (changed) _list.addItem(line, color);
        else _list.updateItem(i, line, color);
        if (_nodeHashes[i] == prevSelected) selected = i;
    }
    if (changed) _list.setSelected(selected);
    if (_pageFocus >= 0 && !_pages.enabled(_pageFocus))
        _pageFocus = _pages.enabled(2) ? 2 : _pages.enabled(1) ? 1 : -1;
    _lastRefresh = millis();
}

void NodesScreen::activatePage(unsigned action) {
    if (!_pages.navigate(action)) return;
    _nodeHashes.clear(); _list.clear();
    refreshList();
}

void NodesScreen::showActionMenu(int nodeIdx) {
    if (!_announces || nodeIdx < 0 || nodeIdx >= (int)_nodeHashes.size()) return;

    _selectedNodeIdx = nodeIdx;
    _selectedNodeHash = _nodeHashes[nodeIdx];

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
    _selectedNodeIdx = -1;
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
        canvas.printf("Page %u/%u  /  %u peers", unsigned(_pages.page() + 1), unsigned(_pages.pages()), unsigned(_pages.total()));

        canvas.drawFastHLine(0, y + headerH, Theme::SCREEN_W, Theme::DIVIDER);
        y += headerH + 2;

        if (_list.itemCount() == 0) {
            Theme::useSmallFont(canvas);
            canvas.setTextColor(Theme::MUTED);
            canvas.setCursor(4, y + 10);
            canvas.print("No peers discovered yet.");
            canvas.setCursor(4, y + 22);
            canvas.print("Waiting for announces...");
        } else {
            _list.render(canvas, 0, y, Theme::SCREEN_W,
                         Theme::CONTENT_H - (y - Theme::CONTENT_Y) - handheld::canvas::NavigationHeight - 4,
                         _pageFocus < 0);
        }
        handheld::canvas::drawPageNavigation(canvas,
            Theme::CONTENT_Y + Theme::CONTENT_H - handheld::canvas::NavigationHeight, _pageFocus,
            [this](int action) { return _pages.enabled(action); });
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

    if (_pageFocus >= 0) {
        if (event.escape || event.backspace || event.navUp()) { _pageFocus = -1; return true; }
        if (event.navLeft() || event.navRight()) {
            const int direction = event.navLeft() ? -1 : 1;
            for (int i = _pageFocus + direction; i >= 0 && i < 4; i += direction)
                if (_pages.enabled(i)) { _pageFocus = i; break; }
            return true;
        }
        if (event.enter) { if (!event.repeat) activatePage(_pageFocus); return true; }
        if (event.navDown()) return true;
    }
    if (event.navUp()) {
        if (_list.getSelectedIndex() > 0) _list.scrollUp();
        return true;
    }
    if (event.navDown()) {
        if (_list.getSelectedIndex() + 1 < _list.itemCount()) _list.scrollDown();
        else if (!event.repeat) _pageFocus = _pages.enabled(2) ? 2 : _pages.enabled(1) ? 1 : -1;
        return true;
    }
    if (event.enter) {
        int idx = _list.getSelectedIndex();
        if (idx >= 0 && idx < (int)_nodeHashes.size() && !_nodeHashes[idx].empty()) {
            showActionMenu(idx);
        }
        return true;
    }
    return false;
}
