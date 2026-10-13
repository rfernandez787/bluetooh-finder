#include "hci_scanner.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace btfind {
namespace {

// BlueZ constants, declared locally so the build needs no libbluetooth-dev.
// These are ABI, fixed by the kernel and the Bluetooth Core spec.
constexpr int kAfBluetooth = 31;
constexpr int kBtprotoHci = 1;
constexpr int kSolHci = 0;
constexpr int kHciFilterOpt = 2;
constexpr unsigned short kHciChannelRaw = 0;

constexpr uint8_t kHciCommandPkt = 0x01;
constexpr uint8_t kHciEventPkt = 0x04;

constexpr uint8_t kEvtCmdComplete = 0x0E;
constexpr uint8_t kEvtCmdStatus = 0x0F;
constexpr uint8_t kEvtLeMeta = 0x3E;

constexpr uint8_t kSubEvtAdvReport = 0x02;
constexpr uint8_t kSubEvtExtAdvReport = 0x0D;

// OGF 0x08 (LE Controller) << 10 | OCF
constexpr uint16_t kOpLeReadLocalFeatures = 0x2003;
constexpr uint16_t kOpLeSetScanParams = 0x200B;
constexpr uint16_t kOpLeSetScanEnable = 0x200C;
constexpr uint16_t kOpLeSetExtScanParams = 0x2041;
constexpr uint16_t kOpLeSetExtScanEnable = 0x2042;

constexpr uint8_t kStatusCommandDisallowed = 0x0C;

struct sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct hci_filter {
    uint32_t type_mask;
    uint32_t event_mask[2];
    uint16_t opcode;
};

void filter_set_type(hci_filter& f, uint8_t t) { f.type_mask |= (1u << (t & 31)); }
void filter_set_event(hci_filter& f, uint8_t e) {
    f.event_mask[(e >> 5) & 1] |= (1u << (e & 31));
}

// HCI expresses scan interval/window in units of 0.625 ms, valid 0x0004..0x4000.
uint16_t to_hci_slots(double ms) {
    long v = static_cast<long>(ms / 0.625 + 0.5);
    return static_cast<uint16_t>(std::clamp(v, 0x0004L, 0x4000L));
}

void put_le16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

const char* status_text(uint8_t s) {
    switch (s) {
        case 0x00: return "success";
        case 0x01: return "unknown HCI command (controller lacks LE support?)";
        case 0x0C:
            return "command disallowed (scan already running, or the other "
                   "generation of scan commands is in use)";
        case 0x11: return "unsupported feature or parameter";
        case 0x12: return "invalid HCI command parameters";
        case 0x1F: return "unspecified error";
        default: return "controller error";
    }
}

}  // namespace

std::vector<std::string> list_adapters() {
    std::vector<std::string> out;
    DIR* d = opendir("/sys/class/bluetooth");
    if (d == nullptr) return out;
    while (struct dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "hci", 3) == 0) out.push_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

HciScanner::~HciScanner() { stop(); }

std::string HciScanner::source_name() const {
    std::string s = "hci" + std::to_string(cfg_.adapter);
    if (observing()) s += " (observing)";
    return s;
}

