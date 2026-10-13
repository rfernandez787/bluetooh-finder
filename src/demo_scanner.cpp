#include "demo_scanner.h"

#include <errno.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "ad_parse.h"

namespace btfind {
namespace {

// Builds one AD record: [len][type][value...]
void add_ad(std::vector<uint8_t>& out, uint8_t type,
            std::initializer_list<uint8_t> value) {
    out.push_back(static_cast<uint8_t>(1 + value.size()));
    out.push_back(type);
    out.insert(out.end(), value.begin(), value.end());
}

void add_name(std::vector<uint8_t>& out, const std::string& name) {
    out.push_back(static_cast<uint8_t>(1 + name.size()));
    out.push_back(ad_type::CompleteName);
    out.insert(out.end(), name.begin(), name.end());
}

// The tick rate of the simulation. Fast enough that a 20 ms beacon still looks
// like a fast beacon, cheap enough to be free.
constexpr int kTickMs = 10;

// Propagation exponent the simulator transmits through. Deliberately its own
// constant rather than the UI's default, so the display is never being fed a
// world that matches its own assumptions exactly.
constexpr double kSimPathLoss = 2.1;

}  // namespace

DemoScanner::DemoScanner() : rng_(0xB1E7007u) { build_devices(); }

DemoScanner::~DemoScanner() { stop(); }

void DemoScanner::build_devices() {
    auto mac = [](uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f) {
        return Addr{a, b, c, d, e, f};
    };

    // A keyring tag that circles you slowly — the flagship "find me" case.
    {
        SimDevice d{};
        d.addr = mac(0xC4, 0x8E, 0x8F, 0x12, 0x34, 0x56);
        d.name = "Tile Mate";
        d.base_distance_m = 6.0;
        d.orbit_m = 5.2;
        d.period_s = 45.0;
        d.phase = 0.0;
        d.adv_interval_ms = 150.0;
        d.tx_1m = -60;
        add_ad(d.ad, ad_type::Flags, {0x06});
        add_name(d.ad, d.name);
        add_ad(d.ad, ad_type::Complete16, {0x5A, 0xFD});
        add_ad(d.ad, ad_type::TxPower, {0xC4});  // -60 dBm
        devices_.push_back(d);
    }
    // Earbuds sitting still and close: the trend readout should stay steady.
    {
        SimDevice d{};
        d.addr = mac(0x8C, 0x2D, 0xAA, 0x77, 0x01, 0x9F);
        d.name = "AirPods Pro";
        d.base_distance_m = 1.4;
        d.orbit_m = 0.35;
        d.period_s = 30.0;
        d.phase = 1.1;
        d.adv_interval_ms = 60.0;
        d.tx_1m = -56;
        add_ad(d.ad, ad_type::Flags, {0x1A});
        add_name(d.ad, d.name);
        add_ad(d.ad, ad_type::ManufacturerData, {0x4C, 0x00, 0x07, 0x19, 0x01, 0x14});
        devices_.push_back(d);
    }
    // A calibrated iBeacon parked across the room.
    {
        SimDevice d{};
        d.addr = mac(0xD0, 0x39, 0x72, 0xBE, 0xAC, 0x01);
        d.base_distance_m = 11.0;
        d.orbit_m = 1.0;
        d.period_s = 90.0;
        d.phase = 2.0;
        d.adv_interval_ms = 100.0;
        d.tx_1m = -65;
        add_ad(d.ad, ad_type::Flags, {0x06});
        add_ad(d.ad, ad_type::ManufacturerData,
               {0x4C, 0x00, 0x02, 0x15,
                0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
                0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0,
                0x00, 0x07, 0x00, 0x2A, 0xBF});  // major 7, minor 42, -65 dBm
        devices_.push_back(d);
    }
    // An Eddystone node, and a nameless phone with a rotating random address —
    // both common, both worth having on screen to test the "unnamed" filter.
    {
        SimDevice d{};
        d.addr = mac(0xF1, 0x02, 0xEE, 0xDD, 0x00, 0x12);
        d.base_distance_m = 18.0;
        d.orbit_m = 3.0;
        d.period_s = 120.0;
        d.phase = 0.6;
        d.adv_interval_ms = 900.0;
        d.tx_1m = -70;
        add_ad(d.ad, ad_type::Flags, {0x06});
        // Eddystone UID frame. Byte 3 is the power at 0 m, which is 41 dB above
        // the 1 m figure by convention — so it must be tx_1m + 41 to describe
        // the same imaginary radio the simulator is transmitting with.
        add_ad(d.ad, ad_type::ServiceData16, {0xAA, 0xFE, 0x00, 0xE3, 0x00, 0x01});
        devices_.push_back(d);
    }
    {
        SimDevice d{};
        d.addr = mac(0x5B, 0x91, 0x3C, 0x0E, 0x44, 0xA2);
        d.base_distance_m = 4.5;
        d.orbit_m = 2.0;
        d.period_s = 20.0;
        d.phase = 3.3;
        d.adv_interval_ms = 250.0;
        d.tx_1m = -58;
        d.intermittent = true;
        add_ad(d.ad, ad_type::Flags, {0x1A});
        add_ad(d.ad, ad_type::ManufacturerData, {0x06, 0x00, 0x01, 0x09, 0x21});
        devices_.push_back(d);
    }
    // A weak, far, slow tag — exercises the RSSI floor and the stale dimming.
    {
        SimDevice d{};
        d.addr = mac(0xA7, 0x55, 0x10, 0x9C, 0x3B, 0xE8);
        d.name = "Chipolo ONE";
        d.base_distance_m = 34.0;
        d.orbit_m = 6.0;
        d.period_s = 70.0;
        d.phase = 4.2;
        d.adv_interval_ms = 1800.0;
        d.tx_1m = -62;
        d.intermittent = true;
        add_ad(d.ad, ad_type::Flags, {0x06});
        add_name(d.ad, d.name);
        add_ad(d.ad, ad_type::ManufacturerData, {0xD7, 0x01, 0x03, 0x11});
        devices_.push_back(d);
    }
}

bool DemoScanner::start(std::string& error) {
    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd_ < 0) {
        error = std::string("timerfd_create: ") + strerror(errno);
        return false;
    }
    struct itimerspec its {};
    its.it_interval.tv_nsec = kTickMs * 1000000L;
    its.it_value.tv_nsec = kTickMs * 1000000L;
    if (::timerfd_settime(timer_fd_, 0, &its, nullptr) < 0) {
        error = std::string("timerfd_settime: ") + strerror(errno);
        ::close(timer_fd_);
        timer_fd_ = -1;
        return false;
    }
    t0_ = now_ms();
    for (auto& d : devices_) d.next_due_ms = 0.0;
    return true;
}

