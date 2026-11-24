// Access to an Anki 2.1.50+ (schema 18) collection file, plus the card-answering
// logic from rslib/src/scheduler/answering. Everything here is portable C++17 so
// it builds both for the 3DS and for the desktop test tools.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fsrs.h"
#include "proto.h"
#include "states.h"

struct sqlite3;
struct sqlite3_stmt;

namespace anki {

enum CardType { CTYPE_NEW = 0, CTYPE_LEARN = 1, CTYPE_REVIEW = 2, CTYPE_RELEARN = 3 };
enum CardQueue {
    Q_USER_BURIED = -2,
    Q_SCHED_BURIED = -3,
    Q_SUSPENDED = -1,
    Q_NEW = 0,
    Q_LEARN = 1,
    Q_REVIEW = 2,
    Q_DAY_LEARN = 3,
    Q_PREVIEW = 4,
};

struct Card {
    int64_t id = 0, nid = 0, did = 0;
    int ord = 0;
    int64_t mtime = 0;
    int usn = 0;
    int ctype = 0, queue = 0;
    int32_t due = 0;
    uint32_t ivl = 0;
    uint16_t factor = 0;
    uint32_t reps = 0, lapses = 0, left = 0;
    int32_t odue = 0;
    int64_t odid = 0;
    int flags = 0;
    // parsed from the `data` JSON column
    std::optional<uint32_t> original_position;
    std::optional<MemoryState> memory_state;
    std::optional<float> desired_retention;
    std::optional<float> decay;
    std::optional<int64_t> last_review_time;
    std::string custom_data;

    float ease_factor() const { return (float)factor / 1000.0f; }
    std::string data_json() const;
    void parse_data(const std::string& json);
};

struct Note {
    int64_t id = 0;
    std::string guid;
    int64_t mid = 0;
    int64_t mtime = 0;
    int usn = 0;
    std::string tags;  // space-separated, with leading/trailing space as stored
    std::vector<std::string> fields;
    std::string sfld;
    int64_t csum = 0;
    int flags = 0;
    std::string data;
};

struct RevlogEntry {
    int64_t id = 0, cid = 0;
    int usn = 0;
    int ease = 0;
    int32_t ivl = 0, last_ivl = 0;
    uint32_t factor = 0;
    uint32_t time = 0;
    int type = 0;
};

struct DeckConfig {
    int64_t id = 0;
    std::string name;
    int64_t mtime = 0;
    int usn = 0;
    pb::Message raw;  // DeckConfig.Config
    // decoded
    std::vector<float> learn_steps, relearn_steps, fsrs_params;
    uint32_t new_per_day = 20, reviews_per_day = 200;
    float initial_ease = 2.5f, easy_multiplier = 1.3f, hard_multiplier = 1.2f, lapse_multiplier = 0.0f,
          interval_multiplier = 1.0f;
    uint32_t maximum_review_interval = 36500, minimum_lapse_interval = 1;
    uint32_t graduating_interval_good = 1, graduating_interval_easy = 4;
    int new_card_gather_priority = 0, new_card_sort_order = 0, new_mix = 0, review_order = 0,
        interday_learning_mix = 0;
    int leech_action = 0;
    uint32_t leech_threshold = 8;
    uint32_t cap_answer_time_to_secs = 60;
    bool bury_new = false, bury_reviews = false, bury_interday_learning = false;
    float desired_retention = 0.9f;
    std::vector<float> easy_days_percentages;
    void decode();
};

struct Deck {
    int64_t id = 0;
    std::string name;  // components separated by \x1f
    int64_t mtime = 0;
    int usn = 0;
    pb::Message common;  // Deck.Common
    pb::Message kind;    // Deck.KindContainer
    bool filtered = false;
    // normal
    int64_t config_id = 1;
    std::optional<uint32_t> review_limit, new_limit;
    uint32_t review_limit_today = 0, review_limit_today_day = 0, new_limit_today = 0, new_limit_today_day = 0;
    bool has_review_limit_today = false, has_new_limit_today = false;
    std::optional<float> desired_retention;
    // filtered
    bool reschedule = true;
    uint32_t preview_again_secs = 0, preview_hard_secs = 0, preview_good_secs = 0;
    void decode();
    std::string human_name() const;  // "Parent::Child"
};

struct NotetypeField {
    std::string name;
    int ord = 0;
};
struct NotetypeTemplate {
    std::string name;
    int ord = 0;
    std::string qfmt, afmt;
};
struct Notetype {
    int64_t id = 0;
    std::string name;
    int64_t mtime = 0;
    int usn = 0;
    pb::Message config;
    bool is_cloze = false;
    std::string css;
    uint32_t sort_field_idx = 0;
    std::vector<NotetypeField> fields;
    std::vector<NotetypeTemplate> templates;
};

struct SchedTimingToday {
    int64_t now = 0;
    uint32_t days_elapsed = 0;
    int64_t next_day_at = 0;
};

class Db {
public:
    sqlite3* h = nullptr;
    bool exec(const std::string& sql, std::string* err = nullptr);
};

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt& bind(int i, int64_t v);
    Stmt& bind(int i, const std::string& v);
    Stmt& bind_blob(int i, const std::string& v);
    Stmt& bind_null(int i);
    bool step();  // true if a row is available
    void reset();
    int64_t i64(int col) const;
    double dbl(int col) const;
    std::string text(int col) const;
    std::string blob(int col) const;
    bool is_null(int col) const;
    bool ok() const { return s_ != nullptr; }

private:
    sqlite3_stmt* s_ = nullptr;
};

