#include "app.h"
#include "../core/version.h"

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
    scratchpad = j.value("scratchpad", scratchpad);
    auto_sync = std::clamp(j.value("auto_sync", auto_sync), 0, 3);
    if (j.contains("buttons") && j["buttons"].is_object())
        for (auto& [k, v] : j["buttons"].items())
            if (v.is_string()) buttons[k] = v.get<std::string>();
    if (j.contains("note_type_buttons") && j["note_type_buttons"].is_object())
        for (auto& [mid, m] : j["note_type_buttons"].items())
            if (m.is_object())
                for (auto& [k, v] : m.items())
                    if (v.is_string()) nt_buttons[mid][k] = v.get<std::string>();
}

void Settings::save(const std::string& path) const {
    json j = {{"endpoint", endpoint}, {"hkey", hkey},       {"username", username},
              {"text_zoom", text_zoom}, {"night", night}, {"auto_media", auto_media},
              {"reveal_hidden", reveal_hidden}, {"tz_auto", tz_auto}, {"run_scripts", run_scripts},
              {"scratchpad", scratchpad}, {"auto_sync", auto_sync}};
    if (minutes_west) j["minutes_west"] = *minutes_west;
    j["buttons"] = buttons;
    j["note_type_buttons"] = nt_buttons;
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
void flush_timing(const std::string& dir);
App::~App() {
    finish_job();
    worker_.wait();
    prep_.wait();
    flush_timing(pf_.data_dir());
}

void App::flush_logs() { flush_timing(pf_.data_dir()); }

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
    engine_ = make_anki_engine();  // Anki's own backend when built with it
    if (!engine_) engine_ = make_cpp_engine();
    if (exists(col_path_)) open_collection();
    else mode_ = Mode::Setup;
    startup_sync_due_ = eng()->is_open() && mode_ == Mode::Decks && (settings_.auto_sync & 1);
    dirty = true;
}

void App::on_pause() {
    if (!paused_at_ms_) paused_at_ms_ = pf_.local_clock_ms();
    flush_timing(pf_.data_dir());
    // the lid may stay closed until the battery runs out: get the answers so
    // far into the collection file (queued behind any answer still saving)
    if (engine_ && engine_->is_open()) {
        Engine* e = engine_.get();
        worker_.submit([e] { e->checkpoint(); });
    }
}

void App::on_resume() {
    if (!paused_at_ms_) return;
    if (mode_ == Mode::Review) shown_at_ms_ += pf_.local_clock_ms() - paused_at_ms_;
    paused_at_ms_ = 0;
}

int64_t App::utc_ms() {
    int32_t west = settings_.minutes_west.value_or(0);
    return pf_.local_clock_ms() + (int64_t)west * 60000;
}

void App::configure_clock() {
    eng()->set_clock([this] { return utc_ms(); }, settings_.minutes_west.value_or(0));
}

