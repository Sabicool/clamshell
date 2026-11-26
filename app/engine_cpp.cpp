// Engine on Clamshell's C++ port of Anki (core/): the desktop simulator and
// tests use it, and so does the 3DS build when not using Anki's backend.
#include <algorithm>
#include <cctype>
#include <set>

#include "engine.h"
#include "media.h"
#include "sync.h"
#include "template.h"

namespace anki {

namespace {

class CppEngine : public Engine {
public:
    const char* name() const override { return "Clamshell C++ engine"; }

    bool open(const std::string& path, std::string& err) override { return col_.open(path, err); }
    bool is_open() const override { return col_.is_open(); }
    void close() override {
        queue_.reset();
        sched_.reset();
        col_.close();
    }
    void set_clock(std::function<int64_t()> utc_ms, int32_t minutes_west) override {
        col_.clock.now_millis = std::move(utc_ms);
        col_.clock.local_minutes_west = [minutes_west] { return minutes_west; };
    }
    std::optional<int32_t> recorded_minutes_west() override {
        if (!col_.is_open()) return std::nullopt;
        return (int32_t)col_.config_int("localOffset", 0);
    }

    std::vector<DeckEntry> decks() override {
        std::vector<DeckEntry> out;
        if (!col_.is_open()) return out;
        auto counts = all_deck_counts(col_);
        std::vector<const Deck*> list;
        for (auto& [id, d] : col_.decks) list.push_back(&d);
        std::sort(list.begin(), list.end(), [](const Deck* a, const Deck* b) {
            // case-insensitive, component-wise
            std::string x = a->name, y = b->name;
            for (auto& ch : x) ch = (char)tolower((unsigned char)ch);
            for (auto& ch : y) ch = (char)tolower((unsigned char)ch);
            return x < y;
        });
        for (const Deck* d : list) {
            bool has_children = false;
            for (const Deck* o : list)
                if (o->name.size() > d->name.size() && o->name.compare(0, d->name.size(), d->name) == 0 &&
                    o->name[d->name.size()] == '\x1f') {
                    has_children = true;
                    break;
                }
            // skip the empty "Default" deck like desktop does when other decks exist
            if (d->id == 1 && list.size() > 1) {
                Stmt s(col_.db(), "select 1 from cards where did = 1 limit 1");
                if (!s.step()) continue;
            }
            size_t p = d->name.rfind('\x1f');
            DeckEntry e;
            e.id = d->id;
            e.name = p == std::string::npos ? d->name : d->name.substr(p + 1);
            e.full_name = d->human_name();
            e.depth = (int)std::count(d->name.begin(), d->name.end(), '\x1f');
            e.has_children = has_children;
            e.collapsed = d->common.boolean(1);
            e.counts = counts[d->id];
            out.push_back(e);
        }
        return out;
    }
    std::string deck_name(int64_t id) override {
        const Deck* d = col_.deck(id);
        return d ? d->human_name() : "";
    }
    std::string deck_preset(int64_t id) override {
        const Deck* d = col_.deck(id);
        if (!d || d->filtered) return "";
        return col_.config_for_deck(*d).name;
    }

    bool start(int64_t deck_id, std::string&) override {
        queue_ = std::make_unique<StudyQueue>(col_, deck_id);
        queue_->build();
        sched_ = std::make_unique<Scheduler>(col_);
        sched_->start_session(true);
        deck_undo_.clear();
        return true;
    }
    std::vector<int64_t> todays_card_ids() override { return queue_ ? queue_->card_ids() : std::vector<int64_t>{}; }