// Registers Anki's "unicase" collation on a raw connection.
void register_unicase(sqlite3* db);

// Time source; the 3DS and tests can substitute their own.
struct Clock {
    std::function<int64_t()> now_millis;
    // Minutes west of UTC for "now" (Anki's localOffset convention; AEDT = -660).
    std::function<int32_t()> local_minutes_west;
};
Clock system_clock();

class Collection {
public:
    ~Collection();
    bool open(const std::string& path, std::string& err);
    void close();
    bool is_open() const { return db_.h != nullptr; }
    sqlite3* db() { return db_.h; }
    const std::string& path() const { return path_; }

    Clock clock = system_clock();
    int64_t now_secs() const { return clock.now_millis() / 1000; }

    void load_metadata();  // decks, configs, notetypes, config keys
    std::map<int64_t, Deck> decks;
    std::map<int64_t, DeckConfig> deck_configs;
    std::map<int64_t, Notetype> notetypes;

    // col table
    int64_t crt = 0, mod = 0, scm = 0, ls = 0;
    int usn = 0, ver = 0;

    // config table (values are JSON text)
    std::optional<std::string> config_json(const std::string& key);
    bool config_bool(const std::string& key, bool def);
    int64_t config_int(const std::string& key, int64_t def);
    void set_config_json(const std::string& key, const std::string& json, int usn_value = -1);

    SchedTimingToday timing_today();
    SchedTimingToday timing_for(int64_t now_secs, int32_t minutes_west);

    std::optional<Card> get_card(int64_t id);
    void update_card(const Card& c);  // writes as-is
    std::optional<Note> get_note(int64_t id);
    void update_note(const Note& n);
    void add_revlog(const RevlogEntry& e, bool uniquify);
    std::optional<int64_t> time_of_last_review(int64_t cid);

    const DeckConfig& config_for_deck(const Deck& d);
    const Deck* deck(int64_t id) const;
    std::vector<int64_t> child_deck_ids(int64_t id) const;  // including self
    std::vector<int64_t> parent_deck_ids(int64_t id) const;

    void begin();
    void commit();
    void rollback();
    void set_modified();  // col.mod = now (marks collection as changed for sync)

    void unbury_if_day_rolled_over(const SchedTimingToday& t);

private:
    Db db_;
    std::string path_;
    DeckConfig default_config_;
};

// ---------------------------------------------------------------- answering

enum class Rating { Again = 1, Hard = 2, Good = 3, Easy = 4 };

class LoadBalancer;

// Holds per-session scheduling context (mirrors rslib's CardQueues lifetime:
// the load balancer is built when study starts and updated after each answer).
class Scheduler {
public:
    explicit Scheduler(Collection& col);
    ~Scheduler();
    void start_session(bool with_load_balancer = true);  // builds the load balancer if enabled

    // States for each button, plus a current state used to validate answers.
    SchedulingStates scheduling_states(const Card& card);
    // Applies an answer (writes card, revlog, buries siblings, leech tag).
    bool answer(int64_t card_id, Rating rating, uint32_t millis_taken, std::string& err,
                const SchedulingStates* precomputed = nullptr);
    // Human-readable button labels ("10m", "3d", "1.2mo").
    std::vector<std::string> describe_next_states(const SchedulingStates& s);

    bool fsrs_enabled() const { return fsrs_; }

    struct Updater;

private:
    Collection& col_;
    bool fsrs_ = false, short_term_with_steps_ = false;
    std::unique_ptr<LoadBalancer> lb_;

    bool make_updater(const Card& card, Updater& u, std::string& err);
};

std::optional<uint64_t> fuzz_seed_for(int64_t card_id, uint32_t reps);
std::optional<float> fuzz_factor_for(std::optional<uint64_t> seed);

// Format seconds like Anki's answer buttons.
std::string format_interval(uint32_t secs, bool collapse_to_now = false);

std::string format_f32_like_serde(float v);  // shortest round-trip, Rust/serde style

}  // namespace anki