void App::open_collection() {
    std::string err;
    if (!eng()->open(col_path_, err)) {
        show_message("Couldn't open your collection", err + "\n\nYou can download it again from your sync server in Settings.",
                     Mode::Settings);
        return;
    }
    if (!settings_.minutes_west || settings_.tz_auto) {
        // Use the host's time zone if it has one (desktop simulator), otherwise the
        // offset desktop Anki last recorded in the collection.
        auto sys = pf_.system_minutes_west();
        int32_t west = sys ? *sys : eng()->recorded_minutes_west().value_or(settings_.minutes_west.value_or(0));
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
    if (!eng()->is_open()) return;
    std::vector<std::string> collapsed_names;  // children of these are hidden
    for (const DeckEntry& d : eng()->decks()) {
        bool hidden = false;
        for (auto& cn : collapsed_names)
            if (d.full_name.size() > cn.size() + 2 && d.full_name.compare(0, cn.size(), cn) == 0 &&
                d.full_name.compare(cn.size(), 2, "::") == 0)
                hidden = true;
        bool collapsed = d.collapsed;
        if (std::find(collapsed_local_.begin(), collapsed_local_.end(), d.id) != collapsed_local_.end())
            collapsed = !collapsed;
        if (collapsed && d.has_children && !hidden) collapsed_names.push_back(d.full_name);
        if (hidden) continue;
        rows_.push_back({d.id, d.name, d.full_name, d.depth, d.has_children, collapsed && d.has_children, d.counts});
    }
    if (sel_ >= (int)rows_.size()) sel_ = std::max(0, (int)rows_.size() - 1);
}

bool App::debug_select(const std::string& needle) {
    for (size_t i = 0; i < rows_.size(); i++) {
        if (rows_[i].full_name.find(needle) != std::string::npos) {
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
    if (startup_sync_due_ && mode_ == Mode::Decks) {
        startup_sync_due_ = false;
        sync_flow(true);
    }
    // the answer saved in the background: show what the backend says
    if (job_pending_ && job_done_) {
        job_pending_ = false;
        apply_job();
    }
    // the next card, prepared in the background, as soon as it's ready
    if (mode_ == Mode::Review) {
        collect_prefetch();
        if (prefetch_due_ && !job_pending_) start_prefetch();
    }
    switch (mode_) {
        case Mode::Setup: on_setup(in); break;
        case Mode::Decks:
            if (in.down & K_START) return false;
            on_decks(in);
            break;
        case Mode::Review: on_review(in); break;
        case Mode::Done:
            if ((in.down & K_SELECT) && can_undo_) {
                undo();
                break;
            }
            if (in.down & (K_A | K_B | K_START) || in.touch_down) {
                build_rows();
                mode_ = Mode::Decks;
                sync_after_studying();
            }
            break;
        case Mode::Settings: on_settings(in); break;
        case Mode::Buttons: on_buttons(in); break;
        case Mode::Message:
            if (in.down & (K_A | K_B) || in.touch_down) {
                mode_ = message_return_;
                if (mode_ == Mode::Decks) build_rows();
            }
            break;
        case Mode::ConfirmDownload:
            if (in.down & K_A) full_download_flow();
            else if (in.down & K_B) mode_ = download_requested_ ? Mode::Settings : Mode::Decks;
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
    draws++;
    pf_.present(top, bottom);
}

// ---------------------------------------------------------------- setup

void App::on_setup(const Input& in) {
    Button own{{20, 50, 280, 56}, "Sign in to my sync server", "", "A"};
    if ((in.down & K_A) || (in.touch_down && hit(own.r, in.tx, in.ty))) login_flow(true);
#ifdef ANKI3DS_ANKIWEB
    // AnkiWeb sign-in is only compiled in with permission from the Anki team (see README)
    Button web{{20, 140, 280, 40}, "Sign in to AnkiWeb", "", "X"};
    if ((in.down & K_X) || (in.touch_down && hit(web.r, in.tx, in.ty))) login_flow(false);
#endif
}

void App::draw_setup() {
    const Theme& t = theme(settings_.night);
    top.resize(400, 240, t.bg);
    text_centered(top, fonts_, {0, 28, 400, 40}, APP_NAME, 30, t.text, FACE_SANS_BOLD);
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
#ifdef ANKI3DS_ANKIWEB
    button(bottom, fonts_, {{20, 140, 280, 40}, "Sign in to AnkiWeb", "", "X", t.hard}, settings_.night);
#else
    paragraph(bottom, fonts_, 24, 140, 272,
              "AnkiWeb isn't available: its terms only allow Anki's own apps. Point your other devices at the "
              "same sync server instead.",
              12, t.dim);
#endif
    text_centered(bottom, fonts_, {0, 214, 320, 20}, "Press HOME to exit", 12, t.dim);
}

void App::confirm_download(bool requested) {
    unsynced_reviews_ = eng()->is_open() ? eng()->unsynced_reviews() : 0;
    download_requested_ = requested;
    mode_ = Mode::ConfirmDownload;
    dirty = true;
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
    if (!server_allowed(endpoint)) {
        show_message("AnkiWeb isn't available",
                     "AnkiWeb's terms only allow Anki's own apps. Use your own sync server's address instead.",
                     eng()->is_open() ? Mode::Settings : Mode::Setup);
        return;
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
    std::string key, final_endpoint;
    if (!eng()->login(endpoint, user, pass, key, final_endpoint, err)) {
        pf_.net_down();
        show_message("Sign-in failed", err, eng()->is_open() ? Mode::Settings : Mode::Setup);
        return;
    }
    settings_.endpoint = final_endpoint;
    settings_.hkey = key;
    settings_.username = user;
    settings_.save(settings_path_);
    if (!eng()->is_open()) {
        pf_.net_down();
        full_download_flow();
    } else {
        pf_.net_down();
        sync_flow();
    }
}

// ---------------------------------------------------------------- sync

bool App::server_allowed(const std::string& endpoint) const {
#ifdef ANKI3DS_ANKIWEB
    (void)endpoint;
    return true;
#else
    std::string host = endpoint;
    size_t p = host.find("://");
    if (p != std::string::npos) host = host.substr(p + 3);
    host = host.substr(0, host.find_first_of("/:"));
    for (auto& ch : host) ch = (char)tolower((unsigned char)ch);
    return !(host == "ankiweb.net" || (host.size() > 12 && host.compare(host.size() - 12, 12, ".ankiweb.net") == 0));
#endif
}

static const char* ANKIWEB_BLOCKED =
    "This 3DS is signed in to AnkiWeb, but AnkiWeb's terms only allow Anki's own apps, so Clamshell won't sync "
    "with it. Sign in to your own sync server in Settings > Sign in again.";

// automatic: started by the "Sync automatically" setting. Problems go in the
// deck list's status line rather than a message, except a full download, which always asks.
void App::sync_flow(bool automatic) {
    if (settings_.hkey.empty()) {
        if (!automatic) show_message("Not signed in", "Sign in to your sync server from Settings first.", Mode::Decks);
        return;
    }
    if (!server_allowed(settings_.endpoint)) {
        if (automatic) {
            status_ = "Not syncing: AnkiWeb isn't allowed. Sign in to your own server in Settings.";
            mode_ = Mode::Decks;
            dirty = true;
        } else {
            show_message("AnkiWeb isn't available", ANKIWEB_BLOCKED, Mode::Decks);
        }
        return;
    }
    std::string err;
    progress("Syncing…", "Connecting");
    if (!pf_.net_up(err)) {
        if (automatic) {
            status_ = "Couldn't sync: " + err;
            mode_ = Mode::Decks;
            build_rows();
            dirty = true;
        } else {
            show_message("No network", err, Mode::Decks);
        }
        return;
    }
    SyncOutcome r = eng()->sync(settings_.endpoint, settings_.hkey,
                                  [this](const std::string& s) { progress("Syncing…", s); });
    pf_.net_down();
    if (r.new_endpoint) {
        settings_.endpoint = *r.new_endpoint;
        settings_.save(settings_path_);
    }
    switch (r.kind) {
        case SyncOutcome::NoChanges:
        case SyncOutcome::Synced: {
            if (settings_.tz_auto && !pf_.system_minutes_west()) {
                int32_t west = eng()->recorded_minutes_west().value_or(settings_.minutes_west.value_or(0));
                if (west != settings_.minutes_west) {
                    settings_.minutes_west = west;
                    settings_.save(settings_path_);
                    configure_clock();
                }
            }
            char buf[64];
            time_t now = (time_t)(pf_.local_clock_ms() / 1000);
            struct tm tmv;
            gmtime_r(&now, &tmv);
            strftime(buf, sizeof buf, "%H:%M", &tmv);
            status_ = std::string(r.kind == SyncOutcome::Synced ? "Synced" : "Already up to date") + " at " + buf;
            if (!r.message.empty()) show_message("Message from the sync server", r.message, Mode::Decks);
            else mode_ = Mode::Decks;
            build_rows();
            break;
        }
        case SyncOutcome::FullRequired:
            if (r.download_ok) {
                confirm_download(false);
            } else {
                show_message("Can't sync",
                             "The server's collection is empty. Sync from desktop Anki first.", Mode::Decks);
            }
            break;
        default:
            if (automatic) {
                status_ = "Couldn't sync: " + r.error;
                mode_ = Mode::Decks;
                build_rows();
            } else {
                show_message("Sync failed", r.error, Mode::Decks);
            }
    }
    dirty = true;
}

void App::sync_after_studying() {
    if (reviewed_ > 0 && (settings_.auto_sync & 2)) sync_flow(true);
}

void App::full_download_flow() {
    if (!server_allowed(settings_.endpoint)) {
        show_message("AnkiWeb isn't available", ANKIWEB_BLOCKED, eng()->is_open() ? Mode::Decks : Mode::Setup);
        return;
    }
    std::string err;
    progress("Downloading collection…", "Connecting");
    if (!pf_.net_up(err)) {
        show_message("No network", err, eng()->is_open() ? Mode::Decks : Mode::Setup);
        return;
    }
    size_t last_mb = (size_t)-1;
    auto on_bytes = [&](size_t done, size_t total) {
        size_t mb = done / (1024 * 1024);
        if (mb != last_mb) {
            last_mb = mb;
            char buf[64];
            if (total) snprintf(buf, sizeof buf, "%.1f of %.1f MB", done / 1048576.0, total / 1048576.0);
            else snprintf(buf, sizeof buf, "%.1f MB", done / 1048576.0);
            progress("Downloading collection…", buf);
        }
    };
    bool ok = eng()->full_download(settings_.endpoint, settings_.hkey, col_path_, on_bytes, err);
    pf_.net_down();
    if (!ok) {
        show_message("Download failed", err, eng()->is_open() ? Mode::Decks : Mode::Setup);
        return;
    }
    open_collection();
    status_ = "Downloaded from the sync server";
    dirty = true;
}

void App::media_flow(int64_t deck_id, bool quiet) {
    if (settings_.hkey.empty()) return;
    auto names = missing_files(eng()->deck_media_names(deck_id), media_dir_);
    if (names.empty()) {
        if (!quiet) show_message("Images", "All images for this deck are already on the SD card.", Mode::Decks);
        return;
    }
    fetch_media(names, quiet);
}

// quiet: fetching before a study session; never interrupt it with a message
void App::fetch_media(const std::vector<std::string>& names, bool quiet) {
    if (!server_allowed(settings_.endpoint)) {
        if (!quiet) show_message("AnkiWeb isn't available", ANKIWEB_BLOCKED, Mode::Decks);
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
    if (quiet) return;  // study anyway; missing images just don't show
    if (!ok) show_message("Image download stopped", err, Mode::Decks);
    else
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
        std::string full = r.full_name;
        float y = paragraph(bottom, fonts_, 12, 10, 296, full, 15, t.text, FACE_SANS_BOLD, 1.25f);
        std::string due = std::to_string(r.counts.new_count) + " new · " + std::to_string(r.counts.learn_count) +
                          " learning · " + std::to_string(r.counts.review_count) + " due";
        text(bottom, fonts_, 12, y + 2, due, 13, t.dim);
        if (preset_deck_ != r.id) {
            preset_deck_ = r.id;
            preset_name_ = eng()->deck_preset(r.id);
        }
        if (!preset_name_.empty())
            text(bottom, fonts_, 12, y + 20, fit(fonts_, "Preset: " + preset_name_, 12, 296), 12, t.dim);
    }
    if (!status_.empty()) text(bottom, fonts_, 12, 128, fit(fonts_, status_, 12, 296), 12, t.dim);
    button(bottom, fonts_, {{10, 150, 145, 44}, "Study", "", "A"}, settings_.night);
    button(bottom, fonts_, {{165, 150, 145, 44}, "Sync", "", "X", t.good}, settings_.night);
    button(bottom, fonts_, {{10, 200, 145, 32}, "Get images", "", "Y", t.hard}, settings_.night);
    button(bottom, fonts_, {{165, 200, 145, 32}, "Settings", "", "SEL", t.hard}, settings_.night);
}

// ---------------------------------------------------------------- review

// Anki's flag colours, in flag order 1-7
static const struct {
    const char* name;
    uint8_t r, g, b;
} FLAGS[] = {{"red", 230, 60, 60},   {"orange", 245, 150, 40}, {"green", 60, 170, 80},  {"blue", 60, 120, 230},
             {"pink", 240, 110, 190}, {"turquoise", 40, 190, 190}, {"purple", 150, 90, 210}};

// How long each step of studying takes, in timing.txt in the data folder:
// written at once by self-test builds, otherwise kept in memory and written
// a few kilobytes at a time (and when leaving a deck), to keep SD writes rare.
static int64_t step_t_ = 0;
static std::string timing_buf_;
void flush_timing(const std::string& dir) {
    if (timing_buf_.empty()) return;
    std::string path = dir + "/timing.txt";
    FILE* f = fopen(path.c_str(), "ab");
    if (f && ftell(f) > 256 * 1024) {
        fclose(f);
        f = fopen(path.c_str(), "wb");
    }
    if (f) {
        fwrite(timing_buf_.data(), 1, timing_buf_.size(), f);
        fclose(f);
    }
    timing_buf_.clear();
}
#define STEP_MARK() (step_t_ = pf_.local_clock_ms())
#define STEP_NOTE(text)                                                         \
    do {                                                                        \
        timing_buf_ += "    ";                                                  \
        timing_buf_ += (text);                                                  \
        timing_buf_ += "\n";                                                    \
    } while (0)
#ifdef ANKI3DS_SELFTEST
#define TIMING_FLUSH_AT 0
#else
#define TIMING_FLUSH_AT 4096
#endif
#define STEP_TIME(label)                                                          \
    do {                                                                          \
        int64_t now_ = pf_.local_clock_ms();                                      \
        char line_[96];                                                           \
        snprintf(line_, sizeof line_, "%s %lld ms\n", label, (long long)(now_ - step_t_)); \
        timing_buf_ += line_;                                                     \
        if (timing_buf_.size() > TIMING_FLUSH_AT) flush_timing(pf_.data_dir());   \
        step_t_ = pf_.local_clock_ms();                                           \
    } while (0)

void App::start_review(int64_t deck_id) {
    cancel_prefetch();
    prefetch_due_ = false;
    STEP_MARK();
    deck_id_ = deck_id;
    progress("Loading…", "Building today's queue");
    std::string err;
    if (!eng()->start(deck_id, err)) {
        show_message("Couldn't open the deck", err, Mode::Decks);
        return;
    }
    STEP_TIME("queue build");
    // fetch images for today's cards only (works the same for filtered decks);
    // Y on the deck list fetches a whole deck
    if (settings_.auto_media && !settings_.hkey.empty()) {
        auto missing = missing_files(eng()->media_names(eng()->todays_card_ids()), media_dir_);
        STEP_TIME("image check");
        if (!missing.empty()) fetch_media(missing, true);
        STEP_TIME("image download");
    }
    reviewed_ = 0;
    next_card();
}

void App::next_card() {
    STEP_MARK();
    std::string err;
    card_ = eng()->next(counts_, next_id_, err, prefetch_ ? prefetch_->card_id : 0);
    can_undo_ = eng()->can_undo();
    present_card(err);
}

void App::present_card(const std::string& err) {
    // the card on screen's answer may still be running in script_
    if (ahead_ && !ahead_->done) prep_.wait_until([this] { return ahead_->done.load(); });
    ahead_.reset();
    collect_prefetch();
    // the next card may be on its way already: better to wait for it than redo it
    if (card_ && prefetch_job_ && prefetch_job_->card_id == card_->id && !prefetch_job_->done) {
        auto job = prefetch_job_;
        prep_.wait_until([job] { return job->done.load(); });
        collect_prefetch();
    }
    if (card_ && card_->prepared && prefetch_ && prefetch_->card_id == card_->id) {
        // details the engine left out because the app already rendered this card
        const StudyCard& p = prefetch_->card;
        card_->note_id = p.note_id;
        card_->notetype_id = p.notetype_id;
        card_->notetype = p.notetype;
        card_->deck_id = p.deck_id;
        card_->question = p.question;
        card_->answer = p.answer;
        card_->css = p.css;
        card_->ord = p.ord;
        card_->flags = p.flags;
        card_->marked = p.marked;
    } else if (card_ && card_->prepared) {
        // shouldn't happen, but render it rather than show nothing
        cancel_prefetch();
        if (auto full = eng()->render(card_->id)) {
            full->kind = card_->kind;
            full->labels = card_->labels;
            card_ = full;
        }
    }
    if (!card_) {
        if (!err.empty()) {
            show_message("Couldn't load the next card", err, Mode::Decks);
            return;
        }
        mode_ = Mode::Done;
        dirty = true;
        return;
    }
    clear_scratch();
    card_note_.clear();
    zoom_open_ = false;
    card_marked_ = card_->marked;
    current_mid_ = card_->notetype_id;
    current_nt_name_ = card_->notetype;
    menu_open_ = false;
    labels_ = card_->labels;
    STEP_TIME("  next states");
    answer_shown_ = false;
    prefetch_due_ = false;
    if (prefetch_ && prefetch_->card_id == card_->id) {
        rendered_ = std::move(prefetch_->rendered);
        script_ = std::move(prefetch_->script);
        current_html_ = std::move(prefetch_->html);
        std::swap(view_, prefetch_->view);
        cancel_prefetch();
        dirty = true;
        STEP_TIME("  prefetched");
    } else {
        cancel_prefetch();
        settle_prep();  // a discarded job may still be using js_storage_
        rendered_.question = card_->question;
        rendered_.answer = card_->answer;
        rendered_.css = card_->css;
        rendered_.ord = card_->ord;
        STEP_TIME("  template");
        script_.reset();
        if (settings_.run_scripts) {
            script_ = std::make_unique<js::CardScript>();
            current_html_ = script_->run(rendered_.question, js_storage_);
        } else {
            current_html_ = rendered_.question;
        }
        STEP_TIME("  card script");
        render_current();
        STEP_TIME("  layout+draw");
    }
    scroll_ = 0;
    shown_at_ms_ = pf_.local_clock_ms();
    mode_ = Mode::Review;
    dirty = true;
    start_answer_ahead();
}

CardViewOptions App::view_options() const {
    CardViewOptions opt;
    opt.media_dir = media_dir_;
    opt.text_zoom = settings_.text_zoom;
    opt.night_mode = settings_.night;
    opt.reveal_hidden = answer_shown_ && settings_.reveal_hidden;  // extra sections belong to the answer side
    return opt;
}

void App::render_current() {
    view_->render(current_html_, rendered_.css, rendered_.ord, view_options());
    dirty = true;
    zoom_dirty_ = true;
    if (zoom_open_) clamp_zoom();
#ifdef ANKI3DS_SELFTEST
    if (FILE* f = fopen((pf_.data_dir() + "/timing.txt").c_str(), "ab")) {
        const Canvas& cv = view_->canvas();
        fprintf(f, "render night=%d view=%p px=%08x\n", (int)settings_.night, (void*)view_.get(),
                cv.data()[(size_t)std::min(cv.height() - 1, 200) * cv.width() + 10]);
        fclose(f);
    }
#endif
}

void App::cancel_prefetch() {
    prefetch_.reset();
    prefetch_job_.reset();  // a job still running finishes on its own, unused
}

void App::settle_prep() {
    prep_.wait();
    collect_prefetch();
}

void App::collect_prefetch() {
    if (!prefetch_job_ || !prefetch_job_->done) return;
    auto job = std::move(prefetch_job_);
    char line[64];
    snprintf(line, sizeof line, "prepared next card in the background: %.0f ms", job->ms);
    STEP_NOTE(line);
    if (job->result) prefetch_ = std::move(job->result);
}

// The likely next card: Anki renders it on worker_, then prep_ decodes its
// images, runs its script and lays it out. Nothing here waits.
void App::start_prefetch() {
    prefetch_due_ = false;
    if (!next_id_ || (prefetch_ && prefetch_->card_id == next_id_)) return;
    if (prefetch_job_ && prefetch_job_->card_id == next_id_) return;
    cancel_prefetch();
    auto job = std::make_shared<PrefetchJob>();
    job->card_id = next_id_;
    prefetch_job_ = job;
    Engine* e = engine_.get();
    int64_t id = next_id_;
    CardViewOptions opt = view_options();
    opt.reveal_hidden = false;  // question side
    bool scripts = settings_.run_scripts;
    std::string dir = media_dir_;
    worker_.submit([this, e, id, job, opt, scripts, dir] {
        auto c = e->render(id);
        if (!c) {
            job->done = true;
            return;
        }
        submit_prep(job, *c, opt, scripts, dir);
    });
}

void App::prepare_prefetch(std::shared_ptr<PrefetchJob> job, const StudyCard& c) {
    CardViewOptions opt = view_options();
    opt.reveal_hidden = false;  // question side
    submit_prep(job, c, opt, settings_.run_scripts, media_dir_);
}

// Any thread: everything it needs is passed in.
void App::submit_prep(std::shared_ptr<PrefetchJob> job, StudyCard c, CardViewOptions opt, bool scripts,
                      std::string dir) {
    prep_.submit([this, job, c, opt, scripts, dir] {
        int64_t t0 = pf_.local_clock_ms();
        preload_media_images(dir, c.question + c.answer);
        auto p = std::make_unique<Prefetch>();
        p->card_id = c.id;
        p->card = c;
        p->rendered.question = c.question;
        p->rendered.answer = c.answer;
        p->rendered.css = c.css;
        p->rendered.ord = c.ord;
        if (scripts) {
            p->script = std::make_unique<js::CardScript>();
            p->html = p->script->run(p->rendered.question, js_storage_);
        } else {
            p->html = p->rendered.question;
        }
        p->view = std::make_unique<CardView>(fonts_);
        p->view->render(p->html, p->rendered.css, p->rendered.ord, opt);
        job->result = std::move(p);
        job->ms = (double)(pf_.local_clock_ms() - t0);
        job->done = true;
    });
}

// The answer of the card on screen, run and laid out on prep_ while the
// question shows. It runs in script_, which the main thread then leaves
// alone until the job is done (settle_prep).
void App::start_answer_ahead() {
    auto a = std::make_shared<AheadJob>();
    a->opts = view_options();
    a->opts.reveal_hidden = settings_.reveal_hidden;  // as it will be once the answer shows
    ahead_ = a;
    js::CardScript* sc = script_.get();
    std::string answer = rendered_.answer, css = rendered_.css, dir = media_dir_;
    int ord = rendered_.ord;
    prep_.submit([this, a, sc, answer, css, ord, dir] {
        int64_t t0 = pf_.local_clock_ms();
        preload_media_images(dir, answer);
        a->html = sc ? sc->show_answer(answer) : answer;
        a->view = std::make_unique<CardView>(fonts_);
        a->view->render(a->html, css, ord, a->opts);
        a->ms = (double)(pf_.local_clock_ms() - t0);
        a->done = true;
    });
}

void App::show_answer() {
    STEP_MARK();
    answer_shown_ = true;
    auto a = std::move(ahead_);
    if (a) {
        if (!a->done) {
            prep_.wait_until([&] { return a->done.load(); });
            STEP_TIME("answer: waited for it");
        }
        char line[64];
        snprintf(line, sizeof line, "answer prepared in the background: %.0f ms", a->ms);
        STEP_NOTE(line);
        current_html_ = a->html;
        CardViewOptions now = view_options();
        if (a->view && now.text_zoom == a->opts.text_zoom && now.night_mode == a->opts.night_mode &&
            now.reveal_hidden == a->opts.reveal_hidden) {
            std::swap(view_, a->view);
            dirty = true;
            zoom_dirty_ = true;
            if (zoom_open_) clamp_zoom();
            STEP_TIME("answer: prepared");
        } else {
            render_current();
            STEP_TIME("answer: layout+draw");
        }
    } else {
        settle_prep();
        current_html_ = script_ ? script_->show_answer(rendered_.answer) : rendered_.answer;
        render_current();
        STEP_TIME("answer: script+layout+draw");
    }
    int anchor = view_->answer_anchor();
    scroll_ = anchor > 60 ? std::min(anchor - 40, max_scroll()) : 0;
    dirty = true;
    prefetch_due_ = true;
}

void App::answer(Rating r) {
    if (!card_) return;
    uint32_t millis = (uint32_t)std::max<int64_t>(0, pf_.local_clock_ms() - shown_at_ms_);
    finish_job();  // the previous answer decides which card this is
    if (!card_ || mode_ != Mode::Review) return;
    STEP_MARK();
    // The card the backend expects next is laid out already: show it now and
    // save the answer on the worker, which also confirms what comes next.
    collect_prefetch();
    if (!prefetch_ && prefetch_job_ && prefetch_job_->card_id == next_id_ && next_id_) {
        auto job = prefetch_job_;  // nearly ready: worth the wait
        prep_.wait_until([job] { return job->done.load(); });
        collect_prefetch();
        STEP_TIME("  waited for the next card");
    }
    bool ahead = prefetch_ && next_id_ && prefetch_->card_id == next_id_;
    if (!ahead) {
        std::string err;
        if (!eng()->answer((int)r, millis, err)) {
            show_message("Couldn't save the answer", err, Mode::Decks);
            return;
        }
        STEP_TIME("answer saved");
        std::string io = eng()->io_stats();
        if (!io.empty()) STEP_NOTE(io.c_str());
        reviewed_++;
        next_card();
        return;
    }
    int64_t shown = next_id_;
    Engine* e = engine_.get();
    int rating = (int)r;
    job_ = AnswerJob{};
    job_.started_ms = pf_.local_clock_ms();
    job_done_ = false;
    job_pending_ = true;
    pending_id_ = shown;
    Platform* pf = &pf_;
    worker_.submit([this, e, pf, rating, millis, shown] {
        AnswerJob& j = job_;  // the worker's alone until job_done_
        j.ok = e->answer(rating, millis, j.err);
        j.saved_ms = pf->local_clock_ms();
        if (j.ok) {
            j.card = e->next(j.counts, j.next_id, j.err, shown);
            j.can_undo = e->can_undo();
            if (j.card && j.card->id == shown && j.next_id) j.next_render = e->render(j.next_id);
            j.io = e->io_stats();
        }
        job_done_ = true;
    });
    reviewed_++;
    // the counts until the backend's arrive: one fewer of the kind just answered
    // (an Again on a learning card brings it back, and the backend's numbers say so)
    if (card_->kind == "new" && counts_.new_count) counts_.new_count--;
    else if (card_->kind == "review" && counts_.review_count) counts_.review_count--;
    else if (card_->kind == "learn" && counts_.learn_count && rating > 1) counts_.learn_count--;
    card_ = prefetch_->card;
    card_->labels.clear();  // the intervals arrive with the backend's answer
    card_->prepared = false;
    can_undo_ = true;
    present_card("");
    STEP_TIME("  next card shown");
}

Engine* App::eng() {
    finish_job();
    worker_.wait();  // e.g. a checkpoint
    return engine_.get();
}

void App::finish_job() {
    if (!job_pending_) return;
    while (!job_done_) worker_.wait();
    job_pending_ = false;
    apply_job();
}

void App::apply_job() {
    AnswerJob j = std::move(job_);
    job_ = AnswerJob{};
    int64_t now = pf_.local_clock_ms();
    char line[96];
    snprintf(line, sizeof line, "background: saved %lld ms, next %lld ms",
             (long long)(j.saved_ms - j.started_ms), (long long)(now - j.saved_ms));
    STEP_NOTE(line);
    if (!j.io.empty()) STEP_NOTE(j.io.c_str());
    int64_t shown = pending_id_;
    pending_id_ = 0;
    if (!j.ok) {
        show_message("Couldn't save the answer", j.err, Mode::Decks);
        return;
    }
    counts_ = j.counts;
    next_id_ = j.next_id;
    can_undo_ = j.can_undo;
    if (mode_ != Mode::Review) return;  // left the deck meanwhile
    if (j.card && card_ && j.card->id == shown && card_->id == shown) {
        // as expected: fill in the intervals, and get the card after ready
        card_->labels = j.card->labels;
        card_->kind = j.card->kind;
        labels_ = card_->labels;
        dirty = true;
        cancel_prefetch();
        if (j.next_render) {
            auto job = std::make_shared<PrefetchJob>();
            job->card_id = j.next_render->id;
            prefetch_job_ = job;
            prepare_prefetch(job, *j.next_render);
            next_id_ = j.next_render->id;
            return;
        }
        prefetch_due_ = next_id_ != 0;
        return;
    }
    // Another card is due instead (a learning card came back, say), or none.
    cancel_prefetch();
    card_ = std::move(j.card);
    if (card_ && card_->prepared) {
        if (auto full = eng()->render(card_->id)) {
            full->kind = card_->kind;
            full->labels = card_->labels;
            card_ = full;
        }
    }
    STEP_MARK();
    present_card(j.err);
}

void App::undo() {
    if (!eng()->can_undo()) return;
    bool was_answer = true;
    std::string err;
    if (!eng()->undo(was_answer, err)) return;
    if (was_answer && reviewed_ > 0) reviewed_--;
    cancel_prefetch();
    status_ = "Undid the last change";
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

void App::clear_scratch() {
    scratch_.resize(320, 240, theme(settings_.night).bg);
    pen_x_ = pen_y_ = -1;
}

static const Rect MORE_BTN{246, 4, 70, 30}, CLEAR_BTN{172, 4, 70, 30}, ZOOM_BTN{172, 4, 70, 30};
static const Rect ZOOM_OUT{8, 4, 44, 26}, ZOOM_IN{56, 4, 44, 26}, ZOOM_CLOSE{246, 4, 70, 26};
static const int ZOOM_TOP = 34;  // the magnified view sits below the toolbar

void App::open_zoom() {
    if (!view_) return;
    zoom_open_ = true;
    menu_open_ = false;
    float vw = 320 / zoom_scale_;
    zoom_x_ = (400 - vw) / 2;
    zoom_y_ = (float)scroll_;
    clamp_zoom();
    zoom_dirty_ = true;
    dirty = true;
}

void App::clamp_zoom() {
    float vw = 320 / zoom_scale_, vh = (240 - ZOOM_TOP) / zoom_scale_;
    zoom_x_ = std::clamp(zoom_x_, 0.0f, std::max(0.0f, 400 - vw));
    zoom_y_ = std::clamp(zoom_y_, 0.0f, std::max(0.0f, (float)view_->content_height() - vh));
    // keep the magnified area on the top screen
    int top = (int)zoom_y_, bottom = (int)(zoom_y_ + vh);
    if (top < scroll_) scroll_ = top;
    if (bottom > scroll_ + 240) scroll_ = std::min(bottom - 240, max_scroll());
}

void App::on_zoom(const Input& in) {
    if (in.down & (K_B | K_START)) {
        zoom_open_ = false;
        dirty = true;
        return;
    }
    float ox = zoom_x_, oy = zoom_y_, os = zoom_scale_;
    if (in.touch_down && hit(ZOOM_CLOSE, in.tx, in.ty)) {
        zoom_open_ = false;
        dirty = true;
        return;
    }
    // change scale around the centre of the view
    auto rescale = [&](float ns) {
        float cx = zoom_x_ + 160 / zoom_scale_, cy = zoom_y_ + (240 - ZOOM_TOP) / 2 / zoom_scale_;
        zoom_scale_ = ns;
        zoom_x_ = cx - 160 / ns;
        zoom_y_ = cy - (240 - ZOOM_TOP) / 2 / ns;
    };
    if (in.touch_down && hit(ZOOM_OUT, in.tx, in.ty)) rescale(2.0f);
    else if (in.touch_down && hit(ZOOM_IN, in.tx, in.ty)) rescale(3.0f);
    else if (in.touching && in.ty >= ZOOM_TOP) {
        // drag: the card follows the stylus
        if (zoom_pen_x_ >= 0 && !in.touch_down) {
            zoom_x_ -= (in.tx - zoom_pen_x_) / zoom_scale_;
            zoom_y_ -= (in.ty - zoom_pen_y_) / zoom_scale_;
        }
        zoom_pen_x_ = in.tx;
        zoom_pen_y_ = in.ty;
    }
    if (!in.touching) zoom_pen_x_ = zoom_pen_y_ = -1;
    float step = 6 / zoom_scale_;
    if (in.held & K_LEFT) zoom_x_ -= step;
    if (in.held & K_RIGHT) zoom_x_ += step;
    if (in.held & K_UP) zoom_y_ -= step;
    if (in.held & K_DOWN) zoom_y_ += step;
    zoom_y_ += in.scroll / zoom_scale_;
    clamp_zoom();
    if (zoom_x_ != ox || zoom_y_ != oy || zoom_scale_ != os) {
        zoom_dirty_ = true;
        dirty = true;
    }
}

void App::draw_zoom() {
    const Theme& t = theme(settings_.night);
    bottom.resize(320, 240, t.bg);
    if (zoom_dirty_) {
        view_->draw_zoomed(zoom_, zoom_scale_, zoom_x_, zoom_y_);
        zoom_dirty_ = false;
    }
    for (int y = 0; y < zoom_.height() && ZOOM_TOP + y < 240; y++)
        std::copy(zoom_.data() + (size_t)y * 320, zoom_.data() + (size_t)y * 320 + 320,
                  bottom.data() + (size_t)(ZOOM_TOP + y) * 320);
    bottom.fill_rect(0, ZOOM_TOP - 1, 320, 1, t.border);
    button(bottom, fonts_, {ZOOM_OUT, "2×", "", "", zoom_scale_ == 2.0f ? 0u : t.hard}, settings_.night);
    button(bottom, fonts_, {ZOOM_IN, "3×", "", "", zoom_scale_ == 3.0f ? 0u : t.hard}, settings_.night);
    text(bottom, fonts_, 108, 11, "drag to move", 12, t.dim);
    button(bottom, fonts_, {ZOOM_CLOSE, "Close", "", ""}, settings_.night);
}


void App::card_action(CardAction op, int flag) {
    if (!card_) return;
    std::string err;
    if (!eng()->card_action(op, flag, *card_, err)) {
        show_message("Couldn't change the card", err, Mode::Review);
        return;
    }
    can_undo_ = true;
    menu_open_ = false;
    dirty = true;
    switch (op) {
        case CardAction::BuryCard:
        case CardAction::BuryNote:
        case CardAction::SuspendCard:
        case CardAction::SuspendNote:
            status_ = op == CardAction::BuryCard      ? "Buried the card"
                      : op == CardAction::BuryNote    ? "Buried the note"
                      : op == CardAction::SuspendCard ? "Suspended the card"
                                                      : "Suspended the note";
            cancel_prefetch();
            next_card();
            card_note_ = status_ + " (SELECT to undo)";
            break;
        case CardAction::SetFlag:
            card_note_ = flag ? std::string("Flagged ") + FLAGS[flag - 1].name : "Flag removed";
            break;
        case CardAction::ToggleMark:
            card_marked_ = card_->marked;
            card_note_ = card_marked_ ? "Marked" : "Unmarked";
            break;
    }
}

std::string App::action_for(const std::string& button) const {
    auto nt = settings_.nt_buttons.find(std::to_string(current_mid_));
    if (nt != settings_.nt_buttons.end()) {
        auto it = nt->second.find(button);
        if (it != nt->second.end()) return it->second;
    }
    auto it = settings_.buttons.find(button);
    if (it != settings_.buttons.end()) return it->second;
    auto d = default_buttons().find(button);
    return d != default_buttons().end() ? d->second : "none";
}

void App::run_action(const std::string& a) {
    using Op = CardAction;
    if (a == "page_up" || a == "page_down") {
        scroll_ = std::clamp(scroll_ + (a == "page_up" ? -200 : 200), 0, max_scroll());
        dirty = true;
    } else if (a == "extra") {
        if (!answer_shown_) return;  // extra sections belong to the answer side
        settings_.reveal_hidden = !settings_.reveal_hidden;
        settings_.save(settings_path_);
        int keep = scroll_;
        render_current();
        scroll_ = std::min(keep, max_scroll());
    } else if (a == "zoom") {
        open_zoom();
    } else if (a == "undo") {
        cancel_prefetch();
        undo();
    } else if (a == "menu") {
        menu_open_ = true;
        dirty = true;
    } else if (a == "scratchpad") {
        settings_.scratchpad = !settings_.scratchpad;
        settings_.save(settings_path_);
        clear_scratch();
        dirty = true;
    } else if (a == "bury") {
        card_action(Op::BuryCard);
    } else if (a == "suspend") {
        card_action(Op::SuspendCard);
    } else if (a == "mark") {
        card_action(Op::ToggleMark);
    } else if (a.rfind("flag:", 0) == 0) {
        int f = a[5] - '0';
        card_action(Op::SetFlag, card_ && (card_->flags & 7) == f ? 0 : f);  // pressing again removes it
    } else if (a.rfind("key:", 0) == 0 && script_) {
        // the card's script may change shared storage, so the prepared next card is redone
        cancel_prefetch();
        if (answer_shown_) prefetch_due_ = true;
        settle_prep();
        bool restart_ahead = false;
        if (!answer_shown_ && ahead_) {
            // the answer was prepared in this script already: start it again on the question
            ahead_.reset();
            script_ = std::make_unique<js::CardScript>();
            current_html_ = script_->run(rendered_.question, js_storage_);
            restart_ahead = true;
        }
        bool changed = script_->key(a.substr(4));
        if (restart_ahead) start_answer_ahead();
        if (changed) {
            current_html_ = script_->html();
            int keep = scroll_;
            render_current();
            scroll_ = std::min(keep, max_scroll());
        }
    }
}

// e.g. "ZL N   ◀▶ ←/→   ZR extra sections"
std::string App::button_hints() const {
    std::string out;
    std::string l = action_for("LEFT"), r = action_for("RIGHT");
    auto add = [&](const std::string& s) { out += (out.empty() ? "" : "  ·  ") + s; };
    for (const char* b : {"L", "R", "ZL", "ZR"}) {
        std::string a = action_for(b);
        if (a == "none") continue;
        if (std::string(b) == "L" && a == "page_up" && action_for("R") == "page_down") {
            add("L/R page");
            continue;
        }
        if (std::string(b) == "R" && a == "page_down" && action_for("L") == "page_up") continue;
        add(std::string(b) + " " + action_short_label(a));
    }
    if (l != "none" || r != "none") {
        if (l.rfind("key:", 0) == 0 && r.rfind("key:", 0) == 0)
            add("◀▶ " + action_short_label(l) + "/" + action_short_label(r));
        else {
            if (l != "none") add("◀ " + action_short_label(l));
            if (r != "none") add("▶ " + action_short_label(r));
        }
    }
    return out;
}

void App::open_buttons(int64_t mid, Mode ret) {
    btn_mid_ = mid;
    btn_return_ = ret;
    btn_sel_ = 0;
    menu_open_ = false;
    mode_ = Mode::Buttons;
    dirty = true;
}

void App::on_buttons(const Input& in) {
    const auto& btns = assignable_buttons();
    std::string key = std::to_string(btn_mid_);
    bool own = btn_mid_ && settings_.nt_buttons.count(key);
    int rows = (int)btns.size() + (btn_mid_ ? 1 : 0);  // last row: back to the shared set
    if (in.down & K_DOWN) btn_sel_ = (btn_sel_ + 1) % rows;
    if (in.down & K_UP) btn_sel_ = (btn_sel_ + rows - 1) % rows;
    if (in.down & (K_B | K_START | K_SELECT)) {
        settings_.save(settings_path_);
        mode_ = btn_return_;
        dirty = true;
        return;
    }
    if (btn_sel_ == (int)btns.size()) {
        if ((in.down & K_A) && own) {
            settings_.nt_buttons.erase(key);
            settings_.save(settings_path_);
        }
        return;
    }
    int delta = (in.down & K_RIGHT) ? 1 : (in.down & K_LEFT) ? -1 : (in.down & K_R) ? 10 : (in.down & K_L) ? -10 : 0;
    if (!delta) return;
    const auto& acts = button_actions();
    const char* id = btns[btn_sel_].id;
    ButtonMap& target = btn_mid_ ? settings_.nt_buttons[key] : settings_.buttons;
    if (btn_mid_ && !own) {
        // start this note type's set from the shared one
        for (auto& b : btns) {
            auto it = settings_.buttons.find(b.id);
            target[b.id] = it != settings_.buttons.end() ? it->second : default_buttons().at(b.id);
        }
    }
    std::string cur = target.count(id) ? target[id] : default_buttons().at(id);
    int i = 0;
    for (int j = 0; j < (int)acts.size(); j++)
        if (acts[j] == cur) i = j;
    int n = (int)acts.size();
    target[id] = acts[((i + delta) % n + n) % n];
    settings_.save(settings_path_);
}

void App::draw_buttons() {
    const Theme& t = theme(settings_.night);
    top.resize(400, 240, t.bg);
    std::string key = std::to_string(btn_mid_);
    bool own = btn_mid_ && settings_.nt_buttons.count(key);
    std::string scope = !btn_mid_ ? "All note types" : fit(fonts_, current_nt_name_, 13, 240);
    draw_header(top, "Buttons", scope);
    const auto& btns = assignable_buttons();
    auto effective = [&](const char* id) {
        if (btn_mid_ && own) {
            auto& m = settings_.nt_buttons.at(key);
            if (m.count(id)) return m.at(id);
        }
        auto it = settings_.buttons.find(id);
        return it != settings_.buttons.end() ? it->second : default_buttons().at(id);
    };
    for (int i = 0; i < (int)btns.size(); i++) {
        int y = 34 + i * 28;
        bool sel = i == btn_sel_;
        if (sel) top.fill_round_rect(4, y, 392, 26, 6, t.accent);
        uint32_t tc = sel ? t.accent_text : t.text;
        text(top, fonts_, 16, (float)y + 5, btns[i].label, 15, tc, FACE_SANS_BOLD);
        std::string v = "‹ " + action_label(effective(btns[i].id)) + " ›";
        text(top, fonts_, 386 - text_w(fonts_, v, 14), (float)y + 5, v, 14, tc);
    }
    if (btn_mid_) {
        int y = 34 + (int)btns.size() * 28;
        bool sel = btn_sel_ == (int)btns.size();
        if (sel) top.fill_round_rect(4, y, 392, 26, 6, t.accent);
        std::string label = own ? "Use the set for all note types (A)" : "Using the set for all note types";
        text(top, fonts_, 16, (float)y + 5, label, 14, sel ? t.accent_text : t.dim);
    }
    bottom.resize(320, 240, t.bg);
    std::string help =
        !btn_mid_ ? "What these buttons do while reviewing. ◀ ▶ change, L/R jump 10. \"Card key\" sends that keyboard key "
                    "to the card, for decks with their own shortcuts (Malleus: N reveals the next cloze on the front and "
                    "the next resource on the back; AnKing: C shows tags). Each note type can have its own set: open "
                    "More > Buttons while reviewing."
                  : own ? "These apply only to this note type. Changing one here doesn't affect other decks."
                        : "Change any button to give this note type its own set, starting from the set for all note "
                          "types.";
    paragraph(bottom, fonts_, 14, 14, 292, help, 13, t.dim);
    text(bottom, fonts_, 14, 216, "B: back", 12, t.dim);
}

void App::on_menu(const Input& in) {
    using Op = CardAction;
    if (in.down & (K_B | K_START)) {
        menu_open_ = false;
        dirty = true;
        return;
    }
    if (!in.touch_down) return;
    int x = in.tx, y = in.ty;
    if (hit({8, 8, 148, 40}, x, y)) card_action(Op::BuryCard);
    else if (hit({164, 8, 148, 40}, x, y)) card_action(Op::BuryNote);
    else if (hit({8, 54, 148, 40}, x, y)) card_action(Op::SuspendCard);
    else if (hit({164, 54, 148, 40}, x, y)) card_action(Op::SuspendNote);
    else if (y >= 104 && y < 140) {
        int i = (x - 8) / 38;  // 0 = no flag, 1-7 flags
        if (i >= 0 && i <= 7) card_action(Op::SetFlag, i);
    } else if (hit({8, 146, 148, 40}, x, y)) card_action(Op::ToggleMark);
    else if (hit({164, 146, 148, 40}, x, y)) {
        settings_.scratchpad = !settings_.scratchpad;
        settings_.save(settings_path_);
        clear_scratch();
        menu_open_ = false;
        dirty = true;
    } else if (hit({8, 194, 96, 40}, x, y)) {
        open_zoom();
    } else if (hit({112, 194, 96, 40}, x, y)) {
        open_buttons(current_mid_, Mode::Review);
    } else if (hit({216, 194, 96, 40}, x, y)) {
        menu_open_ = false;
        dirty = true;
    }
}

void App::draw_menu() {
    const Theme& t = theme(settings_.night);
    bottom.resize(320, 240, t.bg);
    button(bottom, fonts_, {{8, 8, 148, 40}, "Bury card", "", "", t.hard}, settings_.night);
    button(bottom, fonts_, {{164, 8, 148, 40}, "Bury note", "", "", t.hard}, settings_.night);
    button(bottom, fonts_, {{8, 54, 148, 40}, "Suspend card", "", "", t.again}, settings_.night);
    button(bottom, fonts_, {{164, 54, 148, 40}, "Suspend note", "", "", t.again}, settings_.night);
    int current = card_ ? (card_->flags & 7) : 0;
    for (int i = 0; i <= 7; i++) {
        Rect r{8 + i * 38, 104, 34, 34};
        if (i == current) bottom.fill_round_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 9, t.text);
        if (i == 0) {
            bottom.fill_round_rect(r.x, r.y, r.w, r.h, 7, t.panel);
            text_centered(bottom, fonts_, r, "none", 11, t.dim);
        } else {
            bottom.fill_round_rect(r.x, r.y, r.w, r.h, 7, rgba(FLAGS[i - 1].r, FLAGS[i - 1].g, FLAGS[i - 1].b));
        }
    }
    button(bottom, fonts_, {{8, 146, 148, 40}, card_marked_ ? "Unmark note" : "Mark note", "", ""}, settings_.night);
    button(bottom, fonts_, {{164, 146, 148, 40}, settings_.scratchpad ? "Scratchpad off" : "Scratchpad on", "", ""},
           settings_.night);
    button(bottom, fonts_, {{8, 194, 96, 40}, "Zoom", "", ""}, settings_.night);
    button(bottom, fonts_, {{112, 194, 96, 40}, "Buttons…", "", ""}, settings_.night);
    button(bottom, fonts_, {{216, 194, 96, 40}, "Close", "", "", t.hard}, settings_.night);
}

void App::on_review(const Input& in) {
    if (zoom_open_) {
        on_zoom(in);
        return;
    }
    if (menu_open_) {
        on_menu(in);
        return;
    }
    if (in.touch_down && !settings_.scratchpad && hit(ZOOM_BTN, in.tx, in.ty)) {
        open_zoom();
        return;
    }
    if (in.touch_down && hit(MORE_BTN, in.tx, in.ty)) {
        menu_open_ = true;
        dirty = true;
        return;
    }
    if (settings_.scratchpad) {
        if (in.touch_down && hit(CLEAR_BTN, in.tx, in.ty)) {
            clear_scratch();
            dirty = true;
            return;
        }
        if (in.touching && in.ty >= 40) {
            uint32_t ink = settings_.night ? rgba(235, 235, 235) : rgba(30, 30, 30);
            if (pen_x_ < 0 || in.touch_down) {
                pen_x_ = in.tx;
                pen_y_ = in.ty;
            }
            int dx = in.tx - pen_x_, dy = in.ty - pen_y_;
            int steps = std::max(1, std::max(std::abs(dx), std::abs(dy)));
            for (int i = 0; i <= steps; i++)
                scratch_.fill_circle(pen_x_ + dx * i / steps, pen_y_ + dy * i / steps, 1, ink);
            pen_x_ = in.tx;
            pen_y_ = in.ty;
            dirty = true;
        } else if (!in.touching) {
            pen_x_ = pen_y_ = -1;
        }
    }
    // scrolling
    int step = 0;
    if (in.held & K_DOWN) step += 6;
    if (in.held & K_UP) step -= 6;
    step += in.scroll;
    if (step) {
        int ns = std::clamp(scroll_ + step, 0, max_scroll());
        if (ns != scroll_) {
            scroll_ = ns;
            dirty = true;
        }
    }
    if (in.down & K_START) {
        cancel_prefetch();
        flush_timing(pf_.data_dir());
        build_rows();
        mode_ = Mode::Decks;
        {
            Engine* e = eng();
            worker_.submit([e] { e->checkpoint(); });
        }
        sync_after_studying();
        return;
    }
    if (in.down & K_SELECT) {
        cancel_prefetch();
        undo();
        return;
    }
    Rect reveal_r{10, 112, 300, 34};
    if (answer_shown_ && in.touch_down && !settings_.scratchpad && hit(reveal_r, in.tx, in.ty)) {
        run_action("extra");
        return;
    }
    // L, R, ZL, ZR, ◀, ▶: whatever they're assigned to (Settings > Buttons)
    for (auto& b : assignable_buttons())
        if (in.down & b.key) {
            run_action(action_for(b.id));
            return;
        }
    auto buttons = review_buttons();
    int touched = -1;
    if (in.touch_down && !settings_.scratchpad)
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
    if (zoom_open_) {
        // outline the magnified area on the top screen
        int bx = (int)zoom_x_, by = (int)zoom_y_ - scroll_;
        int bw = (int)(320 / zoom_scale_), bh = (int)((240 - ZOOM_TOP) / zoom_scale_);
        uint32_t c = t.accent;
        top.fill_rect(bx, by, bw, 2, c);
        top.fill_rect(bx, by + bh - 2, bw, 2, c);
        top.fill_rect(bx, by, 2, bh, c);
        top.fill_rect(bx + bw - 2, by, 2, bh, c);
        draw_zoom();
        return;
    }
    if (menu_open_) {
        draw_menu();
        return;
    }
    bottom.resize(320, 240, t.bg);
    if (settings_.scratchpad) std::copy(scratch_.data(), scratch_.data() + 320 * 240, bottom.data());
    {
        DeckCounts c = counts_;
        std::string kind = card_ ? card_->kind : "";
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
        x = chip(x, c.review_count, t.review_c, kind == "review");
        // flag and mark on the current card
        if (card_ && (card_->flags & 7)) {
            auto& f = FLAGS[(card_->flags & 7) - 1];
            bottom.fill_circle((int)x + 4, 21, 6, rgba(f.r, f.g, f.b));
            x += 18;
        }
        if (card_marked_) text(bottom, fonts_, x, 14, "★", 14, t.learn_c, FACE_SANS_BOLD);
    }
    button(bottom, fonts_, {MORE_BTN, "More", "", ""}, settings_.night);
    if (!settings_.scratchpad) button(bottom, fonts_, {ZOOM_BTN, "Zoom", "", "", t.hard}, settings_.night);
    if (settings_.scratchpad) {
        button(bottom, fonts_, {CLEAR_BTN, "Clear", "", ""}, settings_.night);
        bottom.fill_rect(0, 39, 320, 1, t.border);
        // the buttons still answer; remind which is which
        std::string hint;
        if (!answer_shown_) hint = "A: show answer";
        else {
            const char* keys[] = {"B", "Y", "A", "X"};
            for (size_t i = 0; i < 4 && i < labels_.size(); i++)
                hint += std::string(i ? "   " : "") + keys[i] + " " + labels_[i];
        }
        text(bottom, fonts_, 12, 222, hint, 12, t.dim);
        if (!card_note_.empty()) text(bottom, fonts_, 12, 46, fit(fonts_, card_note_, 12, 296), 12, t.dim);
        return;
    }
    if (!card_note_.empty())
        text(bottom, fonts_, 12, 50, fit(fonts_, card_note_, 12, 296), 12, t.accent, FACE_SANS_BOLD);
    else
        text(bottom, fonts_, 12, 50, fit(fonts_, button_hints(), 12, 296), 12, t.dim);
    text(bottom, fonts_, 12, 68, can_undo_ ? "START: decks   SELECT: undo" : "START: back to decks", 12, t.dim);
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
                                      "Download images automatically", "Sync automatically", "Run card scripts",
                                      "Buttons", "Sign in again", "Replace this 3DS copy with the server's", "About"};
static const int SETTING_COUNT = 10;

void App::on_settings(const Input& in) {
    if (in.down & K_DOWN) setting_sel_ = (setting_sel_ + 1) % SETTING_COUNT;
    if (in.down & K_UP) setting_sel_ = (setting_sel_ + SETTING_COUNT - 1) % SETTING_COUNT;
    int delta = (in.down & K_RIGHT) ? 1 : (in.down & K_LEFT) ? -1 : 0;
    bool activate = (in.down & K_A) != 0;
    if (in.down & (K_B | K_SELECT | K_START)) {
        settings_.save(settings_path_);
        if (eng()->is_open()) {
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
            if (delta) settings_.auto_sync = (settings_.auto_sync + delta + 4) % 4;
            break;
        case 5:
            if (delta || activate) settings_.run_scripts = !settings_.run_scripts;
            break;
        case 6:
            if (activate || delta) open_buttons(0, Mode::Settings);
            break;
        case 7:
#ifdef ANKI3DS_ANKIWEB
            if (activate) login_flow(settings_.endpoint != "https://sync.ankiweb.net/");
#else
            if (activate) login_flow(true);
#endif
            break;
        case 8:
            if (activate) {
                if (settings_.hkey.empty()) show_message("Not signed in", "Sign in first.", Mode::Settings);
                else confirm_download(true);
            }
            break;
        case 9:
            if (activate)
                show_message("About " APP_NAME,
                             "An unofficial, Anki-compatible client, not made by the Anki team. Scheduling is a port of Anki 26.08 (FSRS, load balancer, "
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
        int y = 27 + i * 21;
        bool sel = i == setting_sel_;
        if (sel) top.fill_round_rect(4, y, 392, 20, 6, t.accent);
        uint32_t tc = sel ? t.accent_text : t.text;
        text(top, fonts_, 14, (float)y + 2, SETTING_NAMES[i], 14, tc);
        std::string value;
        switch (i) {
            case 0: value = "‹ " + std::to_string((int)std::lround(settings_.text_zoom * 100)) + "% ›"; break;
            case 1: value = settings_.night ? "On" : "Off"; break;
            case 2:
                value = "‹ " + fmt_offset(settings_.minutes_west.value_or(0)) + (settings_.tz_auto ? " (auto)" : "") + " ›";
                break;
            case 3: value = settings_.auto_media ? "On" : "Off"; break;
            case 4: {
                static const char* names[] = {"Off", "When opened", "After studying", "Both"};
                value = std::string("‹ ") + names[settings_.auto_sync & 3] + " ›";
                break;
            }
            case 5: value = settings_.run_scripts ? "On" : "Off"; break;
            case 6: value = "›"; break;
            case 7: value = settings_.hkey.empty() ? "Not signed in" : "Signed in"; break;
            default: break;
        }
        if (!value.empty()) text(top, fonts_, 386 - text_w(fonts_, value, 14), (float)y + 2, value, 14, tc);
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
            help = "Sync when Clamshell opens, after you finish studying a deck, both, or only when you press X. "
                   "If it can't connect (for example away from home), it says so on the deck list and you can "
                   "keep studying.";
            break;
        case 5:
            help = "Runs the JavaScript in card templates (for example Malleus one-by-one clozes: press ▶ to reveal "
                   "the next line, ◀ to hide it). Turn off if a deck's cards misbehave.";
            break;
        case 6:
            help = "Choose what L, R, ZL, ZR and ◀ ▶ do while reviewing: page, extra sections, undo, flags, or a "
                   "keyboard key for decks with their own shortcuts. Press A to open.";
            break;
        case 7: help = "Sign in again, e.g. after changing your password. Server: " + settings_.endpoint; break;
        case 8:
            help = "Downloads the whole collection from the sync server again. Reviews on this 3DS that haven't "
                   "been synced will be lost.";
            break;
        default: help = "Version " ANKI3DS_VERSION_LABEL ". Expect rough edges."; break;
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
    draws++;
    switch (mode_) {
        case Mode::Setup: draw_setup(); break;
        case Mode::Decks: draw_decks(); break;
        case Mode::Review: draw_review(); break;
        case Mode::Done: draw_done(); break;
        case Mode::Settings: draw_settings(); break;
        case Mode::Buttons: draw_buttons(); break;
        case Mode::Message: draw_message(); break;
        case Mode::ConfirmDownload: {
            const Theme& t = theme(settings_.night);
            top.resize(400, 240, t.bg);
            text_centered(top, fonts_, {0, 30, 400, 30},
                          download_requested_ ? "Replace this copy?" : "Full download needed", 20, t.text,
                          FACE_SANS_BOLD);
            std::string lost = unsynced_reviews_ == 0
                                   ? "Nothing done on this 3DS is waiting to sync, so no reviews will be lost."
                               : unsynced_reviews_ == 1
                                   ? "1 review done on this 3DS since the last sync will be lost."
                                   : std::to_string(unsynced_reviews_) +
                                         " reviews done on this 3DS since the last sync will be lost.";
            paragraph(top, fonts_, 30, 75, 340,
                      std::string(download_requested_
                                      ? "Download the whole collection from the sync server and replace the one "
                                        "on this 3DS?"
                                      : "The server's collection changed in a way that can't be merged (for "
                                        "example a note type was edited on desktop). Download the server's copy "
                                        "to replace the one on this 3DS?") +
                          "\n\n" + lost,
                      14, t.text);
            bottom.resize(320, 240, t.bg);
            button(bottom, fonts_, {{20, 60, 280, 50}, "Download from server", "", "A", t.again}, settings_.night);
            button(bottom, fonts_, {{20, 125, 280, 40}, "Not now", "", "B", t.hard}, settings_.night);
            break;
        }
    }
}

}  // namespace anki
