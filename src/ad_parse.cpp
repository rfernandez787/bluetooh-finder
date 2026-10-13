#include "ad_parse.h"

#include <algorithm>

namespace btfind {
namespace {

// Names are sanitised on the way in: adverts are attacker-controllable and land
// straight in a terminal, so anything that could move the cursor or start an
// escape sequence is replaced.
std::string sanitize_name(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c == 0) break;                       // some stacks null-pad the name
        if (c < 0x20 || c == 0x7F) c = '.';      // control chars, incl. ESC
        out.push_back(static_cast<char>(c));
    }
    // Trim trailing whitespace; leading is kept so odd names stay recognisable.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace

AdInfo parse_ad(const uint8_t* data, size_t len) {
    AdInfo info;
    if (data == nullptr) return info;

    size_t i = 0;
    while (i < len) {
        const uint8_t field_len = data[i];
        if (field_len == 0) break;  // padding: rest of the payload is dead space

        // field_len covers the type byte + value. Need that many bytes after it.
        if (i + 1 + field_len > len) break;  // truncated record: stop, keep prior

        const uint8_t type = data[i + 1];
        const uint8_t* value = data + i + 2;
        const size_t value_len = field_len - 1;

        switch (type) {
            case ad_type::Flags:
                if (value_len >= 1) {
                    info.flags = value[0];
                    info.has_flags = true;
                }
                break;

            case ad_type::CompleteName:
            case ad_type::ShortName: {
                // A complete name always wins over a shortened one.
                const bool complete = (type == ad_type::CompleteName);
                if (info.name.empty() || (complete && !info.name_is_complete)) {
                    std::string n = sanitize_name(value, value_len);
                    if (!n.empty()) {
                        info.name = std::move(n);
                        info.name_is_complete = complete;
                    }
                }
                break;
            }

            case ad_type::TxPower:
                if (value_len >= 1) info.tx_power = static_cast<int8_t>(value[0]);
                break;

            case ad_type::Incomplete16:
            case ad_type::Complete16:
                for (size_t k = 0; k + 1 < value_len; k += 2) {
                    info.service_uuids16.push_back(
                        static_cast<uint16_t>(value[k] | (value[k + 1] << 8)));
                }
                break;

            case ad_type::ServiceData16:
                if (value_len >= 2) {
                    const uint16_t uuid =
                        static_cast<uint16_t>(value[0] | (value[1] << 8));
                    info.service_uuids16.push_back(uuid);
                    // Eddystone: 0xFEAA. UID/URL frames put the 0 m reference
                    // power at byte 1 of the frame; it is calibrated at 0 m, and
                    // the convention is to subtract 41 dB to get a 1 m figure.
                    if (uuid == 0xFEAA && value_len >= 4) {
                        const uint8_t frame = value[2];
                        if (frame == 0x00 || frame == 0x10) {
                            info.beacon_kind = "Eddystone";
                            info.beacon_tx_1m =
                                static_cast<int8_t>(static_cast<int8_t>(value[3]) - 41);
                        }
                    }
                }
                break;

            case ad_type::ManufacturerData:
                if (value_len >= 2) {
                    if (!info.company_id) {
                        info.company_id =
                            static_cast<uint16_t>(value[0] | (value[1] << 8));
                    }
                    // iBeacon: Apple (0x004C), subtype 0x02, subtype length 0x15,
                    // then 16 UUID + 2 major + 2 minor + 1 measured power @ 1 m.
                    if (value[0] == 0x4C && value[1] == 0x00 && value_len >= 25 &&
                        value[2] == 0x02 && value[3] == 0x15) {
                        info.beacon_kind = "iBeacon";
                        info.beacon_tx_1m = static_cast<int8_t>(value[24]);
                    }
                }
                break;

            default:
                break;
        }

        i += 1 + field_len;
    }

    return info;
}

std::string company_name(uint16_t id) {
    switch (id) {
        case 0x0006: return "Microsoft";
        case 0x004C: return "Apple";
        case 0x0075: return "Samsung";
        case 0x00E0: return "Google";
        case 0x0087: return "Garmin";
        case 0x0157: return "Huawei";
        case 0x038F: return "Xiaomi";
        case 0x0171: return "Amazon";
        case 0x0499: return "Ruuvi";
        case 0x0059: return "Nordic";
        case 0x02E5: return "Espressif";
        case 0x0180: return "Tile";
        case 0x03DA: return "Fitbit";
        case 0x0110: return "Sonos";
        case 0x00D2: return "Logitech";
        case 0x01D7: return "Chipolo";
        default: return "";
    }
}

std::string guess_kind(const AdInfo& info) {
    if (!info.beacon_kind.empty()) return info.beacon_kind;

    for (uint16_t u : info.service_uuids16) {
        switch (u) {
            case 0xFD5A: case 0xFEED: return "tag";       // Tile
            case 0xFE2C: case 0xFEAA: return "tag";       // Fast Pair / Eddystone
            case 0x110B: case 0x110A: return "audio";
            case 0x180D: return "heart rate";
            case 0x180F: return "battery";
            case 0x1812: return "HID";
            case 0xFD6F: return "exposure";               // contact tracing
            default: break;
        }
    }

    if (info.company_id) {
        const std::string vendor = company_name(*info.company_id);
        if (!vendor.empty()) return vendor;
    }
    return "";
}

}  // namespace btfind
