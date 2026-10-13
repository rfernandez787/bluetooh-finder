#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "ad_parse.h"
#include "demo_scanner.h"
#include "device.h"
#include "hci_scanner.h"
#include "term.h"
#include "ui.h"

namespace btfind {

int run_selftest();  // selftest.cpp

namespace {

struct Options {
    bool demo = false;
    bool selftest = false;
    bool color = true;
    HciConfig hci;
    Config ui;
    std::string log_path;
};

void usage() {
    std::printf(
        "btfind — find Bluetooth LE devices by signal strength\n"
        "\n"
        "usage: btfind [options]\n"
        "\n"
        "  -i, --adapter N       use hciN (default 0)\n"
        "      --demo            simulate devices instead of using the radio\n"
        "      --passive         passive scan; does not solicit scan responses,\n"
        "                        so fewer devices reveal a name\n"
        "      --takeover        stop a scan another process is running and\n"
        "                        impose our own parameters. Breaks their\n"
        "                        discovery, and a kernel-managed scan will\n"
        "                        simply be restored\n"
        "      --interval MS     scan interval (default 100)\n"
        "      --window MS       scan window (default 100 — equal means listen\n"
        "                        continuously)\n"
        "      --txpower DBM     RSSI a device shows at 1 m (default %d)\n"
        "      --path-loss N     path-loss exponent: 2.0 open air, 3-4 indoors\n"
        "      --min-rssi DBM    hide anything weaker\n"
        "      --stale S         dim devices unheard for this long (default 30)\n"
        "      --forget S        drop devices unheard for this long (default 120)\n"
        "      --log FILE        append every advertisement to FILE as CSV\n"
        "      --no-color        plain output\n"
        "      --selftest        run the built-in checks and exit\n"
        "  -h, --help            this message\n"
        "\n"
        "Scanning the radio needs raw socket access:\n"
        "  sudo setcap 'cap_net_raw,cap_net_admin+eip' ./btfind\n"
        "\n"
        "Keys: ↑↓ select · ⏎ hunt · / filter · s sort · f floor · b beep · ? help\n",
        static_cast<int>(kDefaultTx1m));
}

// Returns false on a malformed argument, having already printed why.
bool parse_args(int argc, char** argv, Options& o) {
    auto need_value = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "btfind: %s needs a value\n", flag);
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            std::exit(0);
        } else if (a == "--demo") {
            o.demo = true;
        } else if (a == "--selftest") {
            o.selftest = true;
        } else if (a == "--no-color") {
            o.color = false;
        } else if (a == "--passive") {
            o.hci.active_scan = false;
        } else if (a == "--takeover") {
            o.hci.takeover = true;
        } else if (a == "-i" || a == "--adapter") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            // Accept both "0" and "hci0".
            o.hci.adapter = std::atoi(strncmp(v, "hci", 3) == 0 ? v + 3 : v);
        } else if (a == "--interval") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            o.hci.interval_ms = std::atof(v);
        } else if (a == "--window") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            o.hci.window_ms = std::atof(v);
        } else if (a == "--txpower") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            o.ui.default_tx_1m = std::atof(v);
        } else if (a == "--path-loss") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            o.ui.path_loss = std::atof(v);
            if (o.ui.path_loss <= 0.0) {
                std::fprintf(stderr, "btfind: --path-loss must be positive\n");
                return false;
            }
        } else if (a == "--min-rssi") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            o.ui.min_rssi = std::atoi(v);
        } else if (a == "--stale") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            o.ui.stale_ms = static_cast<uint64_t>(std::atof(v) * 1000.0);
        } else if (a == "--forget") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            o.ui.prune_ms = static_cast<uint64_t>(std::atof(v) * 1000.0);
        } else if (a == "--log") {
            const char* v = need_value(i, a.c_str());
            if (v == nullptr) return false;
            o.log_path = v;
        } else {
            std::fprintf(stderr, "btfind: unknown option '%s' (try --help)\n",
                         a.c_str());
            return false;
        }
    }

    if (o.hci.window_ms > o.hci.interval_ms) {
        std::fprintf(stderr,
                     "btfind: --window cannot exceed --interval; clamping to %.0f\n",
                     o.hci.interval_ms);
        o.hci.window_ms = o.hci.interval_ms;
    }
    return true;
}

