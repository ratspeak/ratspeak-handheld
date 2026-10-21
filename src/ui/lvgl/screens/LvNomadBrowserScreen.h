#pragma once

// D1L-only (touch-only, square panel): a dedicated fullscreen NomadNet page
// viewer. Reached either as its own "Nomad" tab (landing on the favorites
// home state, see openHome()) or from the Peers screen's "Nomad Browser"
// action (openFor()). Renders Micron markup (headings, colors, alignment,
// horizontal rules, tappable links) -- see ratspeak-handheld TODO.md's
// "Nomad browser from Peers menu". No close button of its own -- like every
// other tab-root screen (Peers/Home/Settings), the bottom tab bar is always
// available as the way back; a dedicated close button here could only ever
// desync the tab bar's active highlight from what's actually on screen.
#if defined(RSSENSECAPD1L)

#include "UIManager.h"
#include "runtime/ServiceClient.h"
#include <string>
#include <vector>

class LvNomadBrowserScreen : public LvScreen {
public:
    void setService(handheld::ServiceClient* service) { _service = service; }

    void createUI(lv_obj_t* parent) override;
    void destroyUI() override;
    void refreshUI() override;
    void onEnter() override;
    bool handleKey(const KeyEvent& event) override;
    bool wantsVirtualKeyboard() const override { return _urlInputActive || _editingFieldActive; }

    const char* title() const override { return "Nomad Browser"; }

    // Stash the peer to browse -- call this BEFORE UIManager::setScreen(),
    // while this screen isn't current yet. The actual request fires from
    // onEnter(), once this screen's own widgets exist.
    void openFor(const std::string& peerHex, const std::string& displayName);

    // Called before switching to this screen from the "Nomad" tab bar entry
    // (not from Peers' per-contact action, which uses openFor() instead) --
    // clears any previously-browsed peer so onEnter() lands on the
    // favorites home state (showHome()) rather than re-browsing whatever
    // was last open.
    void openHome();

