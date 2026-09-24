#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

namespace handheld {
// IDs, not names or node pointers, survive directory compaction. Re-announces
// update a row in place; only entering the screen sorts the directory again.
class PeerPager {
public:
    static constexpr size_t PageSize = 10;
    void reset() { _entries.clear(); _page = 0; }
    size_t total() const { return _entries.size(); }
    size_t pages() const { return std::max(size_t(1), (total() + PageSize - 1) / PageSize); }
    size_t page() const { return _page; }
    size_t count() const { return std::min(PageSize, total() - _page * PageSize); }
    const std::string& hash(size_t row) const { return _entries[_page * PageSize + row].hash; }
    size_t sourceIndex(size_t row) const { return _entries[_page * PageSize + row].index; }
    bool enabled(unsigned action) const { return action < 2 ? _page > 0 : _page + 1 < pages(); }
    bool navigate(unsigned action) {
        if (action > 3 || !enabled(action)) return false;
        _page = action == 0 ? 0 : action == 1 ? _page - 1 : action == 2 ? _page + 1 : pages() - 1;
        return true;
    }
    template<class Nodes> void sync(const Nodes& nodes, const std::string& anchor = {}) {
        std::vector<Entry> incoming;
        incoming.reserve(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) incoming.push_back({nodes[i].hash.toHex(), i});
        std::sort(incoming.begin(), incoming.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
        // Binary lookup keeps reconciliation O(n log n), including at capacity.
        // Preserve existing positions and update only their source indices.
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(), [&](Entry& entry) {
            auto found = std::lower_bound(incoming.begin(), incoming.end(), entry.hash,
                [](const Entry& e, const std::string& hash) { return e.hash < hash; });
            if (found == incoming.end() || found->hash != entry.hash) return true;
            entry.index = found->index;
            found->index = nodes.size(); // Already present; do not append again.
            return false;
        }), _entries.end());
        incoming.erase(std::remove_if(incoming.begin(), incoming.end(), [&](const Entry& e) {
            return e.index == nodes.size();
        }), incoming.end());
        std::sort(incoming.begin(), incoming.end(), [&](const Entry& a, const Entry& b) {
            const auto& x = nodes[a.index]; const auto& y = nodes[b.index];
            if (x.saved != y.saved) return x.saved;
            if (x.lastSeen != y.lastSeen) return x.lastSeen > y.lastSeen;
            return a.hash < b.hash;
        });
        // The directory is bounded (100 peers, 50 on Cardputer); newly heard
        // peers append without moving existing selections or page boundaries.
        _entries.insert(_entries.end(), std::make_move_iterator(incoming.begin()), std::make_move_iterator(incoming.end()));
        if (!anchor.empty()) {
            for (size_t i = 0; i < total(); ++i) if (_entries[i].hash == anchor) { _page = i / PageSize; break; }
        }
        _page = std::min(_page, pages() - 1);
    }
private:
    struct Entry { std::string hash; size_t index; };
    std::vector<Entry> _entries;
    size_t _page = 0;
};
}
