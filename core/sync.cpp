#include "sync.h"

#include <cstdio>
#include <chrono>
#include <random>
#include <set>

#include "http.h"
#include "nlohmann/json.hpp"
#include "schema11.h"
#include "sqlite3.h"
#include "zstd.h"

using json = nlohmann::json;

namespace anki {

const char* sync_status_name(SyncStatus s) {
    switch (s) {
        case SyncStatus::NoChanges: return "no changes";
        case SyncStatus::Synced: return "synced";
        case SyncStatus::FullSyncRequired: return "full sync required";
        case SyncStatus::Downloaded: return "downloaded";
        default: return "failed";
    }
}

std::string zstd_compress(const std::string& in) {
    std::string out(ZSTD_compressBound(in.size()), '\0');
    size_t n = ZSTD_compress(&out[0], out.size(), in.data(), in.size(), 0);
    if (ZSTD_isError(n)) return std::string();
    out.resize(n);
    return out;
}

bool zstd_decompress(const std::string& in, std::string& out) {
    out.clear();
    ZSTD_DStream* ds = ZSTD_createDStream();
    ZSTD_initDStream(ds);
    ZSTD_inBuffer ib{in.data(), in.size(), 0};
    std::string buf(ZSTD_DStreamOutSize(), '\0');
    bool ok = true;
    while (ib.pos < ib.size) {
        ZSTD_outBuffer ob{&buf[0], buf.size(), 0};
        size_t r = ZSTD_decompressStream(ds, &ob, &ib);
        if (ZSTD_isError(r)) { ok = false; break; }
        out.append(buf.data(), ob.pos);
    }
    ZSTD_freeDStream(ds);
    return ok;
}

static std::string random_session() {
    static const char* chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    // std::random_device isn't available on every target (newlib on the 3DS)
    uint64_t seed = (uint64_t)std::chrono::system_clock::now().time_since_epoch().count() ^
                    (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937 g((uint32_t)(seed ^ (seed >> 32)));
    std::string s;
    for (int i = 0; i < 8; i++) s += chars[g() % 62];
    return s;
}

SyncClient::SyncClient(std::string ep) : endpoint(std::move(ep)), session_(random_session()) {
    if (!endpoint.empty() && endpoint.back() != '/') endpoint += '/';
}

bool SyncClient::request_path(const std::string& path, const std::string& body, std::string& out, std::string& err,
                              int* status_out) {
    json hdr = {{"v", 11}, {"k", hkey}, {"c", client_version}, {"s", session_}};
    std::map<std::string, std::string> headers = {{"anki-sync", hdr.dump()},
                                                  {"Content-Type", "application/octet-stream"}};
    std::string url = endpoint + path;
    HttpResponse resp;
    if (!http_post(url, headers, zstd_compress(body), resp, err)) return false;
    if (status_out) *status_out = resp.status;
    if (resp.status == 308) {
        auto loc = resp.headers.find("location");
        if (loc != resp.headers.end()) {
            err = "redirect:" + loc->second;
            return false;
        }
    }
    if (resp.status < 200 || resp.status >= 300) {
        std::string text;
        if (!zstd_decompress(resp.body, text) || text.empty()) text = resp.body;
        if (text.size() > 300) text.resize(300);
        if (resp.status == 403) err = "AnkiWeb rejected the login (check username/password, or log in again)";
        else err = "server returned HTTP " + std::to_string(resp.status) + (text.empty() ? "" : ": " + text);
        return false;
    }
    if (!zstd_decompress(resp.body, out)) {
        err = "could not decompress server response";
        return false;
    }
    return true;
}

std::optional<std::string> SyncClient::login(const std::string& user, const std::string& pass, std::string& err) {
    std::string out;
    json body = {{"u", user}, {"p", pass}};
    if (!request("hostKey", body.dump(), out, err)) return std::nullopt;
    json j = json::parse(out, nullptr, false);
    if (!j.is_object() || !j.contains("key")) {
        err = "unexpected login response";
        return std::nullopt;
    }
    hkey = j["key"].get<std::string>();
    return hkey;
}

// --------------------------------------------------------------- helpers

namespace {

int64_t count(Collection& col, const char* table) {
    std::string sql = std::string("select count() from ") + table;
    Stmt s(col.db(), sql.c_str());
    return s.step() ? s.i64(0) : 0;
}

bool any_pending(Collection& col, const char* table) {
    std::string sql = std::string("select 1 from ") + table + " where usn = -1 limit 1";
    Stmt s(col.db(), sql.c_str());
    return s.step();
}

std::vector<int64_t> pending_ids(Collection& col, const char* table) {
    std::string sql = std::string("select id from ") + table + " where usn = -1";
    Stmt s(col.db(), sql.c_str());
    std::vector<int64_t> out;
    while (s.step()) out.push_back(s.i64(0));
    return out;
}

void set_usn(Collection& col, const char* table, const std::vector<int64_t>& ids, int usn) {
    std::string sql = std::string("update ") + table + " set usn = ? where id = ?";
    Stmt s(col.db(), sql.c_str());
    for (int64_t id : ids) {
        s.bind(1, (int64_t)usn).bind(2, id);
        s.step();
        s.reset();
    }
}

json card_entry(Collection& col, int64_t id, int usn) {
    auto c = col.get_card(id);
    if (!c) return nullptr;
    return json::array({c->id, c->nid, c->did, c->ord, c->mtime, usn, c->ctype, c->queue, c->due, c->ivl, c->factor,
                        c->reps, c->lapses, c->left, c->odue, c->odid, c->flags, c->data_json()});
}

json note_entry(Collection& col, int64_t id, int usn) {
    auto n = col.get_note(id);
    if (!n) return nullptr;
    std::string flds;
    for (size_t i = 0; i < n->fields.size(); i++) {
        if (i) flds += '\x1f';
        flds += n->fields[i];
    }
    // tags are sent space-separated without the leading/trailing spaces
    std::string tags = n->tags;
    while (!tags.empty() && tags.front() == ' ') tags.erase(0, 1);
    while (!tags.empty() && tags.back() == ' ') tags.pop_back();
    return json::array({n->id, n->guid, n->mid, n->mtime, usn, tags, flds, "", "", 0, ""});
}

json revlog_entry(Collection& col, int64_t id, int usn) {
    Stmt s(col.db(), "select id, cid, ease, ivl, lastIvl, factor, time, type from revlog where id = ?");
    s.bind(1, id);
    if (!s.step()) return nullptr;
    return json::array({s.i64(0), s.i64(1), usn, s.i64(2), s.i64(3), s.i64(4), s.i64(5), s.i64(6), s.i64(7)});
}

int64_t jint(const json& v) {
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number()) return (int64_t)v.get<double>();
    if (v.is_string()) {
        try { return std::stoll(v.get<std::string>()); } catch (...) {}
    }
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    return 0;
}

void apply_chunk(Collection& col, const json& chunk) {
    sqlite3* db = col.db();
    if (chunk.contains("revlog"))
        for (auto& r : chunk["revlog"]) {
            if (!r.is_array() || r.size() < 9) continue;
            RevlogEntry e;
            e.id = jint(r[0]); e.cid = jint(r[1]); e.usn = (int)jint(r[2]); e.ease = (int)jint(r[3]);
            e.ivl = (int32_t)jint(r[4]); e.last_ivl = (int32_t)jint(r[5]); e.factor = (uint32_t)jint(r[6]);
            e.time = (uint32_t)jint(r[7]); e.type = (int)jint(r[8]);
            col.add_revlog(e, false);
        }
    if (chunk.contains("cards")) {
        Stmt get(db, "select usn, mod from cards where id = ?");
        Stmt put(db,
                 "insert or replace into cards (id, nid, did, ord, mod, usn, type, queue, due, ivl, factor, reps, "
                 "lapses, left, odue, odid, flags, data) values (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        for (auto& c : chunk["cards"]) {
            if (!c.is_array() || c.size() < 18) continue;
            int64_t id = jint(c[0]);
            int64_t mtime = jint(c[4]);
            get.bind(1, id);
            bool proceed = true;
            if (get.step()) {
                int local_usn = (int)get.i64(0);
                int64_t local_mod = get.i64(1);
                proceed = local_usn != -1 || local_mod < mtime;
            }
            get.reset();
            if (!proceed) continue;
            Card card;
            card.parse_data(c[17].is_string() ? c[17].get<std::string>() : "");
            for (int i = 0; i < 17; i++) put.bind(i + 1, jint(c[i]));
            put.bind(18, card.data_json());
            put.step();
            put.reset();
        }
    }
    if (chunk.contains("notes")) {
        Stmt get(db, "select usn, mod from notes where id = ?");
        for (auto& n : chunk["notes"]) {
            if (!n.is_array() || n.size() < 11) continue;
            int64_t id = jint(n[0]);
            int64_t mtime = jint(n[3]);
            get.bind(1, id);
            bool proceed = true;
            if (get.step()) proceed = get.i64(0) != -1 || get.i64(1) < mtime;
            get.reset();
            if (!proceed) continue;
            Note note;
            note.id = id;
            note.guid = n[1].get<std::string>();
            note.mid = jint(n[2]);
            note.mtime = mtime;
            note.usn = (int)jint(n[4]);
            // split_tags + join_tags: single spaces, with a leading and trailing space
            std::string raw = n[5].is_string() ? n[5].get<std::string>() : "";
            std::string joined;
            size_t i = 0;
            while (i < raw.size()) {
                while (i < raw.size() && isspace((unsigned char)raw[i])) i++;
                size_t j = i;
                while (j < raw.size() && !isspace((unsigned char)raw[j])) j++;
                if (j > i) joined += " " + raw.substr(i, j - i);
                i = j;
            }
            note.tags = joined.empty() ? "" : joined + " ";
            std::string flds = n[6].get<std::string>();
            size_t start = 0;
            while (true) {
                size_t p = flds.find('\x1f', start);
                note.fields.push_back(flds.substr(start, p == std::string::npos ? std::string::npos : p - start));
                if (p == std::string::npos) break;
                start = p + 1;
            }
            // sort field and first-field checksum, as rslib's prepare_for_update
            auto nt = col.notetypes.find(note.mid);
            size_t sidx = nt != col.notetypes.end() ? nt->second.sort_field_idx : 0;
            if (sidx >= note.fields.size()) sidx = 0;
            note.sfld = strip_html_preserving_media(note.fields.empty() ? "" : note.fields[sidx]);
            note.csum = field_checksum(strip_html_preserving_media(note.fields.empty() ? "" : note.fields[0]));
            note.flags = 0;
            col.update_note(note);
        }
    }
}

}  // namespace

// --------------------------------------------------------------- normal sync

SyncResult SyncClient::normal_sync(Collection& col, std::string& err) {
    SyncResult res;
    col.load_metadata();
    progress("Checking AnkiWeb...");
    std::string out;
    json meta_req = {{"v", 11}, {"cv", client_version}};
    if (!request("meta", meta_req.dump(), out, err)) {
        if (err.rfind("redirect:", 0) == 0) {
            endpoint = err.substr(9);
            if (!endpoint.empty() && endpoint.back() != '/') endpoint += '/';
            res.new_endpoint = endpoint;
            err.clear();
            if (!request("meta", meta_req.dump(), out, err)) return res;
        } else {
            return res;
        }
    }
    json remote = json::parse(out, nullptr, false);
    if (!remote.is_object()) {
        err = "unexpected meta response";
        return res;
    }
    res.message = remote.value("msg", "");
    if (!remote.value("cont", true)) {
        err = res.message.empty() ? "AnkiWeb asked this client to stop syncing" : res.message;
        return res;
    }
    int64_t remote_ts = jint(remote.value("ts", json(0)));
    int64_t delta = remote_ts - col.now_secs();
    if (delta > 300 || delta < -300) {
        err = "the 3DS clock is off by " + std::to_string(delta / 60) +
              " minutes; fix the date/time (and UTC offset in settings) and try again";
        return res;
    }
    int64_t remote_mod = jint(remote.value("mod", json(0)));
    int64_t remote_scm = jint(remote.value("scm", json(0)));
    int remote_usn = (int)jint(remote.value("usn", json(0)));
    bool remote_empty = remote.value("empty", false);
    bool local_empty = count(col, "cards") == 0;
    if (remote_mod == col.mod) {
        res.status = SyncStatus::NoChanges;
        return res;
    }
    if (remote_scm != col.scm) {
        res.status = SyncStatus::FullSyncRequired;
        res.download_ok = !remote_empty || local_empty;
        res.message = "AnkiWeb and this 3DS can't be merged. Download the AnkiWeb copy to replace the 3DS copy "
                      "(reviews done on the 3DS since the last sync would be lost).";
        return res;
    }
    // The 3DS never edits these; if something marked them pending, stop.
    for (const char* t : {"deck_config", "notetypes"})
        if (any_pending(col, t)) {
            err = std::string("unexpected local changes in ") + t + "; please do a full download";
            return res;
        }

    col.unbury_if_day_rolled_over(col.timing_today());
    bool local_is_newer = col.mod > remote_mod;
    int usn_at_last_sync = col.usn;
    int server_usn = remote_usn;
    const int pending_usn = -1;
    (void)pending_usn;

    col.begin();
    auto fail = [&](const std::string& e) {
        col.rollback();
        std::string ignore, o;
        request("abort", "{}", o, ignore);
        if (err.empty()) err = e;
        return res;
    };

    // start: server sends its deletions
    progress("Syncing (deletions)...");
    json start = {{"minUsn", usn_at_last_sync}, {"lnewer", local_is_newer}};
    if (!request("start", start.dump(), out, err)) return fail(err);
    json graves = json::parse(out, nullptr, false);
    // local graves (we don't delete things, but desktop-made ones may exist)
    {
        json local = {{"cards", json::array()}, {"notes", json::array()}, {"decks", json::array()}};
        Stmt s(col.db(), "select oid, type from graves where usn = -1");
        while (s.step()) {
            int t = (int)s.i64(1);
            const char* k = t == 0 ? "cards" : t == 1 ? "notes" : "decks";
            local[k].push_back(s.i64(0));
        }
        Stmt u(col.db(), "update graves set usn = ? where usn = -1");
        u.bind(1, (int64_t)server_usn);
        u.step();
        if (!local["cards"].empty() || !local["notes"].empty() || !local["decks"].empty()) {
            json req = {{"chunk", local}};
            if (!request("applyGraves", req.dump(), out, err)) return fail(err);
        }
    }
    if (graves.is_object()) {
        sqlite3* db = col.db();
        for (auto& nid : graves.value("notes", json::array())) {
            Stmt a(db, "delete from notes where id = ?"); a.bind(1, jint(nid)); a.step();
            Stmt b(db, "insert into graves (oid, type, usn) values (?, 1, ?)"); b.bind(1, jint(nid)).bind(2, (int64_t)server_usn); b.step();
        }
        for (auto& cid : graves.value("cards", json::array())) {
            Stmt a(db, "delete from cards where id = ?"); a.bind(1, jint(cid)); a.step();
            Stmt b(db, "insert into graves (oid, type, usn) values (?, 0, ?)"); b.bind(1, jint(cid)).bind(2, (int64_t)server_usn); b.step();
        }
        for (auto& did : graves.value("decks", json::array())) {
            Stmt a(db, "delete from decks where id = ?"); a.bind(1, jint(did)); a.step();
            Stmt b(db, "insert into graves (oid, type, usn) values (?, 2, ?)"); b.bind(1, jint(did)).bind(2, (int64_t)server_usn); b.step();
        }
    }

    // unchunked changes
    progress("Syncing (settings)...");
    json changed_decks = json::array();
    {
        std::vector<int64_t> ids = pending_ids(col, "decks");
        for (int64_t id : ids)
            if (const Deck* d = col.deck(id)) changed_decks.push_back(deck_to_json(*d, server_usn));
        set_usn(col, "decks", ids, server_usn);
    }
    json changes = {{"models", json::array()}, {"decks", json::array({changed_decks, json::array()})}};
    {
        json tags = json::array();
        Stmt s(col.db(), "select tag from tags where usn = -1");
        while (s.step()) tags.push_back(s.text(0));
        Stmt u(col.db(), "update tags set usn = ? where usn = -1");
        u.bind(1, (int64_t)server_usn);
        u.step();
        changes["tags"] = tags;
    }
    if (local_is_newer) {
        changes["conf"] = all_config_json(col);
        changes["crt"] = col.crt;
        Stmt u(col.db(), "update config set usn = 0 where usn = -1");
        u.step();
    }
    json apply = {{"changes", changes}};
    if (!request("applyChanges", apply.dump(), out, err)) return fail(err);
    json remote_changes = json::parse(out, nullptr, false);
    if (remote_changes.is_object()) {
        for (auto& nt : remote_changes.value("models", json::array()))
            if (!merge_notetype_json(col, nt, err)) return fail(err);
        json dc = remote_changes.value("decks", json::array());
        if (dc.is_array() && dc.size() == 2) {
            for (auto& d : dc[0])
                if (!merge_deck_json(col, d, err)) return fail(err);
            for (auto& c : dc[1])
                if (!merge_deck_config_json(col, c, err)) return fail(err);
        }
        for (auto& t : remote_changes.value("tags", json::array())) {
            Stmt s(col.db(), "insert or ignore into tags (tag, usn, collapsed, config) values (?, ?, 0, null)");
            s.bind(1, t.get<std::string>()).bind(2, (int64_t)server_usn);
            s.step();
        }
        if (remote_changes.contains("crt")) {
            Stmt s(col.db(), "update col set crt = ?");
            s.bind(1, jint(remote_changes["crt"]));
            s.step();
        }
        if (remote_changes.contains("conf") && remote_changes["conf"].is_object()) {
            Stmt d(col.db(), "delete from config");
            d.step();
            for (auto it = remote_changes["conf"].begin(); it != remote_changes["conf"].end(); ++it)
                col.set_config_json(it.key(), it.value().dump(), server_usn);
        }
        col.load_metadata();
    }

    // chunks from server
    int nchunks = 0;
    while (true) {
        progress("Receiving changes (" + std::to_string(++nchunks) + ")...");
        if (!request("chunk", "{}", out, err)) return fail(err);
        json chunk = json::parse(out, nullptr, false);
        if (!chunk.is_object()) return fail("bad chunk from server");
        apply_chunk(col, chunk);
        if (chunk.value("done", false)) break;
    }

    // chunks to server
    {
        std::vector<int64_t> rev = pending_ids(col, "revlog"), cards = pending_ids(col, "cards"),
                             notes = pending_ids(col, "notes");
        size_t sent = 0, total = rev.size() + cards.size() + notes.size();
        while (true) {
            int limit = 250;
            json chunk = json::object();
            json jr = json::array(), jc = json::array(), jn = json::array();
            std::vector<int64_t> rs, cs, ns;
            while (limit > 0) {
                int last = limit;
                if (!rev.empty()) { rs.push_back(rev.back()); rev.pop_back(); limit--; }
                if (!notes.empty()) { ns.push_back(notes.back()); notes.pop_back(); limit--; }
                if (!cards.empty()) { cs.push_back(cards.back()); cards.pop_back(); limit--; }
                if (limit == last) break;
            }
            bool done = limit > 0;
            for (auto id : rs) jr.push_back(revlog_entry(col, id, server_usn));
            for (auto id : cs) jc.push_back(card_entry(col, id, server_usn));
            for (auto id : ns) jn.push_back(note_entry(col, id, server_usn));
            set_usn(col, "revlog", rs, server_usn);
            set_usn(col, "cards", cs, server_usn);
            set_usn(col, "notes", ns, server_usn);
            chunk["done"] = done;
            if (!jr.empty()) chunk["revlog"] = jr;
            if (!jc.empty()) chunk["cards"] = jc;
            if (!jn.empty()) chunk["notes"] = jn;
            sent += rs.size() + cs.size() + ns.size();
            progress("Sending reviews (" + std::to_string(sent) + "/" + std::to_string(total) + ")...");
            json req = {{"chunk", chunk}};
            if (!request("applyChunk", req.dump(), out, err)) return fail(err);
            if (done) break;
        }
    }

    // sanity check
    progress("Checking...");
    json counts = json::array({json::array({0, 0, 0}), count(col, "cards"), count(col, "notes"), count(col, "revlog"),
                               count(col, "graves"), count(col, "notetypes"), count(col, "decks"),
                               count(col, "deck_config")});
    json sreq = {{"client", counts}};
    if (!request("sanityCheck2", sreq.dump(), out, err)) return fail(err);
    json sresp = json::parse(out, nullptr, false);
    if (!sresp.is_object() || sresp.value("status", "") != "ok") {
        std::string detail = sresp.is_object() ? sresp.dump() : out;
        if (detail.size() > 300) detail.resize(300);
        // Like desktop Anki: mark the schema modified so the next sync is a full one.
        col.rollback();
        std::string o, ignore;
        request("abort", "{}", o, ignore);
        Stmt s(col.db(), "update col set scm = ?");
        s.bind(1, col.clock.now_millis());
        s.step();
        col.load_metadata();
        err = "sync sanity check failed; a full download is needed (" + detail + ")";
        return res;
    }

    // finish
    if (!request("finish", "{}", out, err)) return fail(err);
    int64_t new_mod = jint(json::parse(out, nullptr, false));
    {
        Stmt s(col.db(), "update col set ls = ?, usn = ?, mod = ?");
        s.bind(1, new_mod).bind(2, (int64_t)(server_usn + 1)).bind(3, new_mod);
        s.step();
    }
    col.commit();
    col.load_metadata();
    res.status = SyncStatus::Synced;
    return res;
}

// --------------------------------------------------------------- full download

SyncResult SyncClient::full_download(Collection& col, const std::string& path, std::string& err) {
    SyncResult res;
    std::string tmp = path + ".download";
    progress("Downloading collection...");
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "can't write " + tmp;
        return res;
    }
    ZSTD_DStream* ds = ZSTD_createDStream();
    ZSTD_initDStream(ds);
    std::string buf(ZSTD_DStreamOutSize(), '\0');
    bool zerr = false;
    size_t received = 0;
    HttpSink sink = [&](const char* data, size_t len) {
        received += len;
        ZSTD_inBuffer ib{data, len, 0};
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob{&buf[0], buf.size(), 0};
            size_t r = ZSTD_decompressStream(ds, &ob, &ib);
            if (ZSTD_isError(r)) { zerr = true; return false; }
            if (ob.pos && fwrite(buf.data(), 1, ob.pos, f) != ob.pos) { zerr = true; return false; }
        }
        return true;
    };
    json hdr = {{"v", 11}, {"k", hkey}, {"c", client_version}, {"s", session_}};
    std::map<std::string, std::string> headers = {{"anki-sync", hdr.dump()},
                                                  {"Content-Type", "application/octet-stream"}};
    HttpResponse resp;
    HttpProgress prog = [&](size_t done, size_t total) {
        if (on_bytes) on_bytes(done, total);
        return true;
    };
    bool ok = http_post(endpoint + "sync/download", headers, zstd_compress("{}"), resp, err, sink, prog);
    ZSTD_freeDStream(ds);
    fclose(f);
    if (!ok || zerr || resp.status != 200) {
        remove(tmp.c_str());
        if (err.empty()) err = zerr ? "download was corrupt" : "server returned HTTP " + std::to_string(resp.status);
        return res;
    }
    progress("Checking download...");
    ensure_rollback_journal(tmp);
    {
        sqlite3* db = nullptr;
        if (sqlite3_open(tmp.c_str(), &db) != SQLITE_OK) {
            sqlite3_close(db);
            remove(tmp.c_str());
            err = "downloaded collection could not be opened";
            return res;
        }
        register_unicase(db);
        sqlite3_stmt* st = nullptr;
        bool good = false;
        std::string check_result;
        if (sqlite3_prepare_v2(db, "pragma quick_check", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        {
            check_result = (const char*)sqlite3_column_text(st, 0);
            good = check_result == "ok";
        }
        sqlite3_finalize(st);
        if (good) sqlite3_exec(db, "update col set ls = mod; pragma journal_mode = delete", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        if (!good) {
            remove(tmp.c_str());
            err = "downloaded collection failed its integrity check: " + check_result;
            return res;
        }
    }
    col.close();
    // the old file's journal must not be applied to the new one
    remove((path + "-journal").c_str());
    std::string backup = path + ".bak";
    remove(backup.c_str());
    rename(path.c_str(), backup.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        rename(backup.c_str(), path.c_str());
        err = "could not replace the collection file";
        col.open(path, err);
        return res;
    }
    remove(backup.c_str());
    if (!col.open(path, err)) return res;
    res.status = SyncStatus::Downloaded;
    return res;
}

}  // namespace anki
