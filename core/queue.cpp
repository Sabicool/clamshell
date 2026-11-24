#include "queue.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>

#include "sqlite3.h"

namespace anki {

namespace {

// ---------------------------------------------------------------- limits.rs

struct Limit {
    int64_t new_left = 0, review_left = 0;
};

// Remaining limits for one deck today (used by the deck list).
Limit deck_limit(Collection& col, const Deck& d, uint32_t today) {
    const DeckConfig& c = col.config_for_deck(d);
    int64_t new_lim = c.new_per_day, rev_lim = c.reviews_per_day;
    if (d.has_new_limit_today && d.new_limit_today_day == today) new_lim = d.new_limit_today;
    else if (d.new_limit) new_lim = *d.new_limit;
    if (d.has_review_limit_today && d.review_limit_today_day == today) rev_lim = d.review_limit_today;
    else if (d.review_limit) rev_lim = *d.review_limit;
    int64_t new_studied = 0, rev_studied = 0;
    if ((uint32_t)d.common.u64(3) == today) {
        new_studied = (int32_t)d.common.u64(4);
        rev_studied = (int32_t)d.common.u64(5);
    }
    Limit l;
    l.new_left = std::max<int64_t>(new_lim - new_studied, 0);
    l.review_left = std::max<int64_t>(rev_lim - rev_studied, 0);
    return l;
}

struct Remaining {
    uint32_t review = 9999, newc = 9999;
    bool cap_new_to_review = false;
};

Remaining remaining_for(Collection& col, const Deck& d, uint32_t today, bool new_ignores_review) {
    Remaining r;
    if (d.filtered) return r;
    auto it = col.deck_configs.find(d.config_id);
    if (it == col.deck_configs.end()) return r;
    const DeckConfig& c = it->second;
    int32_t review_limit = (int32_t)c.reviews_per_day, new_limit = (int32_t)c.new_per_day;
    if (d.has_review_limit_today && d.review_limit_today_day == today) review_limit = (int32_t)d.review_limit_today;
    else if (d.review_limit) review_limit = (int32_t)*d.review_limit;
    if (d.has_new_limit_today && d.new_limit_today_day == today) new_limit = (int32_t)d.new_limit_today;
    else if (d.new_limit) new_limit = (int32_t)*d.new_limit;
    int32_t new_today = 0, review_today = 0;
    if ((uint32_t)d.common.u64(3) == today) {
        new_today = (int32_t)d.common.u64(4);
        review_today = (int32_t)d.common.u64(5);
    }
    review_limit -= review_today;
    new_limit -= new_today;
    if (!new_ignores_review) {
        review_limit -= new_today;
        new_limit = std::min<int32_t>(new_limit, review_limit);
    }
    r.review = (uint32_t)std::max<int32_t>(review_limit, 0);
    r.newc = (uint32_t)std::max<int32_t>(new_limit, 0);
    r.cap_new_to_review = !new_ignores_review;
    return r;
}

struct LimitTree {
    struct Node {
        int level;
        Remaining lim;
        int parent;
        std::vector<int> children;
    };
    std::vector<Node> nodes;
    std::unordered_map<int64_t, int> map;

