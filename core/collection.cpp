#include "collection.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "rng.h"
#include "sqlite3.h"

using json = nlohmann::json;

namespace anki {

// ---------------------------------------------------------------- sqlite helpers

bool Db::exec(const std::string& sql, std::string* err) {
    char* msg = nullptr;
    int rc = sqlite3_exec(h, sql.c_str(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        if (err) *err = msg ? msg : "sqlite error";
        sqlite3_free(msg);
        return false;
    }
    return true;
}

Stmt::Stmt(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr;
}
Stmt::~Stmt() {
    if (s_) sqlite3_finalize(s_);
}
Stmt& Stmt::bind(int i, int64_t v) { sqlite3_bind_int64(s_, i, v); return *this; }
Stmt& Stmt::bind(int i, const std::string& v) {
    sqlite3_bind_text(s_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
    return *this;
}
Stmt& Stmt::bind_blob(int i, const std::string& v) {
    sqlite3_bind_blob(s_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
    return *this;
}
Stmt& Stmt::bind_null(int i) { sqlite3_bind_null(s_, i); return *this; }
bool Stmt::step() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
void Stmt::reset() { if (s_) { sqlite3_reset(s_); sqlite3_clear_bindings(s_); } }
int64_t Stmt::i64(int c) const { return sqlite3_column_int64(s_, c); }
double Stmt::dbl(int c) const { return sqlite3_column_double(s_, c); }
std::string Stmt::text(int c) const {
    const unsigned char* t = sqlite3_column_text(s_, c);
    int n = sqlite3_column_bytes(s_, c);
    return t ? std::string((const char*)t, n) : std::string();
}
std::string Stmt::blob(int c) const {
    const void* b = sqlite3_column_blob(s_, c);
    int n = sqlite3_column_bytes(s_, c);
    return b ? std::string((const char*)b, n) : std::string();
}
bool Stmt::is_null(int c) const { return sqlite3_column_type(s_, c) == SQLITE_NULL; }

// Anki registers a "unicase" collation (case-insensitive Unicode compare) and
// uses it in indexes on decks, notetypes and tags. We approximate with simple
// case folding over decoded code points, which matches for Latin, Greek and
// Cyrillic names.
static uint32_t next_cp(const unsigned char*& p, const unsigned char* end) {
    uint32_t c = *p++;
    if (c < 0x80) return c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    c &= (0x3F >> extra);
    while (extra-- > 0 && p < end) c = (c << 6) | (*p++ & 0x3F);
    return c;
}
static uint32_t fold(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x391 && c <= 0x3A9) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
static int unicase_cmp(void*, int la, const void* a, int lb, const void* b) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* ea = pa + la;
    const unsigned char* pb = (const unsigned char*)b;
    const unsigned char* eb = pb + lb;
    while (pa < ea && pb < eb) {
        uint32_t ca = fold(next_cp(pa, ea)), cb = fold(next_cp(pb, eb));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (pa < ea) return 1;
    if (pb < eb) return -1;
    return 0;
}

bool ensure_rollback_journal(const std::string& path) {
    FILE* wal = fopen((path + "-wal").c_str(), "rb");
    if (wal) {
        fseek(wal, 0, SEEK_END);
        long n = ftell(wal);
        fclose(wal);
        if (n > 0) return false;
        remove((path + "-wal").c_str());
        remove((path + "-shm").c_str());
    }
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f) return true;
    unsigned char hdr[20];
    if (fread(hdr, 1, 20, f) == 20 && memcmp(hdr, "SQLite format 3", 15) == 0 && (hdr[18] == 2 || hdr[19] == 2)) {
        unsigned char one[2] = {1, 1};
        fseek(f, 18, SEEK_SET);
        fwrite(one, 1, 2, f);
    }
    fclose(f);
    return true;
}

void register_unicase(sqlite3* db) {
    sqlite3_create_collation_v2(db, "unicase", SQLITE_UTF8, nullptr, unicase_cmp, nullptr);
}

// ---------------------------------------------------------------- clock

Clock system_clock() {
    Clock c;
    c.now_millis = [] {
        return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    c.local_minutes_west = [] {
        time_t t = time(nullptr);
        struct tm lt;
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
#if defined(__3DS__)
        return (int32_t)0;
#else
        return (int32_t)(-lt.tm_gmtoff / 60);
#endif
    };
    return c;
}

// ---------------------------------------------------------------- card data JSON

std::string format_f32_like_serde(float v) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    std::string s(buf, r.ptr);
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}

static float round_to_places(float v, int places) {
    float factor = 1.0f;
    for (int i = 0; i < places; i++) factor *= 10.0f;
    return std::round(v * factor) / factor;
}

std::string Card::data_json() const {
    std::string out = "{";
    bool first = true;
    auto key = [&](const char* k) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        out += k;
        out += "\":";
    };
    if (original_position) { key("pos"); out += std::to_string(*original_position); }
    if (memory_state) {
        key("s"); out += format_f32_like_serde(round_to_places(memory_state->stability, 4));
        key("d"); out += format_f32_like_serde(round_to_places(memory_state->difficulty, 3));
    }
    if (desired_retention) { key("dr"); out += format_f32_like_serde(round_to_places(*desired_retention, 2)); }
    if (decay) { key("decay"); out += format_f32_like_serde(round_to_places(*decay, 3)); }
    if (last_review_time) { key("lrt"); out += std::to_string(*last_review_time); }
    if (!custom_data.empty() && custom_data != "{}") { key("cd"); out += json(custom_data).dump(); }
    out += "}";
    return out;
}

void Card::parse_data(const std::string& s) {
    original_position.reset();
    memory_state.reset();
    desired_retention.reset();
    decay.reset();
    last_review_time.reset();
    custom_data.clear();
    if (s.empty()) return;
    json j = json::parse(s, nullptr, false);
    if (!j.is_object()) return;
    auto num = [&](const char* k) -> std::optional<double> {
        auto it = j.find(k);
        if (it != j.end() && it->is_number()) return it->get<double>();
        return std::nullopt;
    };
    if (auto v = num("pos"); v && *v >= 0) original_position = (uint32_t)*v;
    auto st = num("s"), di = num("d");
    if (st && di) memory_state = MemoryState{(float)*st, (float)*di};
    if (auto v = num("dr")) desired_retention = (float)*v;
    if (auto v = num("decay")) decay = (float)*v;
    if (auto v = num("lrt")) last_review_time = (int64_t)*v;
    auto it = j.find("cd");
    if (it != j.end() && it->is_string()) custom_data = it->get<std::string>();
}

// ---------------------------------------------------------------- deck / config decode

void DeckConfig::decode() {
    const pb::Message& m = raw;
    learn_steps = m.packed_f32(1);
    relearn_steps = m.packed_f32(2);
    auto p6 = m.packed_f32(6), p5 = m.packed_f32(5), p4 = m.packed_f32(3);
    fsrs_params = !p6.empty() ? p6 : !p5.empty() ? p5 : p4;
    new_per_day = (uint32_t)m.u64(9);
    reviews_per_day = (uint32_t)m.u64(10);
    initial_ease = m.f32(11);
    easy_multiplier = m.f32(12);
    hard_multiplier = m.f32(13);
    lapse_multiplier = m.f32(14);
    interval_multiplier = m.f32(15);
    maximum_review_interval = (uint32_t)m.u64(16);
    minimum_lapse_interval = (uint32_t)m.u64(17);
    graduating_interval_good = (uint32_t)m.u64(18);
    graduating_interval_easy = (uint32_t)m.u64(19);
    leech_action = (int)m.u64(21);
    leech_threshold = (uint32_t)m.u64(22);
    cap_answer_time_to_secs = (uint32_t)m.u64(24);
    bury_new = m.boolean(27);
    bury_reviews = m.boolean(28);
    bury_interday_learning = m.boolean(29);
    new_mix = (int)m.u64(30);
    interday_learning_mix = (int)m.u64(31);
    new_card_sort_order = (int)m.u64(32);
    review_order = (int)m.u64(33);
    new_card_gather_priority = (int)m.u64(34);
    desired_retention = m.f32(37);
    easy_days_percentages = m.packed_f32(4);
}

void Deck::decode() {
    pb::Message normal = kind.sub(1);
    filtered = kind.has(2);
    if (!filtered) {
        config_id = normal.i64(1);
        if (normal.has(6)) review_limit = (uint32_t)normal.u64(6); else review_limit.reset();
        if (normal.has(7)) new_limit = (uint32_t)normal.u64(7); else new_limit.reset();
        has_review_limit_today = normal.has(8);
        if (has_review_limit_today) {
            pb::Message d = normal.sub(8);
            review_limit_today = (uint32_t)d.u64(1);
            review_limit_today_day = (uint32_t)d.u64(2);
        }
        has_new_limit_today = normal.has(9);
        if (has_new_limit_today) {
            pb::Message d = normal.sub(9);
            new_limit_today = (uint32_t)d.u64(1);
            new_limit_today_day = (uint32_t)d.u64(2);
        }
        if (normal.has(10)) desired_retention = normal.f32(10); else desired_retention.reset();
    } else {
        pb::Message f = kind.sub(2);
        reschedule = f.boolean(1);
        preview_again_secs = (uint32_t)f.u64(7);
        preview_hard_secs = (uint32_t)f.u64(5);
        preview_good_secs = (uint32_t)f.u64(6);
    }
}

std::string Deck::human_name() const {
    std::string s = name;
    std::string out;
    for (char c : s) {
        if (c == '\x1f') out += "::";
        else out += c;
    }
    return out;
}

// ---------------------------------------------------------------- Collection

Collection::~Collection() { close(); }

bool Collection::open(const std::string& path, std::string& err) {
    close();
    if (!ensure_rollback_journal(path)) {
        err = "this collection has unsaved changes in a -wal file; close desktop Anki before copying it";
        return false;
    }
    if (sqlite3_open_v2(path.c_str(), &db_.h, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        err = db_.h ? sqlite3_errmsg(db_.h) : "could not open collection";
        close();
        return false;
    }
    path_ = path;
    sqlite3_create_collation_v2(db_.h, "unicase", SQLITE_UTF8, nullptr, unicase_cmp, nullptr);
    db_.exec("pragma cache_size = -8000");
    db_.exec("pragma temp_store = memory");
    // Anki uses WAL on desktop; a single-file journal is safer on an SD card.
    db_.exec("pragma journal_mode = delete");
    {
        Stmt s(db_.h, "select ver from col");
        if (!s.step()) {
            err = "not an Anki collection";
            close();
            return false;
        }
        ver = (int)s.i64(0);
    }
    if (ver < 18) {
        err = "collection schema " + std::to_string(ver) + " is too old; open it in Anki 2.1.50+ first";
        close();
        return false;
    }
    load_metadata();
    return true;
}

void Collection::close() {
    if (db_.h) {
        sqlite3_close_v2(db_.h);
        db_.h = nullptr;
    }
}

void Collection::load_metadata() {
    {
        Stmt s(db_.h, "select crt, mod, scm, ver, usn, ls from col");
        if (s.step()) {
            crt = s.i64(0);
            mod = s.i64(1);
            scm = s.i64(2);
            ver = (int)s.i64(3);
            usn = (int)s.i64(4);
            ls = s.i64(5);
        }
    }
    decks.clear();
    {
        Stmt s(db_.h, "select id, name, mtime_secs, usn, common, kind from decks");
        while (s.step()) {
            Deck d;
            d.id = s.i64(0);
            d.name = s.text(1);
            d.mtime = s.i64(2);
            d.usn = (int)s.i64(3);
            d.common = pb::Message(s.blob(4));
            d.kind = pb::Message(s.blob(5));
            d.decode();
            decks[d.id] = std::move(d);
        }
    }
    deck_configs.clear();
    {
        Stmt s(db_.h, "select id, name, mtime_secs, usn, config from deck_config");
        while (s.step()) {
            DeckConfig c;
            c.id = s.i64(0);
            c.name = s.text(1);
            c.mtime = s.i64(2);
            c.usn = (int)s.i64(3);
            c.raw = pb::Message(s.blob(4));
            c.decode();
            deck_configs[c.id] = std::move(c);
        }
    }
    notetypes.clear();
    {
        Stmt s(db_.h, "select id, name, mtime_secs, usn, config from notetypes");
        while (s.step()) {
            Notetype n;
            n.id = s.i64(0);
            n.name = s.text(1);
            n.mtime = s.i64(2);
            n.usn = (int)s.i64(3);
            n.config = pb::Message(s.blob(4));
            n.is_cloze = n.config.u64(1) == 1;
            n.sort_field_idx = (uint32_t)n.config.u64(2);
            n.css = n.config.str(3);
            notetypes[n.id] = std::move(n);
        }
    }
    {
        Stmt s(db_.h, "select ntid, ord, name from fields order by ntid, ord");
        while (s.step()) {
            auto it = notetypes.find(s.i64(0));
            if (it == notetypes.end()) continue;
            it->second.fields.push_back({s.text(2), (int)s.i64(1)});
        }
    }
    {
        Stmt s(db_.h, "select ntid, ord, name, config from templates order by ntid, ord");
        while (s.step()) {
            auto it = notetypes.find(s.i64(0));
            if (it == notetypes.end()) continue;
            pb::Message cfg(s.blob(3));
            it->second.templates.push_back({s.text(2), (int)s.i64(1), cfg.str(1), cfg.str(2)});
        }
    }
    // Anki's defaults, used when a deck points at a missing preset.
    default_config_ = DeckConfig();
    default_config_.learn_steps = {1.0f, 10.0f};
    default_config_.relearn_steps = {10.0f};
    default_config_.desired_retention = 0.9f;
    default_config_.cap_answer_time_to_secs = 60;
    default_config_.leech_threshold = 8;
}

std::optional<std::string> Collection::config_json(const std::string& key) {
    Stmt s(db_.h, "select val from config where key = ?");
    s.bind(1, key);
    if (s.step()) return s.blob(0);
    return std::nullopt;
}

bool Collection::config_bool(const std::string& key, bool def) {
    auto v = config_json(key);
    if (!v) return def;
    json j = json::parse(*v, nullptr, false);
    return j.is_boolean() ? j.get<bool>() : def;
}

int64_t Collection::config_int(const std::string& key, int64_t def) {
    auto v = config_json(key);
    if (!v) return def;
    json j = json::parse(*v, nullptr, false);
    return j.is_number() ? j.get<int64_t>() : def;
}

void Collection::set_config_json(const std::string& key, const std::string& val, int usn_value) {
    Stmt s(db_.h, "insert or replace into config (key, usn, mtime_secs, val) values (?, ?, ?, ?)");
    s.bind(1, key).bind(2, (int64_t)usn_value).bind(3, now_secs()).bind_blob(4, val);
    s.step();
}

// rslib/src/scheduler/timing.rs
SchedTimingToday Collection::timing_for(int64_t now, int32_t minutes_west) {
    int64_t rollover = std::min<int64_t>(config_int("rollover", 4), 23);
    auto creation = config_json("creationOffset");
    auto clamp_west = [](int32_t m) { return std::clamp<int32_t>(m, -23 * 60, 23 * 60); };
    int64_t cur_west = clamp_west(minutes_west);
    auto floor_div = [](int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    SchedTimingToday t;
    t.now = now;
    if (creation) {
        int64_t crt_west = clamp_west((int32_t)config_int("creationOffset", 0));
        int64_t created_day = floor_div(crt - crt_west * 60, 86400);
        int64_t now_local = now - cur_west * 60;
        int64_t now_day = floor_div(now_local, 86400);
        int64_t rollover_today = now_day * 86400 + rollover * 3600 + cur_west * 60;
        bool passed = rollover_today <= now;
        t.next_day_at = passed ? rollover_today + 86400 : rollover_today;
        int64_t days = now_day - created_day;
        if (!passed) days -= 1;
        t.days_elapsed = (uint32_t)std::max<int64_t>(days, 0);
    } else {
        // sched_timing_today_v2_legacy
        int64_t crt_local_day = floor_div(crt - cur_west * 60, 86400);
        int64_t crt_at_rollover = crt_local_day * 86400 + rollover * 3600 + cur_west * 60;
        int64_t days = (now - crt_at_rollover) / 86400;
        int64_t now_day = floor_div(now - cur_west * 60, 86400);
        int64_t next = now_day * 86400 + rollover * 3600 + cur_west * 60;
        if (next < now) next += 86400;
        t.days_elapsed = (uint32_t)days;
        t.next_day_at = next;
    }
    return t;
}

SchedTimingToday Collection::timing_today() { return timing_for(now_secs(), clock.local_minutes_west()); }

static const char* CARD_COLS =
    "id, nid, did, ord, mod, usn, type, queue, due, ivl, factor, reps, lapses, left, odue, odid, flags, data";

static Card card_from_row(const Stmt& s) {
    Card c;
    c.id = s.i64(0);
    c.nid = s.i64(1);
    c.did = s.i64(2);
    c.ord = (int)s.i64(3);
    c.mtime = s.i64(4);
    c.usn = (int)s.i64(5);
    c.ctype = (int)s.i64(6);
    c.queue = (int)s.i64(7);
    c.due = (int32_t)s.i64(8);
    c.ivl = (uint32_t)s.i64(9);
    c.factor = (uint16_t)s.i64(10);
    c.reps = (uint32_t)s.i64(11);
    c.lapses = (uint32_t)s.i64(12);
    c.left = (uint32_t)s.i64(13);
    c.odue = (int32_t)s.i64(14);
    c.odid = s.i64(15);
    c.flags = (int)s.i64(16);
    c.parse_data(s.text(17));
    return c;
}

std::optional<Card> Collection::get_card(int64_t id) {
    std::string sql = std::string("select ") + CARD_COLS + " from cards where id = ?";
    Stmt s(db_.h, sql.c_str());
    s.bind(1, id);
    if (s.step()) return card_from_row(s);
    return std::nullopt;
}

void Collection::update_card(const Card& c) {
    Stmt s(db_.h,
           "update cards set nid=?, did=?, ord=?, mod=?, usn=?, type=?, queue=?, due=?, ivl=?, factor=?, "
           "reps=?, lapses=?, left=?, odue=?, odid=?, flags=?, data=? where id=?");
    s.bind(1, c.nid).bind(2, c.did).bind(3, (int64_t)c.ord).bind(4, c.mtime).bind(5, (int64_t)c.usn)
        .bind(6, (int64_t)c.ctype).bind(7, (int64_t)c.queue).bind(8, (int64_t)c.due).bind(9, (int64_t)c.ivl)
        .bind(10, (int64_t)c.factor).bind(11, (int64_t)c.reps).bind(12, (int64_t)c.lapses)
        .bind(13, (int64_t)c.left).bind(14, (int64_t)c.odue).bind(15, c.odid).bind(16, (int64_t)c.flags)
        .bind(17, c.data_json()).bind(18, c.id);
    s.step();
}

std::optional<Note> Collection::get_note(int64_t id) {
    Stmt s(db_.h, "select id, guid, mid, mod, usn, tags, flds, sfld, csum, flags, data from notes where id = ?");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    Note n;
    n.id = s.i64(0);
    n.guid = s.text(1);
    n.mid = s.i64(2);
    n.mtime = s.i64(3);
    n.usn = (int)s.i64(4);
    n.tags = s.text(5);
    std::string flds = s.text(6);
    size_t start = 0;
    while (true) {
        size_t p = flds.find('\x1f', start);
        n.fields.push_back(flds.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    n.sfld = s.text(7);
    n.csum = s.i64(8);
    n.flags = (int)s.i64(9);
    n.data = s.text(10);
    return n;
}

void Collection::update_note(const Note& n) {
    std::string flds;
    for (size_t i = 0; i < n.fields.size(); i++) {
        if (i) flds += '\x1f';
        flds += n.fields[i];
    }
    Stmt s(db_.h,
           "insert or replace into notes (id, guid, mid, mod, usn, tags, flds, sfld, csum, flags, data) "
           "values (?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, n.id).bind(2, n.guid).bind(3, n.mid).bind(4, n.mtime).bind(5, (int64_t)n.usn).bind(6, n.tags)
        .bind(7, flds).bind(8, n.sfld).bind(9, n.csum).bind(10, (int64_t)n.flags).bind(11, n.data);
    s.step();
}

void Collection::add_revlog(const RevlogEntry& e, bool uniquify) {
    Stmt s(db_.h,
           "insert or ignore into revlog (id, cid, usn, ease, ivl, lastIvl, factor, time, type) values ("
           "(case when ?1 and ?2 in (select id from revlog) then (select max(id) + 1 from revlog) else ?2 end),"
           "?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
    s.bind(1, (int64_t)(uniquify ? 1 : 0)).bind(2, e.id).bind(3, e.cid).bind(4, (int64_t)e.usn)
        .bind(5, (int64_t)e.ease).bind(6, (int64_t)e.ivl).bind(7, (int64_t)e.last_ivl)
        .bind(8, (int64_t)e.factor).bind(9, (int64_t)e.time).bind(10, (int64_t)e.type);
    s.step();
}

std::optional<int64_t> Collection::time_of_last_review(int64_t cid) {
    Stmt s(db_.h,
           "select id / 1000 from revlog where cid = ? and ease between 1 and 4 and (type != 3 or factor != 0) "
           "order by id desc limit 1");
    s.bind(1, cid);
    if (s.step()) return s.i64(0);
    return std::nullopt;
}

const Deck* Collection::deck(int64_t id) const {
    auto it = decks.find(id);
    return it == decks.end() ? nullptr : &it->second;
}

const DeckConfig& Collection::config_for_deck(const Deck& d) {
    auto it = deck_configs.find(d.config_id);
    if (it != deck_configs.end()) return it->second;
    return default_config_;
}

std::vector<int64_t> Collection::child_deck_ids(int64_t id) const {
    std::vector<int64_t> out;
    const Deck* root = deck(id);
    if (!root) return out;
    std::string prefix = root->name + "\x1f";
    for (const auto& [did, d] : decks)
        if (did == id || d.name.compare(0, prefix.size(), prefix) == 0) out.push_back(did);
    return out;
}

std::vector<int64_t> Collection::parent_deck_ids(int64_t id) const {
    std::vector<int64_t> out;
    const Deck* d = deck(id);
    if (!d) return out;
    std::string name = d->name;
    while (true) {
        size_t p = name.rfind('\x1f');
        if (p == std::string::npos) break;
        name = name.substr(0, p);
        for (const auto& [pid, pd] : decks)
            if (pd.name == name) out.push_back(pid);
    }
    return out;
}

void Collection::begin() { db_.exec("begin"); }
void Collection::commit() { db_.exec("commit"); }
void Collection::rollback() { db_.exec("rollback"); }

void Collection::set_modified() {
    mod = clock.now_millis();
    Stmt s(db_.h, "update col set mod = ?");
    s.bind(1, mod);
    s.step();
}

void Collection::unbury_if_day_rolled_over(const SchedTimingToday& t) {
    int64_t last = config_int("lastUnburied", 0);
    uint32_t today = t.days_elapsed;
    if (last < (int64_t)today || (int64_t)today + 7 < last) {
        // restore_queue_from_type for buried cards; not marked as modified
        Stmt s(db_.h,
               "update cards set queue = (case type when 0 then 0 when 2 then 2 else "
               "(case when due > 1000000000 then 1 else 3 end) end) where queue in (-2, -3)");
        s.step();
        set_config_json("lastUnburied", std::to_string(today), 0);
    }
}

// ---------------------------------------------------------------- fuzz helpers

std::optional<uint64_t> fuzz_seed_for(int64_t card_id, uint32_t reps) {
    return (uint64_t)card_id + (uint64_t)reps;
}

std::optional<float> fuzz_factor_for(std::optional<uint64_t> seed) {
    if (!seed) return std::nullopt;
    return StdRng::seed_from_u64(*seed).range_f32(0.0f, 1.0f);
}

// ---------------------------------------------------------------- load balancer (load_balancer.rs)

static const size_t MAX_LOAD_BALANCE_INTERVAL = 90;
static const size_t LOAD_BALANCE_DAYS = (size_t)(MAX_LOAD_BALANCE_INTERVAL * 1.1f);  // 99
static const int SIBLING_MODIFIER_STEPS[11] = {-5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5};
static const float SIBLING_MODIFIER_RANGE[11] = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f, 0.000001f,
                                                 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};

enum class EasyDay { Minimum, Reduced, Normal };
static float load_modifier(EasyDay d) {
    switch (d) {
        case EasyDay::Minimum: return 0.0001f;
        case EasyDay::Reduced: return 0.5f;
        default: return 1.0f;
    }
}

struct LbDay {
    std::vector<std::pair<int64_t, int64_t>> cards;  // (cid, nid)
    std::unordered_set<int64_t> notes;
    void add(int64_t cid, int64_t nid) {
        cards.emplace_back(cid, nid);
        notes.insert(nid);
    }
};

class LoadBalancer {
public:
    std::unordered_map<int64_t, std::vector<LbDay>> days_by_preset;
    std::unordered_map<int64_t, std::array<EasyDay, 7>> easy_days;
    int64_t next_day_at = 0;
    int32_t minutes_west = 0;

    std::optional<uint32_t> find_interval(float interval, uint32_t minimum, uint32_t maximum, int64_t dcid,
                                          std::optional<uint64_t> fuzz_seed, std::optional<int64_t> note_id) const {
        if ((size_t)f2u(interval) > MAX_LOAD_BALANCE_INTERVAL || minimum > MAX_LOAD_BALANCE_INTERVAL)
            return std::nullopt;
        auto [before, after] = constrained_fuzz_bounds(interval, minimum, maximum);
        auto it = days_by_preset.find(dcid);
        if (it == days_by_preset.end()) return std::nullopt;
        const auto& days = it->second;
        if (after >= days.size()) return std::nullopt;  // Rust would panic on the slice
        std::vector<size_t> review_counts;
        std::vector<size_t> weekdays;
        for (uint32_t i = before; i <= after; i++) {
            review_counts.push_back(days[i].cards.size());
            weekdays.push_back(interval_to_weekday(i));
        }
        auto ed = easy_days.find(dcid);
        if (ed == easy_days.end()) return std::nullopt;
        std::vector<float> easy_mod = easy_days_modifiers(ed->second, weekdays, review_counts);
        std::vector<float> sibling_mod = sibling_modifiers(before, after, note_id);
        if (!fuzz_seed) return std::nullopt;
        std::vector<float> weights;
        std::vector<uint32_t> targets;
        for (size_t i = 0; i < review_counts.size(); i++) {
            uint32_t target = (uint32_t)i + before;
            float w;
            if (review_counts[i] == 0) {
                w = 1.0f;
            } else {
                float card_count_weight = std::pow(1.0f / (float)review_counts[i], 2.15f);
                float ti = 1.0f / (float)target;
                float card_interval_weight = ti * ti * ti;  // powi(3)
                w = card_count_weight * card_interval_weight * sibling_mod[i] * easy_mod[i];
            }
            weights.push_back(w);
            targets.push_back(target);
        }
        StdRng rng = StdRng::seed_from_u64(*fuzz_seed);
        int idx = weighted_index_sample(weights, rng);
        if (idx < 0) return std::nullopt;
        return targets[idx];
    }

    size_t interval_to_weekday(uint32_t interval) const {
        int64_t ts = next_day_at + ((int64_t)interval - 1) * 86400;
        int64_t local = ts - (int64_t)minutes_west * 60;
        int64_t day = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
        int64_t wd = (day + 3) % 7;  // 1970-01-01 was a Thursday (Mon=0)
        if (wd < 0) wd += 7;
        return (size_t)wd;
    }

    static std::vector<float> easy_days_modifiers(const std::array<EasyDay, 7>& load, const std::vector<size_t>& weekdays,
                                                  const std::vector<size_t>& counts) {
        size_t total = 0;
        for (auto c : counts) total += c;
        float total_percents = 0.0f;
        for (auto w : weekdays) total_percents += load_modifier(load[w]);
        std::vector<float> out;
        for (size_t i = 0; i < weekdays.size(); i++) {
            EasyDay d = load[weekdays[i]];
            if (d == EasyDay::Reduced) {
                const float HALF = 0.5f;
                float other_total = (float)(total - counts[i]);
                float other_percent = total_percents - HALF;
                float normalized = (float)counts[i] / HALF;
                float threshold = other_total / other_percent;
                d = normalized > threshold ? EasyDay::Minimum : EasyDay::Normal;
            }
            out.push_back(load_modifier(d));
        }
        return out;
    }

    std::vector<float> sibling_modifiers(uint32_t before, uint32_t after, std::optional<int64_t> nid) const {
        std::vector<float> mods(after - before + 1, 1.0f);
        if (!nid) return mods;
        std::set<size_t> sibling_days;
        for (const auto& [_, days] : days_by_preset)
            for (size_t i = 0; i < days.size(); i++)
                if (days[i].notes.count(*nid)) sibling_days.insert(i);
        for (size_t sd : sibling_days) {
            for (int k = 0; k < 11; k++) {
                long target = (long)sd + SIBLING_MODIFIER_STEPS[k] - (long)before;
                if (target >= 0 && (size_t)target < mods.size()) mods[(size_t)target] *= SIBLING_MODIFIER_RANGE[k];
            }
        }
        return mods;
    }

    void add_card(int64_t cid, int64_t nid, int64_t dcid, uint32_t interval) {
        auto it = days_by_preset.find(dcid);
        if (it == days_by_preset.end()) return;
        if (interval < it->second.size()) it->second[interval].add(cid, nid);
    }
};

void Scheduler::start_session(bool with_load_balancer) {
    fsrs_ = col_.config_bool("fsrs", false);
    short_term_with_steps_ = col_.config_bool("fsrsShortTermWithStepsEnabled", false);
    lb_.reset();
    if (!with_load_balancer || !col_.config_bool("loadBalancerEnabled", true)) return;
    auto t = col_.timing_today();
    auto lb = std::make_unique<LoadBalancer>();
    lb->next_day_at = t.next_day_at;
    lb->minutes_west = col_.clock.local_minutes_west();
    std::unordered_map<int64_t, int64_t> did_to_dcid;
    for (const auto& [did, d] : col_.decks)
        if (!d.filtered) did_to_dcid[did] = d.config_id;
    Stmt s(col_.db(), "select id, nid, did, due from cards where due >= ?1 and due < ?2 ");
    s.bind(1, (int64_t)t.days_elapsed).bind(2, (int64_t)(t.days_elapsed + LOAD_BALANCE_DAYS));
    while (s.step()) {
        auto it = did_to_dcid.find(s.i64(2));
        if (it == did_to_dcid.end()) continue;
        auto& days = lb->days_by_preset[it->second];
        if (days.empty()) days.resize(LOAD_BALANCE_DAYS);
        size_t idx = (size_t)(s.i64(3) - t.days_elapsed);
        days[idx].add(s.i64(0), s.i64(1));
    }
    for (const auto& [id, c] : col_.deck_configs) {
        std::array<EasyDay, 7> ed;
        ed.fill(EasyDay::Normal);
        if (c.easy_days_percentages.size() == 7) {
            for (int i = 0; i < 7; i++) {
                float v = c.easy_days_percentages[i];
                ed[i] = v == 1.0f ? EasyDay::Normal : v == 0.0f ? EasyDay::Minimum : EasyDay::Reduced;
            }
        } else if (!c.easy_days_percentages.empty()) {
            continue;  // rslib errors here; skip the preset so it falls back to fuzz
        }
        lb->easy_days[id] = ed;
    }
    lb_ = std::move(lb);
}

Scheduler::Scheduler(Collection& col) : col_(col) {}
Scheduler::~Scheduler() = default;

// ---------------------------------------------------------------- answering (answering/mod.rs)

struct Scheduler::Updater {
    Card card;
    const Deck* deck = nullptr;
    const Deck* home = nullptr;
    const DeckConfig* config = nullptr;
    SchedTimingToday timing;
    int64_t now = 0;
    std::optional<uint64_t> fuzz_seed;
    std::optional<NextStates> fsrs_next_states;
    std::optional<float> desired_retention;
    bool fsrs_short_term_with_steps = false;
    bool fsrs_allow_short_term = false;

    uint32_t secs_until_rollover() const { return (uint32_t)(timing.next_day_at - now); }

    uint32_t learning_ivl_with_fuzz(std::optional<uint64_t> seed, uint32_t secs) const {
        if (!seed) return secs;
        StdRng rng = StdRng::seed_from_u64(*seed);
        uint32_t upper_exclusive = secs + f2u(std::floor(std::min((float)secs * 0.25f, 300.0f)));
        if (secs >= upper_exclusive) return secs;
        return rng.range_u32(secs, upper_exclusive);
    }

    CardState current_card_state() const {
        int32_t due;
        if (!deck->filtered) {
            due = card.ctype == CTYPE_REVIEW ? std::min(card.due, (int32_t)timing.days_elapsed) : card.due;
        } else {
            due = card.odue != 0 ? card.odue : card.due;
        }
        NormalState n = normal_study_state(due);
        if (!deck->filtered) return CardState::from_normal(n);
        CardState c;
        if (deck->reschedule) {
            c.kind = CardStateKind::Rescheduling;
            c.normal = n;
        } else {
            c.kind = CardStateKind::Preview;
            c.preview_scheduled_secs = deck->preview_again_secs;
            c.preview_finished = false;
        }
        return c;
    }

    NormalState normal_study_state(int32_t due) const {
        auto elapsed_secs = [&](uint32_t last_ivl) -> uint32_t {
            if (card.queue == Q_LEARN) {
                uint32_t last_ivl_with_fuzz =
                    learning_ivl_with_fuzz(fuzz_seed_for(card.id, card.reps - 1), last_ivl);
                int64_t last_answered = (int64_t)due - (int64_t)last_ivl_with_fuzz;
                return (uint32_t)(now - last_answered);
            } else if (card.queue == Q_DAY_LEARN) {
                int32_t days_since = (int32_t)timing.days_elapsed;
                int32_t last_ivl_days = (int32_t)std::max<uint32_t>(last_ivl / 86400, 1u);
                int32_t elapsed_days = days_since - due + last_ivl_days;
                return (uint32_t)(elapsed_days * 86400);
            }
            return 0;
        };
        NormalState n;
        float ease = card.ease_factor();
        switch (card.ctype) {
            case CTYPE_NEW:
                n.kind = NormalKind::New;
                n.new_position = (uint32_t)std::max<int32_t>(due, 0);
                break;
            case CTYPE_LEARN: {
                n.kind = NormalKind::Learning;
                LearningSteps steps(&config->learn_steps);
                uint32_t last = steps.current_delay_secs(card.left);
                n.learning.scheduled_secs = last;
                n.learning.remaining_steps = card.left;
                n.learning.elapsed_secs = elapsed_secs(last);
                n.learning.memory_state = card.memory_state;
                break;
            }
            case CTYPE_REVIEW:
                n.kind = NormalKind::Review;
                n.review.scheduled_days = card.ivl;
                n.review.elapsed_days =
                    (uint32_t)std::max<int32_t>((int32_t)card.ivl - (due - (int32_t)timing.days_elapsed), 0);
                n.review.ease_factor = ease;
                n.review.lapses = card.lapses;
                n.review.leeched = false;
                n.review.memory_state = card.memory_state;
                break;
            default: {  // relearn
                n.kind = NormalKind::Relearning;
                LearningSteps steps(&config->relearn_steps);
                uint32_t last = steps.current_delay_secs(card.left);
                n.learning.scheduled_secs = last;
                n.learning.elapsed_secs = elapsed_secs(last);
                n.learning.remaining_steps = card.left;
                n.learning.memory_state = card.memory_state;
                n.review.scheduled_days = card.ivl;
                n.review.elapsed_days = card.ivl;
                n.review.ease_factor = ease;
                n.review.lapses = card.lapses;
                n.review.memory_state = card.memory_state;
                break;
            }
        }
        return n;
    }

    int32_t fuzzed_next_learning_timestamp(uint32_t secs) const {
        return (int32_t)now + (int32_t)learning_ivl_with_fuzz(fuzz_seed, secs);
    }
};

bool Scheduler::make_updater(const Card& card, Updater& u, std::string& err) {
    u.card = card;
    u.timing = col_.timing_today();
    u.now = col_.now_secs();
    u.deck = col_.deck(card.did);
    if (!u.deck) { err = "card's deck is missing"; return false; }
    u.home = card.odid == 0 ? u.deck : col_.deck(card.odid);
    if (!u.home || u.home->filtered) { err = "card's home deck is missing or filtered"; return false; }
    u.config = &col_.config_for_deck(*u.home);
    float desired_retention = u.home->desired_retention.value_or(u.config->desired_retention);
    const auto& params = u.config->fsrs_params;
    if (fsrs_) {
        Fsrs fsrs;
        if (!fsrs.init(params)) { err = "invalid FSRS parameters in preset " + u.config->name; return false; }
        u.card.decay = decay_from_params(params);
        if (!u.card.memory_state && u.card.ctype != CTYPE_NEW) {
            err = "this card has no FSRS memory state yet; open the deck options on desktop Anki and save once";
            return false;
        }
        uint32_t days_elapsed;
        if (u.card.last_review_time) {
            days_elapsed = (uint32_t)(std::max<int64_t>(u.timing.next_day_at - *u.card.last_review_time, 0) / 86400);
        } else if (auto ts = col_.time_of_last_review(card.id)) {
            days_elapsed = (uint32_t)(std::max<int64_t>(u.timing.next_day_at - *ts, 0) / 86400);
        } else {
            days_elapsed = 0;
        }
        u.fsrs_next_states = fsrs.next_states(u.card.memory_state, desired_retention, days_elapsed);
        u.desired_retention = desired_retention;
        if (params.size() >= 19) u.fsrs_allow_short_term = params[17] > 0.0f && params[18] > 0.0f;
        else u.fsrs_allow_short_term = params.empty();
    }
    u.fsrs_short_term_with_steps = short_term_with_steps_;
    u.fuzz_seed = fuzz_seed_for(u.card.id, u.card.reps);
    return true;
}

SchedulingStates Scheduler::scheduling_states(const Card& card) {
    Updater u;
    std::string err;
    if (!make_updater(card, u, err)) return {};
    StateContext ctx;
    ctx.fuzz_factor = fuzz_factor_for(u.fuzz_seed);
    ctx.learn_steps = &u.config->learn_steps;
    ctx.relearn_steps = &u.config->relearn_steps;
    ctx.graduating_interval_good = u.config->graduating_interval_good;
    ctx.graduating_interval_easy = u.config->graduating_interval_easy;
    ctx.initial_ease_factor = u.config->initial_ease;
    ctx.hard_multiplier = u.config->hard_multiplier;
    ctx.easy_multiplier = u.config->easy_multiplier;
    ctx.interval_multiplier = u.config->interval_multiplier;
    ctx.maximum_review_interval = u.config->maximum_review_interval;
    ctx.leech_threshold = u.config->leech_threshold;
    ctx.lapse_multiplier = u.config->lapse_multiplier;
    ctx.minimum_lapse_interval = u.config->minimum_lapse_interval;
    ctx.in_filtered_deck = u.deck->filtered;
    if (u.deck->filtered) {
        ctx.preview_again = u.deck->preview_again_secs;
        ctx.preview_hard = u.deck->preview_hard_secs;
        ctx.preview_good = u.deck->preview_good_secs;
    }
    ctx.fsrs_next_states = u.fsrs_next_states;
    ctx.fsrs_short_term_with_steps_enabled = u.fsrs_short_term_with_steps;
    ctx.fsrs_allow_short_term = u.fsrs_allow_short_term;
    if (lb_) {
        LoadBalancer* lb = lb_.get();
        int64_t dcid = u.home->config_id;
        std::optional<int64_t> nid;
        auto cit = col_.deck_configs.find(dcid);
        if (cit != col_.deck_configs.end() && cit->second.bury_reviews) nid = card.nid;
        auto seed = u.fuzz_seed;
        ctx.load_balancer = [lb, dcid, nid, seed](float iv, uint32_t mn, uint32_t mx) {
            return lb->find_interval(iv, mn, mx, dcid, seed, nid);
        };
    }
    return next_states(u.current_card_state(), ctx);
}

struct RevlogPartial {
    IntervalKind interval, last_interval;
    float ease_factor;
    RevlogKind kind;
};

static RevlogPartial make_partial(const CardState& current, const CardState& next, float ease_factor,
                                  uint32_t secs_until_rollover) {
    return {next.interval_kind().maybe_as_days(secs_until_rollover),
            current.interval_kind().maybe_as_days(secs_until_rollover), ease_factor, current.revlog_kind()};
}

static bool same_state(const CardState& a, const CardState& b);

bool Scheduler::answer(int64_t card_id, Rating rating, uint32_t millis_taken, std::string& err,
                       const SchedulingStates* precomputed) {
    auto oc = col_.get_card(card_id);
    if (!oc) { err = "card not found"; return false; }
    Card original = *oc;
    SchedulingStates states = precomputed ? *precomputed : scheduling_states(original);
    Updater u;
    if (!make_updater(original, u, err)) return false;
    millis_taken = std::min(millis_taken, u.config->cap_answer_time_to_secs * 1000);
    CardState current = u.current_card_state();
    const CardState& next = rating == Rating::Again ? states.again
                            : rating == Rating::Hard ? states.hard
                            : rating == Rating::Good ? states.good
                                                     : states.easy;
    Card& c = u.card;
    RevlogPartial rp{};
    auto apply_normal = [&](const NormalState& n) {
        c.reps += 1;
        c.desired_retention = u.desired_retention;
        uint32_t sur = u.secs_until_rollover();
        CardState next_cs = CardState::from_normal(n);
        switch (n.kind) {
            case NormalKind::New:
                c.ctype = CTYPE_NEW;
                c.queue = Q_NEW;
                c.due = (int32_t)n.new_position;
                c.original_position.reset();
                c.memory_state.reset();
                rp = make_partial(current, next_cs, 0.0f, sur);
                break;
            case NormalKind::Learning: {
                c.left = n.learning.remaining_steps;
                c.ctype = CTYPE_LEARN;
                if (auto pos = current.new_position()) c.original_position = *pos;
                c.memory_state = n.learning.memory_state;
                IntervalKind iv = next_cs.interval_kind().maybe_as_days(sur);
                if (iv.unit == IntervalUnit::Secs) {
                    c.queue = Q_LEARN;
                    c.due = u.fuzzed_next_learning_timestamp(iv.value);
                } else {
                    c.queue = Q_DAY_LEARN;
                    c.due = (int32_t)(u.timing.days_elapsed + iv.value);
                }
                float ef = c.memory_state ? (c.memory_state->difficulty - 1.0f) / 9.0f + 0.1f : 0.0f;
                rp = make_partial(current, next_cs, ef, sur);
                break;
            }
            case NormalKind::Review: {
                c.queue = Q_REVIEW;
                c.ctype = CTYPE_REVIEW;
                c.ivl = n.review.scheduled_days;
                c.due = (int32_t)(u.timing.days_elapsed + n.review.scheduled_days);
                c.factor = (uint16_t)std::round(n.review.ease_factor * 1000.0f);
                c.lapses = n.review.lapses;
                c.left = 0;
                if (auto pos = current.new_position()) c.original_position = *pos;
                c.memory_state = n.review.memory_state;
                float ef = c.memory_state ? (c.memory_state->difficulty - 1.0f) / 9.0f + 0.1f : n.review.ease_factor;
                rp = make_partial(current, next_cs, ef, sur);
                break;
            }
            case NormalKind::Relearning: {
                c.ivl = n.review.scheduled_days;
                c.left = n.learning.remaining_steps;
                c.ctype = CTYPE_RELEARN;
                c.lapses = n.review.lapses;
                c.factor = (uint16_t)std::round(n.review.ease_factor * 1000.0f);
                if (auto pos = current.new_position()) c.original_position = *pos;
                c.memory_state = n.learning.memory_state;
                IntervalKind iv = next_cs.interval_kind().maybe_as_days(sur);
                if (iv.unit == IntervalUnit::Secs) {
                    c.queue = Q_LEARN;
                    c.due = u.fuzzed_next_learning_timestamp(iv.value);
                } else {
                    c.queue = Q_DAY_LEARN;
                    c.due = (int32_t)(u.timing.days_elapsed + iv.value);
                }
                float ef = c.memory_state ? (c.memory_state->difficulty - 1.0f) / 9.0f + 0.1f : n.review.ease_factor;
                rp = make_partial(current, next_cs, ef, sur);
                break;
            }
        }
        if (n.leeched() && u.config->leech_action == 0) c.queue = Q_SUSPENDED;
    };

    if (next.kind == CardStateKind::Normal) {
        if (current.kind == CardStateKind::Preview) { err = "preview card returned a normal state"; return false; }
        if (current.kind == CardStateKind::Rescheduling && c.odid != 0) {
            c.did = c.odid;
            c.odid = 0;
            c.odue = 0;
        }
        apply_normal(next.normal);
    } else {
        if (c.odid == 0) { err = "card answering can't transition into filtered state"; return false; }
        if (next.kind == CardStateKind::Preview) {
            rp = make_partial(current, next, 0.0f, u.secs_until_rollover());
            if (next.preview_finished) {
                // remove_from_filtered_deck_restoring_queue
                c.did = c.odid;
                c.odid = 0;
                if (c.odue != 0) c.due = c.odue;
                if (c.queue >= 0) {
                    if (c.ctype == CTYPE_LEARN || c.ctype == CTYPE_RELEARN)
                        c.queue = c.due > 1000000000 ? Q_LEARN : Q_DAY_LEARN;
                    else if (c.ctype == CTYPE_NEW) c.queue = Q_NEW;
                    else c.queue = Q_REVIEW;
                }
                c.odue = 0;
            } else {
                c.queue = Q_PREVIEW;
                c.due = u.fuzzed_next_learning_timestamp(next.preview_scheduled_secs);
            }
        } else {
            apply_normal(next.normal);
            c.odue = c.due;
        }
    }

    // revlog
    RevlogEntry e;
    e.id = col_.clock.now_millis();
    e.cid = c.id;
    e.usn = -1;
    e.ease = (int)rating;
    e.ivl = rp.interval.as_revlog_interval();
    e.last_ivl = rp.last_interval.as_revlog_interval();
    e.factor = (uint32_t)std::round(rp.ease_factor * 1000.0f);
    e.time = millis_taken;
    e.type = (int)rp.kind;

    col_.begin();
    col_.add_revlog(e, true);
    // bury siblings
    {
        bool bury_new = u.config->bury_new, bury_rev = u.config->bury_reviews, bury_il = u.config->bury_interday_learning;
        auto gather_ord = [](int q) {
            switch (q) {
                case Q_LEARN: case Q_PREVIEW: return 0;
                case Q_DAY_LEARN: return 1;
                case Q_REVIEW: return 2;
                case Q_NEW: return 3;
                default: return 255;
            }
        };
        bury_il = bury_il && gather_ord(original.queue) <= 1;
        bury_rev = bury_rev && gather_ord(original.queue) <= 2;
        if (bury_new || bury_rev || bury_il) {
            Stmt s(col_.db(),
                   "update cards set queue = -3, mod = ?1, usn = -1 where id != ?2 and nid = ?3 and queue != -3 and "
                   "queue != -1 and ((?4 and queue = 0) or (?5 and queue = 2) or (?6 and queue = 3))");
            s.bind(1, col_.now_secs()).bind(2, c.id).bind(3, c.nid).bind(4, (int64_t)bury_new)
                .bind(5, (int64_t)bury_rev).bind(6, (int64_t)bury_il);
            s.step();
        }
    }
    if (current.kind != CardStateKind::Preview) c.last_review_time = col_.clock.now_millis() / 1000;
    c.mtime = col_.now_secs();
    c.usn = -1;
    col_.update_card(c);
    if (next.leeched()) {
        if (auto note = col_.get_note(c.nid)) {
            std::string tags = note->tags;
            std::string lower;
            for (char ch : tags) lower += (char)tolower((unsigned char)ch);
            if (lower.find(" leech ") == std::string::npos) {
                std::string t = tags;
                while (!t.empty() && t.back() == ' ') t.pop_back();
                while (!t.empty() && t.front() == ' ') t.erase(0, 1);
                note->tags = " " + (t.empty() ? std::string("leech") : t + " leech") + " ";
                note->mtime = col_.now_secs();
                note->usn = -1;
                col_.update_note(*note);
                Stmt s(col_.db(), "insert or ignore into tags (tag, usn, collapsed, config) values ('leech', -1, 0, null)");
                s.step();
            }
        }
    }
    col_.set_modified();
    col_.commit();
    if (lb_ && c.queue == Q_REVIEW) lb_->add_card(c.id, c.nid, u.home->config_id, c.ivl);
    return true;
}

// ---------------------------------------------------------------- display

std::string format_interval(uint32_t secs, bool) {
    // Mirrors Anki's answer_button_time: <1m shows seconds, then m/h/d/mo/y with 2 sig digits.
    char buf[32];
    double s = secs;
    auto fmt = [&](double v, const char* unit) {
        if (v < 10 && std::fabs(v - std::round(v)) > 0.05) snprintf(buf, sizeof buf, "%.1f%s", v, unit);
        else snprintf(buf, sizeof buf, "%.0f%s", std::round(v), unit);
        return std::string(buf);
    };
    if (s < 60) return fmt(s, "s");
    if (s < 3600) return fmt(s / 60, "m");
    if (s < 86400) return fmt(s / 3600, "h");
    if (s < 30 * 86400) return fmt(s / 86400, "d");
    if (s < 365 * 86400) return fmt(s / (30.0 * 86400), "mo");
    return fmt(s / (365.0 * 86400), "y");
}

std::vector<std::string> Scheduler::describe_next_states(const SchedulingStates& st) {
    auto t = col_.timing_today();
    uint32_t sur = (uint32_t)std::max<int64_t>(t.next_day_at - t.now, 0);
    std::vector<std::string> out;
    for (const CardState* cs : {&st.again, &st.hard, &st.good, &st.easy})
        out.push_back(format_interval(cs->interval_kind().maybe_as_days(sur).as_seconds()));
    return out;
}

}  // namespace anki
