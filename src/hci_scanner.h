#pragma once

#include <string>

#include "scanner.h"

namespace btfind {

struct HciConfig {
    int adapter = 0;          // hciN
    bool active_scan = true;  // active scan solicits scan responses (and names)
    double interval_ms = 100.0;
    double window_ms = 100.0;  // window == interval means continuous listening
    // Stop a scan another process is running so we can impose our own
    // parameters. Off by default: it breaks their discovery, and on a
    // kernel-managed scan it does not even work.
    bool takeover = false;
};

// Lists adapters visible in /sys/class/bluetooth, e.g. {"hci0"}.
std::vector<std::string> list_adapters();

// Why the scanner is consuming somebody else's scan instead of running its own.
enum class ObserveReason {
    None,          // not observing: the scan is ours
    Unprivileged,  // we may not send HCI commands, so we cannot start one
    ScanHeld,      // another process or the kernel holds the scan
};

// Parses the payload of an LE Advertising Report (subevent 0x02) or Extended
// Advertising Report (0x0D) — the bytes after the subevent code — invoking
// `sink` once per well-formed report. Exposed as free functions so the
// self-test can drive them with fixture bytes; malformed input is dropped
// without ever reading past `len`.
void parse_le_adv_report(const uint8_t* p, size_t len, const EventSink& sink);
void parse_le_ext_adv_report(const uint8_t* p, size_t len, const EventSink& sink);

class HciScanner : public Scanner {
public:
    explicit HciScanner(HciConfig cfg) : cfg_(cfg) {}
    ~HciScanner() override;

    bool start(std::string& error) override;
    void stop() override;
    int fd() const override { return fd_; }
    bool drain(const EventSink& sink) override;
    std::string source_name() const override;

    // Set when scanning began but another process (usually bluetoothd) was
    // already scanning; reports may be shared or filtered in that case.
    const std::string& warning() const { return warning_; }

    // True when we are consuming somebody else's scan rather than running our
    // own. Reports arrive only as fast as that scan produces them, and its
    // parameters (including active vs passive) are not ours to choose.
    bool observing() const { return observe_reason_ != ObserveReason::None; }
    ObserveReason observe_reason() const { return observe_reason_; }

private:
    bool send_cmd(uint16_t opcode, const uint8_t* params, uint8_t plen,
                  std::string& error);
    // Waits for the Command Complete/Status matching `opcode`. `status` receives
    // the controller's status byte (0x00 = success). When `ret`/`retlen` are
    // given, up to retlen bytes of the Command Complete return parameters
    // (after the status byte) are copied out.
    bool await_cmd(uint16_t opcode, uint8_t& status, std::string& error,
                   uint8_t* ret = nullptr, size_t retlen = 0);
    // Chooses legacy vs extended scan commands from the controller's LE
    // features. Modern kernels drive controllers with the extended set, and the
    // spec forbids mixing generations: once one set is in use, the other is
    // rejected with Command Disallowed.
    bool detect_command_set(std::string& error);
    bool send_scan_params(std::string& error, uint8_t& status);
    bool set_scan_enable(bool enable, std::string& error, uint8_t* status_out = nullptr);
    void handle_packet(const uint8_t* pkt, size_t len, const EventSink& sink);

    HciConfig cfg_;
    int fd_ = -1;
    bool scanning_ = false;
    ObserveReason observe_reason_ = ObserveReason::None;
    bool use_extended_ = false;
    int last_send_errno_ = 0;
    std::string warning_;
};

}  // namespace btfind
