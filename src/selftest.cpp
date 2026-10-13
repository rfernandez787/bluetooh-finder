// Built-in checks, runnable anywhere with `btfind --selftest`.
//
// The parsing tests matter more than they look: AD payloads come off the air
// from devices we do not control, and a length field that lies is the easiest
// way to walk a parser off the end of a buffer.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "ad_parse.h"
#include "device.h"
#include "hci_scanner.h"
#include "term.h"
#include "ui.h"

namespace btfind {
namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void check_near(double got, double want, double tol, const std::string& what) {
    ++g_checks;
    if (std::fabs(got - want) > tol) {
        ++g_failures;
        std::printf("  FAIL  %s (got %.4f, want %.4f ±%.4f)\n", what.c_str(), got,
                    want, tol);
    }
}

void section(const char* name) { std::printf("%s\n", name); }

// --- AD parsing -------------------------------------------------------------

void test_ad_parse() {
    section("ad_parse");

    // Flags + complete name + TX power.
    {
        const std::vector<uint8_t> d = {0x02, 0x01, 0x06,
                                        0x05, 0x09, 'T', 'i', 'l', 'e',
                                        0x02, 0x0A, 0xC4};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.name == "Tile", "complete local name");
        check(i.name_is_complete, "name flagged complete");
        check(i.has_flags && i.flags == 0x06, "flags byte");
        check(i.tx_power.has_value() && *i.tx_power == -60, "tx power -60");
    }

    // A complete name must win even when a shortened one came first.
    {
        const std::vector<uint8_t> d = {0x03, 0x08, 'A', 'B',
                                        0x05, 0x09, 'A', 'B', 'C', 'D'};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.name == "ABCD", "complete name overrides short name");
    }

    // Empty and null payloads.
    {
        const AdInfo a = parse_ad(nullptr, 0);
        check(a.name.empty() && !a.tx_power, "null payload is inert");
        const uint8_t none[1] = {0};
        const AdInfo b = parse_ad(none, 0);
        check(b.name.empty(), "zero-length payload is inert");
    }

    // A length that overruns the buffer must be dropped, keeping earlier fields.
    {
        const std::vector<uint8_t> d = {0x02, 0x01, 0x06,
                                        0x40, 0x09, 'X', 'Y'};  // claims 64 bytes
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.has_flags && i.flags == 0x06, "field before a bad length survives");
        check(i.name.empty(), "overrunning record is dropped, not read");
    }

    // Zero-length record terminates: it is padding, and the bytes after it are
    // not a record at all.
    {
        const std::vector<uint8_t> d = {0x00, 0x05, 0x09, 'N', 'O', 'P', 'E'};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.name.empty(), "zero length terminates the payload");
    }

    // A record whose length is exactly the remaining bytes is valid.
    {
        const std::vector<uint8_t> d = {0x03, 0x09, 'H', 'i'};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.name == "Hi", "record ending exactly at the buffer end");
    }

    // Escape sequences in a name must not reach the terminal.
    {
        const std::vector<uint8_t> d = {0x08, 0x09, 'a', 0x1b, '[', '2', 'J', 'b', 0x07};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.name.find('\x1b') == std::string::npos, "ESC stripped from name");
        check(i.name.find('\a') == std::string::npos, "BEL stripped from name");
        check(i.name.size() == 7, "sanitised name keeps its length");
    }

    // Manufacturer data and the iBeacon layout, whose last byte is the
    // calibrated power at 1 m.
    {
        std::vector<uint8_t> d = {0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15};
        for (int k = 0; k < 16; ++k) d.push_back(static_cast<uint8_t>(k));
        d.push_back(0x00); d.push_back(0x07);  // major
        d.push_back(0x00); d.push_back(0x2A);  // minor
        d.push_back(0xBF);                     // -65 dBm
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.company_id.has_value() && *i.company_id == 0x004C, "Apple company id");
        check(i.beacon_kind == "iBeacon", "iBeacon recognised");
        check(i.beacon_tx_1m.has_value() && *i.beacon_tx_1m == -65, "iBeacon 1 m power");
        check(guess_kind(i) == "iBeacon", "kind reports iBeacon");
    }

    // A truncated iBeacon must not be read past its end.
    {
        const std::vector<uint8_t> d = {0x06, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0x01};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.beacon_kind.empty(), "short iBeacon is not claimed");
        check(!i.beacon_tx_1m.has_value(), "short iBeacon yields no 1 m power");
    }

    // Eddystone service data: byte 3 is power at 0 m, 41 dB above the 1 m figure.
    {
        const std::vector<uint8_t> d = {0x07, 0x16, 0xAA, 0xFE, 0x00, 0xD4, 0x00, 0x01};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.beacon_kind == "Eddystone", "Eddystone recognised");
        check(i.beacon_tx_1m.has_value() && *i.beacon_tx_1m == -85,
              "Eddystone 0 m power converted to 1 m");
    }

    // 16-bit service UUID list, little-endian pairs.
    {
        const std::vector<uint8_t> d = {0x05, 0x03, 0x0D, 0x18, 0x0F, 0x18};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.service_uuids16.size() == 2, "two service UUIDs");
        check(i.service_uuids16[0] == 0x180D && i.service_uuids16[1] == 0x180F,
              "UUIDs decoded little-endian");
        check(guess_kind(i) == "heart rate", "kind from service UUID");
    }

    // An odd trailing byte in a UUID list is ignored rather than read.
    {
        const std::vector<uint8_t> d = {0x04, 0x03, 0x0D, 0x18, 0x0F};
        const AdInfo i = parse_ad(d.data(), d.size());
        check(i.service_uuids16.size() == 1, "dangling half UUID ignored");
    }
}

