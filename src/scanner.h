// Backend-agnostic scanning interface.
//
// A Scanner produces AdvEvents (one per received BLE advertisement) and exposes
// a pollable fd so the main loop can wait on the radio and the keyboard together.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace btfind {

using Addr = std::array<uint8_t, 6>;

// Monotonic milliseconds since an arbitrary epoch. Never jumps with wall clock.
uint64_t now_ms();

// Format an address big-endian, the way every vendor prints it: AA:BB:CC:DD:EE:FF
std::string format_addr(const Addr& a);

enum class AddrType : uint8_t {
    Public = 0x00,
    Random = 0x01,
    PublicIdentity = 0x02,
    RandomIdentity = 0x03,
};

struct AdvEvent {
    Addr addr{};
    AddrType addr_type = AddrType::Public;
    int8_t rssi = 0;
    uint8_t event_type = 0;      // advertising event type from the report
    std::vector<uint8_t> data;   // raw AD payload, may be empty
    uint64_t mono_time = 0;
};

using EventSink = std::function<void(const AdvEvent&)>;

class Scanner {
public:
    virtual ~Scanner() = default;

    // Begins scanning. Returns false and fills `error` on failure; the error text
    // is user-facing and should say what to do next, not just what went wrong.
    virtual bool start(std::string& error) = 0;

    // Stops scanning. Safe to call twice, and safe to call after a failed start.
    virtual void stop() = 0;

    // fd to poll for readability. -1 if this backend has nothing to wait on.
    virtual int fd() const = 0;

    // Drains everything currently readable, invoking `sink` per advertisement.
    // Returns false if the backend died and scanning cannot continue.
    virtual bool drain(const EventSink& sink) = 0;

    // Short description for the status bar, e.g. "hci0" or "demo".
    virtual std::string source_name() const = 0;
};

}  // namespace btfind
