// Parser for the AD (Advertising Data) / EIR TLV structure carried in BLE adverts.
//
// Layout is a bare sequence of [len][type][value...] where `len` counts the type
// byte plus the value. A zero length legitimately terminates the payload early
// (padding), and real-world controllers do emit truncated final records, so every
// field here is bounds-checked and a malformed tail is dropped rather than fatal.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace btfind {

namespace ad_type {
constexpr uint8_t Flags = 0x01;
constexpr uint8_t Incomplete16 = 0x02;
constexpr uint8_t Complete16 = 0x03;
constexpr uint8_t Incomplete32 = 0x04;
constexpr uint8_t Complete32 = 0x05;
constexpr uint8_t Incomplete128 = 0x06;
constexpr uint8_t Complete128 = 0x07;
constexpr uint8_t ShortName = 0x08;
constexpr uint8_t CompleteName = 0x09;
constexpr uint8_t TxPower = 0x0A;
constexpr uint8_t ServiceData16 = 0x16;
constexpr uint8_t Appearance = 0x19;
constexpr uint8_t ManufacturerData = 0xFF;
}  // namespace ad_type

struct AdInfo {
    std::string name;                       // short or complete local name
    bool name_is_complete = false;
    std::optional<int8_t> tx_power;         // AD 0x0A, dBm at 0 m (radiated power)
    std::optional<uint16_t> company_id;     // first manufacturer-data record
    std::vector<uint16_t> service_uuids16;
    uint8_t flags = 0;
    bool has_flags = false;

    // Beacon formats worth calling out by name in the UI, since they advertise a
    // calibrated 1 m reference power that beats our generic default.
    std::optional<int8_t> beacon_tx_1m;
    std::string beacon_kind;  // "iBeacon", "Eddystone", or empty
};

// Parses `data`. Never throws; returns whatever was recoverable.
AdInfo parse_ad(const uint8_t* data, size_t len);

// Human-readable vendor for a Bluetooth SIG company identifier, or "" if unknown.
// Covers only the handful that dominate the air in practice.
std::string company_name(uint16_t company_id);

// Best-effort device class guess from the parsed advert, e.g. "tag", "audio".
// Empty when nothing distinctive was advertised.
std::string guess_kind(const AdInfo& info);

}  // namespace btfind