    // A tapped relative Micron link ("`[label`:/page/other.mu]`", no destination hash
    // of its own -- same node, different page): re-browse the SAME peer at the new
    // path. Public so the renderer's link click handlers (static LVGL callbacks) can
    // reach it directly via lv_event_get_user_data().
    void navigateTo(const std::string& path);

private:
    void startBrowse();
    // Back/Forward: a stack of every page actually navigated to (openFor()/
    // navigateTo()/openHome(), NOT Reload or the internal cancel-then-retry
    // path), with _historyIndex as the current position -- same shape as a
    // real browser's history. pushHistory() dedupes against the current top
    // entry (repeatedly tapping the "Nomad" tab bar entry while already on
    // Home, for instance, shouldn't pile up identical entries) and drops any
    // abandoned forward entries, matching standard back/forward semantics.
    // Back/Forward themselves restore a stored entry via showHome()/
    // startBrowse() directly (not through openFor()/navigateTo()) so landing
    // on a history entry never pushes a new one.
    struct HistoryEntry { std::string peerHex, displayName, path; };
    std::vector<HistoryEntry> _history;
    int _historyIndex = -1;
    // Set by openFor()/navigateTo() (a genuine new destination), consumed by
    // startBrowse() right before it actually submits a request -- not any
    // earlier, so submitUrlInput()'s openFor()-then-override-_path sequence
    // and the admission-conflict cancel-then-retry dance in startBrowse()
    // both push using the final, correct state instead of a half-set one.
    bool _needsHistoryPush = false;
    void pushHistory();
    void goBack();
    void goForward();
    void restoreHistoryEntry();
    void updateHistoryButtons();
    void appendLog(const std::string& line);
    void pollProgress();
    void cycleFontSize();
    void applyFontSize();
    // Parses the fetched page's Micron source and rebuilds _body's children as styled,
    // tappable LVGL widgets -- see the .cpp's anonymous-namespace parser for the actual
    // markup grammar handled (headings, colors, alignment, rules, links).
    void renderMicron(const std::string& source);
    // Plain, unparsed view of the last fetched page -- one wrapped label, no Micron
    // styling. Toggled by the "{}" header button; see _showRaw.
    void showRawText(const std::string& source);
    // Dispatches to renderMicron() or showRawText() on _pageSource depending on
    // _showRaw -- the one place that decides which view is currently shown, so
    // toggling the raw view doesn't need a re-fetch.
    void renderPage();
    void toggleRawView();
    void toggleAddressBar();
    // Off by default (see TODO.md) -- whether the next request identifies on
    // the Link before sending (an extra LoRa frame most servers never read
    // remote_identity from). Session-only, same as _showRaw/_addressBarHidden.
    void toggleIdentify();
    // Favorites live here, not in the Peers screen -- the star favorites
    // whichever node you're currently browsing (like a normal browser's
    // address-bar bookmark star), and the list jumps straight into any
    // favorited node at its default page. Storage is the same
    // AnnounceManager/NodeView contact-favorite plumbing the Peers screen
    // itself could also use -- this screen is just where the UI lives.
    void toggleFavoriteCurrent();
    // Re-derives the star button's color from NodeView's ground truth --
    // used on loading a (possibly different) peer. toggleFavoriteCurrent()
    // sets the color directly instead of calling this, since right after
    // toggling, NodeView's cached copy is still stale (the mutation is an
    // async round-trip -- see AnnounceManager::setContactFavorite).
    void updateFavoriteButton();
    // Builds favorite-node rows into `container` (the "Favs" overlay list,
    // or the home state's _body) -- same tap-to-browse behavior either way.
    void populateFavoriteRows(lv_obj_t* container);
    void openFavoritesList();
    void closeFavoritesList();
    // Stops the in-flight browse (device-side NomadBrowseClient::cancel(),
    // via Operation::CancelNomadBrowse) -- the actual UI update (log text,
    // hiding the Cancel button) happens when that request's async
    // Outcome::Cancelled result reaches startBrowse()'s own completion
    // callback, same as any other outcome; this just fires the request.
    void cancelBrowse();
    void setCancelVisible(bool visible);
    // Nomad tab's landing state: no peer selected, _body shows the
    // favorites list in place of a page. See openHome()/onEnter().
    void showHome();
    // Escape hatch off the home state's "Open Browser" row: a page-less
    // browsing state with no peer/path set, address bar forced visible, with
    // an "Enter URL..." row that starts manual entry (startUrlInput()).
    void openBlankBrowser();
    // Manual destination entry, step 1 (see TODO.md): resolves a typed
    // "<32-hex-hash>" or "<32-hex-hash>:<path>" against contacts you already
    // know (NodeView::findNodeByHex) -- the same public-key-derivation path
    // Peers' own "Nomad Browser" action uses, just reached by typing the
    // hash instead of tapping through Peers. Does NOT resolve a stranger's
    // hash you've never seen before -- that needs real investigation into
    // whether this device's known-destination cache even covers the
    // nomadnetwork.node aspect (see TODO.md), separate follow-up.
    //
    // Reachable two ways: the blank state's "Enter URL..." row (starts
    // empty), or tapping the address bar while a page is loaded (starts
    // pre-filled with "_peerHex:_path" -- NOT whatever's currently
    // displayed, which may be the resolved nomad dest hash from
    // pollProgress(); only _peerHex is actually a valid NodeView lookup key).
    void startUrlInput();
    void submitUrlInput();
    // _peerHex empty (came from the blank state) -> back to openBlankBrowser().
    // Otherwise restores _addressBeforeEdit verbatim and re-renders _pageSource
    // (renderPage(), no re-fetch) -- whatever page was already loaded.
    void cancelUrlInput();
    // Re-renders the address bar as "<typed so far>_" while typing.
    void updateUrlInputDisplay();

