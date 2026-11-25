// Desktop simulator for the 3DS app: runs the real App with scripted input and
// writes screenshots laid out like the console (top screen above bottom).
//
//   anki3ds-sim <data dir> <script file> <screenshot dir>
//
// Script lines:  key A | key DOWN | hold DOWN 10 | touch 160 120 | type some text |
//                shot name | frames 5      (blank lines and # comments ignored)
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "app.h"

using namespace anki;

class SimPlatform : public Platform {
public:
    std::string dir, fonts;
    std::deque<std::string> typed;
    std::string data_dir() override { return dir; }
    std::string fonts_dir() override { return fonts; }
    bool keyboard(const std::string& hint, const std::string&, bool password, std::string& out) override {
        if (typed.empty()) {
            fprintf(stderr, "[sim] keyboard '%s' cancelled (no scripted text)\n", hint.c_str());
            return false;
        }
        out = typed.front();
        typed.pop_front();
        fprintf(stderr, "[sim] keyboard '%s' -> %s\n", hint.c_str(), password ? "****" : out.c_str());
        return true;
    }
    void present(const Canvas&, const Canvas&) override {}
    int64_t local_clock_ms() override {
        time_t t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        return (int64_t)t * 1000 + (int64_t)lt.tm_gmtoff * 1000;
    }
    std::optional<int32_t> system_minutes_west() override {
        time_t t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        return (int32_t)(-lt.tm_gmtoff / 60);
    }
    bool net_up(std::string&) override { return true; }
    void net_down() override {}
};

static void save_shot(App& app, const std::string& path) {
    Canvas out(400, 490, rgba(40, 40, 46));
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 400; x++) out.data()[(size_t)y * 400 + x] = app.top.data()[(size_t)y * 400 + x];
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) out.data()[(size_t)(y + 250) * 400 + x + 40] = app.bottom.data()[(size_t)y * 320 + x];
    out.save_png(path);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: anki3ds-sim <data dir> <script> <shot dir>\n");
        return 2;
    }
    SimPlatform pf;
    pf.dir = argv[1];
    std::string exe = argv[0];
    pf.fonts = exe.substr(0, exe.rfind("/build/")) + "/3ds/romfs/fonts";
    App app(pf);
    app.init();
    Input idle;
    app.frame(idle);
    static const std::map<std::string, uint32_t> keys = {
        {"A", K_A},     {"B", K_B},         {"X", K_X},         {"Y", K_Y},          {"L", K_L},
        {"R", K_R},     {"UP", K_UP},       {"DOWN", K_DOWN},   {"LEFT", K_LEFT},    {"RIGHT", K_RIGHT},
        {"START", K_START}, {"SELECT", K_SELECT}, {"ZL", K_ZL}, {"ZR", K_ZR}};
    std::ifstream script(argv[2]);
    std::string line;
    while (std::getline(script, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        if (cmd == "key") {
            std::string k;
            ss >> k;
            Input in;
            in.down = in.held = keys.count(k) ? keys.at(k) : 0;
            if (!app.frame(in)) break;
            app.frame(idle);
        } else if (cmd == "hold") {
            std::string k;
            int n = 1;
            ss >> k >> n;
            for (int i = 0; i < n; i++) {
                Input in;
                in.held = keys.count(k) ? keys.at(k) : 0;
                app.frame(in);
            }
        } else if (cmd == "touch") {
            Input in;
            ss >> in.tx >> in.ty;
            in.touch_down = in.touching = true;
            app.frame(in);
            app.frame(idle);
        } else if (cmd == "drag") {
            // drag x1 y1 x2 y2 [frames]: a stylus stroke
            int x1, y1, x2, y2, n = 20;
            ss >> x1 >> y1 >> x2 >> y2;
            if (!(ss >> n)) n = 20;
            for (int i = 0; i <= n; i++) {
                Input in;
                in.touching = true;
                in.touch_down = i == 0;
                in.tx = x1 + (x2 - x1) * i / n;
                in.ty = y1 + (y2 - y1) * i / n;
                app.frame(in);
            }
            app.frame(idle);
        } else if (cmd == "type") {
            std::string rest;
            std::getline(ss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            pf.typed.push_back(rest);
        } else if (cmd == "select") {
            std::string rest;
            std::getline(ss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            if (!app.debug_select(rest)) fprintf(stderr, "[sim] no deck matching %s\n", rest.c_str());
            app.frame(idle);
        } else if (cmd == "frames") {
            int n = 1;
            ss >> n;
            for (int i = 0; i < n; i++) app.frame(idle);
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            app.dirty = true;
            app.frame(idle);
            save_shot(app, std::string(argv[3]) + "/" + name + ".png");
            fprintf(stderr, "[sim] shot %s\n", name.c_str());
        }
    }
    return 0;
}