bool HciScanner::start(std::string& error) {
    fd_ = ::socket(kAfBluetooth, SOCK_RAW | SOCK_CLOEXEC, kBtprotoHci);
    if (fd_ < 0) {
        if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT) {
            error =
                "this kernel has no Bluetooth stack (AF_BLUETOOTH unsupported).\n"
                "  Containers usually lack it entirely. Run with --demo to try the "
                "UI,\n  or run on a host with a Bluetooth adapter.";
        } else if (errno == EPERM || errno == EACCES) {
            error = std::string("cannot open HCI socket: ") + strerror(errno) +
                    "\n  Grant the binary raw-socket access:\n"
                    "    sudo setcap 'cap_net_raw,cap_net_admin+eip' ./btfind";
        } else {
            error = std::string("socket(AF_BLUETOOTH): ") + strerror(errno);
        }
        return false;
    }

    // Ask only for the events we act on. An unfiltered raw socket delivers every
    // packet the controller sees, including ACL traffic we would only discard.
    hci_filter flt{};
    filter_set_type(flt, kHciEventPkt);
    filter_set_event(flt, kEvtLeMeta);
    filter_set_event(flt, kEvtCmdComplete);
    filter_set_event(flt, kEvtCmdStatus);
    if (::setsockopt(fd_, kSolHci, kHciFilterOpt, &flt, sizeof(flt)) < 0) {
        error = std::string("setsockopt(HCI_FILTER): ") + strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    sockaddr_hci sa{};
    sa.hci_family = kAfBluetooth;
    sa.hci_dev = static_cast<unsigned short>(cfg_.adapter);
    sa.hci_channel = kHciChannelRaw;
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
        const int err = errno;
        if (err == EPERM || err == EACCES) {
            error =
                "permission denied binding hci" + std::to_string(cfg_.adapter) +
                ".\n  Grant the binary raw-socket access:\n"
                "    sudo setcap 'cap_net_raw,cap_net_admin+eip' ./btfind\n"
                "  (or run under sudo)";
        } else if (err == ENODEV) {
            error = "no adapter hci" + std::to_string(cfg_.adapter) + ".";
            const auto found = list_adapters();
            if (found.empty()) {
                error += " No Bluetooth adapters present at all.";
            } else {
                error += " Available:";
                for (const auto& a : found) error += " " + a;
                error += "  (select with -i)";
            }
        } else {
            error = "bind(hci" + std::to_string(cfg_.adapter) + "): " + strerror(err);
        }
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Commands are written synchronously, then we wait for their completion, so
    // non-blocking mode is only needed once the socket is purely a stream of
    // advertising reports.
    //
    // Setting scan parameters is only allowed while no scan is running. On a
    // desktop something almost always is: bluetoothd, or the kernel's own
    // background LE scan for auto-connect and advertisement monitors.
    //
    // That is not a failure. The controller is already producing advertising
    // reports, and the kernel copies every HCI event to all raw sockets bound
    // to the device — so we can simply listen to the scan that exists. Doing so
    // also needs no privileges at all, since we never write to the socket.
    std::string cmd_error;
    uint8_t status = 0;

    if (!detect_command_set(cmd_error)) {
        if (last_send_errno_ == EPERM || last_send_errno_ == EACCES) {
            // Explicitly asked to take the scan over, but we may not even send
            // a command. Silently observing instead would ignore what the user
            // said; fail with the whole ladder of ways out.
            if (cfg_.takeover) {
                error =
                    "--takeover needs raw-socket capabilities, and this process "
                    "has none.\n"
                    "  Grant them:  sudo setcap 'cap_net_raw,cap_net_admin+eip' "
                    "./btfind\n"
                    "  If setcap says \"Operation not supported\", the binary "
                    "sits on a filesystem\n"
                    "  that cannot store capabilities (network mounts like "
                    "SMB/NFS cannot). Then:\n"
                    "    cp btfind ~/.local/bin/ && sudo setcap "
                    "'cap_net_raw,cap_net_admin+eip' ~/.local/bin/btfind\n"
                    "  or simply:  sudo ./btfind --takeover\n"
                    "  (Rebuilding wipes the capability — re-apply after every "
                    "make.)\n"
                    "  Without --takeover, btfind runs unprivileged by "
                    "observing an existing scan.";
                stop();
                return false;
            }
            observe_reason_ = ObserveReason::Unprivileged;
            warning_ =
                "running unprivileged — observing whatever scan is already "
                "active. For a scan on your own terms: make caps (or sudo)";
            return true;
        }
        error = cmd_error;
        stop();
        return false;
    }

    if (!send_scan_params(error, status)) {
        stop();
        return false;
    }

    if (status == kStatusCommandDisallowed) {
        if (!cfg_.takeover) {
            observe_reason_ = ObserveReason::ScanHeld;
            warning_ =
                "another scan is already running (bluetoothd or the kernel) — "
                "observing it. Pass --takeover to impose our own parameters.";
            return true;
        }
        // Explicitly asked for. Disable whatever is running — trying our
        // command set first, then the other generation, since 0x0C is also what
        // a controller says when the running scan was started with the set we
        // are not using.
        uint8_t off_status = 0;
        if (!set_scan_enable(false, cmd_error, &off_status)) {
            error = cmd_error;
            stop();
            return false;
        }
        if (off_status == kStatusCommandDisallowed) {
            use_extended_ = !use_extended_;
            if (!set_scan_enable(false, cmd_error, &off_status)) {
                error = cmd_error;
                stop();
                return false;
            }
        }
        if (off_status != 0x00) {
            error = "could not stop the running scan to take it over: ";
            error += status_text(off_status);
            error += "\n  Run without --takeover to observe it instead.";
            stop();
            return false;
        }
        if (!send_scan_params(error, status)) {
            stop();
            return false;
        }
        if (status == kStatusCommandDisallowed) {
            error =
                "the scan was re-enabled underneath us — the kernel is managing "
                "it and will not hand it over.\n"
                "  Run without --takeover to observe it instead.";
            stop();
            return false;
        }
        warning_ = "took over a scan that another process had running";
    }

    if (status != 0x00) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "LE Set %sScan Parameters failed: %s (0x%02X)",
                      use_extended_ ? "Extended " : "", status_text(status), status);
        error = buf;
        stop();
        return false;
    }

    if (!set_scan_enable(true, error, &status)) {
        stop();
        return false;
    }
    if (status == kStatusCommandDisallowed) {
        // Someone won the race between our parameters and our enable.
        observe_reason_ = ObserveReason::ScanHeld;
        warning_ = "a scan started underneath us — observing it instead";
        return true;
    }
    if (status != 0x00) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "LE Set %sScan Enable failed: %s (0x%02X)",
                      use_extended_ ? "Extended " : "", status_text(status), status);
        error = buf;
        stop();
        return false;
    }
    scanning_ = true;
    return true;
}

