// A plain 32-bit software canvas (0xAARRGGBB) with clipping and alpha blending.
// Cards and UI are painted here and then copied to the 3DS framebuffer, which
// also lets the desktop build produce real screenshots.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace anki {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    Rect intersect(const Rect& o) const;
    bool contains(int px, int py) const { return px >= x && py >= y && px < right() && py < bottom(); }
};

inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // 0xAARRGGBB
};

class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h, uint32_t bg = 0xFFFFFFFF) { resize(w, h, bg); }
    void resize(int w, int h, uint32_t bg = 0xFFFFFFFF);
    int width() const { return w_; }
    int height() const { return h_; }
    uint32_t* data() { return px_.data(); }
    const uint32_t* data() const { return px_.data(); }
    uint32_t get(int x, int y) const { return px_[(size_t)y * w_ + x]; }

    // Translation applied to all drawing (used to paint one page of a long card).
    int ox = 0, oy = 0;

    void push_clip(const Rect& r);
    void pop_clip();
    Rect clip() const;

    void blend(int x, int y, uint32_t argb, uint8_t coverage = 255);
    void fill_rect(int x, int y, int w, int h, uint32_t argb);
    void fill_round_rect(int x, int y, int w, int h, int radius, uint32_t argb);
    void hline(int x, int y, int w, uint32_t argb) { fill_rect(x, y, w, 1, argb); }
    void draw_image(const Image& img, int dx, int dy, int dw, int dh);
    void fill_circle(int cx, int cy, int r, uint32_t argb);
    bool save_png(const std::string& path) const;  // desktop only

private:
    int w_ = 0, h_ = 0;
    std::vector<uint32_t> px_;
    std::vector<Rect> clips_;
};

}  // namespace anki
