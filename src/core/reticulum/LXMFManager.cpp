#include "reticulum/LXMFManager.h"

#include <Arduino.h>

bool LXMFManager::beginStoreOnly(MessageStore* store) {
    _store = store;
    Serial.println("[LXMF] Manager started (store-only: reads via MessageStore, send via backend)");
    return true;
}

const std::vector<std::string>& LXMFManager::conversations() const {
    if (_store) return _store->conversations();
    static std::vector<std::string> empty;
    return empty;
}

std::vector<LXMFMessage> LXMFManager::getMessages(const std::string& peerHex) const {
    if (_store) return _store->loadConversation(peerHex);
    return {};
}

std::vector<LXMFMessage> LXMFManager::getRecentMessages(const std::string& peerHex,
                                                        size_t maxMessages) const {
    if (_store) return _store->loadConversationTail(peerHex, maxMessages);
    return {};
}

int LXMFManager::unreadCount(const std::string& peerHex) const {
    if (!_store) return 0;
    if (peerHex.empty()) return _store->totalUnreadCount();
    const ConversationSummary* s = _store->getSummary(peerHex);
    return s ? s->unreadCount : 0;
}

const ConversationSummary* LXMFManager::getConversationSummary(const std::string& peerHex) const {
    if (!_store) return nullptr;
    return _store->getSummary(peerHex);
}

void LXMFManager::markRead(const std::string& peerHex) {
    if (_store) { _store->markConversationRead(peerHex); }
}

bool LXMFManager::deleteConversation(const std::string& peerHex) {
    // Engine-side outbound queue/proof purge is the backend's concern.
    return _store ? _store->deleteConversation(peerHex) : false;
}