// LE Read Local Supported Features: bit 12 (byte 1, bit 4) is LE Extended
// Advertising. When the controller has it, the kernel drives scanning with the
// extended commands, and per spec the two command generations must not be
// mixed — so we follow the controller's capability, which is what the kernel
// followed too.
bool HciScanner::detect_command_set(std::string& error) {
    if (!send_cmd(kOpLeReadLocalFeatures, nullptr, 0, error)) return false;
    uint8_t status = 0;
    uint8_t features[8] = {0};
    if (!await_cmd(kOpLeReadLocalFeatures, status, error, features, sizeof(features))) {
        return false;
    }
    use_extended_ = (status == 0x00) && (features[1] & 0x10) != 0;
    return true;
}

bool HciScanner::send_scan_params(std::string& error, uint8_t& status) {
    const uint16_t interval = to_hci_slots(cfg_.interval_ms);
    const uint16_t window = to_hci_slots(cfg_.window_ms);
    const uint8_t scan_type = cfg_.active_scan ? 0x01 : 0x00;

    if (use_extended_) {
        // own_addr_type, filter_policy, scanning_phys (bit 0 = LE 1M), then
        // per selected PHY: type, interval, window.
        uint8_t p[8];
        p[0] = 0x00;
        p[1] = 0x00;
        p[2] = 0x01;
        p[3] = scan_type;
        put_le16(p + 4, interval);
        put_le16(p + 6, window);
        if (!send_cmd(kOpLeSetExtScanParams, p, sizeof(p), error)) return false;
        return await_cmd(kOpLeSetExtScanParams, status, error);
    }

    uint8_t p[7];
    p[0] = scan_type;
    put_le16(p + 1, interval);
    put_le16(p + 3, window);
    p[5] = 0x00;  // own address type: public
    p[6] = 0x00;  // filter policy: accept everything
    if (!send_cmd(kOpLeSetScanParams, p, sizeof(p), error)) return false;
    return await_cmd(kOpLeSetScanParams, status, error);
}

bool HciScanner::set_scan_enable(bool enable, std::string& error, uint8_t* status_out) {
    // Duplicate filtering would give us one report per device and then silence —
    // but every repeat advert is an RSSI sample, and samples are the product.
    uint8_t status = 0;
    if (use_extended_) {
        // enable, filter_duplicates, duration (10 ms units, 0 = forever),
        // period (0 = no cycling).
        uint8_t params[6] = {0};
        params[0] = enable ? 0x01 : 0x00;
        if (!send_cmd(kOpLeSetExtScanEnable, params, sizeof(params), error)) return false;
        if (!await_cmd(kOpLeSetExtScanEnable, status, error)) return false;
    } else {
        uint8_t params[2];
        params[0] = enable ? 0x01 : 0x00;
        params[1] = 0x00;
        if (!send_cmd(kOpLeSetScanEnable, params, sizeof(params), error)) return false;
        if (!await_cmd(kOpLeSetScanEnable, status, error)) return false;
    }
    if (status_out != nullptr) *status_out = status;
    return true;
}