    void cap(Remaining& a, const Remaining& b) {
        a.review = std::min(a.review, b.review);
        a.newc = std::min(a.newc, b.newc);
    }
    void build(Collection& col, const std::vector<const Deck*>& decks, uint32_t today, bool nir) {
        auto level = [](const Deck* d) { return (int)std::count(d->name.begin(), d->name.end(), '\x1f') + 1; };
        nodes.push_back({level(decks[0]), remaining_for(col, *decks[0], today, nir), -1, {}});
        map[decks[0]->id] = 0;
        size_t i = 1;
        std::function<void(int)> add_children = [&](int parent) {
            while (i < decks.size()) {
                int l = level(decks[i]);
                int pl = nodes[parent].level;
                if (l <= pl) break;
                if (l == pl + 1) {
                    Remaining r = remaining_for(col, *decks[i], today, nir);
                    cap(r, nodes[parent].lim);
                    int id = (int)nodes.size();
                    nodes.push_back({l, r, parent, {}});
                    nodes[parent].children.push_back(id);
                    map[decks[i]->id] = id;
                    i++;
                } else if (!nodes[parent].children.empty()) {
                    add_children(nodes[parent].children.back());
                } else {
                    i++;
                }
            }
        };
        add_children(0);
    }
    bool has(int64_t did) const { return map.count(did) > 0; }
    uint32_t get(int node, bool is_new) const { return is_new ? nodes[node].lim.newc : nodes[node].lim.review; }
    bool root_reached(bool is_new) const { return get(0, is_new) == 0; }
    // rslib returns an error for decks missing from the map; treat as reached
    bool reached(int64_t did, bool is_new) const {
        auto it = map.find(did);
        return it == map.end() || get(it->second, is_new) == 0;
    }
    void cap_desc(int node, const Remaining& l) {
        cap(nodes[node].lim, l);
        for (int c : nodes[node].children) cap_desc(c, l);
    }
    void decrement(int64_t did, bool is_new) {
        auto it = map.find(did);
        if (it == map.end()) return;
        for (int n = it->second; n >= 0; n = nodes[n].parent) {
            Remaining& r = nodes[n].lim;
            Remaining before = r;
            if (!is_new) {
                r.review = r.review > 0 ? r.review - 1 : 0;
                if (r.cap_new_to_review) r.newc = std::min(r.newc, r.review);
            } else {
                r.newc = r.newc > 0 ? r.newc - 1 : 0;
            }
            bool zero = (before.review > 0 && r.review == 0) || (before.newc > 0 && r.newc == 0);
            if (zero) cap_desc(n, r);
        }
    }
};

struct Bury {
    bool bury_new = false, bury_reviews = false, bury_il = false;
};

// ---------------------------------------------------------------- iterators

template <class T>
std::vector<T> intersperse(const std::vector<T>& one, const std::vector<T>& two) {
    std::vector<T> out;
    size_t i1 = 0, i2 = 0;
    float ratio = (float)(one.size() + 1) / (float)(two.size() + 1);
    while (i1 < one.size() || i2 < two.size()) {
        if (i1 < one.size() && i2 < two.size()) {
            float rel = (float)(i2 + 1) * ratio;
            if (rel < (float)(i1 + 1)) out.push_back(two[i2++]);
            else out.push_back(one[i1++]);
        } else if (i1 < one.size()) {
            out.push_back(one[i1++]);
        } else {
            out.push_back(two[i2++]);
        }
    }
    return out;
}

template <class T>
std::vector<T> chain(const std::vector<T>& a, const std::vector<T>& b) {
    std::vector<T> out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

uint64_t fnv_i64s(std::initializer_list<int64_t> vals) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int64_t v : vals) {
        unsigned char b[8];
        memcpy(b, &v, 8);
        for (unsigned char c : b) {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
    }
    return h;
}

bool learn_less(const StudyQueue::LearnEntry& a, const StudyQueue::LearnEntry& b) {
    // cmp_by_reps_then_due: (reps == 0, due)
    bool az = a.reps == 0, bz = b.reps == 0;
    if (az != bz) return !az && bz;
    return a.due < b.due;
}

}  // namespace

// ---------------------------------------------------------------- builder

void StudyQueue::build() {
    main_.clear();
    learning_.clear();
    counts_ = DeckCounts();
    SchedTimingToday t = col_.timing_today();
    col_.unbury_if_day_rolled_over(t);
    learn_ahead_secs_ = col_.config_int("collapseTime", 1200);
    const Deck* root = col_.deck(deck_id_);
    if (!root) return;
    bool nir = col_.config_bool("newCardsIgnoreReviewLimit", false);
    bool all_parents = col_.config_bool("applyAllParentLimits", false);
    bool fsrs = col_.config_bool("fsrs", false);
    sqlite3* db = col_.db();

    // active_decks: the selected deck and its children
    {
        Db d{db};
        d.exec("drop table if exists active_decks; create temporary table active_decks (id integer not null unique);");
        Stmt s(db, "insert into active_decks select id from decks where name = ?1 or (name >= ?2 and name < ?3)");
        s.bind(1, root->name).bind(2, root->name + "\x1f").bind(3, root->name + "\x20");
        s.step();
        d.h = nullptr;
    }
    // limit tree over [parents..., root, children sorted by name]
    std::vector<const Deck*> decks;
    {
        Stmt s(db, "select id from decks where name >= ?1 and name < ?2 order by name");
        s.bind(1, root->name + "\x1f").bind(2, root->name + "\x20");
        decks.push_back(root);
        while (s.step())
            if (const Deck* d = col_.deck(s.i64(0))) decks.push_back(d);
        if (all_parents)
            for (int64_t pid : col_.parent_deck_ids(deck_id_))
                if (const Deck* p = col_.deck(pid)) decks.insert(decks.begin(), p);
    }
    LimitTree limits;
    limits.build(col_, decks, t.days_elapsed, nir);

    // sort options come from the selected deck's preset
    int review_order = 0, gather = 0, new_sort = 1, il_mix = 0, new_mix = 0;
    {
        auto it = root->filtered ? col_.deck_configs.end() : col_.deck_configs.find(root->config_id);
        if (it != col_.deck_configs.end()) {
            review_order = it->second.review_order;
            gather = it->second.new_card_gather_priority;
            new_sort = it->second.new_card_sort_order;
            il_mix = it->second.interday_learning_mix;
            new_mix = it->second.new_mix;
        }
    }
    // burying state per note (get_and_update_bury_mode_for_note)
    std::unordered_map<int64_t, Bury> seen;
    auto bury_mode_for_deck = [&](int64_t did) {
        Bury b;
        const Deck* d = col_.deck(did);
        if (d && !d->filtered) {
            auto it = col_.deck_configs.find(d->config_id);
            if (it != col_.deck_configs.end()) {
                b.bury_new = it->second.bury_new;
                b.bury_reviews = it->second.bury_reviews;
                b.bury_il = it->second.bury_interday_learning;
            }
        }
        return b;
    };
    auto update_bury = [&](int64_t nid, int64_t home_did) -> std::optional<Bury> {
        Bury mode = bury_mode_for_deck(home_did);
        auto it = seen.find(nid);
        if (it == seen.end()) {
            seen[nid] = mode;
            return std::nullopt;
        }
        Bury prev = it->second;
        it->second.bury_new |= mode.bury_new;
        it->second.bury_reviews |= mode.bury_reviews;
        it->second.bury_il |= mode.bury_il;
        return prev;
    };

    struct Due {
        int64_t id, nid, due, ivl, mtime, did, odid;
        uint32_t reps;
    };
    struct New {
        int64_t id, nid, ord, mtime, did, odid;
        uint64_t hash;
    };
    std::vector<Due> learning, day_learning, review;
    std::vector<New> news;

    // 1. intraday learning (due before the next day starts)
    {
        Stmt s(db,
               "select id, nid, due, cast(mod as integer), did, odid, reps from cards where did in (select id from "
               "active_decks) and (queue in (1, 4) and due <= ?)");
        s.bind(1, t.next_day_at);
        while (s.step()) {
            Due c{s.i64(0), s.i64(1), s.i64(2), 0, s.i64(3), s.i64(4), s.i64(5), (uint32_t)s.i64(6)};
            update_bury(c.nid, c.odid ? c.odid : c.did);
            learning.push_back(c);
        }
    }
    // 2/3. interday learning, then reviews, in the preset's review order
    auto order_sql = [&]() {
        std::vector<std::string> sub;
        std::string today = std::to_string(t.days_elapsed), nda = std::to_string(t.next_day_at),
                    now = std::to_string(t.now);
        auto rfsrs = [&](const char* ord) {
            return "extract_fsrs_retrievability(data, case when odue !=0 then odue else due end, ivl, " + today + ", " +
                   nda + ", " + now + ") " + ord;
        };
        switch (review_order) {
            case 0: sub = {"due"}; break;
            case 1: sub = {"due", "(select rowid from active_decks ad where ad.id = did)"}; break;
            case 2: sub = {"(select rowid from active_decks ad where ad.id = did)", "due"}; break;
            case 3: sub = {"ivl asc"}; break;
            case 4: sub = {"ivl desc"}; break;
            case 5: sub = {fsrs ? "extract_fsrs_variable(data, 'd') desc" : "factor asc"}; break;
            case 6: sub = {fsrs ? "extract_fsrs_variable(data, 'd') asc" : "factor desc"}; break;
            case 7: sub = {rfsrs("asc")}; break;
            case 11: sub = {rfsrs("desc")}; break;
            case 12:
                sub = {fsrs ? "extract_fsrs_relative_retrievability(data, case when odue !=0 then odue else due end, ivl, " +
                                  today + ", " + nda + ", " + now + ") asc"
                            : "-(1 + cast(" + today + "-due+0.001 as real)/ivl) asc"};
                break;
            case 8: break;
            case 9: sub = {"nid asc, ord asc"}; break;
            case 10: sub = {"nid desc, ord asc"}; break;
        }
        sub.push_back("fnvhash(id, mod)");
        std::string o;
        for (size_t i = 0; i < sub.size(); i++) o += (i ? ", " : "") + sub[i];
        return o;
    };
    for (int pass = 0; pass < 2; pass++) {
        bool learning_pass = pass == 0;
        if (limits.root_reached(false)) break;
        std::string sql =
            "select id, nid, due, cast(ivl as integer), cast(mod as integer), did, odid, reps from cards where did in "
            "(select id from active_decks) and (queue = ? and due <= ?) order by " +
            order_sql();
        Stmt s(db, sql.c_str());
        s.bind(1, (int64_t)(learning_pass ? Q_DAY_LEARN : Q_REVIEW)).bind(2, (int64_t)t.days_elapsed);
        while (s.step()) {
            if (limits.root_reached(false)) break;
            Due c{s.i64(0), s.i64(1), s.i64(2), s.i64(3), s.i64(4), s.i64(5), s.i64(6), (uint32_t)s.i64(7)};
            if (limits.reached(c.did, false)) continue;
            auto prev = update_bury(c.nid, c.odid ? c.odid : c.did);
            bool bury = prev && (learning_pass ? prev->bury_il : prev->bury_reviews);
            if (bury) continue;
            (learning_pass ? day_learning : review).push_back(c);
            limits.decrement(c.did, false);
        }
    }
    // 4. new cards, gathered by priority
    {
        uint32_t salt = t.days_elapsed * 2654435761u;  // knuth_salt
        std::string by_deck_sort = "due ASC, ord ASC";
        std::string sorted;
        switch (gather) {
            case 1: sorted = "due ASC, ord ASC"; break;
            case 2: sorted = "due DESC, ord ASC"; break;
            case 3: sorted = "fnvhash(nid, " + std::to_string(salt) + "), ord ASC"; break;
            case 4: sorted = "fnvhash(id, " + std::to_string(salt) + ")"; break;
            case 5: by_deck_sort = "fnvhash(nid, " + std::to_string(salt) + "), ord ASC"; break;
            default: break;
        }
        auto take = [&](const Stmt& s) {
            New c{s.i64(0), s.i64(1), s.i64(2), s.i64(3), s.i64(4), s.i64(5), 0};
            if (limits.reached(c.did, true)) return;
            auto prev = update_bury(c.nid, c.odid ? c.odid : c.did);
            if (prev && prev->bury_new) return;
            news.push_back(c);
            limits.decrement(c.did, true);
        };
        if (gather == 0 || gather == 5) {
            std::vector<int64_t> active;
            Stmt d(db, "select id from decks where id in (select id from active_decks) order by name");
            while (d.step()) active.push_back(d.i64(0));
            for (int64_t did : active) {
                if (limits.root_reached(true)) break;
                if (limits.reached(did, true)) continue;
                std::string sql = "select id, nid, ord, cast(mod as integer), did, odid from cards where did = ? and "
                                  "queue = 0 order by " + by_deck_sort;
                Stmt s(db, sql.c_str());
                s.bind(1, did);
                while (s.step()) {
                    if (limits.reached(did, true)) break;
                    take(s);
                }
            }
        } else {
            std::string sql = "select id, nid, ord, cast(mod as integer), did, odid from cards where did in (select id "
                              "from active_decks) and queue = 0 order by " + sorted;
            Stmt s(db, sql.c_str());
            while (s.step()) {
                if (limits.root_reached(true)) break;
                take(s);
            }
        }
    }
    // sort_new
    int64_t day = t.days_elapsed;
    switch (new_sort) {
        case 0:
            std::stable_sort(news.begin(), news.end(), [](const New& a, const New& b) { return a.ord < b.ord; });
            break;
        case 2:
            for (auto& c : news) c.hash = fnv_i64s({c.id, day});
            std::sort(news.begin(), news.end(), [](const New& a, const New& b) {
                return a.ord != b.ord ? a.ord < b.ord : a.hash < b.hash;
            });
            break;
        case 3:
            for (auto& c : news) c.hash = fnv_i64s({c.nid, day});
            std::sort(news.begin(), news.end(), [](const New& a, const New& b) {
                return a.hash != b.hash ? a.hash < b.hash : a.ord < b.ord;
            });
            break;
        case 4:
            for (auto& c : news) c.hash = fnv_i64s({c.id, day});
            std::sort(news.begin(), news.end(), [](const New& a, const New& b) { return a.hash < b.hash; });
            break;
        default: break;  // NoSort
    }
    // build()
    std::vector<LearnEntry> lq;
    for (auto& c : learning) lq.push_back({c.due, c.id, c.mtime, c.reps});
    std::stable_sort(lq.begin(), lq.end(), learn_less);
    learning_.assign(lq.begin(), lq.end());
    int64_t now = col_.now_secs();
    int64_t cutoff = now + learn_ahead_secs_;
    uint32_t learn_count = 0;
    for (auto& e : learning_)
        if (e.due <= cutoff) learn_count++;
    counts_.learn_count = learn_count + (uint32_t)day_learning.size();
    counts_.review_count = (uint32_t)review.size();
    counts_.new_count = (uint32_t)news.size();
    std::vector<MainEntry> rev_m, il_m, new_m;
    for (auto& c : review) rev_m.push_back({c.id, c.mtime, 1});
    for (auto& c : day_learning) il_m.push_back({c.id, c.mtime, 2});
    for (auto& c : news) new_m.push_back({c.id, c.mtime, 0});
    std::vector<MainEntry> with_il = il_mix == 1   ? chain(rev_m, il_m)
                                     : il_mix == 2 ? chain(il_m, rev_m)
                                                   : intersperse(rev_m, il_m);
    std::vector<MainEntry> all = new_mix == 2   ? chain(new_m, with_il)
                                 : new_mix == 1 ? chain(with_il, new_m)
                                                : intersperse(with_il, new_m);
    main_.assign(all.begin(), all.end());
    learning_cutoff_ = now;
}

// ---------------------------------------------------------------- CardQueues

std::vector<QueuedCard> StudyQueue::order(size_t limit) const {
    // intraday_now_iter, main, intraday_ahead_iter
    std::vector<QueuedCard> out;
    for (auto& e : learning_)
        if (e.due <= learning_cutoff_ && out.size() < limit) out.push_back({e.id, "learn"});
    for (auto& e : main_) {
        if (out.size() >= limit) break;
        out.push_back({e.id, e.kind == 0 ? "new" : e.kind == 1 ? "review" : "learn"});
    }
    int64_t ahead = learning_cutoff_ + learn_ahead_secs_;
    for (auto& e : learning_)
        if (e.due > learning_cutoff_ && e.due <= ahead && out.size() < limit) out.push_back({e.id, "learn"});
    return out;
}

std::optional<QueuedCard> StudyQueue::next() {
    auto o = order(1);
    if (o.empty()) return std::nullopt;
    return o[0];
}

std::optional<QueuedCard> StudyQueue::peek(size_t i) {
    auto o = order(i + 1);
    if (i < o.size()) return o[i];
    return std::nullopt;
}

void StudyQueue::update_learning_cutoff_and_count() {
    int64_t last_ahead = learning_cutoff_ + learn_ahead_secs_;
    learning_cutoff_ = col_.now_secs();
    int64_t new_ahead = learning_cutoff_ + learn_ahead_secs_;
    for (auto& e : learning_)
        if (e.due > last_ahead && e.due <= new_ahead) counts_.learn_count++;
}

void StudyQueue::insert_learning(LearnEntry e) {
    if (e.due <= learning_cutoff_ + learn_ahead_secs_) counts_.learn_count++;
    // binary_search_by(cmp_by_reps_then_due): any equal position, else the insertion point
    size_t lo = 0, hi = learning_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (learn_less(learning_[mid], e)) lo = mid + 1;
        else hi = mid;
    }
    learning_.insert(learning_.begin() + lo, e);
}

