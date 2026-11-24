// Study queue: decides which card to show next for a deck, honouring the
// preset's daily limits, learn-ahead, sibling burying and new/review mixing.
// This follows the v3 scheduler's structure (rslib/src/scheduler/queue) but is
// a simplified re-implementation; per-card scheduling (states.cpp) is exact.
#pragma once
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "collection.h"

namespace anki {

struct DeckCounts {
    uint32_t new_count = 0, learn_count = 0, review_count = 0;
};

struct QueuedCard {
    int64_t card_id;
    std::string kind;  // "new", "learn", "review", "daylearn"
};

class StudyQueue {
public:
    StudyQueue(Collection& col, int64_t deck_id) : col_(col), deck_id_(deck_id) {}
    void build();
    std::optional<QueuedCard> next();          // card to show now (learning cards when due)
    std::optional<QueuedCard> peek(size_t i);  // for debugging: i-th card of the combined order
    void on_answered(const Card& updated);     // remove/requeue after an answer
    DeckCounts counts() const;

private:
    Collection& col_;
    int64_t deck_id_;
    std::deque<QueuedCard> main_;        // reviews, interday learning, new (mixed)
    std::vector<std::pair<int64_t, int64_t>> learning_;  // (due_secs, cid) intraday
    uint32_t learn_ahead_secs_ = 1200;
    void sort_learning();
};

DeckCounts deck_counts(Collection& col, int64_t deck_id);

// Counts for every deck at once (one query), applying each deck's daily
// limits bottom-up like the deck list on desktop. Cheap enough for the 3DS.
std::map<int64_t, DeckCounts> all_deck_counts(Collection& col);

// Bumps the deck's "studied today" counters locally (not synced; see README).
void record_studied(Collection& col, int64_t deck_id, bool was_new, bool was_review, uint32_t millis);

}  // namespace anki
