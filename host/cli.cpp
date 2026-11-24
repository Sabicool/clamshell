// Desktop test harness for the portable core. Used by tools/difftest.py to
// compare this implementation against the real Anki engine, and handy for
// poking at a collection without a 3DS.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "collection.h"
#include "nlohmann/json.hpp"
#include "queue.h"
#ifndef ANKI3DS_NO_SYNC
#include "sync.h"
#include "media.h"
#endif
#ifndef ANKI3DS_NO_TEMPLATE
#include "template.h"
#include "render/card_view.h"
#endif

using namespace anki;
using json = nlohmann::json;

static json state_json(const CardState& s) {
    json j;
    if (s.kind == CardStateKind::Preview) {
        j["kind"] = "preview";
        j["scheduled_secs"] = s.preview_scheduled_secs;
        j["finished"] = s.preview_finished;
        return j;
    }
    const NormalState& n = s.normal;
    j["filtered"] = s.kind == CardStateKind::Rescheduling;
    auto mem = [](const std::optional<MemoryState>& m) -> json {
        if (!m) return nullptr;
        return json::array({m->stability, m->difficulty});
    };
    switch (n.kind) {
        case NormalKind::New:
            j["kind"] = "new";
            j["position"] = n.new_position;
            break;
        case NormalKind::Learning:
            j["kind"] = "learning";
            j["remaining_steps"] = n.learning.remaining_steps;
            j["scheduled_secs"] = n.learning.scheduled_secs;
            j["memory"] = mem(n.learning.memory_state);
            break;
        case NormalKind::Review:
            j["kind"] = "review";
            j["scheduled_days"] = n.review.scheduled_days;
            j["elapsed_days"] = n.review.elapsed_days;
            j["ease"] = n.review.ease_factor;
            j["lapses"] = n.review.lapses;
            j["leeched"] = n.review.leeched;
            j["memory"] = mem(n.review.memory_state);
            break;
        case NormalKind::Relearning:
            j["kind"] = "relearning";
            j["remaining_steps"] = n.learning.remaining_steps;
            j["scheduled_secs"] = n.learning.scheduled_secs;
            j["memory"] = mem(n.learning.memory_state);
            j["scheduled_days"] = n.review.scheduled_days;
            j["ease"] = n.review.ease_factor;
            j["lapses"] = n.review.lapses;
            j["leeched"] = n.review.leeched;
            break;
    }
    return j;
}