void StudyQueue::on_answered(const Card& c) {
    // pop_entry
    auto it = std::find_if(learning_.begin(), learning_.end(), [&](const LearnEntry& e) { return e.id == c.id; });
    if (it != learning_.end()) {
        learning_.erase(it);
        counts_.learn_count = counts_.learn_count ? counts_.learn_count - 1 : 0;
    } else if (!main_.empty() && main_.front().id == c.id) {
        int k = main_.front().kind;
        main_.pop_front();
        if (k == 0) counts_.new_count--;
        else if (k == 1) counts_.review_count--;
        else counts_.learn_count = counts_.learn_count ? counts_.learn_count - 1 : 0;
    } else {
        // not where rslib expects it (e.g. after undo): drop it wherever it is
        main_.erase(std::remove_if(main_.begin(), main_.end(), [&](const MainEntry& e) { return e.id == c.id; }),
                    main_.end());
    }
    // maybe_requeue_learning_card
    SchedTimingToday t = col_.timing_today();
    if ((c.queue == Q_LEARN || c.queue == Q_PREVIEW) && c.due < t.next_day_at) {
        LearnEntry e{c.due, c.id, c.mtime, c.reps};
        int64_t cutoff = learning_cutoff_ + learn_ahead_secs_;
        if (e.due <= cutoff && main_.empty() && !learning_.empty()) {
            const LearnEntry& next = learning_.front();
            if (next.due >= e.due && next.due + 1 < cutoff) e.due = next.due + 1;
        }
        insert_learning(e);
    }
    update_learning_cutoff_and_count();
}

