#pragma once
#include "config/BoardConfig.h"
#if defined(RATPAGER_LR1121)
#include <Arduino.h>
#include <SPI.h>
#include <optional>
#include <hal/Arduino/ArduinoHal.h>
#include <modules/LR11x0/LR1121.h>
#include "hal/SharedSPIBus.h"

// RadioLib's HAL owns individual SPI transactions, including CS assertion.
// BUSY waits and airtime do not retain the shared LCD/SD bus mutex.
class SharedRadioHal final : public ArduinoHal {
public:
    explicit SharedRadioHal(SPIClass& spi) : ArduinoHal(spi, SPISettings(SPI_FREQUENCY, MSBFIRST, SPI_MODE0)) {}
    void spiBeginTransaction() override {
        _lease.emplace();
        configASSERT(_lease->locked());
        ArduinoHal::spiBeginTransaction();
    }
    void spiEndTransaction() override { ArduinoHal::spiEndTransaction(); _lease.reset(); }
private:
    std::optional<SharedSPILock> _lease;
};

class LR1121Chip final : public LR1121 {
public:
    explicit LR1121Chip(Module* module) : LR1121(module) {}
    // DIO5/DIO6 RF-switch table for the LilyGo T-Pager LR1121 module, confirmed
    // against Meshtastic's official support for this exact board/module
    // (variants/esp32s3/tlora-pager/rfswitch.h) — do not reuse the ThinkNode
    // M9 board's bit pattern, its DIO5/DIO6 polarity is different hardware.
    int16_t configureRfSwitch() {
        static const uint32_t dioPins[Module::RFSWITCH_MAX_PINS] = {
            RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
        };
        static const Module::RfSwitchMode_t table[] = {
            // mode                  DIO5   DIO6
            {LR11x0::MODE_STBY,   {LOW,  LOW}},  {LR11x0::MODE_RX,    {LOW,  HIGH}},
            {LR11x0::MODE_TX,     {HIGH, LOW}},  {LR11x0::MODE_TX_HP, {HIGH, LOW}},
            {LR11x0::MODE_TX_HF,  {LOW,  LOW}},  {LR11x0::MODE_GNSS,  {LOW,  LOW}},
            {LR11x0::MODE_WIFI,   {LOW,  LOW}},  END_OF_MODE_TABLE,
        };
        setRfSwitchTable(dioPins, table); // void on this RadioLib version — can't fail
        return RADIOLIB_ERR_NONE;
    }
    int16_t readIrq(uint32_t* irq) { return readStatusFrame(nullptr, nullptr, irq); }
    int16_t readStatus(uint8_t* status) { return readStatusFrame(status, nullptr, nullptr); }
    int16_t readStatusFrame(uint8_t* first, uint8_t* second, uint32_t* irq) {
        // This six-byte response includes both status bytes. Ordinary reads
        // discard one status byte; doing that here shifts TX_DONE into TIMEOUT
        // and loses RX_DONE. Match RadioLib's getIrqStatus framing while keeping
        // its transport error result, and restore normal framing on every exit.
        auto& width = mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS];
        const auto saved = width;
        width = Module::BITS_0;
        const int16_t result = getStatus(first, second, irq);
        width = saved;
        return result;
    }
};

// Raw physical frames only. LoRaInterface remains the sole RNode-framing,
// split-packet, queue, airtime policy and receipt owner.
class LR1121Radio {
public:
    explicit LR1121Radio(SPIClass* spi);
    bool begin(uint32_t frequency);
    void end();
    int beginPacket(int implicitHeader = 0);
    int endPacket(bool async = false);
    bool isTxBusy();
    bool txFailed() const { return _txFailed; }
    size_t write(uint8_t byte) { return write(&byte, 1); }
    size_t write(const uint8_t* buffer, size_t size);
    void receive(int size = 0);
    int parsePacket(int size = 0);
    const uint8_t* packetBuffer() const { return _packet; }
    void setFrequency(uint32_t frequency);
    uint32_t getFrequency() const { return _frequency; }
    void setTxPower(int level);
    int8_t getTxPower() const { return _power; }
    void setSpreadingFactor(int sf);
    uint8_t getSpreadingFactor() const { return _sf; }
    void setSignalBandwidth(uint32_t hz);
    uint32_t getSignalBandwidth() const { return _bandwidth; }
    void setCodingRate4(int denominator);
    uint8_t getCodingRate4() const { return _cr; }
    void setPreambleLength(long symbols);
    long getPreambleLength() const { return _preamble; }
    void setInvertIQ(bool enabled);
    bool getInvertIQ() const { return _invertIq; }
    bool isRadioOnline() const { return _online; }
    int currentRssi();
    int packetRssi() const { return _rssi; }
    float packetSnr() const { return _snr; }
    uint8_t getStatus();
    uint32_t getIrqFlags();
    float getAirtime(uint16_t bytes);
    uint32_t getBitrate() const;
    bool lowDataRateEnabled() const;
    void standby();
    void sleep();
    using YieldCallback = void(*)();
    void setYieldCallback(YieldCallback cb) { _yield = cb; }
    void printDiagnostics();
    volatile bool packetAvailable = false;
private:
    bool configureReady();
    bool checked(int16_t result, const char* operation);
    static void IRAM_ATTR interrupt();
    static LR1121Radio* _instance;
    SharedRadioHal _hal;
    Module _module;
    LR1121Chip _chip;
    uint8_t _packet[MAX_PACKET_SIZE]{};
    size_t _written = 0;
    uint32_t _frequency = LORA_DEFAULT_FREQ, _bandwidth = LORA_DEFAULT_BW;
    uint32_t _txStarted = 0, _txBudget = 0;
    uint8_t _sf = LORA_DEFAULT_SF, _cr = LORA_DEFAULT_CR;
    int8_t _power = LORA_DEFAULT_TX_POWER;
    uint16_t _preamble = LORA_DEFAULT_PREAMBLE;
    int _rssi = -127;
    float _snr = 0;
    int16_t _lastError = 0;
    bool _online = false, _tx = false, _txFailed = false, _invertIq = false;
    YieldCallback _yield = nullptr;
};
#endif
