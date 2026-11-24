#include "schema11.h"

#include <cstring>
#include <regex>
#include <set>

#include "sha1.h"

using json = nlohmann::json;

namespace anki {

namespace {

// --- tiny protobuf encoder that writes fields in number order and can force
// presence for proto3 `optional` fields.
struct Enc {
    std::string out;
    void key(uint32_t n, int wt) { pb::put_varint(out, ((uint64_t)n << 3) | wt); }
    void u(uint32_t n, uint64_t v, bool force = false) {
        if (!v && !force) return;
        key(n, 0);
        pb::put_varint(out, v);
    }
    void i(uint32_t n, int64_t v, bool force = false) { u(n, (uint64_t)v, force); }
    void b(uint32_t n, bool v) { u(n, v ? 1 : 0); }
    void f(uint32_t n, float v, bool force = false) {
        if (v == 0.0f && !force) return;
        key(n, 5);
        uint32_t bits;
        memcpy(&bits, &v, 4);
        out.append((const char*)&bits, 4);
    }
    void s(uint32_t n, const std::string& v) {
        if (v.empty()) return;
        key(n, 2);
        pb::put_varint(out, v.size());
        out += v;
    }
    void msg(uint32_t n, const std::string& v, bool force = false) {
        if (v.empty() && !force) return;
        key(n, 2);
        pb::put_varint(out, v.size());
        out += v;
    }
    void packed_f(uint32_t n, const std::vector<float>& v) {
        if (v.empty()) return;
        std::string b(v.size() * 4, '\0');
        memcpy(&b[0], v.data(), b.size());
        s(n, b);
    }
    void packed_u(uint32_t n, const std::vector<uint32_t>& v) {
        if (v.empty()) return;
        std::string b;
        for (auto x : v) pb::put_varint(b, x);
        s(n, b);
    }
};

int64_t num(const json& j, const char* k, int64_t def = 0) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<int64_t>();
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    if (it->is_string()) {
        try { return std::stoll(it->get<std::string>()); } catch (...) { return def; }
    }
    return def;
}
double fnum(const json& j, const char* k, double def = 0) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<double>();
}
bool flag(const json& j, const char* k, bool def = false) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    if (it->is_string()) { auto s = it->get<std::string>(); return s == "true" || s == "1"; }
    return def;
}
std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
std::vector<float> floats(const json& j, const char* k, std::vector<float> def = {}) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_array()) return def;
    std::vector<float> v;
    for (auto& x : *it)
        if (x.is_number()) v.push_back((float)x.get<double>());
    return v;
}
// Collect keys not in `known` into a JSON object (serde's #[serde(flatten)] other).
std::string other_bytes(const json& j, const std::set<std::string>& known, const json* extra = nullptr) {
    json o = json::object();
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.count(it.key())) o[it.key()] = it.value();
    if (extra)
        for (auto it = extra->begin(); it != extra->end(); ++it) o[it.key()] = it.value();
    return o.empty() ? std::string() : o.dump();
}

