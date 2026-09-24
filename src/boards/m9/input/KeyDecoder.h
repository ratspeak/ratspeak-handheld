#pragma once
#include <stdint.h>
#include "input/KeyEvent.h"

namespace m9 {
enum class KeyKind : uint8_t { None, Key, Hold, Reserved };
inline KeyKind decodeKey(uint8_t raw, KeyEvent& event) {
    event = {};
    switch (raw) {
    case 0: case 0x88: return KeyKind::None;
    case 0x0d: event.enter = true; break;
    case 0x08: event.del = true; break;
    case 0x09: event.tab = true; break;
    case 0xb4: event.left = true; break;
    case 0xb5: event.up = true; break;
    case 0xb6: event.down = true; break;
    case 0xb7: event.right = true; break;
    case 0x86: event.character = 0x1b; break;
    case 0xa3: return KeyKind::Hold;
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x87:
        return KeyKind::Reserved;
    default:
        if (raw < 0x20 || raw > 0x7e) return KeyKind::None;
        event.character = static_cast<char>(raw);
        event.space = raw == ' ';
    }
    return KeyKind::Key;
}
}