static int usage() {
    fprintf(stderr,
            "usage:\n"
            "  anki3ds-cli decks <collection>\n"
            "  anki3ds-cli states <collection> [--lb] < card ids (one per line)\n"
            "  anki3ds-cli replay <collection> < lines: cid rating millis\n"
            "  anki3ds-cli render <collection> <card id>\n"
            "  anki3ds-cli shot <collection> <card id> <out prefix> [media dir] [text zoom]\n"
            "  anki3ds-cli queue <collection> <deck id> [n]\n"
            "  anki3ds-cli sync <collection> <endpoint> <hkey> [--full-download]\n"
            "  anki3ds-cli login <endpoint> <user> <pass>\n"
            "  anki3ds-cli media <collection> <endpoint> <hkey> <deck id> <media dir>\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
#ifndef ANKI3DS_NO_SYNC
    if (cmd == "login") {
        if (argc < 5) return usage();
        SyncClient client(argv[2]);
        std::string err;
        auto key = client.login(argv[3], argv[4], err);
        if (!key) { fprintf(stderr, "login failed: %s\n", err.c_str()); return 1; }
        printf("%s\n", key->c_str());
        return 0;
    }
#endif
#ifndef ANKI3DS_NO_TEMPLATE
    if (cmd == "htmlshot" && argc >= 4) {
        // htmlshot <file.html> <out.png> [css file]
        auto slurp = [](const char* path) {
            std::string s;
            FILE* f = fopen(path, "rb");
            if (!f) return s;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
            fclose(f);
            return s;
        };
        FontLib fonts;
        std::string e;
        std::string exe = argv[0];
        fonts.load(exe.substr(0, exe.rfind("/build/")) + "/3ds/romfs/fonts", e);
        CardView view(fonts);
        CardViewOptions opt;
        view.render(slurp(argv[2]), argc > 4 ? slurp(argv[4]) : std::string(), 0, opt);
        view.canvas().save_png(argv[3]);
        printf("%d px tall\n", view.content_height());
        return 0;
    }
#endif
    if (argc < 3) return usage();
    Collection col;
    std::string err;
    bool first_download = cmd == "sync" && argc > 5 && std::string(argv[5]) == "--full-download" &&
                          fopen(argv[2], "rb") == nullptr;
    if (!first_download && !col.open(argv[2], err)) {
        fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    if (const char* off = getenv("ANKI3DS_MINUTES_WEST"); off && col.is_open()) {
        int v = atoi(off);
        col.clock.local_minutes_west = [v] { return (int32_t)v; };
    }
    if (cmd == "decks") {
        auto t = col.timing_today();
        printf("today=%u next_day_at=%lld\n", t.days_elapsed, (long long)t.next_day_at);
        for (const auto& [id, d] : col.decks) {
            DeckCounts c = deck_counts(col, id);
            printf("%lld\t%s\tnew=%u learn=%u review=%u\n", (long long)id, d.human_name().c_str(), c.new_count,
                   c.learn_count, c.review_count);
        }
        return 0;
    }
    if (cmd == "states") {
        bool lb = argc > 3 && std::string(argv[3]) == "--lb";
        Scheduler sched(col);
        sched.start_session(lb);
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            int64_t cid = std::stoll(line);
            auto card = col.get_card(cid);
            if (!card) continue;
            SchedulingStates s = sched.scheduling_states(*card);
            json j;
            j["cid"] = cid;
            j["current"] = state_json(s.current);
            j["again"] = state_json(s.again);
            j["hard"] = state_json(s.hard);
            j["good"] = state_json(s.good);
            j["easy"] = state_json(s.easy);
            printf("%s\n", j.dump().c_str());
        }
        return 0;
    }
    if (cmd == "replay") {
        Scheduler sched(col);
        sched.start_session(true);
        std::string line;
        int n = 0;
        while (std::getline(std::cin, line)) {
            std::istringstream ss(line);
            int64_t cid;
            int rating;
            uint32_t ms;
            if (!(ss >> cid >> rating >> ms)) continue;
            if (!sched.answer(cid, (Rating)rating, ms, err)) {
                fprintf(stderr, "answer %lld failed: %s\n", (long long)cid, err.c_str());
                return 1;
            }
            n++;
        }
        fprintf(stderr, "answered %d\n", n);
        return 0;
    }
    if (cmd == "queue") {
        if (argc < 4) return usage();
        int64_t did = std::stoll(argv[3]);
        size_t n = argc > 4 ? (size_t)atoi(argv[4]) : 20;
        StudyQueue q(col, did);
        q.build();
        auto c = q.counts();
        printf("counts new=%u learn=%u review=%u\n", c.new_count, c.learn_count, c.review_count);
        for (size_t i = 0; i < n; i++) {
            auto next = q.peek(i);
            if (!next) break;
            printf("%lld %s\n", (long long)next->card_id, next->kind.c_str());
        }
        return 0;
    }
#ifndef ANKI3DS_NO_TEMPLATE
    if (cmd == "shot") {
        if (argc < 5) return usage();
        auto card = col.get_card(std::stoll(argv[3]));
        if (!card) { fprintf(stderr, "no such card\n"); return 1; }
        RenderedCard r = render_card(col, *card);
        FontLib fonts;
        std::string exe = argv[0];
        std::string root = exe.substr(0, exe.rfind("/build/"));
        if (!fonts.load(root + "/3ds/romfs/fonts", err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        CardViewOptions opt;
        if (argc > 5) opt.media_dir = argv[5];
        if (argc > 6) opt.text_zoom = (float)atof(argv[6]);
        CardView view(fonts);
        std::string prefix = argv[4];
        view.render(r.question, r.css, r.ord, opt);
        view.canvas().save_png(prefix + "-q.png");
        printf("question: %d px tall\n", view.content_height());
        view.render(r.answer, r.css, r.ord, opt);
        view.canvas().save_png(prefix + "-a.png");
        printf("answer: %d px tall (answer anchor at %d)\n", view.content_height(), view.answer_anchor());
        return 0;
    }
    if (cmd == "render") {
        if (argc < 4) return usage();
        auto card = col.get_card(std::stoll(argv[3]));
        if (!card) { fprintf(stderr, "no such card\n"); return 1; }
        RenderedCard r = render_card(col, *card);
        printf("=== QUESTION\n%s\n=== ANSWER\n%s\n=== CSS bytes: %zu\n", r.question.c_str(), r.answer.c_str(),
               r.css.size());
        return 0;
    }
#endif
#ifndef ANKI3DS_NO_SYNC
    if (cmd == "media") {
        if (argc < 7) return usage();
        SyncClient client(argv[3]);
        client.hkey = argv[4];
        auto names = missing_media_for_deck(col, std::stoll(argv[5]), argv[6]);
        MediaDownloadResult res;
        bool ok = download_media(client, names, argv[6], res, err);
        printf("missing %zu, downloaded %d%s%s\n", names.size(), res.downloaded, ok ? "" : ", error: ", err.c_str());
        return ok ? 0 : 1;
    }
    if (cmd == "sync") {
        if (argc < 5) return usage();
        SyncClient client(argv[3]);
        client.hkey = argv[4];
        bool full = argc > 5 && std::string(argv[5]) == "--full-download";
        SyncResult res = full ? client.full_download(col, argv[2], err) : client.normal_sync(col, err);
        printf("result: %s\n%s\n", sync_status_name(res.status), res.message.c_str());
        if (!err.empty()) printf("error: %s\n", err.c_str());
        return res.status == SyncStatus::Failed ? 1 : 0;
    }
#endif
    return usage();
}
