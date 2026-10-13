#pragma once

#include <random>
#include <vector>

#include "scanner.h"

namespace btfind {

// Synthesises advertisements from imaginary devices. This exists because the
// real backend needs a Bluetooth adapter and CAP_NET_RAW, neither of which is
// available in a container — so the UI would otherwise be undevelopable and
// untestable there. The signals it produces are deliberately noisy and drifting,
// matching what the smoothing and trend code has to cope with for real.
class DemoScanner : public Scanner {
public:
    DemoScanner();
    ~DemoScanner() override;

    bool start(std::string& error) override;
    void stop() override;
    int fd() const override { return timer_fd_; }
    bool drain(const EventSink& sink) override;
    std::string source_name() const override { return "demo"; }

private:
    struct SimDevice {
        Addr addr;
        std::string name;
        double base_distance_m;   // metres it hovers around
        double orbit_m;           // how far it wanders from that
        double period_s;          // how long a full approach/retreat takes
        double phase;
        double adv_interval_ms;   // how often it advertises
        double next_due_ms;
        int8_t tx_1m;
        std::vector<uint8_t> ad;  // fixed AD payload
        bool intermittent;        // goes quiet for stretches, like a real tag
    };

    void build_devices();
    double distance_at(const SimDevice& d, double t_s) const;

    int timer_fd_ = -1;
    uint64_t t0_ = 0;
    std::vector<SimDevice> devices_;
    std::mt19937 rng_;
};

}  // namespace btfind