// --- HCI advertising report parsing ------------------------------------------
//
// These bytes come straight off the controller, so the parsers must survive
// every malformation — and the fixtures pin the field offsets, which is exactly
// where an off-by-one hides until the first real packet arrives.

void test_hci_reports() {
    section("hci report parsing");

    std::vector<AdvEvent> got;
    const EventSink sink = [&](const AdvEvent& ev) { got.push_back(ev); };

    // Legacy report: num(1) evt_type(1) addr_type(1) addr(6 LE) dlen(1)
    //                data[dlen] rssi(1)
    {
        got.clear();
        const std::vector<uint8_t> r = {
            0x01,                                  // one report
            0x00, 0x01,                            // ADV_IND, random address
            0x66, 0x55, 0x44, 0x33, 0x22, 0x11,    // addr, wire order
            0x03, 0x02, 0x01, 0x06,                // 3 bytes of AD (flags)
            0xC0,                                  // -64 dBm
        };
        parse_le_adv_report(r.data(), r.size(), sink);
        check(got.size() == 1, "legacy report parsed");
        if (got.size() == 1) {
            check(format_addr(got[0].addr) == "11:22:33:44:55:66",
                  "legacy address is byte-reversed off the wire");
            check(got[0].rssi == -64, "legacy RSSI field");
            check(got[0].data.size() == 3 && got[0].data[2] == 0x06,
                  "legacy AD payload copied");
            check(got[0].addr_type == AddrType::Random, "legacy address type");
        }

        // Any truncation must yield nothing rather than a misread.
        for (size_t cut = 1; cut < r.size(); ++cut) {
            got.clear();
            parse_le_adv_report(r.data(), cut, sink);
            check(got.empty(), "legacy report truncated at " + std::to_string(cut));
        }
    }

    // Two reports in one event; a data_len lying about the second must still
    // deliver the first.
    {
        got.clear();
        std::vector<uint8_t> r = {0x02};
        const std::vector<uint8_t> one = {0x00, 0x00, 0x01, 0x02, 0x03,
                                          0x04, 0x05, 0x06, 0x00, 0xBB};
        r.insert(r.end(), one.begin(), one.end());
        std::vector<uint8_t> two = one;
        two[8] = 0x30;  // claims 48 bytes of data that are not there
        r.insert(r.end(), two.begin(), two.end());
        parse_le_adv_report(r.data(), r.size(), sink);
        check(got.size() == 1, "first report survives a lying second one");
        if (!got.empty()) check(got[0].rssi == -69, "first report's RSSI intact");
    }

    // RSSI 127 means "not available" and must be dropped, not shown as strong.
    {
        got.clear();
        const std::vector<uint8_t> r = {0x01, 0x00, 0x00, 1, 2, 3,
                                        4,    5,    6,    0x00, 0x7F};
        parse_le_adv_report(r.data(), r.size(), sink);
        check(got.empty(), "RSSI 127 sentinel is dropped");
    }

    // Extended report: num(1) evt_type(2) addr_type(1) addr(6) primary_phy(1)
    // secondary_phy(1) sid(1) tx_power(1) rssi(1) periodic(2) direct_type(1)
    // direct_addr(6) dlen(1) data[dlen]
    //
    // tx_power and rssi are adjacent int8s; the fixture gives them different
    // values so a swapped offset cannot pass.
    {
        got.clear();
        const std::vector<uint8_t> r = {
            0x01,                                  // one report
            0x10, 0x00,                            // legacy ADV_IND equivalent
            0x00,                                  // public
            0xF6, 0xE5, 0xD4, 0xC3, 0xB2, 0xA1,    // addr, wire order
            0x01, 0x00, 0xFF,                      // 1M, no secondary, no SID
            0x09,                                  // tx_power +9 dBm
            0xC5,                                  // rssi -59 dBm
            0x00, 0x00,                            // no periodic interval
            0x00, 0, 0, 0, 0, 0, 0,                // no direct address
            0x02, 0xAA, 0xBB,                      // 2 bytes of data
        };
        parse_le_ext_adv_report(r.data(), r.size(), sink);
        check(got.size() == 1, "extended report parsed");
        if (got.size() == 1) {
            check(format_addr(got[0].addr) == "A1:B2:C3:D4:E5:F6",
                  "extended address is byte-reversed off the wire");
            check(got[0].rssi == -59, "extended RSSI is offset 13, not tx_power");
            check(got[0].data.size() == 2 && got[0].data[1] == 0xBB,
                  "extended AD payload copied");
        }

        for (size_t cut = 1; cut < r.size(); ++cut) {
            got.clear();
            parse_le_ext_adv_report(r.data(), cut, sink);
            check(got.empty(), "extended report truncated at " + std::to_string(cut));
        }
    }

    // Empty payloads are inert.
    {
        got.clear();
        parse_le_adv_report(nullptr, 0, sink);
        parse_le_ext_adv_report(nullptr, 0, sink);
        const uint8_t zero = 0x00;
        parse_le_adv_report(&zero, 1, sink);
        parse_le_ext_adv_report(&zero, 1, sink);
        check(got.empty(), "empty and zero-report payloads are inert");
    }
}

