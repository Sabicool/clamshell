#include <chrono>
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
#include "js/card_js.h"
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
        bool fast = argc > 3 && std::string(argv[3]) == "--fast";
        auto all = all_deck_counts(col);
        for (const auto& [id, d] : col.decks) {
            DeckCounts c = fast ? all[id] : deck_counts(col, id);
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
    if (cmd == "op" && argc >= 5) {
        // op <collection> <card id> <bury|burynote|suspend|suspendnote|flag0..7|mark>
        Scheduler sched(col);
        std::string what = argv[4], err;
        using Op = Scheduler::CardOp;
        Op op = Op::BuryCard;
        int flag = 0;
        if (what == "bury") op = Op::BuryCard;
        else if (what == "burynote") op = Op::BuryNote;
        else if (what == "suspend") op = Op::SuspendCard;
        else if (what == "suspendnote") op = Op::SuspendNote;
        else if (what == "mark") op = Op::ToggleMark;
        else if (what.rfind("flag", 0) == 0) {
            op = Op::SetFlag;
            flag = std::stoi(what.substr(4));
        } else {
            fprintf(stderr, "unknown op %s\n", what.c_str());
            return 1;
        }
        if (!sched.card_op(std::stoll(argv[3]), op, flag, err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
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
            auto before = col.get_card(cid);
            if (!sched.answer(cid, (Rating)rating, ms, err)) {
                fprintf(stderr, "answer %lld failed: %s\n", (long long)cid, err.c_str());
                return 1;
            }
            // update the deck's studied-today counters like the app does
            if (before && getenv("ANKI3DS_DECK_STATS"))
                record_studied(col, before->did, before->queue == Q_NEW,
                               before->queue == Q_REVIEW || before->queue == Q_DAY_LEARN, ms);
            n++;
        }
        fprintf(stderr, "answered %d\n", n);
        return 0;
    }
    if (cmd == "study") {
        // study <col> <deck id> <ratings csv> <n>: picks cards with StudyQueue like the app
        if (argc < 6) return usage();
        int64_t did = std::stoll(argv[3]);
        std::vector<int> pattern;
        for (char ch : std::string(argv[4]))
            if (ch >= '1' && ch <= '4') pattern.push_back(ch - '0');
        int n = atoi(argv[5]);
        StudyQueue q(col, did);
        q.build();
        Scheduler sched(col);
        sched.start_session(true);
        for (int i = 0; i < n; i++) {
            auto next = q.next();
            if (!next) break;
            auto card = col.get_card(next->card_id);
            if (!card) break;
            int rating = pattern[i % pattern.size()];
            uint32_t ms = 4000 + 100 * (i % 7);
            if (!sched.answer(card->id, (Rating)rating, ms, err)) {
                fprintf(stderr, "answer failed: %s\n", err.c_str());
                return 1;
            }
            record_studied(col, card->did, card->queue == Q_NEW, card->queue == Q_REVIEW || card->queue == Q_DAY_LEARN, ms);
            if (auto updated = col.get_card(card->id)) q.on_answered(*updated);
            printf("%lld %d %u\n", (long long)card->id, rating, ms);
        }
        return 0;
    }
    if (cmd == "undotest") {
        // answers the given card ids (cycling Again/Good/Easy/Hard), then undoes them all
        Scheduler sched(col);
        sched.start_session(true);
        std::string line;
        int n = 0;
        const Rating cycle[] = {Rating::Again, Rating::Good, Rating::Easy, Rating::Hard};
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            if (!sched.answer(std::stoll(line), cycle[n % 4], 3000, err)) {
                fprintf(stderr, "answer failed: %s\n", err.c_str());
                return 1;
            }
            n++;
        }
        int u = 0;
        while (sched.can_undo() && sched.undo(err)) u++;
        printf("answered %d, undid %d\n", n, u);
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
        if (getenv("ANKI3DS_REVEAL")) opt.reveal_hidden = true;
        if (getenv("ANKI3DS_NIGHT")) opt.night_mode = true;
        CardView view(fonts);
        std::string prefix = argv[4];
        bool js = getenv("ANKI3DS_JS") != nullptr;
        std::map<std::string, std::string> storage;
        js::CardScript qs, as;
        std::string qhtml = js ? qs.run(r.question, storage) : r.question;
        if (js && !qs.log().empty()) fprintf(stderr, "[js question]\n%s", qs.log().c_str());
        if (js && getenv("ANKI3DS_JS_EVAL")) fprintf(stderr, "[eval] %s\n", qs.eval(getenv("ANKI3DS_JS_EVAL")).c_str());
        view.render(qhtml, r.css, r.ord, opt);
        view.canvas().save_png(prefix + "-q.png");
        printf("question: %d px tall\n", view.content_height());
        if (js && getenv("ANKI3DS_QKEYS")) {
            std::string keys = getenv("ANKI3DS_QKEYS");
            size_t p = 0;
            int k = 0;
            while (p <= keys.size()) {
                size_t q = keys.find(',', p);
                std::string code = keys.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (!code.empty() && qs.key(code)) {
                    view.render(qs.html(), r.css, r.ord, opt);
                    view.canvas().save_png(prefix + "-q-key" + std::to_string(++k) + ".png");
                    printf("question after %s: changed\n", code.c_str());
                } else if (!code.empty()) {
                    printf("question after %s: no change\n", code.c_str());
                }
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        size_t qlog = qs.log().size();
        std::string ahtml = js ? qs.show_answer(r.answer) : r.answer;
        if (js && qs.log().size() > qlog) fprintf(stderr, "[js answer]\n%s", qs.log().c_str() + qlog);
        view.render(ahtml, r.css, r.ord, opt);
        view.canvas().save_png(prefix + "-a.png");
        if (js && getenv("ANKI3DS_KEYS")) {
            std::string keys = getenv("ANKI3DS_KEYS");
            size_t p = 0;
            int k = 0;
            while (p <= keys.size()) {
                size_t q = keys.find(',', p);
                std::string code = keys.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (!code.empty() && qs.key(code)) {
                    view.render(qs.html(), r.css, r.ord, opt);
                    view.canvas().save_png(prefix + "-a-key" + std::to_string(++k) + ".png");
                    printf("answer after %s: changed\n", code.c_str());
                } else if (!code.empty()) {
                    printf("answer after %s: no change\n", code.c_str());
                }
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }

        printf("answer: %d px tall (answer anchor at %d)\n", view.content_height(), view.answer_anchor());
        return 0;
    }
    if (cmd == "bench") {
        // anki3ds-cli bench <collection> [n] [media dir]: time rendering n review cards
        int n = argc > 3 ? atoi(argv[3]) : 40;
        FontLib fonts;
        std::string exe = argv[0];
        std::string root = exe.substr(0, exe.rfind("/build/"));
        if (!fonts.load(root + "/3ds/romfs/fonts", err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        CardViewOptions opt;
        if (argc > 4) opt.media_dir = argv[4];
        std::vector<int64_t> ids;
        {
            Stmt s(col.db(), "select id from cards where queue = 2 order by due, id limit ?");
            s.bind(1, (int64_t)n);
            while (s.step()) ids.push_back(s.i64(0));
        }
        auto ms = [] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        double tmpl = 0, js_q = 0, js_a = 0;
        std::map<std::string, std::string> storage;
        for (int64_t id : ids) {
            auto card = col.get_card(id);
            if (!card) continue;
            double a = ms();
            RenderedCard r = render_card(col, *card);
            double b = ms();
            js::CardScript qs;
            std::string q = qs.run(r.question, storage);
            double c = ms();
            CardView v(fonts);
            v.render(q, r.css, r.ord, opt);
            double d = ms();
            std::string an = qs.show_answer(r.answer);
            double e = ms();
            v.render(an, r.css, r.ord, opt);
            if (getenv("BENCH_SIZES")) {
                size_t tags = 0;
                for (size_t i = 0; i + 1 < an.size(); i++) if (an[i] == '<' && an[i + 1] != '/') tags++;
                printf("card %lld: question %zu bytes, answer %zu bytes, ~%zu elements, css %zu bytes\n", (long long)id,
                       q.size(), an.size(), tags, r.css.size());
            }
            tmpl += b - a;
            js_q += c - b;
            js_a += e - d;
        }
        size_t k = ids.size() ? ids.size() : 1;
        auto& t = g_render_times;
        printf("%zu cards, per card (question + answer):\n", ids.size());
        printf("  template %.1f ms, script q %.1f a %.1f ms\n", tmpl / k, js_q / k, js_a / k);
        printf("  render: prep %.1f, parse+css %.1f, layout %.1f, draw %.1f ms\n", t.prep_ms / k, t.parse_ms / k,
               t.layout_ms / k, t.draw_ms / k);
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
        printf("missing %zu, downloaded %d, not on server %d%s%s\n", names.size(), res.downloaded, res.unavailable,
               ok ? "" : ", error: ", err.c_str());
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
