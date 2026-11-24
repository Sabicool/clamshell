#include "queue.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "rng.h"

namespace anki {

namespace {

struct Limit {
    int64_t new_left = 0, review_left = 0;
};

// Remaining limits for one deck today: deck override > preset, minus studied.
Limit deck_limit(Collection& col, const Deck& d, uint32_t today) {
    const DeckConfig& c = col.config_for_deck(d);
    int64_t new_lim = c.new_per_day, rev_lim = c.reviews_per_day;
    if (d.has_new_limit_today && d.new_limit_today_day == today) new_lim = d.new_limit_today;
    else if (d.new_limit) new_lim = *d.new_limit;
    if (d.has_review_limit_today && d.review_limit_today_day == today) rev_lim = d.review_limit_today;
    else if (d.review_limit) rev_lim = *d.review_limit;
    int64_t new_studied = 0, rev_studied = 0;
    if ((uint32_t)d.common.u64(3) == today) {
        new_studied = d.common.i64(4);
        rev_studied = d.common.i64(5);
    }
    Limit l;
    l.new_left = std::max<int64_t>(new_lim - new_studied, 0);
    l.review_left = std::max<int64_t>(rev_lim - rev_studied, 0);
    return l;
}

// Stable pseudo-random order that does not change within a day (like Anki's
// "random" sorts, which hash the card id with the day number).
uint64_t day_hash(int64_t id, uint32_t today) {
    StdRng r = StdRng::seed_from_u64((uint64_t)id ^ ((uint64_t)today << 32));
    return r.next_u64();
}

}  // namespace

void StudyQueue::build() {
    main_.clear();
    learning_.clear();
    SchedTimingToday t = col_.timing_today();
    col_.unbury_if_day_rolled_over(t);
    learn_ahead_secs_ = (uint32_t)col_.config_int("collapseTime", 1200);
    const Deck* root = col_.deck(deck_id_);
    if (!root) return;

    std::vector<int64_t> dids = col_.child_deck_ids(deck_id_);
    std::set<int64_t> didset(dids.begin(), dids.end());
    // Limit nodes: every deck in the tree, plus parents if applyAllParentLimits.
    std::unordered_map<int64_t, Limit> limits;
    for (int64_t did : dids) {
        const Deck* d = col_.deck(did);
        if (d && !d->filtered) limits[did] = deck_limit(col_, *d, t.days_elapsed);
    }
    std::vector<int64_t> extra_parents;
    if (col_.config_bool("applyAllParentLimits", false))
        for (int64_t p : col_.parent_deck_ids(deck_id_)) {
            const Deck* d = col_.deck(p);
            if (d && !d->filtered) {
                limits[p] = deck_limit(col_, *d, t.days_elapsed);
                extra_parents.push_back(p);
            }
        }
    bool new_ignores_review_limit = col_.config_bool("newCardsIgnoreReviewLimit", false);
    // Path from a card's deck up to the selected deck (and beyond, if enabled).
    std::unordered_map<int64_t, std::vector<int64_t>> path_cache;
    auto path_for = [&](int64_t did) -> const std::vector<int64_t>& {
        auto it = path_cache.find(did);
        if (it != path_cache.end()) return it->second;
        std::vector<int64_t> p{did};
        if (did != deck_id_)
            for (int64_t parent : col_.parent_deck_ids(did)) {
                p.push_back(parent);
                if (parent == deck_id_) break;
            }
        for (int64_t e : extra_parents) p.push_back(e);
        return path_cache[did] = p;
    };
    auto take = [&](int64_t did, bool is_new) -> bool {
        const auto& p = path_for(did);
        for (int64_t d : p) {
            auto it = limits.find(d);
            if (it == limits.end()) continue;
            if (is_new) {
                if (it->second.new_left <= 0) return false;
                if (!new_ignores_review_limit && it->second.review_left <= 0) return false;
            } else if (it->second.review_left <= 0) {
                return false;
            }
        }
        for (int64_t d : p) {
            auto it = limits.find(d);
            if (it == limits.end()) continue;
            if (is_new) {
                it->second.new_left--;
                if (!new_ignores_review_limit) it->second.review_left--;
            } else {
                it->second.review_left--;
            }
        }
        return true;
    };

    const DeckConfig& rootcfg = col_.config_for_deck(*root);
    std::set<int64_t> seen_notes_new, seen_notes_rev;
    bool bury_new = rootcfg.bury_new, bury_rev = rootcfg.bury_reviews, bury_il = rootcfg.bury_interday_learning;

    struct Cand { int64_t cid, nid, did; int32_t due; uint32_t ivl; uint16_t factor; int queue; int ord; int64_t pos; };
    std::vector<Cand> learn, daylearn, reviews, news;
    {
        Stmt s(col_.db(), "select id, nid, did, due, ivl, factor, queue, ord, odid, odue from cards where queue in (0,1,2,3,4)");
        int64_t learn_cutoff = t.now + learn_ahead_secs_;
        while (s.step()) {
            Cand c{s.i64(0), s.i64(1), s.i64(2), (int32_t)s.i64(3), (uint32_t)s.i64(4), (uint16_t)s.i64(5),
                   (int)s.i64(6), (int)s.i64(7), 0};
            if (!didset.count(c.did)) continue;
            switch (c.queue) {
                case Q_LEARN:
                case Q_PREVIEW:
                    if (c.due < learn_cutoff + 86400) learn.push_back(c);
                    break;
                case Q_DAY_LEARN:
                    if (c.due <= (int32_t)t.days_elapsed) daylearn.push_back(c);
                    break;
                case Q_REVIEW:
                    if (c.due <= (int32_t)t.days_elapsed) reviews.push_back(c);
                    break;
                case Q_NEW:
                    news.push_back(c);
                    break;
            }
        }
    }
    // Intraday learning: no limit, ordered by due time.
    for (auto& c : learn) learning_.push_back({c.due, c.cid});
    sort_learning();
    if (bury_il || bury_rev || bury_new)
        for (auto& c : learn) { seen_notes_new.insert(c.nid); seen_notes_rev.insert(c.nid); }

    // Interday learning (counts against the review limit).
    std::sort(daylearn.begin(), daylearn.end(), [](const Cand& a, const Cand& b) {
        return a.due != b.due ? a.due < b.due : a.cid < b.cid;
    });
    std::vector<QueuedCard> il_out;
    for (auto& c : daylearn) {
        if (bury_il && seen_notes_rev.count(c.nid)) continue;
        if (!take(c.did, false)) continue;
        il_out.push_back({c.cid, "daylearn"});
        seen_notes_rev.insert(c.nid);
    }

    // Reviews, in the preset's review order.
    uint32_t today = t.days_elapsed;
    auto deck_name = [&](int64_t did) { const Deck* d = col_.deck(did); return d ? d->name : std::string(); };
    switch (rootcfg.review_order) {
        case 1:  // day then deck
            std::sort(reviews.begin(), reviews.end(), [&](const Cand& a, const Cand& b) {
                if (a.due != b.due) return a.due < b.due;
                return deck_name(a.did) < deck_name(b.did);
            });
            break;
        case 2:  // deck then day
            std::sort(reviews.begin(), reviews.end(), [&](const Cand& a, const Cand& b) {
                auto na = deck_name(a.did), nb = deck_name(b.did);
                if (na != nb) return na < nb;
                return a.due < b.due;
            });
            break;
        case 3: std::sort(reviews.begin(), reviews.end(), [](const Cand& a, const Cand& b) { return a.ivl < b.ivl; }); break;
        case 4: std::sort(reviews.begin(), reviews.end(), [](const Cand& a, const Cand& b) { return a.ivl > b.ivl; }); break;
        case 5: std::sort(reviews.begin(), reviews.end(), [](const Cand& a, const Cand& b) { return a.factor < b.factor; }); break;
        case 6: std::sort(reviews.begin(), reviews.end(), [](const Cand& a, const Cand& b) { return a.factor > b.factor; }); break;
        case 8:
            std::sort(reviews.begin(), reviews.end(), [&](const Cand& a, const Cand& b) {
                return day_hash(a.cid, today) < day_hash(b.cid, today);
            });
            break;
        case 9: std::sort(reviews.begin(), reviews.end(), [](const Cand& a, const Cand& b) { return a.nid < b.nid; }); break;
        case 10: std::sort(reviews.begin(), reviews.end(), [](const Cand& a, const Cand& b) { return a.nid > b.nid; }); break;
        default:  // day (and retrievability orders, approximated by overdueness)
            std::sort(reviews.begin(), reviews.end(), [&](const Cand& a, const Cand& b) {
                if (a.due != b.due) return a.due < b.due;
                return day_hash(a.cid, today) < day_hash(b.cid, today);
            });
    }
    std::vector<QueuedCard> rev_out;
    for (auto& c : reviews) {
        if (bury_rev && seen_notes_rev.count(c.nid)) continue;
        if (!take(c.did, false)) continue;
        rev_out.push_back({c.cid, "review"});
        seen_notes_rev.insert(c.nid);
    }

    // New cards: gather by priority, then sort.
    std::sort(news.begin(), news.end(), [&](const Cand& a, const Cand& b) {
        switch (rootcfg.new_card_gather_priority) {
            case 1: return a.due != b.due ? a.due < b.due : a.ord < b.ord;  // lowest position
            case 2: return a.due != b.due ? a.due > b.due : a.ord < b.ord;  // highest position
            case 3: return day_hash(a.nid, today) < day_hash(b.nid, today);  // random notes
            case 4: return day_hash(a.cid, today) < day_hash(b.cid, today);  // random cards
            default: {  // deck order, then position
                auto na = deck_name(a.did), nb = deck_name(b.did);
                if (na != nb) return na < nb;
                return a.due != b.due ? a.due < b.due : a.ord < b.ord;
            }
        }
    });
    std::vector<Cand> new_taken;
    for (auto& c : news) {
        if (bury_new && (seen_notes_new.count(c.nid) || seen_notes_rev.count(c.nid))) continue;
        if (!take(c.did, true)) continue;
        new_taken.push_back(c);
        seen_notes_new.insert(c.nid);
    }
    switch (rootcfg.new_card_sort_order) {
        case 0:  // template, then gather order
            std::stable_sort(new_taken.begin(), new_taken.end(), [](const Cand& a, const Cand& b) { return a.ord < b.ord; });
            break;
        case 2:  // template then random
            std::sort(new_taken.begin(), new_taken.end(), [&](const Cand& a, const Cand& b) {
                if (a.ord != b.ord) return a.ord < b.ord;
                return day_hash(a.cid, today) < day_hash(b.cid, today);
            });
            break;
        case 3:  // random note then template
            std::sort(new_taken.begin(), new_taken.end(), [&](const Cand& a, const Cand& b) {
                if (a.nid != b.nid) return day_hash(a.nid, today) < day_hash(b.nid, today);
                return a.ord < b.ord;
            });
            break;
        case 4:
            std::sort(new_taken.begin(), new_taken.end(), [&](const Cand& a, const Cand& b) {
                return day_hash(a.cid, today) < day_hash(b.cid, today);
            });
            break;
        default: break;  // no sort
    }
    std::vector<QueuedCard> new_out;
    for (auto& c : new_taken) new_out.push_back({c.cid, "new"});

    // Interday learning mixed with reviews, then new cards per new_mix.
    auto intersperse = [](const std::vector<QueuedCard>& a, const std::vector<QueuedCard>& b) {
        std::vector<QueuedCard> out;
        if (a.empty()) return b;
        if (b.empty()) return a;
        double ratio = (double)(a.size() + 1) / (double)(b.size() + 1);
        size_t ia = 0, ib = 0;
        double next_b = ratio;
        while (ia < a.size() || ib < b.size()) {
            if (ib < b.size() && (ia >= a.size() || (double)(ia + 1) >= next_b)) {
                out.push_back(b[ib++]);
                next_b += ratio;
            } else {
                out.push_back(a[ia++]);
            }
        }
        return out;
    };
    std::vector<QueuedCard> rev_and_il;
    switch (rootcfg.interday_learning_mix) {
        case 1: rev_and_il = rev_out; rev_and_il.insert(rev_and_il.end(), il_out.begin(), il_out.end()); break;
        case 2: rev_and_il = il_out; rev_and_il.insert(rev_and_il.end(), rev_out.begin(), rev_out.end()); break;
        default: rev_and_il = intersperse(rev_out, il_out);
    }
    std::vector<QueuedCard> all;
    switch (rootcfg.new_mix) {
        case 1: all = rev_and_il; all.insert(all.end(), new_out.begin(), new_out.end()); break;
        case 2: all = new_out; all.insert(all.end(), rev_and_il.begin(), rev_and_il.end()); break;
        default: all = intersperse(rev_and_il, new_out);
    }
    main_.assign(all.begin(), all.end());
}

void StudyQueue::sort_learning() {
    std::sort(learning_.begin(), learning_.end());
}

std::optional<QueuedCard> StudyQueue::next() {
    int64_t now = col_.now_secs();
    if (!learning_.empty() && learning_.front().first <= now) return QueuedCard{learning_.front().second, "learn"};
    if (!main_.empty()) return main_.front();
    if (!learning_.empty() && learning_.front().first <= now + learn_ahead_secs_)
        return QueuedCard{learning_.front().second, "learn"};
    return std::nullopt;
}

std::optional<QueuedCard> StudyQueue::peek(size_t i) {
    if (i < learning_.size()) return QueuedCard{learning_[i].second, "learn"};
    i -= learning_.size();
    if (i < main_.size()) return main_[i];
    return std::nullopt;
}

void StudyQueue::on_answered(const Card& c) {
    learning_.erase(std::remove_if(learning_.begin(), learning_.end(),
                                   [&](const auto& p) { return p.second == c.id; }),
                    learning_.end());
    main_.erase(std::remove_if(main_.begin(), main_.end(), [&](const QueuedCard& q) { return q.card_id == c.id; }),
                main_.end());
    // Drop siblings that were just buried.
    {
        Stmt s(col_.db(), "select id from cards where nid = ? and queue = -3");
        s.bind(1, c.nid);
        std::set<int64_t> buried;
        while (s.step()) buried.insert(s.i64(0));
        if (!buried.empty())
            main_.erase(std::remove_if(main_.begin(), main_.end(),
                                       [&](const QueuedCard& q) { return buried.count(q.card_id) > 0; }),
                        main_.end());
    }
    if (c.queue == Q_LEARN || c.queue == Q_PREVIEW) {
        SchedTimingToday t = col_.timing_today();
        if (c.due < t.next_day_at) {
            learning_.push_back({c.due, c.id});
            sort_learning();
        }
    }
}

void StudyQueue::push_front(const QueuedCard& q) {
    learning_.erase(std::remove_if(learning_.begin(), learning_.end(),
                                   [&](const auto& p) { return p.second == q.card_id; }),
                    learning_.end());
    main_.erase(std::remove_if(main_.begin(), main_.end(), [&](const QueuedCard& x) { return x.card_id == q.card_id; }),
                main_.end());
    main_.push_front(q);
}

DeckCounts StudyQueue::counts() const {
    DeckCounts d;
    d.learn_count = (uint32_t)learning_.size();
    for (const auto& q : main_) {
        if (q.kind == "new") d.new_count++;
        else if (q.kind == "daylearn") d.learn_count++;
        else d.review_count++;
    }
    return d;
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
