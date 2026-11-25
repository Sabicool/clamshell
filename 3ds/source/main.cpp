// 3DS entry point: owns the hardware (screens, buttons, touch, keyboard,
// sockets) and runs the platform-independent App.
#include <3ds.h>
#include <malloc.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "app.h"
#include "version.h"

#include <sys/stat.h>

using namespace anki;

// The default 32 KB main-thread stack is too small for HTML layout and SQLite.
extern "C" {
u32 __stacksize__ = 0x80000;
}

namespace {

// Before the rename to Clamshell the app kept its files in sdmc:/3ds/anki3ds.
// Move them over once, so an update keeps the collection, images and settings.
void migrate_old_folder() {
    struct stat st;
    const char* old_dir = "sdmc:/3ds/anki3ds";
    const char* new_dir = "sdmc:/3ds/" APP_ID;
    if (stat(new_dir, &st) != 0 && stat(old_dir, &st) == 0) rename(old_dir, new_dir);
}


u32* g_soc_buffer = nullptr;
bool g_soc_up = false;

// Converts a 0xAARRGGBB canvas into the 3DS framebuffer layout: BGR8, rotated
// 90 degrees (each framebuffer "row" is a screen column, bottom-to-top).
void blit(const Canvas& c, gfxScreen_t screen) {
    u16 fbw, fbh;  // fbw = 240 (screen height), fbh = 400/320 (screen width)
    u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &fbw, &fbh);
    const int sw = c.width(), sh = c.height();
    const uint32_t* px = c.data();
    for (int x = 0; x < sw && x < fbh; x++) {
        u8* col = fb + (size_t)x * fbw * 3;
        for (int y = 0; y < sh && y < fbw; y++) {
            uint32_t p = px[(size_t)y * sw + x];
            u8* d = col + (size_t)(fbw - 1 - y) * 3;
            d[0] = p & 0xFF;
            d[1] = (p >> 8) & 0xFF;
            d[2] = (p >> 16) & 0xFF;
        }
    }
}

class Platform3ds : public Platform {
public:
    std::string data_dir() override { return "sdmc:/3ds/" APP_ID; }
    std::string fonts_dir() override { return "romfs:/fonts"; }

    bool keyboard(const std::string& hint, const std::string& initial, bool password, std::string& out) override {
        SwkbdState kb;
        swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, 256);
        swkbdSetHintText(&kb, hint.c_str());
        swkbdSetInitialText(&kb, initial.c_str());
        swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
        if (password) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
        swkbdSetFeatures(&kb, SWKBD_DARKEN_TOP_SCREEN);
        char buf[512] = {0};
        SwkbdButton b = swkbdInputText(&kb, buf, sizeof buf);
        if (b != SWKBD_BUTTON_CONFIRM) return false;
        out = buf;
        // trim
        while (!out.empty() && out.back() == ' ') out.pop_back();
        while (!out.empty() && out.front() == ' ') out.erase(0, 1);
        return true;
    }

    void present(const Canvas& top, const Canvas& bottom) override {
        blit(top, GFX_TOP);
        blit(bottom, GFX_BOTTOM);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        // keep the console responsive to HOME/power during long operations
        aptMainLoop();
    }

    int64_t local_clock_ms() override {
        // osGetTime: milliseconds since 1900-01-01 in the console's local time.
        return (int64_t)osGetTime() - 2208988800000LL;
    }

    bool net_up(std::string& err) override {
        if (g_soc_up) return true;
        u32 wifi = 0;
        if (R_SUCCEEDED(ACU_GetWifiStatus(&wifi)) && wifi == 0) {
            err = "Wi-Fi isn't connected. Check the wireless switch and your internet settings.";
            return false;
        }
        const u32 size = 0x100000;
        if (!g_soc_buffer) g_soc_buffer = (u32*)memalign(0x1000, size);
        if (!g_soc_buffer || R_FAILED(socInit(g_soc_buffer, size))) {
            err = "Couldn't start networking.";
            return false;
        }
        g_soc_up = true;
        return true;
    }

    void net_down() override {
        if (g_soc_up) {
            socExit();
            g_soc_up = false;
        }
    }
};