// --- history ring -----------------------------------------------------------

void test_history() {
    section("history ring");

    RssiHistory h;
    check(h.empty(), "starts empty");

    for (int i = 0; i < 10; ++i) h.push(static_cast<int8_t>(-50 - i), 1000 + i);
    check(h.size() == 10, "counts pushes");
    check(h.rssi_at(0) == -50, "oldest first");
    check(h.rssi_at(9) == -59, "newest last");
    check(h.time_at(0) == 1000, "timestamps track samples");

    // Overfill by half a lap: the oldest samples must be the ones that fell out.
    h.clear();
    check(h.empty() && h.size() == 0, "clear empties");
    const size_t total = kHistoryLen + kHistoryLen / 2;
    for (size_t i = 0; i < total; ++i) {
        h.push(static_cast<int8_t>(-100 + static_cast<int>(i % 60)), 1000 + i);
    }
    check(h.size() == kHistoryLen, "saturates at capacity");
    const size_t first_kept = total - kHistoryLen;
    check(h.rssi_at(0) == static_cast<int8_t>(-100 + static_cast<int>(first_kept % 60)),
          "after wrap, index 0 is the oldest retained sample");
    check(h.time_at(0) == 1000 + first_kept, "wrapped timestamps line up");
    check(h.rssi_at(kHistoryLen - 1) ==
              static_cast<int8_t>(-100 + static_cast<int>((total - 1) % 60)),
          "after wrap, last index is the newest sample");
    check(h.rssi_at(kHistoryLen) == -127, "out-of-range read is a sentinel");
}

