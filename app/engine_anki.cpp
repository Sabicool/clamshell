// Engine on Anki's own backend: the Rust library built from the Anki fork's
// 3ds/clamshell-bridge, called through one JSON function. Collections,
// scheduling, card rendering and sync are upstream Anki code.
#include "engine.h"

#ifdef CLAMSHELL_ANKI

#include "nlohmann/json.hpp"

extern "C" {
char* clamshell_call(const char* method, const char* args);
void clamshell_free(char* s);
}

namespace anki {

namespace {

using json = nlohmann::json;

json call(const char* method, const json& args = json::object()) {
    std::string a = args.dump();
    char* out = clamshell_call(method, a.c_str());
    json r = json::parse(out ? out : "{}", nullptr, false);
    clamshell_free(out);
    if (r.is_discarded()) return json{{"error", "unreadable reply from the backend"}};
    return r;
}

bool failed(const json& r, std::string& err) {
    if (r.contains("error")) {
        err = r["error"].get<std::string>();
        return true;
    }
    return false;
}

StudyCard card_from(const json& j) {
    StudyCard c;
    c.id = j.value("card_id", (int64_t)0);
    c.note_id = j.value("note_id", (int64_t)0);
    c.notetype_id = j.value("notetype_id", (int64_t)0);
    c.deck_id = j.value("deck_id", (int64_t)0);
    c.notetype = j.value("notetype", "");
    c.kind = j.value("kind", "review");
    c.question = j.value("question", "");
    c.answer = j.value("answer", "");
    c.css = j.value("css", "");
    c.ord = j.value("ord", 0);
    c.flags = j.value("flags", 0);
    c.marked = j.value("marked", false);
    if (j.contains("labels")) c.labels = j["labels"].get<std::vector<std::string>>();
    c.prepared = j.value("prepared", false);
    return c;
}

class AnkiEngine : public Engine {
public:
    AnkiEngine() {
#ifdef ANKI3DS_SELFTEST
        call("init", {{"ca_file", "romfs:/cacert.pem"}, {"trace_file", "sdmc:/3ds/clamshell/bridge-trace.txt"}});
#else
        call("init", {{"ca_file", "romfs:/cacert.pem"}});
#endif
    }
    const char* name() const override { return "Anki backend"; }

    bool open(const std::string& path, std::string& err) override {
        path_ = path;
        open_ = !failed(call("open", {{"path", path}}), err);
        return open_;
    }
    bool is_open() const override { return open_; }
    void close() override {
        call("close");
        open_ = false;
    }
    void set_clock(std::function<int64_t()>, int32_t minutes_west) override {
        call("set_clock", {{"minutes_west", minutes_west}});
    }
    std::optional<int32_t> recorded_minutes_west() override {
        json r = call("utc_offset");
        if (r.contains("minutes_west") && r["minutes_west"].is_number()) return r["minutes_west"].get<int32_t>();
        return std::nullopt;
    }

    std::vector<DeckEntry> decks() override {
        std::vector<DeckEntry> out;
        json r = call("deck_tree");
        if (!r.contains("decks")) return out;
        for (auto& d : r["decks"]) {
            DeckEntry e;
            e.id = d.value("id", (int64_t)0);
            e.name = d.value("name", "");
            e.full_name = d.value("full_name", "");
            e.depth = std::max(0, d.value("level", 1) - 1);
            e.has_children = d.value("has_children", false);
            e.collapsed = d.value("collapsed", false);
            e.counts.new_count = d.value("new", 0u);
            e.counts.learn_count = d.value("learn", 0u);
            e.counts.review_count = d.value("review", 0u);
            out.push_back(e);
        }
        return out;
    }
    std::string deck_name(int64_t id) override { return call("deck_info", {{"id", id}}).value("name", ""); }
    std::string deck_preset(int64_t id) override {
        json r = call("deck_info", {{"id", id}});
        return r.value("filtered", false) ? "" : r.value("preset", "");
    }

