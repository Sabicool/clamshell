#include "ui.h"

#include <cmath>

namespace anki::ui {

const Theme& theme(bool night) {
    static const Theme day = {
        rgba(0xf4, 0xf4, 0xf6), rgba(0xff, 0xff, 0xff), rgba(0x20, 0x22, 0x28), rgba(0x75, 0x78, 0x80),
        rgba(0x2f, 0x6f, 0xe0), rgba(0xff, 0xff, 0xff), rgba(0xdd, 0xdf, 0xe4),
        rgba(0x1e, 0x66, 0xf5), rgba(0xd2, 0x0f, 0x39), rgba(0x40, 0xa0, 0x2b),
        rgba(0xd9, 0x3b, 0x3b), rgba(0x8a, 0x8f, 0x99), rgba(0x2e, 0x9e, 0x4f), rgba(0x2f, 0x6f, 0xe0)};
    static const Theme dark = {
        rgba(0x1e, 0x1f, 0x24), rgba(0x2a, 0x2c, 0x33), rgba(0xe8, 0xe8, 0xea), rgba(0x9a, 0x9d, 0xa6),
        rgba(0x4f, 0x8c, 0xf5), rgba(0xff, 0xff, 0xff), rgba(0x3a, 0x3d, 0x46),
        rgba(0x7c, 0xb7, 0xff), rgba(0xff, 0x6b, 0x81), rgba(0x8b, 0xd5, 0x6a),
        rgba(0xd9, 0x4c, 0x4c), rgba(0x6f, 0x74, 0x7f), rgba(0x37, 0xa8, 0x5b), rgba(0x4f, 0x8c, 0xf5)};
    return night ? dark : day;
}

void text(Canvas& c, FontLib& f, float x, float y, const std::string& s, float px, uint32_t col, FaceId face) {
    f.draw(c, face, px, x, y, s.c_str(), col);
}

float text_w(FontLib& f, const std::string& s, float px, FaceId face) { return f.text_width(face, px, s.c_str()); }

void text_centered(Canvas& c, FontLib& f, const Rect& r, const std::string& s, float px, uint32_t col, FaceId face) {
    float w = text_w(f, s, px, face);
    FontMetricsPx m = f.metrics(face, px);
    float x = r.x + (r.w - w) / 2;
    float y = r.y + (r.h - (m.ascent + m.descent)) / 2;
    text(c, f, x, y, s, px, col, face);
}

std::string fit(FontLib& f, const std::string& s, float px, float max_w, FaceId face) {
    if (text_w(f, s, px, face) <= max_w) return s;
    std::string t = s;
    while (!t.empty()) {
        // drop one UTF-8 code point from the end
        do t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80);
        if (text_w(f, t + "…", px, face) <= max_w) return t + "…";
    }
    return "…";
}

std::vector<std::string> wrap(FontLib& f, const std::string& s, float px, float max_w, FaceId face) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_w(f, cand, px, face) > max_w) {
                lines.push_back(line);
                line = word;
            } else {
                line = cand;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

float paragraph(Canvas& c, FontLib& f, float x, float y, float max_w, const std::string& s, float px, uint32_t col,
                FaceId face, float line_h) {
    for (auto& l : wrap(f, s, px, max_w, face)) {
        text(c, f, x, y, l, px, col, face);
        y += px * line_h;
    }
    return y;
}

void button(Canvas& c, FontLib& f, const Button& b, bool night) {
    const Theme& t = theme(night);
    uint32_t col = b.color ? b.color : t.accent;
    c.fill_round_rect(b.r.x, b.r.y + 2, b.r.w, b.r.h, 8, rgba(0, 0, 0, 40));
    c.fill_round_rect(b.r.x, b.r.y, b.r.w, b.r.h, 8, col);
    bool has_sub = !b.sub.empty();
    Rect main = b.r;
    if (has_sub) {
        main.h = b.r.h * 6 / 10;
        main.y += b.r.h / 10;
    }
    text_centered(c, f, main, b.label, has_sub ? 15 : 16, t.accent_text, FACE_SANS_BOLD);
    if (has_sub) {
        Rect sub{b.r.x, b.r.y + b.r.h * 6 / 10, b.r.w, b.r.h * 3 / 10};
        text_centered(c, f, sub, b.sub, 13, rgba(255, 255, 255, 220));
    }
    if (!b.hint.empty()) {
        float w = text_w(f, b.hint, 11, FACE_SANS_BOLD) + 8;
        c.fill_round_rect(b.r.x + 5, b.r.y + 5, (int)w, 15, 7, rgba(255, 255, 255, 70));
        text(c, f, b.r.x + 9, b.r.y + 5, b.hint, 11, t.accent_text, FACE_SANS_BOLD);
    }
}

bool hit(const Rect& r, int x, int y) { return r.contains(x, y); }

}  // namespace anki::ui