// --- distance ---------------------------------------------------------------

void test_distance() {
    section("distance model");

    // At the reference power the model must return exactly 1 m.
    check_near(distance_estimate(-59, -59, 2.0), 1.0, 1e-9, "reference power is 1 m");

    // Each 6 dB of loss doubles the distance at n = 2.
    check_near(distance_estimate(-65, -59, 2.0), 2.0, 0.01, "-6 dB doubles distance");
    check_near(distance_estimate(-71, -59, 2.0), 4.0, 0.02, "-12 dB quadruples");

    // A higher exponent (more obstruction) shortens the inferred distance for
    // the same loss.
    check(distance_estimate(-71, -59, 3.0) < distance_estimate(-71, -59, 2.0),
          "greater path loss implies nearer for the same RSSI");

    // Clamped at both ends: BLE neither reaches a kilometre nor gets to 0 m.
    check(distance_estimate(-10, -59, 2.0) >= 0.05, "clamped near");
    check(distance_estimate(-127, -59, 2.0) <= 500.0, "clamped far");

    // A zero or negative exponent must not divide by zero.
    check(std::isfinite(distance_estimate(-70, -59, 0.0)), "zero exponent is handled");
}

// --- smoothing and trend ----------------------------------------------------

void feed(DeviceTable& t, const Addr& a, int8_t rssi, uint64_t at) {
    AdvEvent ev;
    ev.addr = a;
    ev.rssi = rssi;
    ev.mono_time = at;
    t.observe(ev, AdInfo{});
}