    bool start(int64_t deck_id, std::string& err) override {
        return !failed(call("set_deck", {{"id", deck_id}}), err);
    }
    std::vector<int64_t> todays_card_ids() override {
        json r = call("queue_ids", {{"limit", 200}});
        if (!r.contains("ids")) return {};
        return r["ids"].get<std::vector<int64_t>>();
    }
    std::optional<StudyCard> next(DeckCounts& counts, int64_t& next_id, std::string& err,
                                  int64_t prepared_id) override {
        next_id = 0;
        json r = call("next", {{"prepared_id", prepared_id}});
        if (failed(r, err)) return std::nullopt;
        if (r.contains("counts")) {
            counts.new_count = r["counts"].value("new", 0u);
            counts.learn_count = r["counts"].value("learn", 0u);
            counts.review_count = r["counts"].value("review", 0u);
        }
        next_id = r.value("next_id", (int64_t)0);
        if (!r.contains("card") || r["card"].is_null()) return std::nullopt;
        return card_from(r["card"]);
    }
    std::optional<StudyCard> render(int64_t card_id) override {
        json r = call("render", {{"card_id", card_id}});
        std::string err;
        if (failed(r, err)) return std::nullopt;
        return card_from(r);
    }
    bool answer(int rating, uint32_t millis, std::string& err) override {
        return !failed(call("answer", {{"rating", rating}, {"millis", millis}}), err);
    }
    bool can_undo() override { return call("can_undo").value("can_undo", false); }
    bool undo(bool& was_answer, std::string& err) override {
        json r = call("undo");
        if (failed(r, err)) return false;
        was_answer = r.value("was_answer", true);
        return true;
    }
    bool card_action(CardAction a, int flag, StudyCard& card, std::string& err) override {
        const char* op = a == CardAction::BuryCard      ? "bury"
                         : a == CardAction::BuryNote    ? "bury_note"
                         : a == CardAction::SuspendCard ? "suspend"
                         : a == CardAction::SuspendNote ? "suspend_note"
                         : a == CardAction::SetFlag     ? "flag"
                                                        : "mark";
        json r = call("card_op", {{"card_id", card.id}, {"op", op}, {"flag", flag}});
        if (failed(r, err)) return false;
        card.flags = r.value("flags", card.flags);
        card.marked = r.value("marked", card.marked);
        return true;
    }

    std::vector<std::string> media_names(const std::vector<int64_t>& card_ids) override {
        json r = call("media_refs", {{"card_ids", card_ids}});
        return r.contains("names") ? r["names"].get<std::vector<std::string>>() : std::vector<std::string>{};
    }
    std::vector<std::string> deck_media_names(int64_t deck_id) override {
        json r = call("media_refs", {{"deck_id", deck_id}});
        return r.contains("names") ? r["names"].get<std::vector<std::string>>() : std::vector<std::string>{};
    }
    int unsynced_reviews() override { return call("unsynced_reviews").value("count", 0); }

    bool login(const std::string& endpoint, const std::string& user, const std::string& pass, std::string& hkey,
               std::string& endpoint_out, std::string& err) override {
        json r = call("login", {{"endpoint", endpoint}, {"username", user}, {"password", pass}});
        if (failed(r, err)) return false;
        hkey = r.value("hkey", "");
        endpoint_out = r.contains("endpoint") && r["endpoint"].is_string() ? r["endpoint"].get<std::string>() : endpoint;
        return true;
    }
    SyncOutcome sync(const std::string& endpoint, const std::string& hkey,
                     const std::function<void(const std::string&)>&) override {
        SyncOutcome o;
        json r = call("sync", {{"endpoint", endpoint}, {"hkey", hkey}});
        if (failed(r, o.error)) return o;
        std::string s = r.value("status", "");
        o.kind = s == "synced" ? SyncOutcome::Synced
                 : s == "no_changes" ? SyncOutcome::NoChanges
                                     : SyncOutcome::FullRequired;
        o.download_ok = r.value("download_ok", false);
        o.message = r.value("message", "");
        if (r.contains("new_endpoint") && r["new_endpoint"].is_string()) o.new_endpoint = r["new_endpoint"].get<std::string>();
        return o;
    }
    bool full_download(const std::string& endpoint, const std::string& hkey, const std::string& path,
                       const std::function<void(size_t, size_t)>&, std::string& err) override {
        if (!open_) call("set_path", {{"path", path}});
        bool ok = !failed(call("full_download", {{"endpoint", endpoint}, {"hkey", hkey}, {"path", path}}), err);
        open_ = ok || open_;
        return ok;
    }

private:
    std::string path_;
    bool open_ = false;
};

}  // namespace

std::unique_ptr<Engine> make_anki_engine() { return std::make_unique<AnkiEngine>(); }

}  // namespace anki

#else

namespace anki {
std::unique_ptr<Engine> make_anki_engine() { return nullptr; }
}  // namespace anki

#endif