int run(const Options& o) {
    std::unique_ptr<Scanner> scanner;
    if (o.demo) {
        scanner = std::make_unique<DemoScanner>();
    } else {
        scanner = std::make_unique<HciScanner>(o.hci);
    }

    std::string error;
    if (!scanner->start(error)) {
        std::fprintf(stderr, "btfind: %s\n", error.c_str());
        return 1;
    }

    std::string notice;
    ObserveReason observe_reason = ObserveReason::None;
    bool real_radio = false;
    if (auto* h = dynamic_cast<HciScanner*>(scanner.get())) {
        if (!h->warning().empty()) notice = h->warning();
        observe_reason = h->observe_reason();
        real_radio = true;
    }
    if (o.demo) {
        notice = "DEMO — simulated devices, the radio is not in use";
    }

    FILE* log = nullptr;
    if (!o.log_path.empty()) {
        log = std::fopen(o.log_path.c_str(), "a");
        if (log == nullptr) {
            std::fprintf(stderr, "btfind: cannot open %s: %s\n", o.log_path.c_str(),
                         strerror(errno));
            return 1;
        }
        std::fprintf(log, "# time_ms,address,rssi,name\n");
    }

    install_signal_handlers();

    Config cfg = o.ui;
    DeviceTable table;
    Terminal term(o.color);
    Ui ui(table, cfg);

    bool scanning_ok = true;
    uint64_t last_prune = now_ms();
    uint64_t notice_until = now_ms() + 8000;  // notices fade rather than nag
    const uint64_t started = now_ms();
    bool silent_warned = false;

    while (!g_should_quit) {
        struct pollfd fds[2];
        int nfds = 0;
        fds[nfds++] = {STDIN_FILENO, POLLIN, 0};
        const int sfd = scanner->fd();
        if (sfd >= 0 && scanning_ok) fds[nfds++] = {sfd, POLLIN, 0};

        const int timeout = ui.next_wakeup_ms();
        const int pr = ::poll(fds, nfds, timeout);
        if (pr < 0 && errno != EINTR) break;

        if (g_resized) {
            g_resized = 0;
            term.refresh_size();
        }

        // Keyboard first: a quit or a mode change should not wait behind a
        // backlog of advertisements.
        bool keep_running = true;
        for (;;) {
            const KeyEvent k = term.read_key();
            if (k.key == Key::None) break;
            if (!ui.handle_key(k, term)) {
                keep_running = false;
                break;
            }
        }
        if (!keep_running) break;

        if (scanning_ok && !ui.paused()) {
            const bool alive = scanner->drain([&](const AdvEvent& ev) {
                const AdInfo info = parse_ad(ev.data.data(), ev.data.size());
                const Device& d = table.observe(ev, info);
                if (log != nullptr) {
                    // Names come off the air and may contain quotes or commas;
                    // RFC 4180 escapes a quote by doubling it.
                    std::string name;
                    name.reserve(d.name.size());
                    for (char ch : d.name) {
                        if (ch == '"') name += '"';
                        name += ch;
                    }
                    std::fprintf(log, "%llu,%s,%d,\"%s\"\n",
                                 static_cast<unsigned long long>(ev.mono_time),
                                 format_addr(ev.addr).c_str(), ev.rssi,
                                 name.c_str());
                }
            });
            if (!alive) {
                scanning_ok = false;
                notice = "the adapter stopped responding (unplugged or rfkill'd)";
                notice_until = now_ms() + 3600000;
            }
        } else if (scanning_ok && ui.paused()) {
            // Keep draining while paused so the fd stays readable-clear and the
            // poll loop does not spin; the samples are simply discarded.
            scanner->drain([](const AdvEvent&) {});
        }

        // Pausing freezes the table entirely. Ageing devices out while paused
        // would quietly empty the screen out from under someone who stopped it
        // precisely so they could read it.
        const uint64_t now = now_ms();
        if (!ui.paused() && now - last_prune > 1000) {
            table.prune(now, cfg.prune_ms);
            last_prune = now;
        }

        // Prolonged silence looks exactly like "no devices nearby", so say what
        // is actually going on — which depends on whose scan this is.
        if (real_radio && !silent_warned && table.size() == 0 &&
            now - started > 6000) {
            silent_warned = true;
            switch (observe_reason) {
                case ObserveReason::Unprivileged:
                    // Suggesting --takeover here would be circular: it needs
                    // the very capabilities whose absence put us in this mode.
                    notice =
                        "no reports — nothing is scanning, and without "
                        "capabilities btfind cannot start a scan. Run "
                        "'bluetoothctl scan on' alongside, or see 'make caps'";
                    break;
                case ObserveReason::ScanHeld:
                    notice =
                        "no reports — the scan we are observing may be idle. "
                        "Start a discovery (bluetoothctl scan on) or rerun "
                        "with --takeover";
                    break;
                case ObserveReason::None:
                    notice =
                        "our scan is running but hearing nothing — check "
                        "rfkill and that the adapter has an antenna";
                    break;
            }
            notice_until = now + 3600000;
        }

        ui.tick_bell(term);
        term.present(ui.render(term, scanner->source_name(), scanning_ok,
                               now < notice_until ? notice : std::string()));
    }

    scanner->stop();
    if (log != nullptr) std::fclose(log);
    return 0;
}

}  // namespace
}  // namespace btfind

int main(int argc, char** argv) {
    btfind::Options opts;
    if (!btfind::parse_args(argc, argv, opts)) return 2;
    if (opts.selftest) return btfind::run_selftest();

    // Colour is pointless when piped, and the TUI needs a real terminal at all.
    if (!::isatty(STDOUT_FILENO)) {
        opts.color = false;
        std::fprintf(stderr,
                     "btfind: stdout is not a terminal — this is an interactive "
                     "TUI.\n        Try --selftest, or --log FILE to capture data.\n");
        return 2;
    }
    return btfind::run(opts);
}
