#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace handheld {
// A directory-sized ID order, with independent keyboard and pixel anchors.
// Record indices are used only inside sync's current publication; renderers
// resolve hash() against their current view whenever they bind visible rows.
class PeerList {
public:
    void reset() { _ids.clear(); _selected = 0; _scrollY = 0; }
    void configure(int rowHeight, int viewportHeight) {
        _rowHeight = std::max(1, rowHeight);
        _viewportHeight = std::max(1, viewportHeight);
        setScrollY(_scrollY);
    }
    size_t total() const { return _ids.size(); }
    const std::string& hash(size_t index) const { return _ids[index]; }
    size_t selected() const { return _selected; }
    const std::string& selectedHash() const {
        static const std::string empty;
        return _ids.empty() ? empty : _ids[_selected];
    }
    void select(size_t index) { _selected = _ids.empty() ? 0 : std::min(index, total() - 1); }
    bool move(int delta) {
        const size_t before = _selected;
        select(size_t(std::max(0, std::min(int(total()) - 1, int(_selected) + delta))));
        return before != _selected;
    }
    int scrollY() const { return _scrollY; }
    int maxScroll() const { return std::max(0, int(total()) * _rowHeight - _viewportHeight); }
    void setScrollY(int y) { _scrollY = std::max(0, std::min(y, maxScroll())); }
    size_t top() const { return size_t(_scrollY / _rowHeight); }
    int topOffset() const { return _scrollY % _rowHeight; }
    size_t visibleBegin(size_t overscan = 0) const { return top() > overscan ? top() - overscan : 0; }
    size_t visibleEnd(size_t overscan = 0) const {
        return std::min(total(), size_t((_scrollY + _viewportHeight + _rowHeight - 1) / _rowHeight) + overscan);
    }
    void ensureSelectedVisible() {
        const int y = int(_selected) * _rowHeight;
        if (y < _scrollY) setScrollY(y);
        else if (y + _rowHeight > _scrollY + _viewportHeight)
            setScrollY(y + _rowHeight - _viewportHeight);
    }

    template<class Nodes> void sync(const Nodes& nodes) {
        struct Incoming { std::string id; size_t index; bool retained = false; };
        std::vector<Incoming> incoming;
        incoming.reserve(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) incoming.push_back({nodes[i].hash.toHex(), i});
        std::sort(incoming.begin(), incoming.end(), [](const Incoming& a, const Incoming& b) { return a.id < b.id; });
        std::vector<bool> survives(total(), false);
        for (size_t i = 0; i < total(); ++i) {
            auto found = std::lower_bound(incoming.begin(), incoming.end(), _ids[i],
                [](const Incoming& entry, const std::string& id) { return entry.id < id; });
            if (found != incoming.end() && found->id == _ids[i]) {
                survives[i] = true;
                found->retained = true;
            }
        }
        // Prefer the successor on an equal-distance tie. Both anchors are
        // chosen from the old order before removals compact that order.
        auto survivor = [&](size_t oldIndex) -> std::string {
            for (size_t distance = 0; distance < total(); ++distance) {
                if (oldIndex + distance < total() && survives[oldIndex + distance]) return _ids[oldIndex + distance];
                if (distance <= oldIndex && survives[oldIndex - distance]) return _ids[oldIndex - distance];
            }
            return {};
        };
        const auto selectedId = survivor(_selected);
        const auto topId = survivor(top());
        const int offset = topOffset();
        size_t kept = 0;
        for (size_t i = 0; i < total(); ++i) if (survives[i]) {
            if (kept != i) _ids[kept] = std::move(_ids[i]);
            ++kept;
        }
        _ids.resize(kept);
        incoming.erase(std::remove_if(incoming.begin(), incoming.end(), [](const Incoming& e) { return e.retained; }), incoming.end());
        std::sort(incoming.begin(), incoming.end(), [&](const Incoming& a, const Incoming& b) {
            const auto& x = nodes[a.index]; const auto& y = nodes[b.index];
            if (x.saved != y.saved) return x.saved;
            if (x.lastSeen != y.lastSeen) return x.lastSeen > y.lastSeen;
            return a.id < b.id;
        });
        for (auto& entry : incoming) _ids.push_back(std::move(entry.id));
        auto indexOf = [&](const std::string& id) {
            const auto found = std::find(_ids.begin(), _ids.end(), id);
            return found == _ids.end() ? size_t(0) : size_t(found - _ids.begin());
        };
        select(indexOf(selectedId));
        setScrollY(topId.empty() ? 0 : int(indexOf(topId)) * _rowHeight + offset);
    }
private:
    std::vector<std::string> _ids;
    size_t _selected = 0;
    int _scrollY = 0;
    int _rowHeight = 1;
    int _viewportHeight = 1;
};
}
