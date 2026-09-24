#pragma once

#include <M5GFX.h>
#include <vector>
#include <string>
#include "Theme.h"

class ScrollList {
public:
    void setItems(const std::vector<std::string>& items);
    void addItem(const std::string& item);
    void addItem(const std::string& item, uint16_t color);
    void clear();
    void updateItem(size_t index, const std::string& item, uint16_t color);

    void render(M5Canvas& canvas, int x, int y, int w, int h, bool showSelection = true);
    // Render an externally owned row without copying it into an items vector.
    static void renderRow(M5Canvas&, const std::string&, int x, int y, int width,
                          bool selected, uint16_t color = 0);

    // Navigation
    void scrollUp();
    void scrollDown();
    void setSelected(int idx);
    int getSelectedIndex() const { return _selected; }
    const std::string& getSelectedItem() const;

    int itemCount() const { return _items.size(); }

private:
    std::vector<std::string> _items;
    std::vector<uint16_t> _itemColors;  // Per-item color override (0 = use default)
    int _selected = 0;
    int _scrollOffset = 0;
    int _visibleRows = 0;
};