bool HciScanner::send_cmd(uint16_t opcode, const uint8_t* params, uint8_t plen,
                          std::string& error) {
    uint8_t pkt[4 + 255];
    pkt[0] = kHciCommandPkt;
    put_le16(pkt + 1, opcode);
    pkt[3] = plen;
    if (plen > 0) memcpy(pkt + 4, params, plen);

    const size_t total = 4u + plen;
    ssize_t n = ::write(fd_, pkt, total);
    while (n < 0 && errno == EINTR) n = ::write(fd_, pkt, total);
    last_send_errno_ = (n < 0) ? errno : 0;
    if (n < 0) {
        error = std::string("writing HCI command: ") + strerror(errno);
        if (errno == EPERM) {
            error +=
                "\n  Sending HCI commands needs CAP_NET_RAW:\n"
                "    sudo setcap 'cap_net_raw,cap_net_admin+eip' ./btfind";
        }
        return false;
    }
    return true;
}

bool HciScanner::await_cmd(uint16_t opcode, uint8_t& status, std::string& error,
                           uint8_t* ret, size_t retlen) {
    // Controllers answer in milliseconds; a second is a generous ceiling. Any
    // advertising reports that arrive meanwhile are dropped, which costs nothing
    // since this only runs during startup and teardown.
    constexpr int kTimeoutMs = 1000;
    const uint64_t deadline = now_ms() + kTimeoutMs;

    for (;;) {
        const uint64_t t = now_ms();
        if (t >= deadline) {
            error = "timed out waiting for the controller to acknowledge a command";
            return false;
        }
        struct pollfd pfd { fd_, POLLIN, 0 };
        const int pr = ::poll(&pfd, 1, static_cast<int>(deadline - t));
        if (pr < 0) {
            if (errno == EINTR) continue;
            error = std::string("poll: ") + strerror(errno);
            return false;
        }
        if (pr == 0) continue;

        uint8_t buf[1024];
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            error = std::string("read: ") + strerror(errno);
            return false;
        }
        if (n < 3 || buf[0] != kHciEventPkt) continue;

        const uint8_t evt = buf[1];
        const uint8_t plen = buf[2];
        if (static_cast<size_t>(n) < 3u + plen) continue;  // short read, discard
        const uint8_t* p = buf + 3;

        // Command Complete: num_slots(1), opcode(2), status(1), ret...
        if (evt == kEvtCmdComplete && plen >= 4) {
            const uint16_t op = static_cast<uint16_t>(p[1] | (p[2] << 8));
            if (op == opcode) {
                status = p[3];
                if (ret != nullptr && retlen > 0) {
                    const size_t have = plen - 4u;
                    memcpy(ret, p + 4, std::min(retlen, have));
                }
                return true;
            }
        }
        // Command Status: status(1), num_slots(1), opcode(2)
        if (evt == kEvtCmdStatus && plen >= 4) {
            const uint16_t op = static_cast<uint16_t>(p[2] | (p[3] << 8));
            if (op == opcode) {
                status = p[0];
                return true;
            }
        }
    }
}

void HciScanner::stop() {
    if (fd_ < 0) return;
    // Only if we enabled it. In observe mode the scan belongs to someone else,
    // and switching it off on our way out would break their discovery.
    if (scanning_) {
        // Leaving the controller scanning would drain the host's battery and
        // block the next process that wants to set scan parameters.
        std::string ignored;
        set_scan_enable(false, ignored);
        scanning_ = false;
    }
    ::close(fd_);
    fd_ = -1;
}

