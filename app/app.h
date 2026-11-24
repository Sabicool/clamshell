// The Anki 3DS application: screens, input handling and flows. Platform code
// (3ds/source/main.cpp, host/sim.cpp) feeds input and shows the two canvases.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "collection.h"
#include "queue.h"
#include "render/card_view.h"
#include "render/fonts.h"
#include "template.h"
#include "ui.h"

namespace anki {

enum Key : uint32_t {
    K_A = 1 << 0, K_B = 1 << 1, K_X = 1 << 2, K_Y = 1 << 3, K_L = 1 << 4, K_R = 1 << 5,
    K_UP = 1 << 6, K_DOWN = 1 << 7, K_LEFT = 1 << 8, K_RIGHT = 1 << 9, K_START = 1 << 10, K_SELECT = 1 << 11,
    K_ZL = 1 << 12, K_ZR = 1 << 13,
};

struct Input {
    uint32_t down = 0;  // pressed this frame
    uint32_t held = 0;
    bool touch_down = false;  // touch started this frame
    bool touching = false;
    int tx = 0, ty = 0;
    int scroll = 0;  // circle pad / c-stick vertical, pixels per frame (+ = down)
};

class Platform {
public:
    virtual ~Platform() = default;
    virtual std::string data_dir() = 0;
    virtual std::string fonts_dir() = 0;
    // System keyboard. Returns false if cancelled.
    virtual bool keyboard(const std::string& hint, const std::string& initial, bool password, std::string& out) = 0;
    // Show the canvases right now (used for progress during long operations).
    virtual void present(const Canvas& top, const Canvas& bottom) = 0;
    // The console clock: local wall-clock time expressed as if it were UTC.
    virtual int64_t local_clock_ms() = 0;
    // Default timezone guess in minutes west of UTC (desktop: system tz).
    virtual std::optional<int32_t> system_minutes_west() { return std::nullopt; }
    virtual bool net_up(std::string& err) = 0;
    virtual void net_down() = 0;
};

struct Settings {
    std::string endpoint = "https://sync.ankiweb.net/";
    std::string hkey;
    std::string username;
    std::optional<int32_t> minutes_west;
    float text_zoom = 0.75f;
    bool night = false;
    bool auto_media = true;  // fetch a deck's images when you open it after a sync
    void load(const std::string& path);
    void save(const std::string& path) const;
};

enum class Mode { Setup, Decks, Review, Done, Settings, Message, ConfirmDownload };

class App {
public:
    explicit App(Platform& p);
    ~App();
    void init();
    // Runs one frame. Returns false when the user asks to quit.
    bool frame(const Input& in);
    Canvas top{400, 240}, bottom{320, 240};
    bool dirty = true;
    Mode mode() const { return mode_; }
    // Test hook for the simulator: select the first deck row whose name contains `needle`.
    bool debug_select(const std::string& needle);

private:
    Platform& pf_;
    FontLib fonts_;
    Settings settings_;
    Collection col_;
    std::string col_path_, media_dir_, settings_path_;
    Mode mode_ = Mode::Setup;
    std::string message_title_, message_body_;
    Mode message_return_ = Mode::Decks;
    std::string status_;  // one-line status on the deck screen

    // deck list
    struct DeckRow {
        int64_t id;
        std::string name;
        int depth;
        bool has_children;
        bool collapsed;
        DeckCounts counts;
    };
    std::vector<DeckRow> rows_;
    std::vector<int64_t> collapsed_local_;
    int sel_ = 0, list_scroll_ = 0;

    // review
    int64_t deck_id_ = 0;
    std::unique_ptr<StudyQueue> queue_;
    std::unique_ptr<Scheduler> sched_;
    std::unique_ptr<CardView> view_;
    std::optional<Card> card_;
    std::optional<QueuedCard> queued_;
    SchedulingStates states_;
    std::vector<std::string> labels_;
    RenderedCard rendered_;
    bool answer_shown_ = false;
    int scroll_ = 0;
    int64_t shown_at_ms_ = 0;
    int reviewed_ = 0;

    // settings
    int setting_sel_ = 0;

    void open_collection();
    void configure_clock();
    void build_rows();
    void draw();
    void draw_setup();
    void draw_decks();
    void draw_review();
    void draw_done();
    void draw_settings();
    void draw_message();
    void draw_header(Canvas& c, const std::string& title, const std::string& right);
    void progress(const std::string& title, const std::string& detail);

    void on_setup(const Input& in);
    void on_decks(const Input& in);
    void on_review(const Input& in);
    void on_settings(const Input& in);

    void login_flow(bool custom_server);
    void sync_flow();
    void full_download_flow();
    void media_flow(int64_t deck_id, bool quiet);
    void start_review(int64_t deck_id);
    void next_card();
    void show_answer();
    void answer(Rating r);
    void show_message(const std::string& title, const std::string& body, Mode ret);
    std::vector<ui::Button> review_buttons();
    int max_scroll() const;
    int64_t utc_ms();
};

}  // namespace anki
