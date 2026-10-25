#pragma once

#include <stdint.h>

// Shared by Standalone, the launcher and RNode. No gauge configuration/unsealing.
// BQ25896: TI SLUSC76C; BQ27220: TI SLUUBD4A. The Pager's /CE is wired low.
namespace pager {

template <typename Bus>
class Battery {
public:
    explicit Battery(Bus& bus) : bus_(bus) {}

    static constexpr uint8_t ChargerAddress = 0x6b;
    static constexpr uint8_t GaugeAddress = 0x55;
    enum class Init { Ready, Unavailable, WrongPart, VerifyFailed };
    enum class Sleep { UsbStandby, BatteryShip, Unverified };

    struct Gauge {
        uint16_t voltageMv = 0, soc = 0, operation = 0, status = 0;
        int16_t currentMa = 0;
        bool read = false;
        bool present() const { return read && (status & 0x0008); }
        bool ready() const {
            return present() && (operation & 0x0020) && !(operation & 0x0400)
                && soc <= 100 && voltageMv > 0 && voltageMv <= 6000;
        }
        int percent() const { return ready() ? static_cast<int>(soc) : -1; }
    };

    Init begin() {
        uint8_t part = 0;
        if (!readCharger(0x14, part)) return Init::Unavailable;
        // PN=000, DEV_REV=10. Do not configure a different chip at this address.
        if ((part & 0x3b) != 0x02) return Init::WrongPart;
        uint8_t input = 0, control = 0, timer = 0, current = 0, voltage = 0, fet = 0;
        if (!readCharger(0x00, input) || !readCharger(0x03, control) ||
            !readCharger(0x07, timer) || !readCharger(0x04, current) ||
            !readCharger(0x06, voltage) || !readCharger(0x09, fet))
            return Init::VerifyFailed;
        // Launcher -> app and warm boots must not toggle a correctly configured
        // charger: toggling CHG_CONFIG also restarts the charging safety timer.
        if (!(input & 0x80) && (control & 0xf0) == 0x10 &&
            (timer & 0xb8) == 0x88 && (current & 0x7f) == 11 &&
            (voltage >> 2) <= 23 && !(fet & 0x20)) return Init::Ready;
        // Inhibit charging/OTG while setting limits. A failed setup must not
        // proceed to the final enable. No register reset or periodic fault retry.
        if (!update(0x03, 0xf0, 0x00) ||
            !update(0x07, 0xb8, 0x88) || // host watchdog off; termination/timer on
            !update(0x04, 0x7f, 11))    // 704 mA: LilyGO's stock-pack recommendation
            return Init::VerifyFailed;
        if (!readCharger(0x06, voltage)) return Init::VerifyFailed;
        // Keep a lower existing limit; cap higher firmware settings at the
        // charger's 4.208 V default. Do not adopt the vendor's 4.288 V override.
        if ((voltage >> 2) > 23 && !update(0x06, 0xfc, 23 << 2))
            return Init::VerifyFailed;
        if (!update(0x09, 0xa0, 0x00) || // BATFET on; do not replay FORCE_ICO
            !update(0x00, 0x80, 0x00) || // exit HIZ, preserve input current limit
            !update(0x03, 0xf0, 0x10))   // charge enabled, OTG/load/watchdog reset off
            return Init::VerifyFailed;
        return Init::Ready;
    }

    Sleep prepareSleep() {
        uint8_t part = 0, status = 0, vbus = 0;
        if (!readCharger(0x14, part) || (part & 0x3b) != 0x02 ||
            !readCharger(0x0b, status) || !readCharger(0x11, vbus)) return Sleep::Unverified;
        // USB shutdown must leave the battery connected so it can charge.
        // If power sensing fails, preserve the path and use MCU deep sleep.
        if ((status & 0x04) || (vbus & 0x80))
            return update(0x09, 0xa0, 0) ? Sleep::UsbStandby : Sleep::Unverified;
        if (!update(0x07, 0x30, 0)) return Sleep::Unverified;
        // A successful battery-only write normally cuts power before readback.
        uint8_t reg = 0;
        if (!readCharger(0x09, reg) || !writeCharger(0x09, (reg & ~0xa8) | 0x20))
            return Sleep::Unverified;
        return Sleep::BatteryShip;
    }

