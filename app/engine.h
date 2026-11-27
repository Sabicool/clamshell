// What the app needs from a collection: the deck list, studying, card
// actions, media references and sync. Two implementations:
//   - engine_anki.cpp: Anki's own backend (Rust, from the Anki fork's
//     3ds/clamshell-bridge), used on the 3DS when built with CLAMSHELL_ANKI;
//   - engine_cpp.cpp: Clamshell's C++ port in core/, used by the desktop
//     simulator and tests, and on the 3DS otherwise.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "queue.h"  // DeckCounts

namespace anki {

struct DeckEntry {
    int64_t id = 0;
    std::string name;       // last component
    std::string full_name;  // "Parent::Child"
    int depth = 0;
    bool has_children = false;
    bool collapsed = false;  // as saved in the collection
    DeckCounts counts;
};

struct StudyCard {
    int64_t id = 0, note_id = 0, notetype_id = 0, deck_id = 0;
    std::string notetype;
    std::string kind;  // "new", "learn", "review"
    std::string question, answer, css;
    int ord = 0;
    int flags = 0;
    bool marked = false;
    std::vector<std::string> labels;  // Again/Hard/Good/Easy intervals
    bool prepared = false;            // HTML left out: the app rendered this card already
};

enum class CardAction { BuryCard, BuryNote, SuspendCard, SuspendNote, SetFlag, ToggleMark };

struct SyncOutcome {
    enum Kind { Synced, NoChanges, FullRequired, Failed } kind = Failed;
    bool download_ok = false;
    std::string message;  // from the server
    std::string error;
    std::optional<std::string> new_endpoint;
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual const char* name() const = 0;

    virtual bool open(const std::string& path, std::string& err) = 0;
    virtual bool is_open() const = 0;
    virtual void close() = 0;
    // utc_ms: the corrected clock; minutes_west: the time zone in use
    virtual void set_clock(std::function<int64_t()> utc_ms, int32_t minutes_west) = 0;
    // the time zone desktop Anki last recorded in the collection
    virtual std::optional<int32_t> recorded_minutes_west() = 0;

    // every deck, in display order
    virtual std::vector<DeckEntry> decks() = 0;
    virtual std::string deck_name(int64_t id) = 0;
    virtual std::string deck_preset(int64_t id) = 0;  // empty for filtered decks

    // studying one deck
    virtual bool start(int64_t deck_id, std::string& err) = 0;
    virtual std::vector<int64_t> todays_card_ids() = 0;  // for fetching their images
    // The card to show now, rendered, with today's counts and the id of the
    // card likely to come after it (0 if none).
    // prepared_id: a card the app already rendered (from render()); if it comes
    // up, the result may leave its HTML out and set `prepared`.
    virtual std::optional<StudyCard> next(DeckCounts& counts, int64_t& next_id, std::string& err,
                                          int64_t prepared_id = 0) = 0;
    virtual std::optional<StudyCard> render(int64_t card_id) = 0;
    virtual bool answer(int rating, uint32_t millis, std::string& err) = 0;  // 1 Again .. 4 Easy
    virtual bool can_undo() = 0;
    virtual bool undo(bool& was_answer, std::string& err) = 0;
    // updates card.flags / card.marked
    virtual bool card_action(CardAction a, int flag, StudyCard& card, std::string& err) = 0;

    // media
    virtual std::vector<std::string> media_names(const std::vector<int64_t>& card_ids) = 0;
    virtual std::vector<std::string> deck_media_names(int64_t deck_id) = 0;
    virtual int unsynced_reviews() = 0;
    // SD card reads/writes/flushes since the last call, for the timing log ("" if not measured)
    virtual std::string io_stats() { return ""; }
    // write the collection's write-ahead log back into it (a safe moment: lid closed, deck left)
    virtual void checkpoint() {}

    // sync
    virtual bool login(const std::string& endpoint, const std::string& user, const std::string& pass,
                       std::string& hkey, std::string& endpoint_out, std::string& err) = 0;
    virtual SyncOutcome sync(const std::string& endpoint, const std::string& hkey,
                             const std::function<void(const std::string&)>& progress) = 0;
    virtual bool full_download(const std::string& endpoint, const std::string& hkey, const std::string& path,
                               const std::function<void(size_t, size_t)>& bytes, std::string& err) = 0;
};

std::unique_ptr<Engine> make_cpp_engine();
std::unique_ptr<Engine> make_anki_engine();  // nullptr unless built with CLAMSHELL_ANKI

}  // namespace anki