    std::optional<StudyCard> next(DeckCounts& counts, int64_t& next_id, std::string&, int64_t) override {
        next_id = 0;
        card_.reset();
        if (!queue_) return std::nullopt;
        auto q = queue_->next();
        if (q) card_ = col_.get_card(q->card_id);
        if (q && !card_) {
            // the card changed underneath us (e.g. a sync): start over
            queue_->build();
            q = queue_->next();
            if (q) card_ = col_.get_card(q->card_id);
        }
        counts = queue_->counts();
        if (!q || !card_) return std::nullopt;
        states_ = sched_->scheduling_states(*card_);
        auto out = describe(*card_);
        out.kind = q->kind;
        out.labels = sched_->describe_next_states(states_);
        if (auto n = queue_->peek(1)) next_id = n->card_id;
        return out;
    }
    std::optional<StudyCard> render(int64_t card_id) override {
        auto c = col_.get_card(card_id);
        if (!c) return std::nullopt;
        return describe(*c);
    }

    bool answer(int rating, uint32_t millis, std::string& err) override {
        if (!card_ || !sched_) {
            err = "no card is being studied";
            return false;
        }
        int original_queue = card_->queue;
        int64_t did = card_->did;
        // remember the deck counters so undo can put them back
        std::vector<std::pair<int64_t, std::string>> saved;
        std::vector<int64_t> ids{did};
        for (int64_t p : col_.parent_deck_ids(did)) ids.push_back(p);
        for (int64_t id : ids)
            if (const Deck* d = col_.deck(id)) saved.push_back({id, d->common.encode()});
        // the deck's studied-today counters go in the same transaction: one SD card commit per answer
        auto studied = [&] {
            record_studied(col_, did, original_queue == Q_NEW,
                           original_queue == Q_REVIEW || original_queue == Q_DAY_LEARN, millis);
        };
        if (!sched_->answer(card_->id, (Rating)rating, millis, err, &states_, studied)) return false;
        deck_undo_.push_back(saved);
        if (deck_undo_.size() > 30) deck_undo_.erase(deck_undo_.begin());
        if (auto updated = col_.get_card(card_->id)) queue_->on_answered(*updated);
        return true;
    }
    bool can_undo() override { return sched_ && sched_->can_undo(); }
    bool undo(bool& was_answer, std::string& err) override {
        was_answer = false;
        if (!can_undo()) return false;
        auto cid = sched_->undo(err);
        if (!cid) return false;
        was_answer = true;
        if (!deck_undo_.empty()) {
            was_answer = !deck_undo_.back().empty();  // card actions push an empty entry
            for (auto& [id, blob] : deck_undo_.back()) {
                auto it = col_.decks.find(id);
                if (it == col_.decks.end()) continue;
                it->second.common = pb::Message(blob);
                Stmt s(col_.db(), "update decks set common = ? where id = ?");
                s.bind_blob(1, blob).bind(2, id);
                s.step();
            }
            deck_undo_.pop_back();
        }
        queue_->build();
        auto c = col_.get_card(*cid);
        std::string kind = !c ? "review" : c->queue == Q_NEW ? "new" : c->queue == Q_REVIEW ? "review" : "learn";
        queue_->push_front({*cid, kind});
        return true;
    }
    bool card_action(CardAction a, int flag, StudyCard& card, std::string& err) override {
        using Op = Scheduler::CardOp;
        Op op = a == CardAction::BuryCard      ? Op::BuryCard
                : a == CardAction::BuryNote    ? Op::BuryNote
                : a == CardAction::SuspendCard ? Op::SuspendCard
                : a == CardAction::SuspendNote ? Op::SuspendNote
                : a == CardAction::SetFlag     ? Op::SetFlag
                                               : Op::ToggleMark;
        if (!sched_ || !sched_->card_op(card.id, op, flag, err)) return false;
        deck_undo_.push_back({});  // keeps the deck-counter undo stack in step with the scheduler's
        if (deck_undo_.size() > 30) deck_undo_.erase(deck_undo_.begin());
        if (a == CardAction::BuryCard || a == CardAction::BuryNote || a == CardAction::SuspendCard ||
            a == CardAction::SuspendNote)
            queue_->build();
        if (auto c = col_.get_card(card.id)) {
            card_ = c;
            auto fresh = describe(*c);
            card.flags = fresh.flags;
            card.marked = fresh.marked;
        }
        return true;
    }