#ifdef ANKI3DS_SELFTEST
// Reads the framebuffer back (undoing the rotation) so screenshots show what
// the console actually displays.
Canvas readback(gfxScreen_t screen, int sw) {
    u16 fbw, fbh;
    u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &fbw, &fbh);
    Canvas c(sw, 240);
    for (int x = 0; x < sw; x++)
        for (int y = 0; y < 240; y++) {
            u8* d = fb + ((size_t)x * fbw + (fbw - 1 - y)) * 3;
            c.data()[(size_t)y * sw + x] = rgba(d[2], d[1], d[0]);
        }
    return c;
}

class SelftestPlatform : public Platform3ds {
public:
    std::vector<std::string> typed;
    bool keyboard(const std::string&, const std::string&, bool, std::string& out) override {
        if (typed.empty()) return false;
        out = typed.front();
        typed.erase(typed.begin());
        return true;
    }
};
#endif

}  // namespace

#ifdef ANKI3DS_SELFTEST
// Runs sdmc:/3ds/clamshell/selftest.txt (same commands as host/sim.cpp) and
// writes screenshots + selftest.log, then exits.
int main() {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    romfsInit();
    psInit();
    acInit();
    SelftestPlatform pf;
    migrate_old_folder();
    std::string dir = pf.data_dir();
    App app(pf);
    app.init();
    FILE* log = fopen((dir + "/selftest.log").c_str(), "w");
    auto logf = [&](const std::string& s) {
        if (log) { fprintf(log, "%s\n", s.c_str()); fflush(log); }
    };
    auto show = [&]() {
        blit(app.top, GFX_TOP);
        blit(app.bottom, GFX_BOTTOM);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    };
    Input idle;
    app.frame(idle);
    show();
    logf("started");
    FILE* sf = fopen((dir + "/selftest.txt").c_str(), "r");
    char linebuf[512];
    struct K { const char* n; uint32_t k; } keys[] = {{"A", K_A}, {"B", K_B}, {"X", K_X}, {"Y", K_Y}, {"L", K_L}, {"R", K_R},
        {"UP", K_UP}, {"DOWN", K_DOWN}, {"LEFT", K_LEFT}, {"RIGHT", K_RIGHT}, {"START", K_START}, {"SELECT", K_SELECT}, {"ZR", K_ZR}, {"ZL", K_ZL}};
    auto keyof = [&](const std::string& n) -> uint32_t { for (auto& k : keys) if (n == k.n) return k.k; return 0; };
    while (sf && fgets(linebuf, sizeof linebuf, sf) && aptMainLoop()) {
        std::string line = linebuf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::string cmd = line.substr(0, line.find(' '));
        std::string arg = line.find(' ') == std::string::npos ? "" : line.substr(line.find(' ') + 1);
        u64 t0 = osGetTime();
        if (cmd == "key") {
            Input in;
            in.down = in.held = keyof(arg);
            app.frame(in);
            show();
            app.frame(idle);
            show();
        } else if (cmd == "hold") {
            std::string k = arg.substr(0, arg.find(' '));
            int n = atoi(arg.substr(arg.find(' ') + 1).c_str());
            for (int i = 0; i < n; i++) {
                Input in;
                in.held = keyof(k);
                app.frame(in);
                show();
            }
        } else if (cmd == "touch") {
            Input in;
            sscanf(arg.c_str(), "%d %d", &in.tx, &in.ty);
            in.touch_down = in.touching = true;
            app.frame(in);
            show();
            app.frame(idle);
            show();
        } else if (cmd == "drag") {
            int x1, y1, x2, y2, n = 20;
            if (sscanf(arg.c_str(), "%d %d %d %d %d", &x1, &y1, &x2, &y2, &n) < 4) continue;
            for (int i = 0; i <= n; i++) {
                Input in;
                in.touching = true;
                in.touch_down = i == 0;
                in.tx = x1 + (x2 - x1) * i / n;
                in.ty = y1 + (y2 - y1) * i / n;
                app.frame(in);
                show();
            }
            app.frame(idle);
            show();
        } else if (cmd == "type") {
            pf.typed.push_back(arg);
        } else if (cmd == "select") {
            app.debug_select(arg);
            app.frame(idle);
            show();
        } else if (cmd == "frames") {
            for (int i = 0; i < atoi(arg.c_str()); i++) { app.frame(idle); show(); }
        } else if (cmd == "shot") {
            app.dirty = true;
            app.frame(idle);
            show();
            show();  // both buffers now hold this frame
            Canvas top = readback(GFX_TOP, 400), bot = readback(GFX_BOTTOM, 320);
            Canvas out(400, 490, rgba(40, 40, 46));
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 400; x++) out.data()[(size_t)y * 400 + x] = top.data()[(size_t)y * 400 + x];
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 320; x++) out.data()[(size_t)(y + 250) * 400 + x + 40] = bot.data()[(size_t)y * 320 + x];
            out.save_png(dir + "/shot-" + arg + ".png");
        }
        logf(line + "  (" + std::to_string(osGetTime() - t0) + " ms)");
    }
    if (sf) fclose(sf);
    logf("done");
    if (log) fclose(log);
    pf.net_down();
    acExit();
    psExit();
    romfsExit();
    gfxExit();
    return 0;
}
#else
int main() {
    osSetSpeedupEnable(true);  // New 3DS: 804 MHz + extra cache
    gfxInitDefault();
    romfsInit();
    psInit();
    acInit();
    hidSetRepeatParameters(20, 6);

    Platform3ds pf;
    migrate_old_folder();
    App app(pf);
    app.init();

    Input prev;
    bool was_touching = false;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        u32 repeat = hidKeysDownRepeat();
        Input in;
        auto map = [](u32 k) {
            uint32_t m = 0;
            if (k & KEY_A) m |= K_A;
            if (k & KEY_B) m |= K_B;
            if (k & KEY_X) m |= K_X;
            if (k & KEY_Y) m |= K_Y;
            if (k & KEY_L) m |= K_L;
            if (k & KEY_R) m |= K_R;
            if (k & KEY_ZL) m |= K_ZL;
            if (k & KEY_ZR) m |= K_ZR;
            if (k & KEY_DUP) m |= K_UP;
            if (k & KEY_DDOWN) m |= K_DOWN;
            if (k & KEY_DLEFT) m |= K_LEFT;
            if (k & KEY_DRIGHT) m |= K_RIGHT;
            if (k & KEY_START) m |= K_START;
            if (k & KEY_SELECT) m |= K_SELECT;
            return m;
        };
        // D-pad up/down repeat in menus; face buttons only on press
        in.down = map(down) | (map(repeat) & (K_UP | K_DOWN));
        in.held = map(held);
        if (held & KEY_TOUCH) {
            touchPosition t;
            hidTouchRead(&t);
            in.touching = true;
            in.tx = t.px;
            in.ty = t.py;
            in.touch_down = !was_touching;
        }
        was_touching = (held & KEY_TOUCH) != 0;
        circlePosition cp;
        hidCircleRead(&cp);
        if (cp.dy > 20 || cp.dy < -20) in.scroll = -cp.dy / 12;
        circlePosition cs;
        hidCstickRead(&cs);
        if (cs.dy > 20 || cs.dy < -20) in.scroll += -cs.dy / 12;

        if (!app.frame(in)) break;
        blit(app.top, GFX_TOP);
        blit(app.bottom, GFX_BOTTOM);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        prev = in;
    }

    pf.net_down();
    acExit();
    psExit();
    romfsExit();
    gfxExit();
    return 0;
}
#endif
