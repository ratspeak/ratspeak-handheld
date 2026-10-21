#pragma once

#include <lvgl.h>

class LvTabBar {
public:
#if defined(RSSENSECAPD1L)
    // D1L-only for now (not yet tried on a narrower panel) -- Nomad sits right
    // after Peers, since that's where favorited nodes come from.
    enum Tab {
        TAB_HOME = 0,
        TAB_MSGS = 1,
        TAB_CONTACTS = 2,
        TAB_NODES = 3,
        TAB_NOMAD = 4,
        TAB_SETTINGS = 5,
        TAB_CHATS = TAB_MSGS,
        TAB_PEERS = TAB_NODES,
        TAB_SETUP = TAB_SETTINGS,
        TAB_COUNT = 6
    };
#else
    enum Tab {
        TAB_HOME = 0,
        TAB_MSGS = 1,
        TAB_CONTACTS = 2,
        TAB_NODES = 3,
        TAB_SETTINGS = 4,
        TAB_CHATS = TAB_MSGS,
        TAB_PEERS = TAB_NODES,
        TAB_SETUP = TAB_SETTINGS,
        TAB_COUNT = 5
    };
#endif

    void create(lv_obj_t* parent);

    void setActiveTab(int tab);
    // Updates only the visual highlight, no callback -- use when the caller
    // is already switching screens itself by other means (e.g. a per-contact
    // action's openFor() + setScreen()) and just needs the tab bar's
    // indicator to match, without re-triggering whatever else that tab's own
    // change-callback does (see LvglApplication.cpp's enterTab() -- the
    // Nomad tab's entry clears the currently-browsed peer to land on its
    // favorites home state, which would stomp on a peer just set via
    // openFor() if setActiveTab() ran that same callback here too).
    void setActiveTabDisplay(int tab);
    int getActiveTab() const { return _activeTab; }
    void cycleTab(int direction);

    void setUnreadCount(int tab, int count);

    using TabCallback = void(*)(int tab);
    void setTabCallback(TabCallback cb) { _tabCb = cb; }

    lv_obj_t* obj() { return _bar; }

    // Re-apply palette-dependent label colors after a theme switch
    void refreshTabs();

private:
    void refreshTab(int idx);

    lv_obj_t* _bar = nullptr;
    lv_obj_t* _cells[TAB_COUNT] = {};
    lv_obj_t* _labels[TAB_COUNT] = {};
    lv_obj_t* _badges[TAB_COUNT] = {};
    lv_obj_t* _badgeLabels[TAB_COUNT] = {};
    int _activeTab = TAB_HOME;
    int _unread[TAB_COUNT] = {};
    TabCallback _tabCb = nullptr;

#if defined(RSSENSECAPD1L)
    static constexpr const char* TAB_NAMES[TAB_COUNT] = {"Home", "Chats", "Contacts", "Peers", "Nomad", "Settings"};
#else
    static constexpr const char* TAB_NAMES[TAB_COUNT] = {"Home", "Chats", "Contacts", "Peers", "Settings"};
#endif
};
