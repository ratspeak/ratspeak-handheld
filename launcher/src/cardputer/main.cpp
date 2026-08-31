#include <Arduino.h>
#include <M5Cardputer.h>
#include <Preferences.h>

#include "RsCardputerModeSwitch.h"
#include "CardputerAdvKeyboard.h"

namespace {

constexpr uint16_t kBg = 0x0841;
constexpr uint16_t kPanel = 0x1082;
constexpr uint16_t kText = 0xF7BE;
constexpr uint16_t kMuted = 0x8C71;
constexpr uint16_t kAccent = 0x06D7;
constexpr uint16_t kWarn = 0xFBA0;
constexpr uint32_t kAutoBootMs = 7000;
constexpr uint32_t kKeyRepeatStartMs = 400;
constexpr uint32_t kKeyRepeatIntervalMs = 125;
constexpr char kPrefsNamespace[] = "rslaunch";
constexpr char kLastChoiceKey[] = "last";

enum class Choice : uint8_t {
  Standalone = 0,
  RNode = 1,
};

enum class KeyAction : uint8_t {
  None = 0,
  Previous,
  Next,
  Select,
  StartStandalone,
  StartRNode,
};

Choice selected = Choice::Standalone;
uint32_t bootStarted = 0;
uint32_t lastRemain = UINT32_MAX;
bool booting = false;
bool autoBootEnabled = true;
KeyAction heldAction = KeyAction::None;
uint32_t heldSince = 0;
uint32_t lastRepeat = 0;
CardputerAdvKeyboard keyboardHardware;
bool pressedKeys[4][14] = {};

uint8_t choiceValue(Choice choice) {
  return choice == Choice::RNode ? 1 : 0;
}

Choice choiceFromValue(uint8_t value) {
  return value == 1 ? Choice::RNode : Choice::Standalone;
}

Choice loadLastChoice() {
  Preferences prefs;
  Choice choice = Choice::Standalone;
  if (prefs.begin(kPrefsNamespace, true)) {
    choice = choiceFromValue(prefs.getUChar(kLastChoiceKey, choiceValue(choice)));
    prefs.end();
  }
  return choice;
}

void saveLastChoice(Choice choice) {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kLastChoiceKey, choiceValue(choice));
    prefs.end();
  }
}

void drawOption(int y, const char *title, const char *subtitle, bool active) {
  auto &d = M5Cardputer.Display;
  uint16_t fill = active ? kAccent : kPanel;
  uint16_t fg = active ? TFT_BLACK : kText;
  uint16_t sub = active ? 0x2104 : kMuted;

  d.fillRoundRect(12, y, 216, 34, 5, fill);
  d.setTextColor(fg, fill);
  d.setTextSize(1);
  d.setCursor(22, y + 7);
  d.print(title);
  d.setTextColor(sub, fill);
  d.setCursor(22, y + 20);
  d.print(subtitle);
}

uint32_t remainingSeconds() {
  uint32_t elapsed = millis() - bootStarted;
  if (elapsed >= kAutoBootMs) {
    return 0;
  }
  return (kAutoBootMs - elapsed + 999) / 1000;
}

void drawCountdown(bool force = false) {
  auto &d = M5Cardputer.Display;
  if (!autoBootEnabled) {
    d.fillRect(200, 8, 28, 20, kBg);
    lastRemain = UINT32_MAX;
    return;
  }

  uint32_t remain = remainingSeconds();
  if (!force && remain == lastRemain) {
    return;
  }
  lastRemain = remain;

  d.fillRect(200, 8, 28, 20, kBg);
  d.fillRoundRect(207, 9, 20, 18, 5, kPanel);
  d.setTextSize(1);
  d.setTextColor(kText, kPanel);
  d.setCursor(remain >= 10 ? 211 : 214, 15);
  d.print(static_cast<unsigned long>(remain));
}

void drawScreen() {
  auto &d = M5Cardputer.Display;
  d.fillScreen(kBg);

  d.setTextSize(2);
  d.setTextColor(kText, kBg);
  d.setCursor(12, 10);
  d.print("Ratspeak");

  d.setTextSize(1);
  d.setTextColor(kMuted, kBg);
  d.setCursor(14, 30);
  d.print("Cardputer Adv");

  drawOption(48, "Standalone", "On-device messenger", selected == Choice::Standalone);
  drawOption(87, "RNode", "BLE / USB radio", selected == Choice::RNode);

  d.setTextColor(kMuted, kBg);
  d.setCursor(75, 125);
  d.print("Arrows + Enter");

  drawCountdown(true);
}

void selectChoice(Choice choice) {
  if (selected == choice) {
    return;
  }
  selected = choice;
  drawScreen();
}

void pauseAutoBoot() {
  if (!autoBootEnabled) {
    return;
  }
  autoBootEnabled = false;
  drawCountdown(true);
}

void showBooting(const char *label) {
  booting = true;
  auto &d = M5Cardputer.Display;
  d.fillScreen(kBg);
  d.setTextSize(2);
  d.setTextColor(kAccent, kBg);
  d.setCursor(20, 44);
  d.print(label);
  d.setTextSize(1);
  d.setTextColor(kMuted, kBg);
  d.setCursor(20, 75);
  d.print("Starting...");
}