void test_smoothing() {
    section("smoothing and trend");

    const Addr a{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    DeviceTable t;

    // First sample primes both averages exactly.
    feed(t, a, -70, 1000);
    const Device* d = t.find(a);
    check(d != nullptr, "device recorded");
    check_near(d->smoothed(), -70.0, 1e-9, "first sample primes the average");
    check(d->trend() == Trend::Steady, "one sample is not a trend");

    // A steady signal must not drift, and must not read as movement.
    for (int i = 1; i <= 40; ++i) feed(t, a, -70, 1000 + i * 100);
    d = t.find(a);
    check_near(d->smoothed(), -70.0, 0.5, "steady signal stays put");
    check(d->trend() == Trend::Steady, "steady signal reads steady");

    // Now approach: the fast average must lead the slow one.
    uint64_t now = 5000;
    for (int i = 0; i < 25; ++i) {
        feed(t, a, static_cast<int8_t>(-70 + i), now);
        now += 100;
    }
    d = t.find(a);
    check(d->trend() == Trend::Warmer, "rising signal reads WARMER");
    check(d->trend_db() > 0.0, "trend_db positive while approaching");

    // And retreat.
    for (int i = 0; i < 25; ++i) {
        feed(t, a, static_cast<int8_t>(-46 - i), now);
        now += 100;
    }
    d = t.find(a);
    check(d->trend() == Trend::Colder, "falling signal reads COLDER");
    check(d->trend_db() < 0.0, "trend_db negative while retreating");

    check(d->best_rssi >= -46, "best RSSI is the strongest ever seen");
    check(d->packets == 1 + 40 + 25 + 25, "every advert counted");

    // Two devices advertising at very different rates must smooth alike in
    // wall-clock terms — that is the whole point of the time-aware alpha.
    {
        DeviceTable t2;
        const Addr fast{1, 0, 0, 0, 0, 1};
        const Addr slow{2, 0, 0, 0, 0, 2};
        for (int i = 0; i <= 40; ++i) feed(t2, fast, -80, i * 50);    // 20 Hz
        for (int i = 0; i <= 4; ++i) feed(t2, slow, -80, i * 500);    // 2 Hz
        for (int i = 1; i <= 20; ++i) feed(t2, fast, -50, 2000 + i * 50);
        for (int i = 1; i <= 2; ++i) feed(t2, slow, -50, 2000 + i * 500);
        // Both have now had 1 s of the new level; their fast averages should be
        // close despite a 10x difference in sample count.
        const double f = t2.find(fast)->smoothed();
        const double s = t2.find(slow)->smoothed();
        check_near(f, s, 4.0, "equal elapsed time gives equal smoothing");
    }
}

// --- table ------------------------------------------------------------------

void test_table() {
    section("device table");

    DeviceTable t;
    const Addr a{1, 1, 1, 1, 1, 1};
    const Addr b{2, 2, 2, 2, 2, 2};
    const Addr c{3, 3, 3, 3, 3, 3};

    feed(t, a, -80, 1000);
    feed(t, b, -40, 1000);
    feed(t, c, -60, 1000);
    check(t.size() == 3, "three devices tracked");

    // Re-observing must update, not duplicate.
    feed(t, a, -75, 1100);
    check(t.size() == 3, "same address does not duplicate");

    auto v = t.view(SortMode::Rssi, -110, false, "", 2000);
    check(v.size() == 3, "view returns all");
    check(v[0]->addr == b && v[2]->addr == a, "sorted strongest first");

    // Floor and name filters.
    check(t.view(SortMode::Rssi, -70, false, "", 2000).size() == 2, "RSSI floor applies");
    check(t.view(SortMode::Rssi, -110, true, "", 2000).empty(), "named-only hides unnamed");

    // Text filter matches the address when there is no name.
    check(t.view(SortMode::Rssi, -110, false, "02:02", 2000).size() == 1,
          "filter matches address");
    check(t.view(SortMode::Rssi, -110, false, "zz", 2000).empty(), "filter can match nothing");

    // Named lookup, case-insensitively.
    {
        AdvEvent ev;
        ev.addr = a;
        ev.rssi = -75;
        ev.mono_time = 1200;
        AdInfo info;
        info.name = "Keyring Tag";
        info.name_is_complete = true;
        t.observe(ev, info);
    }
    check(t.view(SortMode::Rssi, -110, false, "keyring", 2000).size() == 1,
          "filter matches name case-insensitively");
    // A later nameless advert must not erase the name we learned.
    feed(t, a, -76, 1300);
    check(t.find(a)->name == "Keyring Tag", "a nameless advert does not clear the name");

    // Pruning is by age, and leaves fresher devices alone.
    feed(t, b, -40, 90000);
    check(t.prune(100000, 30000) == 2, "two stale devices pruned");
    check(t.size() == 1 && t.find(b) != nullptr, "the fresh device survived");

    t.clear();
    check(t.size() == 0, "clear empties the table");
}

// --- presentation -----------------------------------------------------------

void test_format() {
    section("formatting");

    check(format_addr({0xC4, 0x8E, 0x8F, 0x12, 0x34, 0x56}) == "C4:8E:8F:12:34:56",
          "address formatting");

    check_near(rssi_fraction(-100), 0.0, 1e-9, "floor maps to 0");
    check_near(rssi_fraction(-35), 1.0, 1e-9, "ceiling maps to 1");
    check(rssi_fraction(-120) == 0.0 && rssi_fraction(0) == 1.0, "fraction clamps");
    check(rssi_fraction(-60) > rssi_fraction(-80), "stronger means a larger fraction");

    // Bars: measured in display columns, not bytes, since the glyphs are UTF-8.
    check(display_width(bar(0.0, 10, false)) == 10, "empty bar is full width");
    check(display_width(bar(1.0, 10, false)) == 10, "full bar is full width");
    check(display_width(bar(0.5, 10, false)) == 10, "half bar is full width");
    check(bar(0.0, 0, false).empty(), "zero-width bar is empty");
    check(bar(2.0, 8, false) == bar(1.0, 8, false), "fraction over 1 clamps");

    // Sparkline width holds for empty, short, and overfull histories.
    {
        RssiHistory h;
        check(display_width(sparkline(h, 20)) == 20, "empty sparkline still fills");
        for (int i = 0; i < 5; ++i) h.push(static_cast<int8_t>(-60 - i), 1000 + i);
        check(display_width(sparkline(h, 20)) == 20, "short history is padded");
        for (int i = 0; i < 300; ++i) h.push(static_cast<int8_t>(-90 + i % 40), 2000 + i);
        check(display_width(sparkline(h, 20)) == 20, "long history is trimmed");
        check(display_width(sparkline(h, 1)) == 1, "single-column sparkline");
    }

    // A flat trace must not be rendered as noise.
    {
        RssiHistory h;
        for (int i = 0; i < 20; ++i) h.push(-70, 1000 + i);
        const std::string s = sparkline(h, 10);
        bool uniform = true;
        for (size_t i = 3; i + 3 < s.size(); i += 3) {
            if (s.substr(i, 3) != s.substr(0, 3)) uniform = false;
        }
        check(uniform, "a constant signal draws a flat line");
    }

    check(format_distance(1.24).find("1.2") != std::string::npos, "metres to one decimal");
    check(format_distance(14.0) == "~14m", "double digits drop the decimal");
    check(format_age(400) == "0.4s", "sub-second age");
    check(format_age(12000) == "12s", "seconds");
    check(format_age(120000) == "2m", "minutes");
    check(format_age(7200000) == "2h", "hours");

    // Bell pacing: closer must always mean faster, never slower.
    check(bell_interval_ms(-40) < bell_interval_ms(-90), "close beeps faster than far");
    check(bell_interval_ms(-40) >= 80, "bell interval stays audible-sane");
    check(bell_interval_ms(-120) <= 1300, "bell interval stays bounded");
}

void test_text_width() {
    section("text width");

    check(display_width("hello") == 5, "ascii width");
    check(display_width("") == 0, "empty width");
    check(display_width("héllo") == 5, "2-byte UTF-8 counts as one column");
    check(display_width("日本") == 4, "CJK counts double");
    check(display_width("→") == 1, "3-byte arrow is one column");

    // Invalid UTF-8 must terminate rather than run off the end.
    check(display_width(std::string("\xFF\xFE")) == 2, "invalid bytes count as one each");
    check(display_width(std::string("a\xC3")) == 2, "truncated sequence terminates");

    // Colour is applied while composing, so measurement runs over strings that
    // already carry escapes. Counting those bytes would make every coloured
    // element measure far wider than it draws, and truncate itself to nothing.
    check(display_width("\x1b[38;5;46mabc\x1b[0m") == 3, "SGR sequences are zero width");
    check(display_width("\x1b[1m\x1b[2J\x1b[Habc") == 3, "several CSIs are zero width");
    check(display_width("\x1b[38;5;46m") == 0, "a bare escape has no width");
    check(display_width(bar(0.5, 20, true)) == 20, "a coloured bar measures its draw width");
    check(display_width(truncate_to(bar(0.5, 40, true), 20)) == 20,
          "a coloured bar truncates by draw width");
    check(truncate_to(bar(0.5, 40, true), 20).find("\x1b[0m") != std::string::npos,
          "truncating a coloured string still resets the colour");
    check(display_width(fit("\x1b[31mhi\x1b[0m", 6)) == 6, "fit pads coloured text correctly");

    check(fit("abc", 5) == "abc  ", "fit pads");
    check(display_width(fit("abcdefgh", 5)) == 5, "fit truncates to width");
    check(display_width(fit("日本語です", 5)) == 5, "fit truncates wide text to width");
    check(display_width(truncate_to("abcdefgh", 4)) == 4, "truncate respects width");
    check(truncate_to("abc", 10) == "abc", "short text is untouched");
    check(truncate_to("abc", 0).empty(), "zero width truncates to nothing");
    check(display_width(fit("", 4)) == 4, "empty string pads to width");
}

}  // namespace

int run_selftest() {
    std::printf("btfind self-test\n\n");
    test_ad_parse();
    test_hci_reports();
    test_history();
    test_distance();
    test_smoothing();
    test_table();
    test_format();
    test_text_width();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace btfind