std::string native_deck_name(const std::string& human) {
    std::string out;
    size_t i = 0;
    while (i < human.size()) {
        if (human.compare(i, 2, "::") == 0) {
            out += '\x1f';
            i += 2;
        } else {
            out += human[i++];
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- notetypes

bool merge_notetype_json(Collection& col, const json& nt, std::string& err) {
    int64_t id = num(nt, "id");
    int64_t mtime = num(nt, "mod");
    auto existing = col.notetypes.find(id);
    if (existing != col.notetypes.end()) {
        if (existing->second.mtime > mtime) return true;  // local is newer
        size_t nflds = nt.contains("flds") ? nt["flds"].size() : 0;
        size_t ntmpls = nt.contains("tmpls") ? nt["tmpls"].size() : 0;
        if (existing->second.fields.size() != nflds || existing->second.templates.size() != ntmpls) {
            err = "a note type's fields or card types changed on another device; a full sync (download) is required";
            return false;
        }
    }
    Enc c;
    c.u(1, (uint64_t)num(nt, "type"));
    c.u(2, (uint64_t)num(nt, "sortf"));
    c.s(3, str(nt, "css"));
    c.i(4, num(nt, "did"));
    c.s(5, str(nt, "latexPre"));
    c.s(6, str(nt, "latexPost"));
    c.b(7, flag(nt, "latexsvg"));
    if (nt.contains("req") && nt["req"].is_array()) {
        for (auto& r : nt["req"]) {
            if (!r.is_array() || r.size() < 3) continue;
            Enc rq;
            rq.u(1, r[0].get<uint64_t>());
            std::string k = r[1].is_string() ? r[1].get<std::string>() : "none";
            rq.u(2, k == "any" ? 1 : k == "all" ? 2 : 0);
            std::vector<uint32_t> ords;
            for (auto& o : r[2]) ords.push_back(o.get<uint32_t>());
            rq.packed_u(3, ords);
            c.msg(8, rq.out, true);
        }
    }
    c.u(9, (uint64_t)num(nt, "originalStockKind"));
    if (nt.contains("originalId") && nt["originalId"].is_number()) c.i(10, num(nt, "originalId"), true);
    static const std::set<std::string> NT_KEYS = {"id", "name", "type", "mod", "usn", "sortf", "did", "tmpls", "flds",
                                                  "css", "latexPre", "latexPost", "latexsvg", "req",
                                                  "originalStockKind", "originalId"};
    c.s(255, other_bytes(nt, NT_KEYS));

    sqlite3* db = col.db();
    {
        Stmt s(db, "insert or replace into notetypes (id, name, mtime_secs, usn, config) values (?,?,?,?,?)");
        s.bind(1, id).bind(2, str(nt, "name")).bind(3, mtime).bind(4, num(nt, "usn")).bind_blob(5, c.out);
        s.step();
    }
    {
        Stmt d(db, "delete from fields where ntid = ?");
        d.bind(1, id);
        d.step();
        static const std::set<std::string> F_KEYS = {"name", "ord", "sticky", "rtl", "plainText", "font", "size",
                                                     "collapsed", "description", "excludeFromSearch", "id", "tag",
                                                     "preventDeletion", "media"};
        int ord = 0;
        for (auto& f : nt.value("flds", json::array())) {
            Enc fc;
            fc.b(1, flag(f, "sticky"));
            fc.b(2, flag(f, "rtl"));
            fc.s(3, f.contains("font") ? str(f, "font") : "Arial");
            fc.u(4, (uint64_t)num(f, "size", 20));
            fc.s(5, str(f, "description"));
            fc.b(6, flag(f, "plainText"));
            fc.b(7, flag(f, "collapsed"));
            fc.b(8, flag(f, "excludeFromSearch"));
            if (f.contains("id") && f["id"].is_number()) fc.i(9, num(f, "id"), true);
            if (f.contains("tag") && f["tag"].is_number()) fc.u(10, (uint64_t)num(f, "tag"), true);
            fc.b(11, flag(f, "preventDeletion"));
            fc.s(255, other_bytes(f, F_KEYS));
            Stmt s(db, "insert into fields (ntid, ord, name, config) values (?,?,?,?)");
            s.bind(1, id).bind(2, (int64_t)ord++).bind(3, str(f, "name")).bind_blob(4, fc.out);
            s.step();
        }
    }
    {
        Stmt d(db, "delete from templates where ntid = ?");
        d.bind(1, id);
        d.step();
        static const std::set<std::string> T_KEYS = {"name", "ord", "qfmt", "afmt", "bqfmt", "bafmt",
                                                     "did", "bfont", "bsize", "id"};
        int ord = 0;
        for (auto& t : nt.value("tmpls", json::array())) {
            Enc tc;
            tc.s(1, str(t, "qfmt"));
            tc.s(2, str(t, "afmt"));
            tc.s(3, str(t, "bqfmt"));
            tc.s(4, str(t, "bafmt"));
            tc.i(5, num(t, "did"));
            tc.s(6, str(t, "bfont"));
            tc.u(7, (uint64_t)num(t, "bsize"));
            if (t.contains("id") && t["id"].is_number()) tc.i(8, num(t, "id"), true);
            tc.s(255, other_bytes(t, T_KEYS));
            Stmt s(db, "insert into templates (ntid, ord, name, mtime_secs, usn, config) values (?,?,?,?,?,?)");
            s.bind(1, id).bind(2, (int64_t)ord++).bind(3, str(t, "name")).bind(4, (int64_t)0).bind(5, (int64_t)0)
                .bind_blob(6, tc.out);
            s.step();
        }
    }
    return true;
}

// ---------------------------------------------------------------- decks

bool merge_deck_json(Collection& col, const json& d, std::string& err) {
    if (!d.contains("dyn")) {
        err = "deck missing dyn";
        return false;
    }
    bool is_dyn = d["dyn"].is_boolean() ? d["dyn"].get<bool>() : num(d, "dyn") == 1;
    int64_t id = num(d, "id");
    int64_t mtime = num(d, "mod");
    if (const Deck* existing = col.deck(id))
        if (existing->mtime > mtime) return true;

    // common
    auto today = [&](const char* k, int64_t& day, int64_t& amount) {
        day = amount = 0;
        auto it = d.find(k);
        if (it != d.end() && it->is_array()) {
            if (it->size() >= 2) {
                day = (*it)[0].is_number() ? (*it)[0].get<int64_t>() : 0;
                amount = (*it)[1].is_number() ? (*it)[1].get<int64_t>() : 0;
            } else if (it->size() == 1) {
                amount = (*it)[0].is_number() ? (*it)[0].get<int64_t>() : 0;
            }
        }
    };
    int64_t lrn_d, lrn_a, rev_d, rev_a, new_d, new_a, time_d, time_a;
    today("lrnToday", lrn_d, lrn_a);
    today("revToday", rev_d, rev_a);
    today("newToday", new_d, new_a);
    today("timeToday", time_d, time_a);
    int64_t max_day = std::max(time_d, std::max(new_d, rev_d));
    if (lrn_d != max_day) lrn_a = 0;
    if (rev_d != max_day) rev_a = 0;
    if (new_d != max_day) new_a = 0;
    static const std::set<std::string> COMMON_KEYS = {
        "id", "mod", "name", "usn", "lrnToday", "revToday", "newToday", "timeToday", "collapsed",
        "browserCollapsed", "desc", "md", "dyn", "return",
        // normal
        "conf", "extendNew", "extendRev", "reviewLimit", "newLimit", "reviewLimitToday", "newLimitToday",
        "desiredRetention",
        // filtered
        "resched", "terms", "separate", "delays", "previewDelay", "previewAgainSecs", "previewHardSecs",
        "previewGoodSecs"};
    Enc common;
    common.b(1, flag(d, "collapsed"));
    common.b(2, flag(d, "browserCollapsed"));
    common.u(3, (uint64_t)(uint32_t)max_day);
    common.i(4, (int32_t)new_a);
    common.i(5, (int32_t)rev_a);
    common.i(6, (int32_t)lrn_a);
    common.i(7, (int32_t)time_a);
    common.s(255, other_bytes(d, COMMON_KEYS));

    Enc kind;
    if (!is_dyn) {
        Enc n;
        n.i(1, num(d, "conf", 1));
        n.u(2, (uint64_t)std::max<int64_t>(num(d, "extendNew"), 0));
        n.u(3, (uint64_t)std::max<int64_t>(num(d, "extendRev"), 0));
        n.s(4, str(d, "desc"));
        n.b(5, flag(d, "md"));
        if (d.contains("reviewLimit") && d["reviewLimit"].is_number()) n.u(6, (uint64_t)num(d, "reviewLimit"), true);
        if (d.contains("newLimit") && d["newLimit"].is_number()) n.u(7, (uint64_t)num(d, "newLimit"), true);
        auto day_limit = [&](const char* k, uint32_t field) {
            auto it = d.find(k);
            if (it == d.end() || !it->is_object()) return;
            Enc dl;
            dl.u(1, (uint64_t)num(*it, "limit"));
            dl.u(2, (uint64_t)num(*it, "today"));
            n.msg(field, dl.out, true);
        };
        day_limit("reviewLimitToday", 8);
        day_limit("newLimitToday", 9);
        if (d.contains("desiredRetention") && d["desiredRetention"].is_number())
            n.f(10, (float)num(d, "desiredRetention") / 100.0f, true);
        kind.msg(1, n.out, true);
    } else {
        Enc f;
        f.b(1, flag(d, "resched"));
        for (auto& t : d.value("terms", json::array())) {
            if (!t.is_array() || t.size() < 3) continue;
            Enc st;
            st.s(1, t[0].is_string() ? t[0].get<std::string>() : "");
            int64_t lim = t[1].is_number() ? t[1].get<int64_t>()
                          : t[1].is_string() ? std::stoll(t[1].get<std::string>()) : 0;
            st.u(2, (uint64_t)std::max<int64_t>(lim, 0));
            st.i(3, t[2].is_number() ? t[2].get<int64_t>() : 0);
            f.msg(2, st.out, true);
        }
        f.packed_f(3, floats(d, "delays"));
        f.u(4, (uint64_t)num(d, "previewDelay"));
        f.u(5, (uint64_t)num(d, "previewHardSecs"));
        f.u(6, (uint64_t)num(d, "previewGoodSecs"));
        f.u(7, (uint64_t)num(d, "previewAgainSecs"));
        kind.msg(2, f.out, true);
    }
    Stmt s(col.db(), "insert or replace into decks (id, name, mtime_secs, usn, common, kind) values (?,?,?,?,?,?)");
    s.bind(1, id).bind(2, native_deck_name(str(d, "name"))).bind(3, mtime).bind(4, num(d, "usn"))
        .bind_blob(5, common.out).bind_blob(6, kind.out);
    s.step();
    return true;
}

// ---------------------------------------------------------------- deck presets

bool merge_deck_config_json(Collection& col, const json& c, std::string& err) {
    int64_t id = num(c, "id");
    int64_t mtime = num(c, "mod");
    auto ex = col.deck_configs.find(id);
    if (ex != col.deck_configs.end() && ex->second.mtime > mtime) return true;
    json nw = c.value("new", json::object());
    json rv = c.value("rev", json::object());
    json lp = c.value("lapse", json::object());
    if (!nw.is_object()) nw = json::object();
    if (!rv.is_object()) rv = json::object();
    if (!lp.is_object()) lp = json::object();

    static const std::set<std::string> NEW_KEYS = {"bury", "delays", "initialFactor", "ints", "order", "perDay"};
    static const std::set<std::string> REV_KEYS = {"bury", "ease4", "ivlFct", "maxIvl", "perDay", "hardFactor"};
    static const std::set<std::string> LAPSE_KEYS = {"delays", "leechAction", "leechFails", "minInt", "mult"};
    static const std::set<std::string> TOP_KEYS = {
        "id", "mod", "name", "usn", "maxTaken", "autoplay", "timer", "replayq", "new", "rev", "lapse", "dyn",
        "newMix", "newPerDayMinimum", "interdayLearningMix", "reviewOrder", "newSortOrder", "newGatherPriority",
        "buryInterdayLearning", "fsrsWeights", "fsrsParams5", "fsrsParams6", "desiredRetention",
        "ignoreRevlogsBeforeDate", "easyDaysPercentages", "stopTimerOnAnswer", "secondsToShowQuestion",
        "secondsToShowAnswer", "questionAction", "answerAction", "waitForAudio", "sm2Retention", "weightSearch"};
    json extra = json::object();
    auto sub_other = [](const json& j, const std::set<std::string>& known) {
        json o = json::object();
        for (auto it = j.begin(); it != j.end(); ++it)
            if (!known.count(it.key())) o[it.key()] = it.value();
        return o;
    };
    json no = sub_other(nw, NEW_KEYS), ro = sub_other(rv, REV_KEYS), lo = sub_other(lp, LAPSE_KEYS);
    if (!no.empty()) extra["new"] = no;
    if (!ro.empty()) extra["rev"] = ro;
    if (!lo.empty()) extra["lapse"] = lo;

    std::vector<float> learn = nw.contains("delays") && nw["delays"].is_array() ? floats(nw, "delays") : std::vector<float>{1.0f, 10.0f};
    std::vector<float> relearn = lp.contains("delays") && lp["delays"].is_array() ? floats(lp, "delays") : std::vector<float>{10.0f};
    uint32_t good = 1, easy = 4;
    if (nw.contains("ints") && nw["ints"].is_array() && nw["ints"].size() >= 2) {
        good = nw["ints"][0].get<uint32_t>();
        easy = nw["ints"][1].get<uint32_t>();
    }
    Enc e;
    e.packed_f(1, learn);
    e.packed_f(2, relearn);
    e.packed_f(3, floats(c, "fsrsWeights"));
    e.packed_f(4, floats(c, "easyDaysPercentages", std::vector<float>(7, 1.0f)));
    e.packed_f(5, floats(c, "fsrsParams5"));
    e.packed_f(6, floats(c, "fsrsParams6"));
    e.u(9, (uint64_t)num(nw, "perDay", 20));
    e.u(10, (uint64_t)num(rv, "perDay", 200));
    e.f(11, (float)num(nw, "initialFactor", 2500) / 1000.0f);
    e.f(12, (float)fnum(rv, "ease4", 1.3));
    e.f(13, (float)fnum(rv, "hardFactor", 1.2));
    e.f(14, (float)fnum(lp, "mult", 0.0));
    e.f(15, (float)fnum(rv, "ivlFct", 1.0));
    e.u(16, (uint64_t)num(rv, "maxIvl", 36500));
    e.u(17, (uint64_t)num(lp, "minInt", 1));
    e.u(18, good);
    e.u(19, easy);
    e.u(20, num(nw, "order", 1) == 0 ? 1 : 0);  // Random=0 -> insert order RANDOM(1)
    e.u(21, (uint64_t)num(lp, "leechAction", 1));
    e.u(22, (uint64_t)num(lp, "leechFails", 8));
    e.b(23, !flag(c, "autoplay", true));
    e.u(24, (uint64_t)std::max<int64_t>(num(c, "maxTaken", 60), 0));
    e.b(25, num(c, "timer") != 0);
    e.b(26, !flag(c, "replayq", true));
    e.b(27, flag(nw, "bury"));
    e.b(28, flag(rv, "bury"));
    e.b(29, flag(c, "buryInterdayLearning"));
    e.i(30, num(c, "newMix"));
    e.i(31, num(c, "interdayLearningMix"));
    e.i(32, num(c, "newSortOrder"));
    e.i(33, num(c, "reviewOrder"));
    e.i(34, num(c, "newGatherPriority"));
    e.u(35, (uint64_t)num(c, "newPerDayMinimum"));
    e.u(36, (uint64_t)num(c, "questionAction"));
    e.f(37, (float)fnum(c, "desiredRetention", 0.0));
    e.b(38, flag(c, "stopTimerOnAnswer"));
    e.f(40, (float)fnum(c, "sm2Retention", 0.0));
    e.f(41, (float)fnum(c, "secondsToShowQuestion"));
    e.f(42, (float)fnum(c, "secondsToShowAnswer"));
    e.u(43, (uint64_t)num(c, "answerAction"));
    e.b(44, flag(c, "waitForAudio", true));
    e.s(45, str(c, "weightSearch"));
    e.s(46, str(c, "ignoreRevlogsBeforeDate"));
    e.s(255, other_bytes(c, TOP_KEYS, extra.empty() ? nullptr : &extra));

    Stmt s(col.db(), "insert or replace into deck_config (id, name, mtime_secs, usn, config) values (?,?,?,?,?)");
    s.bind(1, id).bind(2, str(c, "name")).bind(3, mtime).bind(4, num(c, "usn")).bind_blob(5, e.out);
    s.step();
    (void)err;
    return true;
}

json all_config_json(Collection& col) {
    json out = json::object();
    Stmt s(col.db(), "select key, val from config");
    while (s.step()) {
        json v = json::parse(s.blob(1), nullptr, false);
        if (!v.is_discarded()) out[s.text(0)] = v;
    }
    return out;
}

// ---------------------------------------------------------------- notes

std::string strip_html_preserving_media(const std::string& html) {
    // Approximation of rslib's strip_html_preserving_media_filenames: keep
    // <img src> names, drop other tags, decode a few entities.
    std::string out;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            std::string tag = html.substr(i, end - i + 1);
            size_t src = tag.find("src=");
            if (tag.compare(0, 4, "<img") == 0 && src != std::string::npos) {
                size_t q = src + 4;
                char quote = q < tag.size() ? tag[q] : ' ';
                size_t start = (quote == '"' || quote == '\'') ? q + 1 : q;
                size_t stop = (quote == '"' || quote == '\'') ? tag.find(quote, start) : tag.find_first_of(" >", start);
                if (stop != std::string::npos) out += " " + tag.substr(start, stop - start) + " ";
            }
            i = end + 1;
        } else if (html[i] == '&') {
            static const std::pair<const char*, const char*> ents[] = {
                {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}};
            bool hit = false;
            for (auto& [e, r] : ents)
                if (html.compare(i, strlen(e), e) == 0) {
                    out += r;
                    i += strlen(e);
                    hit = true;
                    break;
                }
            if (!hit) out += html[i++];
        } else {
            out += html[i++];
        }
    }
    // trim
    size_t a = out.find_first_not_of(" \t\n\r");
    size_t b = out.find_last_not_of(" \t\n\r");
    return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

int64_t field_checksum(const std::string& s) {
    uint8_t digest[20];
    sha1((const uint8_t*)s.data(), s.size(), digest);
    return ((int64_t)digest[0] << 24) | ((int64_t)digest[1] << 16) | ((int64_t)digest[2] << 8) | digest[3];
}

}  // namespace anki