void StudyQueue::push_front(const QueuedCard& q) {
    auto c = col_.get_card(q.card_id);
    if (!c) return;
    on_answered(*c);  // remove it if it was requeued
    if (c->queue == Q_LEARN || c->queue == Q_PREVIEW) {
        learning_.push_front({std::min<int64_t>(c->due, learning_cutoff_), c->id, c->mtime, c->reps});
        counts_.learn_count++;
    } else {
        int kind = c->queue == Q_NEW ? 0 : c->queue == Q_REVIEW ? 1 : 2;
        main_.push_front({c->id, c->mtime, kind});
        if (kind == 0) counts_.new_count++;
        else if (kind == 1) counts_.review_count++;
        else counts_.learn_count++;
    }
}

DeckCounts StudyQueue::counts() {
    if (counts_.new_count == 0 && counts_.learn_count == 0 && counts_.review_count == 0)
        update_learning_cutoff_and_count();
    return counts_;
}

DeckCounts deck_counts(Collection& col, int64_t deck_id) {
    StudyQueue q(col, deck_id);
    q.build();
    return q.counts();
}

std::map<int64_t, DeckCounts> all_deck_counts(Collection& col) {
    SchedTimingToday t = col.timing_today();
    int64_t learn_cutoff = t.now + col.config_int("collapseTime", 1200);
    std::map<int64_t, DeckCounts> raw;
    {
        Stmt s(col.db(),
               "select did, queue, count() from cards where (queue = 0) or (queue in (2, 3) and due <= ?1) "
               "or (queue in (1, 4) and due < ?2) group by did, queue");
        s.bind(1, (int64_t)t.days_elapsed).bind(2, learn_cutoff);
        while (s.step()) {
            DeckCounts& d = raw[s.i64(0)];
            int q = (int)s.i64(1);
            uint32_t n = (uint32_t)s.i64(2);
            if (q == 0) d.new_count += n;
            else if (q == 2) d.review_count += n;
            else d.learn_count += n;
        }
    }
    // children first: sort deck ids by name depth, deepest first
    std::vector<const Deck*> order;
    for (auto& [id, d] : col.decks) order.push_back(&d);
    auto depth = [](const Deck* d) { return std::count(d->name.begin(), d->name.end(), '\x1f'); };
    std::sort(order.begin(), order.end(), [&](const Deck* a, const Deck* b) { return depth(a) > depth(b); });
    std::map<std::string, int64_t> by_name;
    for (auto& [id, d] : col.decks) by_name[d.name] = id;
    std::map<int64_t, DeckCounts> out;
    for (const Deck* d : order) {
        DeckCounts c = raw[d->id];
        DeckCounts& acc = out[d->id];
        acc.new_count += c.new_count;
        acc.review_count += c.review_count;
        acc.learn_count += c.learn_count;
        if (!d->filtered) {
            Limit l = deck_limit(col, *d, t.days_elapsed);
            acc.new_count = (uint32_t)std::min<int64_t>(acc.new_count, l.new_left);
            acc.review_count = (uint32_t)std::min<int64_t>(acc.review_count, l.review_left);
        }
        size_t p = d->name.rfind('\x1f');
        if (p != std::string::npos) {
            auto it = by_name.find(d->name.substr(0, p));
            if (it != by_name.end()) {
                DeckCounts& parent = out[it->second];
                parent.new_count += acc.new_count;
                parent.review_count += acc.review_count;
                parent.learn_count += acc.learn_count;
            }
        }
    }
    return out;
}