    // --- Form fields (see TODO.md's investigation -- grammar and submit semantics
    // reverse-engineered from reticulum-meshchat's real, working implementation, not
    // guessed). _formFieldValues holds the CURRENT live value of every named field on
    // the page currently rendered -- seeded from each field's parsed default the
    // first time renderMicron() encounters it, then updated as the user interacts
    // (checkbox tap, or a committed text-field edit). Cleared at the start of every
    // renderMicron() -- form state does not survive a real page change.
    std::vector<std::pair<std::string, std::string>> _formFieldValues;
    std::string getFormFieldValue(const std::string& name) const;
    void setFormFieldValue(const std::string& name, const std::string& value);
    // Checkbox/radio widgets' click handlers are indexed into this the same way
    // _links/_favoriteRows are -- {fieldName, submitValue} per rendered checkbox.
    struct CheckboxRef { std::string name, value; };
    std::vector<CheckboxRef> _checkboxRefs;
    // Same index-into-vector convention for text fields' click handlers -- just the
    // field name (the tap only ever needs to know which field to start editing).
    // Deliberately NOT a heap-allocated std::string* per widget: lv_obj_clean()
    // (every render/re-render) destroys the LVGL objects but would never free that,
    // a steady leak on a device that stays up for days between reloads.
    std::vector<std::string> _textFieldRefs;
    // Tapping a text FIELD widget starts this -- a generalized version of the URL
    // entry flow (same on-screen-keyboard machinery, same reachable-Cancel-button
    // lesson learned there), just targeting one named field's value instead of the
    // address bar. Kept entirely separate from _urlInputActive/_urlInputText rather
    // than unified with them -- submitting a URL edit navigates; submitting a field
    // edit just updates local state and re-renders, different enough behavior that
    // sharing one state machine wasn't worth the risk of disturbing the already-
    // working URL-entry code to force a merge.
    bool _editingFieldActive = false;
    std::string _editingFieldName;
    std::string _editingFieldText;
    void startFieldInput(const std::string& name, const std::string& currentText);
    void submitFieldInput();
    void cancelFieldInput();
    void updateFieldInputDisplay();
    // Set by a submit-carrying link's click handler (see renderMicron()'s Link
    // handling) right before calling startBrowse() -- already-msgpack-encoded form
    // field data (a fixmap of name->value strings), consumed and cleared by
    // startBrowse() on its next real (non-deferred) request so an unrelated later
    // Reload/navigation never accidentally resubmits stale form data.
    std::vector<uint8_t> _pendingFieldData;
    // Resolves a link's submitFields tokens ("*" / "name" / "key=value") against
    // the CURRENT _formFieldValues, msgpack-encodes the result into
    // _pendingFieldData, and returns whether anything was actually set (an empty
    // submitFields list, or one that resolves to zero fields, leaves
    // _pendingFieldData untouched -- a plain navigation, same as before this round).
    bool buildPendingFieldData(const std::vector<std::string>& submitFields);

    handheld::ServiceClient* _service = nullptr;