bool HciScanner::drain(const EventSink& sink) {
    if (fd_ < 0) return false;
    // Bounded so a very busy channel cannot starve the redraw and the keyboard.
    for (int i = 0; i < 256; ++i) {
        uint8_t buf[2048];
        ssize_t n = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;  // socket is gone: adapter unplugged or rfkill'd
        }
        if (n == 0) return false;
        handle_packet(buf, static_cast<size_t>(n), sink);
    }
    return true;
}

void HciScanner::handle_packet(const uint8_t* pkt, size_t len, const EventSink& sink) {
    if (len < 3 || pkt[0] != kHciEventPkt) return;
    const uint8_t evt = pkt[1];
    const uint8_t plen = pkt[2];
    if (len < 3u + plen) return;  // truncated frame
    if (evt != kEvtLeMeta || plen < 1) return;

    const uint8_t* p = pkt + 3;
    const uint8_t subevent = p[0];
    if (subevent == kSubEvtAdvReport) {
        parse_le_adv_report(p + 1, plen - 1u, sink);
    } else if (subevent == kSubEvtExtAdvReport) {
        parse_le_ext_adv_report(p + 1, plen - 1u, sink);
    }
}

// LE Advertising Report: num_reports(1), then per report
//   evt_type(1) addr_type(1) addr(6) data_len(1) data[data_len] rssi(1)
//
// The spec formally describes the fields as parallel arrays across reports, but
// controllers in the wild emit num_reports == 1 and BlueZ has always read them
// sequentially. We do the same, and let the length checks stop us if a device
// ever disagrees.
void parse_le_adv_report(const uint8_t* p, size_t len, const EventSink& sink) {
    if (len < 1) return;
    const uint8_t num_reports = p[0];
    size_t off = 1;
    const uint64_t t = now_ms();

    for (uint8_t r = 0; r < num_reports; ++r) {
        if (off + 9 > len) return;  // evt_type + addr_type + addr + data_len
        AdvEvent ev;
        ev.event_type = p[off];
        ev.addr_type = static_cast<AddrType>(p[off + 1]);
        // The address is little-endian on the wire; store it display order.
        for (int i = 0; i < 6; ++i) ev.addr[i] = p[off + 2 + (5 - i)];
        const uint8_t data_len = p[off + 8];
        off += 9;

        if (off + data_len + 1u > len) return;  // data + rssi would overrun
        ev.data.assign(p + off, p + off + data_len);
        off += data_len;
        ev.rssi = static_cast<int8_t>(p[off]);
        off += 1;
        ev.mono_time = t;

        // 127 is the spec's "RSSI not available" sentinel, not a huge signal.
        if (ev.rssi == 127) continue;
        sink(ev);
    }
}

// LE Extended Advertising Report: num_reports(1), then per report
//   evt_type(2) addr_type(1) addr(6) primary_phy(1) secondary_phy(1) sid(1)
//   tx_power(1) rssi(1) periodic_interval(2) direct_addr_type(1)
//   direct_addr(6) data_len(1) data[data_len]
void parse_le_ext_adv_report(const uint8_t* p, size_t len, const EventSink& sink) {
    if (len < 1) return;
    const uint8_t num_reports = p[0];
    size_t off = 1;
    const uint64_t t = now_ms();
    constexpr size_t kFixed = 24;  // bytes before the data field

    for (uint8_t r = 0; r < num_reports; ++r) {
        if (off + kFixed > len) return;
        AdvEvent ev;
        ev.event_type = p[off];  // low byte carries the legacy-equivalent bits
        ev.addr_type = static_cast<AddrType>(p[off + 2]);
        for (int i = 0; i < 6; ++i) ev.addr[i] = p[off + 3 + (5 - i)];
        // Fixed part: evt_type(2) addr_type(1) addr(6) primary_phy(1)
        // secondary_phy(1) sid(1) tx_power(1) rssi(1) — RSSI is offset 13;
        // offset 12 is TX power, an easy off-by-one with painful symptoms.
        ev.rssi = static_cast<int8_t>(p[off + 13]);
        const uint8_t data_len = p[off + kFixed - 1];
        off += kFixed;

        if (off + data_len > len) return;
        ev.data.assign(p + off, p + off + data_len);
        off += data_len;
        ev.mono_time = t;

        if (ev.rssi == 127) continue;
        sink(ev);
    }
}

}  // namespace btfind