void DemoScanner::stop() {
    if (timer_fd_ >= 0) {
        ::close(timer_fd_);
        timer_fd_ = -1;
    }
}

double DemoScanner::distance_at(const SimDevice& d, double t_s) const {
    const double w = 2.0 * M_PI / d.period_s;
    return std::max(0.3, d.base_distance_m + d.orbit_m * std::sin(w * t_s + d.phase));
}

bool DemoScanner::drain(const EventSink& sink) {
    if (timer_fd_ < 0) return false;

    uint64_t expirations = 0;
    ssize_t n = ::read(timer_fd_, &expirations, sizeof(expirations));
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return false;
    }

    const uint64_t now = now_ms();
    const double elapsed_ms = static_cast<double>(now - t0_);

    std::normal_distribution<double> fading(0.0, 3.2);   // multipath, in dB
    std::uniform_real_distribution<double> jitter(0.85, 1.15);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    for (auto& d : devices_) {
        // Emit every advert that came due since the last drain, so a slow redraw
        // does not silently lower a device's apparent advertising rate.
        while (d.next_due_ms <= elapsed_ms) {
            const double at_s = d.next_due_ms / 1000.0;
            d.next_due_ms += d.adv_interval_ms * jitter(rng_);

            // Real tags drop packets in bursts; a device that never misses one
            // would make the staleness and age handling look better than it is.
            if (d.intermittent && unit(rng_) < 0.25) continue;

            const double dist = distance_at(d, at_s);
            const double ideal = d.tx_1m - 10.0 * kSimPathLoss * std::log10(dist);
            const double rssi = ideal + fading(rng_);

            AdvEvent ev;
            ev.addr = d.addr;
            ev.addr_type = d.name.empty() ? AddrType::Random : AddrType::Public;
            ev.rssi = static_cast<int8_t>(std::clamp(std::lround(rssi), -110L, -20L));
            ev.event_type = 0x00;
            ev.data = d.ad;
            // Timestamp when it was due, not now: keeps the time-aware EMA honest.
            ev.mono_time = t0_ + static_cast<uint64_t>(at_s * 1000.0);
            sink(ev);
        }
    }
    return true;
}

}  // namespace btfind