    std::vector<std::string> media_names(const std::vector<int64_t>& card_ids) override {
        return missing_media_for_cards(col_, card_ids, "");  // nothing is present in "": every name
    }
    std::vector<std::string> deck_media_names(int64_t deck_id) override {
        return missing_media_for_deck(col_, deck_id, "");
    }
    int unsynced_reviews() override {
        if (!col_.is_open()) return 0;
        Stmt s(col_.db(), "select count() from revlog where usn = -1");
        return s.step() ? (int)s.i64(0) : 0;
    }

    bool login(const std::string& endpoint, const std::string& user, const std::string& pass, std::string& hkey,
               std::string& endpoint_out, std::string& err) override {
        SyncClient client(endpoint);
        auto key = client.login(user, pass, err);
        if (!key) return false;
        hkey = *key;
        endpoint_out = client.endpoint;
        return true;
    }
    SyncOutcome sync(const std::string& endpoint, const std::string& hkey,
                     const std::function<void(const std::string&)>& progress) override {
        SyncClient client(endpoint);
        client.hkey = hkey;
        client.on_progress = progress;
        std::string err;
        SyncResult r = client.normal_sync(col_, err);
        SyncOutcome o;
        o.new_endpoint = r.new_endpoint;
        o.message = r.message;
        o.download_ok = r.download_ok;
        o.error = err.empty() ? r.message : err;
        switch (r.status) {
            case SyncStatus::Synced: o.kind = SyncOutcome::Synced; break;
            case SyncStatus::NoChanges: o.kind = SyncOutcome::NoChanges; break;
            case SyncStatus::FullSyncRequired: o.kind = SyncOutcome::FullRequired; break;
            default: o.kind = SyncOutcome::Failed;
        }
        if (o.kind != SyncOutcome::Failed) o.error.clear();
        queue_.reset();
        sched_.reset();
        return o;
    }
    bool full_download(const std::string& endpoint, const std::string& hkey, const std::string& path,
                       const std::function<void(size_t, size_t)>& bytes, std::string& err) override {
        SyncClient client(endpoint);
        client.hkey = hkey;
        client.on_bytes = bytes;
        queue_.reset();
        sched_.reset();
        SyncResult r = client.full_download(col_, path, err);
        return r.status == SyncStatus::Downloaded;
    }

private:
    Collection col_;
    std::unique_ptr<StudyQueue> queue_;
    std::unique_ptr<Scheduler> sched_;
    std::optional<Card> card_;
    SchedulingStates states_;
    std::vector<std::vector<std::pair<int64_t, std::string>>> deck_undo_;

    StudyCard describe(const Card& c) {
        StudyCard out;
        out.id = c.id;
        out.note_id = c.nid;
        out.deck_id = c.did;
        out.flags = c.flags;
        RenderedCard r = render_card(col_, c);
        out.question = std::move(r.question);
        out.answer = std::move(r.answer);
        out.css = std::move(r.css);
        out.ord = r.ord;
        out.kind = c.queue == Q_NEW ? "new" : c.queue == Q_REVIEW ? "review" : "learn";
        if (auto note = col_.get_note(c.nid)) {
            out.notetype_id = note->mid;
            std::string lower = " ";
            for (char ch : note->tags) lower += (char)tolower((unsigned char)ch);
            lower += " ";
            out.marked = lower.find(" marked ") != std::string::npos;
            auto nt = col_.notetypes.find(note->mid);
            if (nt != col_.notetypes.end()) out.notetype = nt->second.name;
        }
        return out;
    }
};

}  // namespace

std::unique_ptr<Engine> make_cpp_engine() { return std::make_unique<CppEngine>(); }

}  // namespace anki
