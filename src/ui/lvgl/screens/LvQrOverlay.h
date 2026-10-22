#pragma once

#include "UIManager.h"
#include <string>

// Public Ratspeak contact card, with an optional legacy lxma:// QR.
class LvQrOverlay {
public:
    void create();
    bool show(const String& name, const String& destHashHex,
              const String& identityHashHex, const String& publicKeyHex);
    void hide();
    bool isVisible() const { return _visible; }
    bool handleKey(const KeyEvent& event);

private:
    lv_obj_t* _overlay = nullptr;
    lv_obj_t* _title = nullptr;
    lv_obj_t* _paper = nullptr;
    lv_obj_t* _qr = nullptr;
    int _qrSize = 0;
    lv_obj_t* _lblAddr = nullptr;
    lv_obj_t* _lblIdentity = nullptr;  // Tall layout only — see create().
    lv_obj_t* _lblFormat = nullptr;
    lv_obj_t* _toggle = nullptr;
    lv_obj_t* _lblToggle = nullptr;
    lv_obj_t* _closeBtn = nullptr;
    lv_group_t* _group = nullptr;
    std::string _payload, _legacyPayload;
    bool _legacy = false;
    bool _visible = false;
    bool _zoomed = false;  // Tall layout only — see toggleZoom().

    bool renderCode(bool legacy);
    void toggleFormat();
    void resizeQr(int size);
    void toggleZoom();
};