void record_studied(Collection& col, int64_t deck_id, bool was_new, bool was_review, uint32_t millis) {
    uint32_t today = col.timing_today().days_elapsed;
    std::vector<int64_t> ids{deck_id};
    for (int64_t p : col.parent_deck_ids(deck_id)) ids.push_back(p);
    for (int64_t id : ids) {
        auto it = col.decks.find(id);
        if (it == col.decks.end()) continue;
        Deck& d = it->second;
        pb::Message& c = d.common;
        if ((uint32_t)c.u64(3) != today) {
            c.set_varint(3, today);
            c.set_varint(4, 0);
            c.set_varint(5, 0);
            c.set_varint(6, 0);
            c.set_varint(7, 0);
        }
        auto bump = [&](uint32_t field, int64_t delta) {
            int64_t v = (int32_t)c.u64(field) + delta;
            c.set_varint(field, (uint64_t)(int64_t)(int32_t)v);
        };
        if (was_new) bump(4, 1);
        if (was_review) bump(5, 1);
        bump(7, millis);
        d.mtime = col.now_secs();
        d.usn = -1;
        Stmt s(col.db(), "update decks set common = ?, mtime_secs = ?, usn = -1 where id = ?");
        s.bind_blob(1, c.encode()).bind(2, d.mtime).bind(3, id);
        s.step();
    }
}

}  // namespace anki

namespace anki {
std::vector<int64_t> StudyQueue::card_ids() const {
    std::vector<int64_t> out;
    for (auto& e : main_) out.push_back(e.id);
    for (auto& e : learning_) out.push_back(e.id);
    return out;
}
}  // namespace anki
