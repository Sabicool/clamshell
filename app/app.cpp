#include "app.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>

#include "media.h"
#include "nlohmann/json.hpp"
#include "sync.h"

using json = nlohmann::json;

namespace anki {

using namespace ui;

// ---------------------------------------------------------------- settings

void Settings::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::string s;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    json j = json::parse(s, nullptr, false);
    if (!j.is_object()) return;
    endpoint = j.value("endpoint", endpoint);
    hkey = j.value("hkey", hkey);
    username = j.value("username", username);
    if (j.contains("minutes_west") && j["minutes_west"].is_number()) minutes_west = j["minutes_west"].get<int32_t>();
    text_zoom = j.value("text_zoom", text_zoom);
    night = j.value("night", night);
    auto_media = j.value("auto_media", auto_media);
    reveal_hidden = j.value("reveal_hidden", reveal_hidden);
    tz_auto = j.value("tz_auto", tz_auto);
    run_scripts = j.value("run_scripts", run_scripts);
}

void Settings::save(const std::string& path) const {
    json j = {{"endpoint", endpoint}, {"hkey", hkey},       {"username", username},
              {"text_zoom", text_zoom}, {"night", night}, {"auto_media", auto_media},
              {"reveal_hidden", reveal_hidden}, {"tz_auto", tz_auto}, {"run_scripts", run_scripts}};
    if (minutes_west) j["minutes_west"] = *minutes_west;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    std::string s = j.dump(2);
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

static bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static std::string fmt_offset(int32_t minutes_west) {
    int east = -minutes_west;
    char buf[32];
    snprintf(buf, sizeof buf, "UTC%c%02d:%02d", east >= 0 ? '+' : '-', std::abs(east) / 60, std::abs(east) % 60);
    return buf;
}

// ---------------------------------------------------------------- lifecycle

App::App(Platform& p) : pf_(p) {}
App::~App() = default;

void App::init() {
    std::string dir = pf_.data_dir();
    // create every missing parent (sdmc:/3ds may not exist on a fresh card)
    for (size_t i = dir.find('/', dir.find(':') + 2); i != std::string::npos; i = dir.find('/', i + 1))
        mkdir(dir.substr(0, i).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    col_path_ = dir + "/collection.anki2";
    media_dir_ = dir + "/collection.media";
    settings_path_ = dir + "/settings.json";
    mkdir(media_dir_.c_str(), 0755);
    settings_.load(settings_path_);
    std::string err;
    if (!fonts_.load(pf_.fonts_dir(), err)) {
        // Nothing can be drawn without fonts; keep going so the platform can show a console error.
    }
    view_ = std::make_unique<CardView>(fonts_);
    if (exists(col_path_)) open_collection();
    else mode_ = Mode::Setup;
    dirty = true;
}

int64_t App::utc_ms() {
    int32_t west = settings_.minutes_west.value_or(0);
    return pf_.local_clock_ms() + (int64_t)west * 60000;
}

void App::configure_clock() {
    col_.clock.now_millis = [this] { return utc_ms(); };
    col_.clock.local_minutes_west = [this] { return settings_.minutes_west.value_or(0); };
}

void App::open_collection() {
    std::string err;
    if (!col_.open(col_path_, err)) {
        show_message("Couldn't open your collection", err + "\n\nYou can download it again from your sync server in Settings.",
                     Mode::Settings);
        return;
    }
    if (!settings_.minutes_west || settings_.tz_auto) {
        // Use the host's time zone if it has one (desktop simulator), otherwise the
        // offset desktop Anki last recorded in the collection.
        auto sys = pf_.system_minutes_west();
        int32_t west = sys ? *sys : (int32_t)col_.config_int("localOffset", settings_.minutes_west.value_or(0));
        if (settings_.minutes_west != west) {
            settings_.minutes_west = west;
            settings_.save(settings_path_);
        }
    }
    configure_clock();
    build_rows();
    mode_ = Mode::Decks;
}

// ---------------------------------------------------------------- deck rows

void App::build_rows() {
    rows_.clear();
    if (!col_.is_open()) return;
    auto counts = all_deck_counts(col_);
    std::vector<const Deck*> decks;
    for (auto& [id, d] : col_.decks) decks.push_back(&d);
    std::sort(decks.begin(), decks.end(), [](const Deck* a, const Deck* b) {
        // case-insensitive, component-wise
        std::string x = a->name, y = b->name;
        for (auto& ch : x) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : y) ch = (char)tolower((unsigned char)ch);
        return x < y;
    });
    std::set<std::string> collapsed_names;
    for (const Deck* d : decks) {
        // hide children of collapsed decks
        bool hidden = false;
        for (auto& cn : collapsed_names)
            if (d->name.size() > cn.size() && d->name.compare(0, cn.size(), cn) == 0 && d->name[cn.size()] == '\x1f')
                hidden = true;
        bool has_children = false;
        for (const Deck* o : decks)
            if (o->name.size() > d->name.size() && o->name.compare(0, d->name.size(), d->name) == 0 &&
                o->name[d->name.size()] == '\x1f') {
                has_children = true;
                break;
            }
        bool collapsed = d->common.boolean(1);
        if (std::find(collapsed_local_.begin(), collapsed_local_.end(), d->id) != collapsed_local_.end())
            collapsed = !collapsed;
        if (collapsed && has_children) collapsed_names.insert(d->name);
        if (hidden) continue;
        // Skip the empty "Default" deck like desktop does when other decks exist
        if (d->id == 1 && decks.size() > 1) {
            Stmt s(col_.db(), "select 1 from cards where did = 1 limit 1");
            if (!s.step()) continue;
        }
        size_t p = d->name.rfind('\x1f');
        DeckRow r{d->id, p == std::string::npos ? d->name : d->name.substr(p + 1),
                  (int)std::count(d->name.begin(), d->name.end(), '\x1f'), has_children, collapsed && has_children,
                  counts[d->id]};
        rows_.push_back(r);
    }
    if (sel_ >= (int)rows_.size()) sel_ = std::max(0, (int)rows_.size() - 1);
}

bool App::debug_select(const std::string& needle) {
    for (size_t i = 0; i < rows_.size(); i++) {
        const Deck* d = col_.deck(rows_[i].id);
        if (d && d->human_name().find(needle) != std::string::npos) {
            sel_ = (int)i;
            list_scroll_ = std::max(0, sel_ - 4);
            dirty = true;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- frame

bool App::frame(const Input& in) {
    if (in.down || in.touch_down || in.scroll) dirty = true;
    switch (mode_) {
        case Mode::Setup: on_setup(in); break;
        case Mode::Decks:
            if (in.down & K_START) return false;
            on_decks(in);
            break;
        case Mode::Review: on_review(in); break;
        case Mode::Done:
            if ((in.down & K_SELECT) && sched_ && sched_->can_undo()) {
                undo();
                break;
            }
            if (in.down & (K_A | K_B | K_START) || in.touch_down) {
                build_rows();
                mode_ = Mode::Decks;
            }
            break;
        case Mode::Settings: on_settings(in); break;
        case Mode::Message:
            if (in.down & (K_A | K_B) || in.touch_down) {
                mode_ = message_return_;
                if (mode_ == Mode::Decks) build_rows();
            }
            break;
        case Mode::ConfirmDownload:
            if (in.down & K_A) full_download_flow();
            else if (in.down & K_B) mode_ = Mode::Decks;
            break;
    }
    if (dirty) draw();
    return true;
}

void App::show_message(const std::string& title, const std::string& body, Mode ret) {
    // Keep a log on the SD card so problems can be reported.
    if (FILE* f = fopen((pf_.data_dir() + "/log.txt").c_str(), "ab")) {
        time_t now = (time_t)(pf_.local_clock_ms() / 1000);
        char ts[32];
        struct tm tmv;
        gmtime_r(&now, &tmv);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &tmv);
        fprintf(f, "[%s] %s: %s\n", ts, title.c_str(), body.c_str());
        fclose(f);
    }
    message_title_ = title;
    message_body_ = body;
    message_return_ = ret;
    mode_ = Mode::Message;
    dirty = true;
}

void App::progress(const std::string& title, const std::string& detail) {
    const Theme& t = theme(settings_.night);
    top.resize(400, 240, t.bg);
    bottom.resize(320, 240, t.bg);
    text_centered(top, fonts_, {0, 70, 400, 30}, title, 20, t.text, FACE_SANS_BOLD);
    auto lines = wrap(fonts_, detail, 15, 360);
    float y = 112;
    for (auto& l : lines) {
        text_centered(top, fonts_, {0, (int)y, 400, 20}, l, 15, t.dim);
        y += 20;
    }
    text_centered(bottom, fonts_, {0, 100, 320, 30}, "Please wait…", 16, t.dim);
    pf_.present(top, bottom);
}

// ---------------------------------------------------------------- setup

void App::on_setup(const Input& in) {
    Button own{{20, 50, 280, 56}, "Sign in to my sync server", "", "A"};
    Button web{{20, 140, 280, 40}, "Sign in to AnkiWeb", "", "X"};
    if ((in.down & K_A) || (in.touch_down && hit(own.r, in.tx, in.ty))) login_flow(true);
    else if ((in.down & K_X) || (in.touch_down && hit(web.r, in.tx, in.ty))) login_flow(false);
}

void App::draw_setup() {
    const Theme& t = theme(settings_.night);
    top.resize(400, 240, t.bg);
    text_centered(top, fonts_, {0, 28, 400, 40}, "Anki for 3DS", 30, t.text, FACE_SANS_BOLD);
    float y = paragraph(top, fonts_, 30, 86, 340,
                        "Sign in to an Anki sync server to download your collection. Reviews you do here sync "
                        "back, using exactly the same scheduling as Anki on your other devices.",
                        15, t.text);
    paragraph(top, fonts_, 30, y + 8, 340,
              "AnkiWeb's terms currently only allow Anki's own apps, so a self-hosted sync server (built into "
              "Anki) is the supported option. This 3DS never uploads a whole collection.",
              13, t.dim);
    bottom.resize(320, 240, t.bg);
    button(bottom, fonts_, {{20, 50, 280, 56}, "Sign in to my sync server", "", "A"}, settings_.night);
    text_centered(bottom, fonts_, {20, 108, 280, 20}, "e.g. http://192.168.1.20:8080/", 12, t.dim);
    button(bottom, fonts_, {{20, 140, 280, 40}, "Sign in to AnkiWeb", "", "X", t.hard}, settings_.night);
    text_centered(bottom, fonts_, {20, 182, 280, 20}, "Only with permission from AnkiWeb", 12, t.dim);
    text_centered(bottom, fonts_, {0, 214, 320, 20}, "Press HOME to exit", 12, t.dim);
}

void App::login_flow(bool custom_server) {
    std::string endpoint = settings_.endpoint;
    if (custom_server) {
        std::string url;
        if (!pf_.keyboard("Sync server URL (e.g. http://192.168.1.20:8080/)",
                          endpoint == "https://sync.ankiweb.net/" ? "http://" : endpoint, false, url))
            return;
        endpoint = url;
    } else {
        endpoint = "https://sync.ankiweb.net/";
    }
    std::string user, pass;
    if (!pf_.keyboard(custom_server ? "Username" : "AnkiWeb email", settings_.username, false, user)) return;
    if (!pf_.keyboard("Password", "", true, pass)) return;
    progress("Signing in…", endpoint);
    std::string err;
    if (!pf_.net_up(err)) {
        show_message("No network", err, mode_ == Mode::Setup ? Mode::Setup : Mode::Settings);
        return;
    }
    SyncClient client(endpoint);
    auto key = client.login(user, pass, err);
    if (!key) {
        pf_.net_down();
        show_message("Sign-in failed", err, col_.is_open() ? Mode::Settings : Mode::Setup);
        return;
    }
    settings_.endpoint = client.endpoint;
    settings_.hkey = *key;
    settings_.username = user;
    settings_.save(settings_path_);
    if (!col_.is_open()) {
        pf_.net_down();
        full_download_flow();
    } else {
        pf_.net_down();
        sync_flow();
    }
}

// ---------------------------------------------------------------- sync

void App::sync_flow() {
    if (settings_.hkey.empty()) {
        show_message("Not signed in", "Sign in to your sync server from Settings first.", Mode::Decks);
        return;
    }
    std::string err;
    progress("Syncing…", "Connecting");
    if (!pf_.net_up(err)) {
        show_message("No network", err, Mode::Decks);
        return;
    }
    SyncClient client(settings_.endpoint);
    client.hkey = settings_.hkey;
    client.on_progress = [this](const std::string& s) { progress("Syncing…", s); };
    SyncResult r = client.normal_sync(col_, err);
    pf_.net_down();
    if (r.new_endpoint) {
        settings_.endpoint = *r.new_endpoint;
        settings_.save(settings_path_);
    }
    switch (r.status) {
        case SyncStatus::NoChanges:
        case SyncStatus::Synced: {
            if (settings_.tz_auto && !pf_.system_minutes_west()) {
                int32_t west = (int32_t)col_.config_int("localOffset", settings_.minutes_west.value_or(0));
                if (west != settings_.minutes_west) {
                    settings_.minutes_west = west;
                    settings_.save(settings_path_);
                }
            }
            char buf[64];
            time_t now = (time_t)(pf_.local_clock_ms() / 1000);
            struct tm tmv;
            gmtime_r(&now, &tmv);
            strftime(buf, sizeof buf, "%H:%M", &tmv);
            status_ = std::string(r.status == SyncStatus::Synced ? "Synced" : "Already up to date") + " at " + buf;
            if (!r.message.empty()) show_message("Message from the sync server", r.message, Mode::Decks);
            else mode_ = Mode::Decks;
            build_rows();
            break;
        }
        case SyncStatus::FullSyncRequired:
            if (r.download_ok) {
                mode_ = Mode::ConfirmDownload;
            } else {
                show_message("Can't sync",
                             "The server's collection is empty. Sync from desktop Anki first.", Mode::Decks);
            }
            break;
        default:
            show_message("Sync failed", err.empty() ? r.message : err, Mode::Decks);
    }
    dirty = true;
}

void App::full_download_flow() {
    std::string err;
    progress("Downloading collection…", "Connecting");
    if (!pf_.net_up(err)) {
        show_message("No network", err, col_.is_open() ? Mode::Decks : Mode::Setup);
        return;
    }
    SyncClient client(settings_.endpoint);
    client.hkey = settings_.hkey;
    client.on_progress = [this](const std::string& s) { progress("Downloading collection…", s); };
    size_t last_mb = (size_t)-1;
    client.on_bytes = [&](size_t done, size_t total) {
        size_t mb = done / (1024 * 1024);
        if (mb != last_mb) {
            last_mb = mb;
            char buf[64];
            if (total) snprintf(buf, sizeof buf, "%.1f of %.1f MB", done / 1048576.0, total / 1048576.0);
            else snprintf(buf, sizeof buf, "%.1f MB", done / 1048576.0);
            progress("Downloading collection…", buf);
        }
    };
    SyncResult r = client.full_download(col_, col_path_, err);
    pf_.net_down();
    if (r.status != SyncStatus::Downloaded) {
        show_message("Download failed", err, col_.is_open() ? Mode::Decks : Mode::Setup);
        return;
    }
    open_collection();
    status_ = "Downloaded from the sync server";
    dirty = true;
}

void App::media_flow(int64_t deck_id, bool quiet) {
    if (settings_.hkey.empty()) return;
    auto names = missing_media_for_deck(col_, deck_id, media_dir_);
    if (names.empty()) {
        if (!quiet) show_message("Images", "All images for this deck are already on the SD card.", Mode::Decks);
        return;
    }
    std::string err;
    progress("Downloading images…", std::to_string(names.size()) + " files");
    if (!pf_.net_up(err)) {
        if (!quiet) show_message("No network", err, Mode::Decks);
        return;
    }
    SyncClient client(settings_.endpoint);
    client.hkey = settings_.hkey;
    MediaDownloadResult res;
    bool ok = download_media(client, names, media_dir_, res, err, [this](int done, int total) {
        progress("Downloading images…", std::to_string(done) + " of " + std::to_string(total));
    });
    pf_.net_down();
    if (!ok) show_message("Image download stopped", err, Mode::Decks);
    else if (!quiet || res.unavailable)
        show_message("Images",
                     "Downloaded " + std::to_string(res.downloaded) + " files." +
                         (res.unavailable ? " " + std::to_string(res.unavailable) + " are missing on the server." : ""),
                     Mode::Decks);
}

// ---------------------------------------------------------------- decks

void App::on_decks(const Input& in) {
    int n = (int)rows_.size();
    const int visible = 9;
    if (in.down & K_DOWN && n) sel_ = (sel_ + 1) % n;
    if (in.down & K_UP && n) sel_ = (sel_ - 1 + n) % n;
    if (in.down & K_R && n) sel_ = std::min(n - 1, sel_ + visible);
    if (in.down & K_L && n) sel_ = std::max(0, sel_ - visible);
    if ((in.down & (K_LEFT | K_RIGHT)) && n && rows_[sel_].has_children) {
        int64_t id = rows_[sel_].id;
        auto it = std::find(collapsed_local_.begin(), collapsed_local_.end(), id);
        bool want_collapsed = (in.down & K_LEFT) != 0;
        if (rows_[sel_].collapsed != want_collapsed) {
            if (it == collapsed_local_.end()) collapsed_local_.push_back(id);
            else collapsed_local_.erase(it);
            build_rows();
        }
    }
    if (sel_ < list_scroll_) list_scroll_ = sel_;
    if (sel_ >= list_scroll_ + visible) list_scroll_ = sel_ - visible + 1;

    Button study{{10, 150, 145, 44}, "Study", "", "A"};
    Button sync{{165, 150, 145, 44}, "Sync", "", "X"};
    Button media{{10, 200, 145, 32}, "Get images", "", "Y"};
    Button sett{{165, 200, 145, 32}, "Settings", "", "SEL"};
    auto touched = [&](const Button& b) { return in.touch_down && hit(b.r, in.tx, in.ty); };
    if (((in.down & K_A) || touched(study)) && n) start_review(rows_[sel_].id);
    else if ((in.down & K_X) || touched(sync)) sync_flow();
    else if (((in.down & K_Y) || touched(media)) && n) media_flow(rows_[sel_].id, false);
    else if ((in.down & K_SELECT) || touched(sett)) {
        setting_sel_ = 0;
        mode_ = Mode::Settings;
    }
}

void App::draw_header(Canvas& c, const std::string& title, const std::string& right) {
    const Theme& t = theme(settings_.night);
    c.fill_rect(0, 0, c.width(), 24, t.panel);
    c.fill_rect(0, 24, c.width(), 1, t.border);
    text(c, fonts_, 8, 3, title, 15, t.text, FACE_SANS_BOLD);
    if (!right.empty()) text(c, fonts_, c.width() - 8 - text_w(fonts_, right, 13), 5, right, 13, t.dim);
}

void App::draw_decks() {
    const Theme& t = theme(settings_.night);
    top.resize(400, 240, t.bg);
    char clock[32];
    time_t now = (time_t)(pf_.local_clock_ms() / 1000);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(clock, sizeof clock, "%a %H:%M", &tmv);
    draw_header(top, "Decks", clock);
    const int row_h = 23, visible = 9;
    text(top, fonts_, 268, 27, "New", 11, t.new_c, FACE_SANS_BOLD);
    text(top, fonts_, 308, 27, "Learn", 11, t.learn_c, FACE_SANS_BOLD);
    text(top, fonts_, 352, 27, "Due", 11, t.review_c, FACE_SANS_BOLD);
    for (int i = 0; i < visible && list_scroll_ + i < (int)rows_.size(); i++) {
        const DeckRow& r = rows_[list_scroll_ + i];
        int y = 40 + i * row_h;
        bool selected = list_scroll_ + i == sel_;
        if (selected) top.fill_round_rect(4, y, 392, row_h - 1, 6, t.accent);
        uint32_t tc = selected ? t.accent_text : t.text;
        int x = 10 + r.depth * 12;
        if (r.has_children) text(top, fonts_, (float)x, (float)y + 3, r.collapsed ? "▸" : "▾", 13, selected ? tc : t.dim);
        text(top, fonts_, (float)x + 12, (float)y + 3, fit(fonts_, r.name, 14, 250 - x), 14, tc);
        auto num = [&](uint32_t v, float cx, uint32_t col) {
            std::string s = std::to_string(v);
            uint32_t c = v == 0 ? (selected ? rgba(255, 255, 255, 120) : t.border) : (selected ? tc : col);
            text(top, fonts_, cx - text_w(fonts_, s, 14) / 2, (float)y + 3, s, 14, c);
        };
        num(r.counts.new_count, 280, t.new_c);
        num(r.counts.learn_count, 324, t.learn_c);
        num(r.counts.review_count, 362, t.review_c);
    }
    if (rows_.empty()) text_centered(top, fonts_, {0, 100, 400, 30}, "No decks yet. Press X to sync.", 15, t.dim);
    if ((int)rows_.size() > visible) {
        int bar_h = 207 * visible / (int)rows_.size();
        int bar_y = 40 + (207 - bar_h) * list_scroll_ / std::max(1, (int)rows_.size() - visible);
        top.fill_round_rect(396, bar_y, 3, bar_h, 1, t.border);
    }

    bottom.resize(320, 240, t.bg);
    if (!rows_.empty()) {
        const DeckRow& r = rows_[sel_];
        const Deck* d = col_.deck(r.id);
        std::string full = d ? d->human_name() : r.name;
        float y = paragraph(bottom, fonts_, 12, 10, 296, full, 15, t.text, FACE_SANS_BOLD, 1.25f);
        std::string due = std::to_string(r.counts.new_count) + " new · " + std::to_string(r.counts.learn_count) +
                          " learning · " + std::to_string(r.counts.review_count) + " due";
        text(bottom, fonts_, 12, y + 2, due, 13, t.dim);
        if (d && !d->filtered) {
            const DeckConfig& cfg = col_.config_for_deck(*d);
            text(bottom, fonts_, 12, y + 20, fit(fonts_, "Preset: " + cfg.name, 12, 296), 12, t.dim);
        }
    }
    if (!status_.empty()) text(bottom, fonts_, 12, 128, fit(fonts_, status_, 12, 296), 12, t.dim);
    button(bottom, fonts_, {{10, 150, 145, 44}, "Study", "", "A"}, settings_.night);
    button(bottom, fonts_, {{165, 150, 145, 44}, "Sync", "", "X", t.good}, settings_.night);
    button(bottom, fonts_, {{10, 200, 145, 32}, "Get images", "", "Y", t.hard}, settings_.night);
    button(bottom, fonts_, {{165, 200, 145, 32}, "Settings", "", "SEL", t.hard}, settings_.night);
}

// ---------------------------------------------------------------- review

void App::start_review(int64_t deck_id) {
    deck_id_ = deck_id;
    if (settings_.auto_media && !settings_.hkey.empty()) {
        auto missing = missing_media_for_deck(col_, deck_id, media_dir_);
        if (!missing.empty() && missing.size() <= 400) media_flow(deck_id, true);
        if (mode_ == Mode::Message) return;
    }
    progress("Loading…", "Building today's queue");
    queue_ = std::make_unique<StudyQueue>(col_, deck_id);
    queue_->build();
    sched_ = std::make_unique<Scheduler>(col_);
    sched_->start_session(true);
    reviewed_ = 0;
    next_card();
}

void App::next_card() {
    card_.reset();
    queued_ = queue_->next();
    if (!queued_) {
        mode_ = Mode::Done;
        dirty = true;
        return;
    }
    card_ = col_.get_card(queued_->card_id);
    if (!card_) {
        queue_->build();
        queued_ = queue_->next();
        if (!queued_ || !(card_ = col_.get_card(queued_->card_id))) {
            mode_ = Mode::Done;
            return;
        }
    }
    states_ = sched_->scheduling_states(*card_);
    labels_ = sched_->describe_next_states(states_);
    rendered_ = render_card(col_, *card_);
    answer_shown_ = false;
    script_.reset();
    if (settings_.run_scripts) {
        script_ = std::make_unique<js::CardScript>();
        current_html_ = script_->run(rendered_.question, js_storage_);
    } else {
        current_html_ = rendered_.question;
    }
    render_current();
    scroll_ = 0;
    shown_at_ms_ = pf_.local_clock_ms();
    mode_ = Mode::Review;
    dirty = true;
}

void App::render_current() {
    CardViewOptions opt;
    opt.media_dir = media_dir_;
    opt.text_zoom = settings_.text_zoom;
    opt.night_mode = settings_.night;
    opt.reveal_hidden = answer_shown_ && settings_.reveal_hidden;  // extra sections belong to the answer side
    view_->render(current_html_, rendered_.css, rendered_.ord, opt);
    dirty = true;
}

void App::show_answer() {
    answer_shown_ = true;
    current_html_ = script_ ? script_->show_answer(rendered_.answer) : rendered_.answer;
    render_current();
    int anchor = view_->answer_anchor();
    scroll_ = anchor > 60 ? std::min(anchor - 40, max_scroll()) : 0;
    dirty = true;
}

void App::answer(Rating r) {
    if (!card_) return;
    uint32_t millis = (uint32_t)std::max<int64_t>(0, pf_.local_clock_ms() - shown_at_ms_);
    std::string err;
    int original_queue = card_->queue;
    int64_t did = card_->did;
    // remember the deck counters so undo can put them back
    std::vector<std::pair<int64_t, std::string>> saved;
    {
        std::vector<int64_t> ids{did};
        for (int64_t p : col_.parent_deck_ids(did)) ids.push_back(p);
        for (int64_t id : ids)
            if (const Deck* d = col_.deck(id)) saved.push_back({id, d->common.encode()});
    }
    if (!sched_->answer(card_->id, r, millis, err, &states_)) {
        show_message("Couldn't save the answer", err, Mode::Decks);
        return;
    }
    deck_undo_.push_back(saved);
    if (deck_undo_.size() > 30) deck_undo_.erase(deck_undo_.begin());
    record_studied(col_, did, original_queue == Q_NEW, original_queue == Q_REVIEW || original_queue == Q_DAY_LEARN,
                   millis);
    if (auto updated = col_.get_card(card_->id)) queue_->on_answered(*updated);
    reviewed_++;
    next_card();
}

void App::undo() {
    if (!sched_ || !sched_->can_undo()) return;
    std::string err;
    auto cid = sched_->undo(err);
    if (!cid) return;
    if (!deck_undo_.empty()) {
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
    if (reviewed_ > 0) reviewed_--;
    queue_->build();
    auto c = col_.get_card(*cid);
    std::string kind = !c ? "review" : c->queue == Q_NEW ? "new" : c->queue == Q_REVIEW ? "review" : "learn";
    queue_->push_front({*cid, kind});
    status_ = "Undid the last answer";
    next_card();
}

int App::max_scroll() const { return std::max(0, view_->content_height() - 240); }

std::vector<Button> App::review_buttons() {
    const Theme& t = theme(settings_.night);
    if (!answer_shown_) return {{{10, 160, 300, 60}, "Show answer", "", "A"}};
    std::vector<Button> b = {
        {{4, 160, 75, 70}, "Again", labels_.size() > 0 ? labels_[0] : "", "B", t.again},
        {{83, 160, 75, 70}, "Hard", labels_.size() > 1 ? labels_[1] : "", "Y", t.hard},
        {{162, 160, 75, 70}, "Good", labels_.size() > 2 ? labels_[2] : "", "A", t.good},
        {{241, 160, 75, 70}, "Easy", labels_.size() > 3 ? labels_[3] : "", "X", t.easy},
    };
    return b;
}

void App::on_review(const Input& in) {
    // scrolling
    int step = 0;
    if (in.held & K_DOWN) step += 6;
    if (in.held & K_UP) step -= 6;
    if (in.down & K_R) step += 200;
    if (in.down & K_L) step -= 200;
    step += in.scroll;
    if (step) {
        int ns = std::clamp(scroll_ + step, 0, max_scroll());
        if (ns != scroll_) {
            scroll_ = ns;
            dirty = true;
        }
    }
    if (in.down & K_START) {
        build_rows();
        mode_ = Mode::Decks;
        return;
    }
    if (in.down & K_SELECT) {
        undo();
        return;
    }
    Rect reveal_r{10, 112, 300, 34};
    if (answer_shown_ && ((in.down & K_ZR) || (in.touch_down && hit(reveal_r, in.tx, in.ty)))) {
        settings_.reveal_hidden = !settings_.reveal_hidden;
        settings_.save(settings_path_);
        int keep = scroll_;
        render_current();
        scroll_ = std::min(keep, max_scroll());
        return;
    }
    // ◀ ▶ go to the card's own keyboard shortcuts (e.g. Malleus one-by-one reveal)
    if (script_ && (in.down & (K_LEFT | K_RIGHT))) {
        if (script_->key((in.down & K_RIGHT) ? "ArrowRight" : "ArrowLeft")) {
            current_html_ = script_->html();
            int keep = scroll_;
            render_current();
            scroll_ = std::min(keep, max_scroll());
        }
        return;
    }
    auto buttons = review_buttons();
    int touched = -1;
    if (in.touch_down)
        for (size_t i = 0; i < buttons.size(); i++)
            if (hit(buttons[i].r, in.tx, in.ty)) touched = (int)i;
    if (!answer_shown_) {
        if ((in.down & K_A) || touched == 0) show_answer();
        return;
    }
    if ((in.down & K_B) || touched == 0) answer(Rating::Again);
    else if ((in.down & K_Y) || touched == 1) answer(Rating::Hard);
    else if ((in.down & K_A) || touched == 2) answer(Rating::Good);
    else if ((in.down & K_X) || touched == 3) answer(Rating::Easy);
}

void App::draw_review() {
    const Theme& t = theme(settings_.night);
    const Canvas& cv = view_->canvas();
    top.resize(400, 240, t.bg);
    for (int y = 0; y < 240 && y + scroll_ < cv.height(); y++)
        std::copy(cv.data() + (size_t)(y + scroll_) * cv.width(), cv.data() + (size_t)(y + scroll_) * cv.width() + 400,
                  top.data() + (size_t)y * 400);
    if (cv.height() > 240) {
        int bar_h = std::max(12, 240 * 240 / cv.height());
        int bar_y = (240 - bar_h) * scroll_ / std::max(1, max_scroll());
        top.fill_round_rect(395, bar_y, 4, bar_h, 2, rgba(0, 0, 0, 90));
    }
    bottom.resize(320, 240, t.bg);
    if (queue_) {
        DeckCounts c = queue_->counts();
        std::string kind = queued_ ? queued_->kind : "";
        auto chip = [&](float x, uint32_t v, uint32_t col, bool current) {
            std::string s = std::to_string(v);
            float w = text_w(fonts_, s, 16, FACE_SANS_BOLD);
            if (current) bottom.fill_rect((int)x, 34, (int)w, 2, col);
            text(bottom, fonts_, x, 12, s, 16, col, FACE_SANS_BOLD);
            return x + w + 14;
        };
        float x = 12;
        x = chip(x, c.new_count, t.new_c, kind == "new");
        x = chip(x, c.learn_count, t.learn_c, kind == "learn" || kind == "daylearn");
        chip(x, c.review_count, t.review_c, kind == "review");
        const Deck* d = col_.deck(deck_id_);
        if (d) {
            std::string name = d->human_name();
            size_t p = name.rfind("::");
            std::string shortname = p == std::string::npos ? name : name.substr(p + 2);
            text(bottom, fonts_, 308 - text_w(fonts_, fit(fonts_, shortname, 13, 170), 13), 14,
                 fit(fonts_, shortname, 13, 170), 13, t.dim);
        }
    }
    text(bottom, fonts_, 12, 50, script_ ? "↑↓ scroll   L/R page   ◀▶ card keys" : "↑↓ / circle pad: scroll   L/R: page", 12, t.dim);
    text(bottom, fonts_, 12, 68, sched_ && sched_->can_undo() ? "START: decks   SELECT: undo" : "START: back to decks", 12, t.dim);
    if (max_scroll() > 0 && scroll_ < max_scroll())
        text(bottom, fonts_, 12, 90, "More below ↓", 13, t.accent, FACE_SANS_BOLD);
    if (answer_shown_)
        button(bottom, fonts_,
               {{10, 112, 300, 34}, settings_.reveal_hidden ? "Hide extra sections" : "Show extra sections", "", "ZR",
                t.hard},
               settings_.night);
    for (auto& b : review_buttons()) button(bottom, fonts_, b, settings_.night);
}

void App::draw_done() {
    const Theme& t = theme(settings_.night);
    top.resize(400, 240, t.bg);
    text_centered(top, fonts_, {0, 70, 400, 34}, "Congratulations!", 24, t.text, FACE_SANS_BOLD);
    std::string msg = "You have finished this deck for now.";
    if (reviewed_) msg += " " + std::to_string(reviewed_) + " cards studied this session.";
    paragraph(top, fonts_, 40, 115, 320, msg, 15, t.dim);
    bottom.resize(320, 240, t.bg);
    button(bottom, fonts_, {{20, 90, 280, 50}, "Back to decks", "", "A"}, settings_.night);
}

// ---------------------------------------------------------------- settings

static const char* SETTING_NAMES[] = {"Text size",          "Night mode",        "Time zone",
                                      "Download images automatically", "Run card scripts", "Sign in again",
                                      "Replace this 3DS copy with the server's", "About"};
static const int SETTING_COUNT = 8;

void App::on_settings(const Input& in) {
    if (in.down & K_DOWN) setting_sel_ = (setting_sel_ + 1) % SETTING_COUNT;
    if (in.down & K_UP) setting_sel_ = (setting_sel_ + SETTING_COUNT - 1) % SETTING_COUNT;
    int delta = (in.down & K_RIGHT) ? 1 : (in.down & K_LEFT) ? -1 : 0;
    bool activate = (in.down & K_A) != 0;
    if (in.down & (K_B | K_SELECT | K_START)) {
        settings_.save(settings_path_);
        if (col_.is_open()) {
            build_rows();
            mode_ = Mode::Decks;
        } else {
            mode_ = Mode::Setup;
        }
        return;
    }
    switch (setting_sel_) {
        case 0:
            if (delta) settings_.text_zoom = std::clamp(settings_.text_zoom + delta * 0.05f, 0.45f, 1.3f);
            break;
        case 1:
            if (delta || activate) settings_.night = !settings_.night;
            break;
        case 2:
            if (delta) {
                settings_.minutes_west = settings_.minutes_west.value_or(0) - delta * 30;
                settings_.tz_auto = false;
            }
            if (activate) settings_.tz_auto = true;
            break;
        case 3:
            if (delta || activate) settings_.auto_media = !settings_.auto_media;
            break;
        case 4:
            if (delta || activate) settings_.run_scripts = !settings_.run_scripts;
            break;
        case 5:
            if (activate) login_flow(settings_.endpoint != "https://sync.ankiweb.net/");
            break;
        case 6:
            if (activate) {
                if (settings_.hkey.empty()) show_message("Not signed in", "Sign in first.", Mode::Settings);
                else full_download_flow();
            }
            break;
        case 7:
            if (activate)
                show_message("About Anki for 3DS",
                             "An unofficial Anki client. Scheduling is a port of Anki 26.08 (FSRS, load balancer, "
                             "fuzz) verified against the real engine.\n\nUses SQLite, zstd, mbedTLS, litehtml, "
                             "QuickJS, stb, nanosvg, nlohmann/json and DejaVu fonts.",
                             Mode::Settings);
            break;
    }
    if (delta) settings_.save(settings_path_);
}

void App::draw_settings() {
    const Theme& t = theme(settings_.night);
    top.resize(400, 240, t.bg);
    draw_header(top, "Settings", settings_.username);
    for (int i = 0; i < SETTING_COUNT; i++) {
        int y = 30 + i * 26;
        bool sel = i == setting_sel_;
        if (sel) top.fill_round_rect(4, y, 392, 24, 6, t.accent);
        uint32_t tc = sel ? t.accent_text : t.text;
        text(top, fonts_, 14, (float)y + 4, SETTING_NAMES[i], 14, tc);
        std::string value;
        switch (i) {
            case 0: value = "‹ " + std::to_string((int)std::lround(settings_.text_zoom * 100)) + "% ›"; break;
            case 1: value = settings_.night ? "On" : "Off"; break;
            case 2:
                value = "‹ " + fmt_offset(settings_.minutes_west.value_or(0)) + (settings_.tz_auto ? " (auto)" : "") + " ›";
                break;
            case 3: value = settings_.auto_media ? "On" : "Off"; break;
            case 4: value = settings_.run_scripts ? "On" : "Off"; break;
            case 5: value = settings_.hkey.empty() ? "Not signed in" : "Signed in"; break;
            default: break;
        }
        if (!value.empty()) text(top, fonts_, 386 - text_w(fonts_, value, 14), (float)y + 4, value, 14, tc);
    }
    bottom.resize(320, 240, t.bg);
    std::string help;
    switch (setting_sel_) {
        case 0: help = "Cards are styled for big screens. Shrink or enlarge card text with ◀ ▶."; break;
        case 1: help = "Dark background for cards and menus."; break;
        case 2:
            help = "The 3DS clock has no time zone. 'Auto' uses the offset desktop Anki last recorded, so daylight "
                   "saving follows your other devices after a sync. Use ◀ ▶ to set it by hand, A to go back to auto. "
                   "The server refuses syncs if clocks disagree by more than 5 minutes.";
            break;
        case 3: help = "When you open a deck, fetch any of its images that aren't on the SD card yet."; break;
        case 4:
            help = "Runs the JavaScript in card templates (for example Malleus one-by-one clozes: press ▶ to reveal "
                   "the next line, ◀ to hide it). Turn off if a deck's cards misbehave.";
            break;
        case 5: help = "Sign in again, e.g. after changing your password. Server: " + settings_.endpoint; break;
        case 6:
            help = "Downloads the whole collection from the sync server again. Reviews on this 3DS that haven't "
                   "been synced will be lost.";
            break;
        default: help = "Version 0.1.0 (first draft)."; break;
    }
    paragraph(bottom, fonts_, 12, 12, 296, help, 14, t.text);
    text(bottom, fonts_, 12, 214, "A: select   ◀ ▶: change   B: back", 12, t.dim);
}

// ---------------------------------------------------------------- message

void App::draw_message() {
    const Theme& t = theme(settings_.night);
    top.resize(400, 240, t.bg);
    text_centered(top, fonts_, {0, 30, 400, 30}, message_title_, 20, t.text, FACE_SANS_BOLD);
    paragraph(top, fonts_, 30, 75, 340, message_body_, 14, t.text);
    bottom.resize(320, 240, t.bg);
    button(bottom, fonts_, {{20, 90, 280, 50}, "OK", "", "A"}, settings_.night);
}

void App::draw() {
    dirty = false;
    switch (mode_) {
        case Mode::Setup: draw_setup(); break;
        case Mode::Decks: draw_decks(); break;
        case Mode::Review: draw_review(); break;
        case Mode::Done: draw_done(); break;
        case Mode::Settings: draw_settings(); break;
        case Mode::Message: draw_message(); break;
        case Mode::ConfirmDownload: {
            const Theme& t = theme(settings_.night);
            top.resize(400, 240, t.bg);
            text_centered(top, fonts_, {0, 30, 400, 30}, "Full download needed", 20, t.text, FACE_SANS_BOLD);
            paragraph(top, fonts_, 30, 75, 340,
                      "The server's collection changed in a way that can't be merged (for example a note type was "
                      "edited on desktop). Download the server's copy to replace the one on this 3DS?\n\nReviews "
                      "done on this 3DS since the last successful sync will be lost.",
                      14, t.text);
            bottom.resize(320, 240, t.bg);
            button(bottom, fonts_, {{20, 60, 280, 50}, "Download from server", "", "A", t.again}, settings_.night);
            button(bottom, fonts_, {{20, 125, 280, 40}, "Not now", "", "B", t.hard}, settings_.night);
            break;
        }
    }
}

}  // namespace anki
