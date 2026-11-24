// Study queue: which card to show next. A port of rslib/src/scheduler/queue
// (builder, gathering SQL, sorting, intersperser, learning requeue) and
// rslib/src/decks/limits.rs, so cards come up in the same order as desktop Anki.
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
    std::string kind;  // "new", "learn", "review"
};

class StudyQueue {
public:
    StudyQueue(Collection& col, int64_t deck_id) : col_(col), deck_id_(deck_id) {}
    void build();
    std::optional<QueuedCard> next();          // the card to show now
    std::optional<QueuedCard> peek(size_t i);  // i-th card in display order
    void on_answered(const Card& updated);     // pop + requeue learning, like rslib
    void push_front(const QueuedCard& q);      // show this card next (undo)
    DeckCounts counts();

    struct LearnEntry {
        int64_t due, id, mtime;
        uint32_t reps;
    };
    struct MainEntry {
        int64_t id, mtime;
        int kind;  // 0 new, 1 review, 2 interday learning
    };

private:
    Collection& col_;
    int64_t deck_id_;
    std::deque<MainEntry> main_;
    std::deque<LearnEntry> learning_;
    DeckCounts counts_;
    int64_t learn_ahead_secs_ = 1200;
    int64_t learning_cutoff_ = 0;
    std::vector<QueuedCard> order(size_t limit) const;
    void update_learning_cutoff_and_count();
    void insert_learning(LearnEntry e);
};

DeckCounts deck_counts(Collection& col, int64_t deck_id);

// Counts for every deck at once (one query), applying each deck's daily
// limits bottom-up like the deck list on desktop. Cheap enough for the 3DS.
std::map<int64_t, DeckCounts> all_deck_counts(Collection& col);

// Bumps the deck's (and its parents') "studied today" counters and marks
// them changed so they sync, like rslib's update_deck_stats.
void record_studied(Collection& col, int64_t deck_id, bool was_new, bool was_review, uint32_t millis);

}  // namespace anki
