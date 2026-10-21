#include "radio/LR1121Radio.h"
#if defined(RATPAGER_LR1121)
#include <cstring>

LR1121Radio* LR1121Radio::_instance = nullptr;
LR1121Radio::LR1121Radio(SPIClass* spi)
    : _hal(*spi), _module(&_hal, LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY), _chip(&_module) {}

bool LR1121Radio::checked(int16_t result, const char* operation) {
    if (result == RADIOLIB_ERR_NONE) return true;
    _lastError = result;
    _online = false;
    Serial.printf("[LR1121] %s failed: %d\n", operation, result);
    return false;
}
// Some units need extra settling time on first power-up. Matches Meshtastic's
// LR11x0Interface::radioInitImpl() retry fix for the identical SPI-command
// failure/timeout symptom on this chip family.
static bool lr1121SpiFailed(int16_t result) {
    return result == RADIOLIB_ERR_SPI_CMD_FAILED || result == RADIOLIB_ERR_SPI_CMD_TIMEOUT;
}
bool LR1121Radio::begin(uint32_t frequency) {
    if (!initializeSharedSPIBus()) return false;
    _online = _tx = _txFailed = false; packetAvailable = false; _written = 0;
    _module.spiConfig.timeout = 2000; // Individual BUSY waits; not a total boot deadline.
    int16_t result = _chip.begin(frequency / 1000000.0f, _bandwidth / 1000.0f,
        _sf, _cr, RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE, _power, _preamble, LORA_TCXO_VOLTAGE_V);
    if (lr1121SpiFailed(result)) {
        Serial.printf("[LR1121] begin failed: %d (SPI cmd failure), retry after delay\n", result);
        delay(100);
        result = _chip.begin(frequency / 1000000.0f, _bandwidth / 1000.0f,
            _sf, _cr, RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE, _power, _preamble, LORA_TCXO_VOLTAGE_V);
    }
    if (!checked(result, "begin")) return false;
    // Initialization resets chip DIO state; install the board switch afterward.
    if (!checked(_chip.configureRfSwitch(), "RF switch") ||
        !checked(_chip.setRegulatorDCDC(), "regulator")) return false;
    if (!checked(_chip.setCRC(2), "CRC")) return false;
    _frequency = frequency; _invertIq = false; _lastError = 0;
    _instance = this;
    _chip.setPacketReceivedAction(interrupt);
    _online = true;
    printDiagnostics();
    return true;
}
void IRAM_ATTR LR1121Radio::interrupt() { if (_instance) _instance->packetAvailable = true; }
void LR1121Radio::end() {
    if (_online) _chip.standby();
    _chip.clearPacketReceivedAction();
    _online = _tx = false; packetAvailable = false;
    if (_instance == this) _instance = nullptr;
}
int LR1121Radio::beginPacket(int implicitHeader) {
    if (!_online || _tx || implicitHeader) return 0;
    if (!checked(_chip.standby(), "TX standby")) return 0;
    _written = 0; _txFailed = false; packetAvailable = false;
    return 1;
}
size_t LR1121Radio::write(const uint8_t* buffer, size_t size) {
    if (!_online || _tx || !buffer || size > sizeof(_packet) - _written) return 0;
    memcpy(_packet + _written, buffer, size); _written += size; return size;
}
int LR1121Radio::endPacket(bool async) {
    if (!_online || _tx || !_written) return 0;
    _txBudget = uint32_t(getAirtime(_written)) + 2000;
    _txStarted = millis(); packetAvailable = false;
    if (!checked(_chip.startTransmit(_packet, _written), "TX start")) { _txFailed = true; return 0; }
    _tx = true;
    if (!async) while (isTxBusy()) { if (_yield) _yield(); else delay(1); }
    return !_txFailed;
}
bool LR1121Radio::isTxBusy() {
    if (!_tx) return false;
    uint32_t irq = 0;
    if (!checked(_chip.readIrq(&irq), "IRQ read")) { _txFailed = _tx; _tx = false; return false; }
    const bool done = irq & RADIOLIB_LR11X0_IRQ_TX_DONE;
    const bool timeout = (irq & RADIOLIB_LR11X0_IRQ_TIMEOUT) || uint32_t(millis() - _txStarted) >= _txBudget;
    if (!done && !timeout) return true;
    _tx = false; packetAvailable = false;
    _txFailed = !done;
    if (_txFailed) Serial.printf("[LR1121] TX timeout: irq=0x%08lx elapsed=%lums budget=%lums\n",
        (unsigned long)irq, (unsigned long)(millis() - _txStarted), (unsigned long)_txBudget);
    if (!checked(_chip.finishTransmit(), "TX finish")) _txFailed = true;
    return false;
}
void LR1121Radio::receive(int size) {
    if (!_online || _tx || size) return;
    packetAvailable = false;
    checked(_chip.startReceive(), "RX start");
}
int LR1121Radio::parsePacket(int size) {
    if (!_online || _tx || size) return 0;
    const uint32_t irq = getIrqFlags();
    if (!(irq & RADIOLIB_LR11X0_IRQ_RX_DONE)) return 0;
    const size_t length = _chip.getPacketLength();
    if (!length || length > sizeof(_packet)) return 0;
    const int16_t result = _chip.readData(_packet, length);
    if (result == RADIOLIB_ERR_CRC_MISMATCH) return 0;
    if (!checked(result, "RX read")) return 0;
    _rssi = int(_chip.getRSSI()); _snr = _chip.getSNR();
    return int(length);
}
bool LR1121Radio::configureReady() {
    return _online && !_tx && checked(_chip.standby(), "configure standby");
}
void LR1121Radio::setFrequency(uint32_t hz) {
    if (configureReady() && checked(_chip.setFrequency(hz / 1000000.0f), "frequency")) _frequency = hz;
}
void LR1121Radio::setTxPower(int dbm) {
    if (configureReady() && checked(_chip.setOutputPower(dbm), "power")) _power = dbm;
}
void LR1121Radio::setSpreadingFactor(int sf) {
    if (configureReady() && checked(_chip.setSpreadingFactor(sf), "SF")) _sf = sf;
}
void LR1121Radio::setSignalBandwidth(uint32_t hz) {
    if (configureReady() && checked(_chip.setBandwidth(hz / 1000.0f), "bandwidth")) _bandwidth = hz;
}
void LR1121Radio::setCodingRate4(int cr) {
    if (configureReady() && checked(_chip.setCodingRate(cr), "coding rate")) _cr = cr;
}
void LR1121Radio::setPreambleLength(long symbols) {
    if (configureReady() && checked(_chip.setPreambleLength(symbols), "preamble")) _preamble = symbols;
}
void LR1121Radio::setInvertIQ(bool enabled) {
    if (configureReady() && checked(_chip.invertIQ(enabled), "IQ")) _invertIq = enabled;
}
void LR1121Radio::standby() { if (_online) checked(_chip.standby(), "standby"); }
void LR1121Radio::sleep() {
    // Initial beta uses RF standby. Retention sleep has chip-firmware-specific
    // errata; do not enable it merely because the UI disables the radio.
    standby();
}
int LR1121Radio::currentRssi() { return _online ? int(_chip.getRSSI(false)) : -127; }
uint8_t LR1121Radio::getStatus() {
    uint8_t result = 0;
    if (_online) checked(_chip.readStatus(&result), "status");
    return result;
}
uint32_t LR1121Radio::getIrqFlags() {
    uint32_t irq = 0;
    if (_online && !checked(_chip.readIrq(&irq), "IRQ read")) return 0;
    return irq;
}
float LR1121Radio::getAirtime(uint16_t bytes) { return (_chip.getTimeOnAir(bytes) + 999) / 1000; }
bool LR1121Radio::lowDataRateEnabled() const { return (uint32_t{1} << _sf) * 1000 > 16 * _bandwidth; }
uint32_t LR1121Radio::getBitrate() const { return uint32_t(uint64_t(_sf) * 4 * _bandwidth / ((uint32_t{1} << _sf) * _cr)); }
void LR1121Radio::printDiagnostics() {
    LR11x0VersionInfo_t info{};
    const int16_t result = _chip.getVersionInfo(&info);
    Serial.printf("[LR1121] version-read=%d hardware=0x%02x chip=0x%02x firmware=0x%02x%02x last-error=%d\n",
                  result, info.hardware, info.device, info.fwMajor, info.fwMinor, _lastError);
}
#endif
