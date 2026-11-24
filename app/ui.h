// Small immediate-mode drawing helpers shared by every screen.
#pragma once
#include <string>
#include <vector>

#include "render/canvas.h"
#include "render/fonts.h"

namespace anki::ui {

struct Theme {
    uint32_t bg, panel, text, dim, accent, accent_text, border;
    uint32_t new_c, learn_c, review_c;
    uint32_t again, hard, good, easy;
};
const Theme& theme(bool night);

void text(Canvas& c, FontLib& f, float x, float y, const std::string& s, float px, uint32_t col,
          FaceId face = FACE_SANS);
float text_w(FontLib& f, const std::string& s, float px, FaceId face = FACE_SANS);
void text_centered(Canvas& c, FontLib& f, const Rect& r, const std::string& s, float px, uint32_t col,
                   FaceId face = FACE_SANS);
// Truncates with an ellipsis to fit max_w.
std::string fit(FontLib& f, const std::string& s, float px, float max_w, FaceId face = FACE_SANS);
// Word-wraps to max_w; returns lines.
std::vector<std::string> wrap(FontLib& f, const std::string& s, float px, float max_w, FaceId face = FACE_SANS);
// Draws wrapped text and returns the y after the last line.
float paragraph(Canvas& c, FontLib& f, float x, float y, float max_w, const std::string& s, float px, uint32_t col,
                FaceId face = FACE_SANS, float line_h = 1.3f);

struct Button {
    Rect r;
    std::string label;
    std::string sub;     // e.g. interval "3d"
    std::string hint;    // e.g. "A"
    uint32_t color = 0;  // 0 = theme accent
};
void button(Canvas& c, FontLib& f, const Button& b, bool night);
bool hit(const Rect& r, int x, int y);

}  // namespace anki::ui
