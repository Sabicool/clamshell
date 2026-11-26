// The Clamshell application: screens, input handling and flows. Platform code
// (3ds/source/main.cpp, host/sim.cpp) feeds input and shows the two canvases.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "buttons.h"
#include "collection.h"
#include "engine.h"
#include "queue.h"
#include "render/card_view.h"
#include "render/fonts.h"
#include "template.h"
#include "ui.h"
#include "js/card_js.h"

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
    bool tz_auto = true;  // follow the offset desktop Anki records (handles daylight saving)
    float text_zoom = 0.75f;
    bool night = false;
    bool auto_media = true;  // fetch a deck's images when you open it after a sync
    bool reveal_hidden = false;  // show sections templates hide behind JS buttons
    bool run_scripts = true;     // run card templates' JavaScript (QuickJS)
    int auto_sync = 3;           // 0 off, 1 when the app opens, 2 after studying, 3 both
    bool scratchpad = false;     // bottom screen is a stylus scratchpad while reviewing
    ButtonMap buttons = default_buttons();         // for every note type
    std::map<std::string, ButtonMap> nt_buttons;   // note type id -> its own set
    void load(const std::string& path);
    void save(const std::string& path) const;
};

enum class Mode { Setup, Decks, Review, Done, Settings, Message, ConfirmDownload, Buttons };

class App {
public:
    explicit App(Platform& p);
    ~App();
    void init();
    // The lid was closed or the HOME Menu opened, and then the app came back:
    // time away doesn't count towards the card's answer time.
    void on_pause();
    void on_resume();
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
    std::unique_ptr<Engine> engine_;  // the collection: Anki's backend or the C++ port
    std::string col_path_, media_dir_, settings_path_;
    Mode mode_ = Mode::Setup;
    std::string message_title_, message_body_;
    Mode message_return_ = Mode::Decks;
    std::string status_;  // one-line status on the deck screen

    // deck list
    struct DeckRow {
        int64_t id;
        std::string name;
        std::string full_name;
        int depth;
        bool has_children;
        bool collapsed;
        DeckCounts counts;
    };
    std::vector<DeckRow> rows_;
    std::vector<int64_t> collapsed_local_;
    int sel_ = 0, list_scroll_ = 0;
    int64_t preset_deck_ = -1;  // cached preset name for the selected deck
    std::string preset_name_;

    // review
    int64_t deck_id_ = 0;
    std::unique_ptr<CardView> view_;
    std::optional<StudyCard> card_;  // the card on screen
    DeckCounts counts_;              // today's remaining counts
    int64_t next_id_ = 0;            // the card likely to come next, for preparing it
    bool can_undo_ = false;
    std::vector<std::string> labels_;
    RenderedCard rendered_;
    std::unique_ptr<js::CardScript> script_;
    std::map<std::string, std::string> js_storage_;  // sessionStorage/localStorage for the session
    std::string current_html_;
    void render_current();
    CardViewOptions view_options() const;
    // The likely next card, laid out while the answer is on screen so it
    // appears as soon as the answer is saved. Thrown away if a different card comes up.
    struct Prefetch {
        int64_t card_id = 0;
        StudyCard card;
        RenderedCard rendered;
        std::unique_ptr<js::CardScript> script;
        std::string html;
        std::unique_ptr<CardView> view;
    };
    std::unique_ptr<Prefetch> prefetch_;
    bool prefetch_due_ = false;
    int unsynced_reviews_ = 0;  // shown before a full download replaces this copy
    bool download_requested_ = false;  // chosen in Settings rather than required by the server
    void confirm_download(bool requested);
    void prefetch_next();

    // touch menu (bury, suspend, flag, mark, scratchpad) and the scratchpad itself
    bool menu_open_ = false;
    bool card_marked_ = false;
    std::string card_note_;  // e.g. "Flagged blue", shown on the bottom screen
    Canvas scratch_{320, 240};
    int pen_x_ = -1, pen_y_ = -1;
    void clear_scratch();
    void on_menu(const Input& in);
    void draw_menu();
    void card_action(CardAction op, int flag = 0);

    // magnifier: part of the card, redrawn at 2x or 3x on the bottom screen
    bool zoom_open_ = false;
    float zoom_scale_ = 2.0f;
    float zoom_x_ = 0, zoom_y_ = 0;  // top-left of the magnified area, in card pixels
    Canvas zoom_{320, 206};
    bool zoom_dirty_ = true;
    int zoom_pen_x_ = -1, zoom_pen_y_ = -1;
    void open_zoom();
    void clamp_zoom();
    void on_zoom(const Input& in);
    void draw_zoom();

    // button assignments (app/buttons.h)
    int64_t current_mid_ = 0;  // note type of the card on screen
    std::string current_nt_name_;
    std::string action_for(const std::string& button) const;
    void run_action(const std::string& action);
    std::string button_hints() const;
    int btn_sel_ = 0;
    int64_t btn_mid_ = 0;  // 0: editing the set for all note types
    Mode btn_return_ = Mode::Settings;
    void open_buttons(int64_t mid, Mode ret);
    void on_buttons(const Input& in);
    void draw_buttons();
    bool answer_shown_ = false;
    int scroll_ = 0;
    int64_t shown_at_ms_ = 0;
    int64_t paused_at_ms_ = 0;
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
    void sync_flow(bool automatic = false);
    // False for AnkiWeb unless built with ANKI3DS_ANKIWEB (its terms only allow Anki's own apps)
    bool server_allowed(const std::string& endpoint) const;
    bool startup_sync_due_ = false;
    void sync_after_studying();
    void full_download_flow();
    void media_flow(int64_t deck_id, bool quiet);
    void fetch_media(const std::vector<std::string>& names, bool quiet);
    void start_review(int64_t deck_id);
    void next_card();
    void show_answer();
    void answer(Rating r);
    void undo();
    void show_message(const std::string& title, const std::string& body, Mode ret);
    std::vector<ui::Button> review_buttons();
    int max_scroll() const;
    int64_t utc_ms();
};

}  // namespace anki