    std::string _peerHex, _displayName;
    // The page currently shown; link navigation (navigateTo()) changes this and
    // re-browses the same peer. Not yet editable by the user directly.
    std::string _path = "/page/index.mu";
    // One entry per link rendered by renderMicron(), indexed by each link widget's
    // LVGL user-data -- see renderMicron()'s doc comment for why an index into this
    // (not the strings themselves) is what the widgets actually carry.
    struct LinkTarget { std::string destHex, path; std::vector<std::string> submitFields; };
    std::vector<LinkTarget> _links;
    // One entry per row in the favorites-list overlay, indexed the same way
    // _links is -- see openFavoritesList().
    struct FavoriteRow { std::string hex, name; };
    std::vector<FavoriteRow> _favoriteRows;
    // Discards a late browse completion callback after a newer openFor()
    // superseded it (re-entering this screen for a different peer without
    // the process restarting in between) -- the mailbox itself guarantees
    // one callback per submitted request id, this just guards against
    // acting on a stale one.
    uint32_t _generation = 0;
    uint32_t _requestId = 0;
    uint8_t _lastPhase = 0xFF;
    uint8_t _lastAttempt = 0;
    bool _done = false;
    // Set when startBrowse() is called while a browse is already in flight
    // (Reload tapped again, or a link/favorite tapped mid-load) -- the
    // device only allows one NomadBrowse at a time (see executeNomad()'s
    // admission guard), so the in-flight one is cancelled first and this
    // flag tells its completion callback to retry once that's confirmed
    // rather than rejecting the new request outright.
    bool _pendingRestart = false;
    // The real nomadnetwork.node dest hash isn't known client-side until the backend
    // computes it (NomadBrowseClient::start(), asynchronous) -- the address bar shows
    // the contact name as a placeholder until this flips true and swaps in the hash.
    bool _destHashShown = false;
    std::string _log;
    // The last successfully fetched page's raw Micron source -- kept around so
    // toggleRawView() can flip between the rendered and raw views without
    // re-browsing. Cleared at the start of each new startBrowse().
    std::string _pageSource;
    // Session-only (resets each time the screen reopens, not persisted to UserConfig):
    // index into the font cycle applyFontSize() reads -- see its .cpp doc comment for
    // the actual size list. Starts at the largest available (index 0), per direct
    // feedback that the previous fixed 14px body text was still too small.
    uint8_t _fontSizeIdx = 0;
    // Sticky across pages/reloads within this screen instance (like _fontSizeIdx) --
    // "show raw Micron source" and "address bar hidden" are viewing preferences, not
    // per-page state.
    bool _showRaw = false;
    // Hidden by default, per direct feedback that the address bar's info (contact
    // name/dest hash + path) isn't needed most of the time -- "@" in the header
    // reveals it on demand.
    bool _addressBarHidden = true;
    // Off by default -- see toggleIdentify()'s doc comment. Read by
    // startBrowse() when it submits the next request.
    bool _identifyOnRequest = false;
    // Manual URL entry (startUrlInput()) -- see wantsVirtualKeyboard()/handleKey().
    bool _urlInputActive = false;
    std::string _urlInputText;
    // The address bar's exact text right before startUrlInput() overwrote it
    // with the live-typed buffer -- restored verbatim on cancelUrlInput() so
    // cancelling while editing an already-loaded page's address doesn't lose
    // whatever was actually displayed (which may be the resolved nomad dest
    // hash from pollProgress(), not just "_peerHex:_path").
    std::string _addressBeforeEdit;

    lv_obj_t* _header = nullptr;
    lv_obj_t* _lblTitle = nullptr;
    lv_obj_t* _btnFontSize = nullptr;
    lv_obj_t* _btnRawToggle = nullptr;
    lv_obj_t* _btnIdentify = nullptr;
    lv_obj_t* _btnAddressToggle = nullptr;
    lv_obj_t* _btnFavorite = nullptr;
    lv_obj_t* _btnFavoritesList = nullptr;
    lv_obj_t* _addressBar = nullptr;
    lv_obj_t* _lblAddress = nullptr;
    lv_obj_t* _btnBack = nullptr;
    lv_obj_t* _btnForward = nullptr;
    lv_obj_t* _btnReload = nullptr;
    // Hidden unless a browse is actually in flight -- see setCancelVisible().
    lv_obj_t* _btnCancel = nullptr;
    // Scrollable content area. While loading/on error it holds exactly one plain
    // _logText label; renderMicron() replaces that with one widget (or a few, for a
    // line with links) per Micron source line -- see its doc comment.
    lv_obj_t* _body = nullptr;
    lv_obj_t* _logText = nullptr;
    // Favorites-list overlay: built fresh (lv_obj_clean + rebuild) each time it's
    // opened rather than pooled -- favorites are expected to stay a short list,
    // not worth the row-recycling machinery LvNodesScreen's much larger peer
    // list needs.
    lv_obj_t* _favoritesOverlay = nullptr;
    lv_obj_t* _favoritesList = nullptr;
};

#endif