void showError(const char *message) {
  booting = false;
  auto &d = M5Cardputer.Display;
  d.fillScreen(kBg);
  d.setTextSize(2);
  d.setTextColor(kWarn, kBg);
  d.setCursor(16, 36);
  d.print("Boot error");
  d.setTextSize(1);
  d.setTextColor(kText, kBg);
  d.setCursor(16, 68);
  d.print(message);
}

void startChoice(Choice choice) {
  using namespace rs_cardputer_adv;

  FirmwareMode mode = choice == Choice::Standalone ? FirmwareMode::Standalone : FirmwareMode::RNode;
  showBooting(mode_name(mode));
  SwitchResult result = set_next_boot(mode);
  if (!result.ok) {
    showError(result.message);
    return;
  }
  saveLastChoice(choice);
  delay(50);
  esp_restart();
}

bool isModifierPosition(uint8_t row, uint8_t col) {
  return (row == 2 && (col == 0 || col == 1)) ||
         (row == 3 && col <= 2);
}

KeyAction actionForKey(uint8_t row, uint8_t col) {
  if (row >= 4 || col >= 14 || isModifierPosition(row, col)) {
    return KeyAction::None;
  }

  const bool fn = pressedKeys[2][0];
  if (fn) {
    if ((row == 2 && col == 11) || (row == 3 && col == 10)) {
      return KeyAction::Previous;
    }
    if (row == 3 && (col == 11 || col == 12)) {
      return KeyAction::Next;
    }
    return KeyAction::None;
  }

  Point2D_t position;
  position.x = col;
  position.y = row;
  const KeyValue_t value = M5Cardputer.Keyboard.getKeyValue(position);
  if (static_cast<uint8_t>(value.value_first) == KEY_ENTER) {
    return KeyAction::Select;
  }

  const char key = pressedKeys[2][1] ? value.value_second : value.value_first;
  if (key == ';' || key == ',' || key == 'w' || key == 'W')
    return KeyAction::Previous;
  if (key == '.' || key == '/' || key == 's' || key == 'S')
    return KeyAction::Next;
  if (key == 'r' || key == 'R') return KeyAction::StartStandalone;
  if (key == 'n' || key == 'N') return KeyAction::StartRNode;
  return KeyAction::None;
}

KeyAction pollKeyboard() {
  CardputerAdvKeyboard::Event events[16];
  const size_t count = keyboardHardware.poll(events, 16);
  KeyAction tapped = KeyAction::None;
  for (size_t i = 0; i < count; ++i) {
    const auto& event = events[i];
    pressedKeys[event.row][event.col] = event.pressed;
    if (event.pressed && tapped == KeyAction::None) {
      tapped = actionForKey(event.row, event.col);
    }
  }
  return tapped;
}

KeyAction currentKeyAction() {
  for (uint8_t row = 0; row < 4; ++row) {
    for (uint8_t col = 0; col < 14; ++col) {
      if (!pressedKeys[row][col] || isModifierPosition(row, col)) continue;
      const KeyAction action = actionForKey(row, col);
      if (action != KeyAction::None) return action;
    }
  }
  return KeyAction::None;
}

bool repeatable(KeyAction action) {
  return action == KeyAction::Previous || action == KeyAction::Next;
}

void performAction(KeyAction action) {
  switch (action) {
    case KeyAction::Previous: selectChoice(Choice::Standalone); break;
    case KeyAction::Next: selectChoice(Choice::RNode); break;
    case KeyAction::Select: startChoice(selected); break;
    case KeyAction::StartStandalone: startChoice(Choice::Standalone); break;
    case KeyAction::StartRNode: startChoice(Choice::RNode); break;
    case KeyAction::None: break;
  }
}

void updateKeyboard() {
  const KeyAction tapped = pollKeyboard();
  const KeyAction action = currentKeyAction();
  const uint32_t now = millis();
  if (action == KeyAction::None) {
    heldAction = KeyAction::None;
    if (tapped != KeyAction::None) {
      pauseAutoBoot();
      performAction(tapped);
    }
    return;
  }

  if (action != heldAction) {
    heldAction = action;
    heldSince = now;
    lastRepeat = now;
    pauseAutoBoot();
    performAction(action);
    return;
  }

  if (repeatable(action) && now - heldSince >= kKeyRepeatStartMs &&
      now - lastRepeat >= kKeyRepeatIntervalMs) {
    lastRepeat = now;
    performAction(action);
  }
}

} // namespace

void setup() {
  auto cfg = M5.config();
  // The generic ESP32-S3 PlatformIO target cannot reliably identify the ADV
  // display. Force the fallback so M5Cardputer selects its TCA8418 keyboard
  // reader and the correct internal I2C pins.
  cfg.fallback_board = m5::board_t::board_M5CardputerADV;
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  M5Cardputer.begin(cfg, true);
  keyboardHardware.begin();
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(180);

  selected = loadLastChoice();
  bootStarted = millis();
  drawScreen();
}

void loop() {
  if (booting) {
    delay(20);
    return;
  }

  M5.update();
  updateKeyboard();

  drawCountdown();

  if (autoBootEnabled && millis() - bootStarted >= kAutoBootMs) {
    startChoice(selected);
    return;
  }
}
