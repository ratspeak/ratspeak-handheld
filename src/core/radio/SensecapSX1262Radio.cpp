#include "radio/SensecapSX1262Radio.h"
#if defined(RSSENSECAPD1L)
#include <cstring>

void (*SensecapRadioHal::s_callback)(void) = nullptr;
uint8_t SensecapRadioHal::s_dio1Bit = 0;
volatile bool SensecapRadioHal::s_pending = false;
void IRAM_ATTR SensecapRadioHal::isrTrampoline() { s_pending = true; }

SensecapSX1262Radio* SensecapSX1262Radio::_instance = nullptr;
SensecapSX1262Radio::SensecapSX1262Radio(SPIClass* spi, IOExpander* expander)
    : _hal(*spi, *expander),
      _module(&_hal, SensecapRadioHal::expanderPin(LORA_CS_EXPANDER_BIT),
              SensecapRadioHal::expanderPin(LORA_IRQ_EXPANDER_BIT),
              SensecapRadioHal::expanderPin(LORA_RST_EXPANDER_BIT),
              SensecapRadioHal::expanderPin(LORA_BUSY_EXPANDER_BIT)),
      _chip(&_module) {}

bool SensecapSX1262Radio::checked(int16_t result, const char* operation) {
    if (result == RADIOLIB_ERR_NONE) return true;
    _lastError = result;
    _online = false;
    Serial.printf("[SENSECAP-SX1262] %s failed: %d\n", operation, result);
    return false;
}
// Some units need extra settling time on first power-up — same symptom and
// fix as the T-Pager LR1121 driver's begin() retry.
static bool sensecapSpiFailed(int16_t result) {
    return result == RADIOLIB_ERR_SPI_CMD_FAILED || result == RADIOLIB_ERR_SPI_CMD_TIMEOUT;
}
bool SensecapSX1262Radio::begin(uint32_t frequency) {
    _online = _tx = _txFailed = false; packetAvailable = false; _written = 0;
    _module.spiConfig.timeout = 2000;
    int16_t result = _chip.begin(frequency / 1000000.0f, _bandwidth / 1000.0f,
        _sf, _cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, _power, _preamble,
        LORA_TCXO_VOLTAGE_V, !LORA_USE_DCDC_REGULATOR);
    if (sensecapSpiFailed(result)) {
        Serial.printf("[SENSECAP-SX1262] begin failed: %d (SPI cmd failure), retry after delay\n", result);
        delay(100);
        result = _chip.begin(frequency / 1000000.0f, _bandwidth / 1000.0f,
            _sf, _cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, _power, _preamble,
            LORA_TCXO_VOLTAGE_V, !LORA_USE_DCDC_REGULATOR);
    }
    if (!checked(result, "begin")) return false;
    if (!checked(_chip.setDio2AsRfSwitch(LORA_DIO2_AS_RF_SWITCH), "RF switch") ||
        !checked(_chip.setCRC(2), "CRC") || !checked(_chip.explicitHeader(), "header") ||
        !checked(_chip.invertIQ(false), "IQ")) return false;
    _frequency = frequency; _invertIq = false; _lastError = 0;
    _instance = this;
    _chip.setPacketReceivedAction(interrupt);
    _online = true;
    printDiagnostics();
    return true;
}
void IRAM_ATTR SensecapSX1262Radio::interrupt() { if (_instance) _instance->packetAvailable = true; }
void SensecapSX1262Radio::end() {
    if (_online) _chip.standby();
    _chip.clearPacketReceivedAction();
    _online = _tx = false; packetAvailable = false;
    if (_instance == this) _instance = nullptr;
}
int SensecapSX1262Radio::beginPacket(int implicitHeader) {
    if (!_online || _tx || implicitHeader) return 0;
    if (!checked(_chip.standby(), "TX standby")) return 0;
    _written = 0; _txFailed = false; packetAvailable = false;
    return 1;
}
size_t SensecapSX1262Radio::write(const uint8_t* buffer, size_t size) {
    if (!_online || _tx || !buffer || size > sizeof(_packet) - _written) return 0;
    memcpy(_packet + _written, buffer, size); _written += size; return size;
}
int SensecapSX1262Radio::endPacket(bool async) {
    if (!_online || _tx || !_written) return 0;
    _txBudget = uint32_t(getAirtime(_written)) + 2000;
    _txStarted = millis(); packetAvailable = false;
    if (!checked(_chip.startTransmit(_packet, _written), "TX start")) { _txFailed = true; return 0; }
    _tx = true;
    if (!async) while (isTxBusy()) { if (_yield) _yield(); else delay(1); }
    return !_txFailed;
}
bool SensecapSX1262Radio::isTxBusy() {
    if (!_tx) return false;
    uint32_t irq = getIrqFlags();
    const bool done = irq & RADIOLIB_SX126X_IRQ_TX_DONE;
    const bool timeout = (irq & RADIOLIB_SX126X_IRQ_TIMEOUT) || uint32_t(millis() - _txStarted) >= _txBudget;
    if (!done && !timeout) return true;
    _tx = false; packetAvailable = false;
    _txFailed = !done;
    if (_txFailed) Serial.printf("[SENSECAP-SX1262] TX timeout: irq=0x%08lx elapsed=%lums budget=%lums\n",
        (unsigned long)irq, (unsigned long)(millis() - _txStarted), (unsigned long)_txBudget);
    if (!checked(_chip.finishTransmit(), "TX finish")) _txFailed = true;
    return false;
}
void SensecapSX1262Radio::receive(int size) {
    if (!_online || _tx || size) return;
    packetAvailable = false;
    checked(_chip.startReceive(), "RX start");
}
int SensecapSX1262Radio::parsePacket(int size) {
    if (!_online || _tx || size) return 0;
    const uint32_t irq = getIrqFlags();
    if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) return 0;
    const size_t length = _chip.getPacketLength();
    if (!length || length > sizeof(_packet)) return 0;
    const int16_t result = _chip.readData(_packet, length);
    if (result == RADIOLIB_ERR_CRC_MISMATCH) return 0;
    if (!checked(result, "RX read")) return 0;
    _rssi = int(_chip.getRSSI()); _snr = _chip.getSNR();
    return int(length);
}
bool SensecapSX1262Radio::configureReady() {
    return _online && !_tx && checked(_chip.standby(), "configure standby");
}
void SensecapSX1262Radio::setFrequency(uint32_t hz) {
    if (configureReady() && checked(_chip.setFrequency(hz / 1000000.0f), "frequency")) _frequency = hz;
}
void SensecapSX1262Radio::setTxPower(int dbm) {
    if (configureReady() && checked(_chip.setOutputPower(dbm), "power")) _power = dbm;
}
void SensecapSX1262Radio::setSpreadingFactor(int sf) {
    if (configureReady() && checked(_chip.setSpreadingFactor(sf), "SF")) _sf = sf;
}
void SensecapSX1262Radio::setSignalBandwidth(uint32_t hz) {
    if (configureReady() && checked(_chip.setBandwidth(hz / 1000.0f), "bandwidth")) _bandwidth = hz;
}
void SensecapSX1262Radio::setCodingRate4(int cr) {
    if (configureReady() && checked(_chip.setCodingRate(cr), "coding rate")) _cr = cr;
}
void SensecapSX1262Radio::setPreambleLength(long symbols) {
    if (configureReady() && checked(_chip.setPreambleLength(symbols), "preamble")) _preamble = symbols;
}
void SensecapSX1262Radio::setInvertIQ(bool enabled) {
    if (configureReady() && checked(_chip.invertIQ(enabled), "IQ")) _invertIq = enabled;
}
void SensecapSX1262Radio::standby() { if (_online) checked(_chip.standby(), "standby"); }
void SensecapSX1262Radio::sleep() {
    // Matches every other driver in this codebase: RF standby only, no
    // retention sleep (chip-firmware-specific errata risk not worth it
    // merely because the UI disables the radio).
    standby();
}
int SensecapSX1262Radio::currentRssi() { return _online ? int(_chip.getRSSI(false)) : -127; }
uint8_t SensecapSX1262Radio::getStatus() {
    // SX126x's own getStatus() isn't exposed the way LR11x0's is; expose
    // what's cheaply available instead of inventing a raw register read.
    return _online ? 1 : 0;
}
uint32_t SensecapSX1262Radio::getIrqFlags() {
    return _online ? _chip.getIrqFlags() : 0;
}
float SensecapSX1262Radio::getAirtime(uint16_t bytes) { return (_chip.getTimeOnAir(bytes) + 999) / 1000; }
bool SensecapSX1262Radio::lowDataRateEnabled() const { return (uint32_t{1} << _sf) * 1000 > 16 * _bandwidth; }
uint32_t SensecapSX1262Radio::getBitrate() const { return uint32_t(uint64_t(_sf) * 4 * _bandwidth / ((uint32_t{1} << _sf) * _cr)); }
void SensecapSX1262Radio::printDiagnostics() {
    Serial.printf("[SENSECAP-SX1262] online=%d last-error=%d\n", _online, _lastError);
}
#endif