    Gauge gauge() {
        Gauge result;
        uint16_t current = 0;
        result.read = readGauge(0x3a, result.operation) &&
            readGauge(0x0a, result.status) && readGauge(0x08, result.voltageMv) &&
            readGauge(0x2c, result.soc) && readGauge(0x14, current);
        result.currentMa = static_cast<int16_t>(current);
        return result;
    }

    bool readCharger(uint8_t reg, uint8_t& value) {
        bus_.beginTransmission(ChargerAddress);
        bus_.write(reg);
        if (bus_.endTransmission(false) != 0 ||
            bus_.requestFrom(ChargerAddress, uint8_t(1)) != 1) return false;
        value = bus_.read();
        return true;
    }

    bool readGauge(uint8_t reg, uint16_t& value) {
        bus_.beginTransmission(GaugeAddress);
        bus_.write(reg);
        if (bus_.endTransmission(false) != 0 ||
            bus_.requestFrom(GaugeAddress, uint8_t(2)) != 2) return false;
        const uint16_t lo = bus_.read();
        value = lo | (uint16_t(bus_.read()) << 8);
        return true;
    }

    template <typename Log>
    void printDiagnostics(Log& log) {
        // REG0C is latched on the first read, current on the second; retain both.
        uint8_t regs[0x15] = {};
        uint32_t valid = 0;
        for (uint8_t reg = 0; reg < sizeof(regs); ++reg) {
            if (readCharger(reg, regs[reg])) valid |= uint32_t(1) << reg;
        }
        uint8_t faultNow = 0;
        const bool faultRead = readCharger(0x0c, faultNow);
        log.printf("[POWER] charger regs00..14 valid=%06lx:", (unsigned long)valid);
        for (uint8_t reg : regs) log.printf(" %02x", reg);
        log.printf("\n[POWER] charger fault-now=%s%02x (REG0C above is latched)\n",
                   faultRead ? "" : "unavailable/", faultNow);
        if (valid & (uint32_t(1) << 0x0b)) {
            static const char* const states[] = {"idle", "precharge", "charging", "complete"};
            log.printf("[POWER] USB=%s charge=%s\n", (regs[0x0b] & 4) ? "yes" : "no",
                       states[(regs[0x0b] >> 3) & 3]);
        }
        const Gauge g = gauge();
        log.printf("[POWER] gauge read=%u ready=%u op=%04x status=%04x mV=%u SOC=%u avg-mA=%d\n",
            g.read, g.ready(), g.operation, g.status, g.voltageMv, g.soc, g.currentMa);
        uint16_t design = 0, full = 0, remaining = 0;
        const bool capacity = readGauge(0x3c, design) && readGauge(0x12, full) && readGauge(0x10, remaining);
        log.printf("[POWER] gauge capacity read=%u design=%u full=%u remaining=%u mAh\n",
                   capacity, design, full, remaining);
    }

    static const char* initName(Init result) {
        switch (result) {
            case Init::Ready: return "configured (704mA; voltage <=4208mV)";
            case Init::Unavailable: return "unavailable";
            case Init::WrongPart: return "unexpected part; unchanged";
            case Init::VerifyFailed: return "configuration failed verification";
        }
        return "unknown";
    }

private:
    bool writeCharger(uint8_t reg, uint8_t value) {
        bus_.beginTransmission(ChargerAddress);
        bus_.write(reg);
        bus_.write(value);
        return bus_.endTransmission() == 0;
    }

    bool update(uint8_t reg, uint8_t mask, uint8_t value) {
        uint8_t before = 0, after = 0;
        if (!readCharger(reg, before)) return false;
        const uint8_t wanted = (before & ~mask) | (value & mask);
        if (wanted == before) return true;
        return writeCharger(reg, wanted) && readCharger(reg, after) &&
            (after & mask) == (wanted & mask);
    }
    Bus& bus_;
};

} // namespace pager
