#include "MessageStore.h"
#include "MessageTransactions.h"
#include "runtime/TaskOwner.h"
#include <algorithm>

using namespace handheld::storage;

bool MessageStore::loadStartupMetadata() {
    handheld::assertDeviceOwner();
    StorageLease lease;
    if (!lease.held()) return false;
    _startupRecentMessageIds.clear();
    int unread = 0; uint32_t total = 0;
    char after[33] = {}, peer[33]; MessageTransactions::RecentIds recent;
    Error scanError = Error::None;
    // Preserve the established lexical peer/counter tie order for boot hints.
    // Runtime views use typed queries and never retain these aggregate rows.
    while (transactions().nextPeer(after, peer, scanError)) {
        memcpy(after, peer, sizeof(after));
        uint8_t peerBytes[16]; decodeHex(peer, 32, peerBytes, 16);
        ConversationView row; ConversationSelector selector;
        const auto error = transactions().summarize(peerBytes, row, selector, &recent, STARTUP_RECENT_CAP);
        // Lists show an unreadable conversation as a placeholder; it must not stop startup.
        if (error == Error::InvalidRecord) continue;
        if (error != Error::None) { releaseStartupSeeds(); return false; }
        if (!row.totalCount) continue;
        if (total < UINT32_MAX) ++total;
        const auto count = int(std::min(row.unreadCount, uint32_t(INT_MAX)));
        unread = count > INT_MAX - unread ? INT_MAX : unread + count;
    }
    if (scanError != Error::None) { releaseStartupSeeds(); return false; }
    std::sort(recent.begin(), recent.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& item : recent)
        if (std::find(_startupRecentMessageIds.begin(), _startupRecentMessageIds.end(), item.second) == _startupRecentMessageIds.end())
            _startupRecentMessageIds.push_back(item.second);
    _totalConversations = total; _totalUnread = unread;
    return true;
}
