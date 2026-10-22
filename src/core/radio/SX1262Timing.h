/**
 * @file      sx126x.c
 *
 * @brief     SX126x radio driver implementation
 *
 * The Clear BSD License
 * Copyright Semtech Corporation 2021. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the disclaimer
 * below) provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
 * THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <stdint.h>

namespace handheld::sx1262_timing {

// Handheld TCXO startup is programmed in 15.625 us units. Calibration from
// RC standby can restart it, then still needs the existing 500 ms work budget.
constexpr uint32_t tcxoStartupTicks = 0x00A000;
constexpr uint32_t tcxoStartupMs = (tcxoStartupTicks + 63) / 64;

// BUSY covers the programmed clock delay plus the transition itself. Allow a
// full second beyond that delay for device variation; an observed 806 ms
// Cardputer cold start exceeded the old fixed 800 ms cap. This is an upper
// bound only: callers proceed as soon as BUSY clears.
constexpr uint32_t tcxoTimeoutMs() {
    return tcxoStartupMs + 1000;
}

constexpr uint32_t calibrationTimeoutMs(bool usesTcxo) {
    return 500 + (usesTcxo ? tcxoTimeoutMs() : 0);
}

// SX126x SetModulationParams bandwidth codes, not the saved/UI labels.
// Semtech sx126x_driver a10c5df, sx126x_get_lora_bw_in_hz. Use the lower
// integer Hz at the two codes whose fractional bandwidth rounds upward in
// that table (10417 and 41667), so integer timing cannot round airtime down.
constexpr uint32_t bandwidthHz(uint8_t code) {
    switch (code) {
        case 0x00: return 7812;
        case 0x08: return 10416;
        case 0x01: return 15625;
        case 0x09: return 20833;
        case 0x02: return 31250;
        case 0x0A: return 41666;
        case 0x03: return 62500;
        case 0x04: return 125000;
        case 0x05: return 250000;
        case 0x06: return 500000;
        default: return 0;
    }
}

// Bandwidth as RNode firmware's getSignalBandwidth() reports each code.
constexpr uint32_t rnodeBandwidthHz(uint8_t code) {
    switch (code) {
        case 0x00: return 7800;
        case 0x08: return 10400;
        case 0x01: return 15600;
        case 0x09: return 20800;
        case 0x02: return 31250;
        case 0x0A: return 41700;
        case 0x03: return 62500;
        case 0x04: return 125000;
        case 0x05: return 250000;
        case 0x06: return 500000;
        default: return 0;
    }
}

// RNode firmware 1.86 handleLowDataRate(): (1 << sf) / (bw / 1000) > 16 in C
// integer arithmetic. LDRO must match the peer's, so keep upstream's
// truncation (SF11/125 kHz off, SF7/7.8 kHz on), not Semtech's 16 ms rule.
constexpr bool lowDataRateOptimize(uint8_t sf, uint8_t bandwidthCode) {
    const uint32_t khz = rnodeBandwidthHz(bandwidthCode) / 1000;
    return sf >= 5 && sf <= 12 && khz != 0 && (uint32_t{1} << sf) / khz > 16;
}

constexpr uint32_t bitrate(uint8_t sf, uint8_t bandwidthCode, uint8_t codingRateDenominator) {
    const uint32_t bw = bandwidthHz(bandwidthCode);
    if (sf < 5 || sf > 12 || !bw || codingRateDenominator < 5 || codingRateDenominator > 8)
        return 0;
    // Nominal coded bit rate, excluding preamble/header overhead. The same
    // definition is used by the trusted Rust RNode transport interface.
    return static_cast<uint32_t>(uint64_t{sf} * 4 * bw /
                                ((uint64_t{1} << sf) * codingRateDenominator));
}

struct Modem {
    uint8_t spreadingFactor;
    uint8_t bandwidthCode;
    uint8_t codingRateDenominator;
    uint16_t preambleSymbols;
    bool implicitHeader;
    bool crc;
    bool lowDataRateOptimize;
};

// Adapted from Semtech sx126x_driver a10c5df's
// sx126x_get_lora_time_on_air_numerator. The SF7+ symbol formula also agrees
// with Lite/rsNode's LoraModemConfig; SX126x has distinct SF5/6 framing.
// Physical bytes include any RNode header; splitting belongs to the caller.
constexpr uint32_t airtimeMs(uint16_t physicalBytes, Modem modem) {
    const uint8_t sf = modem.spreadingFactor;
    const uint32_t bw = bandwidthHz(modem.bandwidthCode);
    const uint8_t cr = modem.codingRateDenominator;
    if (physicalBytes > 255 || sf < 5 || sf > 12 || !bw || cr < 5 || cr > 8)
        return 0;

    int32_t numerator = 8 * static_cast<int32_t>(physicalBytes) + (modem.crc ? 16 : 0) -
                        4 * sf + (modem.implicitHeader ? 0 : 20) + (sf > 6 ? 8 : 0);
    if (numerator < 0) numerator = 0;
    const uint32_t denominator = 4 * (sf - (sf > 6 && modem.lowDataRateOptimize ? 2 : 0));
    const uint32_t groups = (static_cast<uint32_t>(numerator) + denominator - 1) / denominator;
    const uint64_t quarterSymbols =
        uint64_t{4} * (uint32_t{modem.preambleSymbols} + groups * cr + (sf <= 6 ? 14 : 12)) + 1;
    const uint64_t numeratorMs = quarterSymbols * (uint64_t{1} << sf) * 1000;
    const uint64_t denominatorMs = uint64_t{4} * bw;
    // u64 intermediates cover the complete 16-bit hardware preamble register;
    // round the complete physical frame up exactly once to milliseconds.
    return static_cast<uint32_t>((numeratorMs + denominatorMs - 1) / denominatorMs);
}

// UserConfig's declared modem envelope: SF5..12, CR5..8, preamble6..65,
// all ten bandwidth codes. A maximum-length physical frame is the worst
// possible next fragment, even when its Reticulum packet is shorter than 500.
constexpr uint32_t MAX_SUPPORTED_FRAME_AIRTIME_MS =
    airtimeMs(255, {12, 0x00, 8, 65, false, true, true});
static_assert(MAX_SUPPORTED_FRAME_AIRTIME_MS == 254428, "update timing envelope evidence");

}  // namespace handheld::sx1262_timing
