#include "canvas.h"

#include <algorithm>
#include <cmath>

#if !defined(__3DS__) || defined(ANKI3DS_SELFTEST)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#endif

namespace anki {

Rect Rect::intersect(const Rect& o) const {
    int nx = std::max(x, o.x), ny = std::max(y, o.y);
    int nr = std::min(right(), o.right()), nb = std::min(bottom(), o.bottom());
    return Rect{nx, ny, std::max(0, nr - nx), std::max(0, nb - ny)};
}

void Canvas::resize(int w, int h, uint32_t bg) {
    w_ = w;
    h_ = h;
    px_.assign((size_t)w * h, bg);
    clips_.clear();
}

void Canvas::push_clip(const Rect& r) {
    Rect shifted{r.x - ox, r.y - oy, r.w, r.h};
    clips_.push_back(clips_.empty() ? shifted : clips_.back().intersect(shifted));
}

void Canvas::pop_clip() {
    if (!clips_.empty()) clips_.pop_back();
}

Rect Canvas::clip() const {
    Rect full{0, 0, w_, h_};
    return clips_.empty() ? full : clips_.back().intersect(full);
}

static inline uint32_t mix(uint32_t dst, uint32_t src, uint32_t a) {
    if (a >= 255) return src | 0xFF000000;
    uint32_t inv = 255 - a;
    uint32_t r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * inv) / 255;
    uint32_t g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * inv) / 255;
    uint32_t b = ((src & 255) * a + (dst & 255) * inv) / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void Canvas::blend(int x, int y, uint32_t argb, uint8_t coverage) {
    x -= ox;
    y -= oy;
    Rect c = clip();
    if (!c.contains(x, y)) return;
    uint32_t a = ((argb >> 24) * coverage) / 255;
    if (!a) return;
    uint32_t& d = px_[(size_t)y * w_ + x];
    d = mix(d, argb, a);
}

void Canvas::fill_rect(int x, int y, int w, int h, uint32_t argb) {
    Rect r = Rect{x - ox, y - oy, w, h}.intersect(clip());
    uint32_t a = argb >> 24;
    if (!a || r.w <= 0 || r.h <= 0) return;
    for (int yy = r.y; yy < r.bottom(); yy++) {
        uint32_t* row = &px_[(size_t)yy * w_];
        for (int xx = r.x; xx < r.right(); xx++) row[xx] = a == 255 ? argb : mix(row[xx], argb, a);
    }
}

void Canvas::fill_round_rect(int x, int y, int w, int h, int radius, uint32_t argb) {
    if (radius <= 0) {
        fill_rect(x, y, w, h, argb);
        return;
    }
    radius = std::min(radius, std::min(w, h) / 2);
    for (int yy = 0; yy < h; yy++) {
        int inset = 0;
        int dy = yy < radius ? radius - yy : (yy >= h - radius ? yy - (h - radius - 1) : 0);
        if (dy > 0) inset = radius - (int)std::lround(std::sqrt((double)radius * radius - (double)dy * dy));
        fill_rect(x + inset, y + yy, w - 2 * inset, 1, argb);
    }
}

void Canvas::fill_circle(int cx, int cy, int r, uint32_t argb) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)std::lround(std::sqrt((double)r * r - (double)dy * dy));
        fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, argb);
    }
}

void Canvas::draw_image(const Image& img, int dx, int dy, int dw, int dh) {
    if (img.w <= 0 || img.h <= 0 || dw <= 0 || dh <= 0) return;
    Rect target = Rect{dx - ox, dy - oy, dw, dh}.intersect(clip());
    for (int y = target.y; y < target.bottom(); y++) {
        // bilinear when downscaling is overkill here; area-average would be
        // nicer but nearest + 2x2 average is a good speed/quality balance
        float sy = ((y - (dy - oy)) + 0.5f) * img.h / (float)dh - 0.5f;
        int y0 = std::clamp((int)sy, 0, img.h - 1), y1 = std::min(y0 + 1, img.h - 1);
        float fy = std::clamp(sy - y0, 0.0f, 1.0f);
        uint32_t* row = &px_[(size_t)y * w_];
        for (int x = target.x; x < target.right(); x++) {
            float sx = ((x - (dx - ox)) + 0.5f) * img.w / (float)dw - 0.5f;
            int x0 = std::clamp((int)sx, 0, img.w - 1), x1 = std::min(x0 + 1, img.w - 1);
            float fx = std::clamp(sx - x0, 0.0f, 1.0f);
            uint32_t p00 = img.px[(size_t)y0 * img.w + x0], p01 = img.px[(size_t)y0 * img.w + x1];
            uint32_t p10 = img.px[(size_t)y1 * img.w + x0], p11 = img.px[(size_t)y1 * img.w + x1];
            auto ch = [&](int s) {
                float a = ((p00 >> s) & 255) * (1 - fx) + ((p01 >> s) & 255) * fx;
                float b = ((p10 >> s) & 255) * (1 - fx) + ((p11 >> s) & 255) * fx;
                return (uint32_t)(a * (1 - fy) + b * fy + 0.5f);
            };
            uint32_t src = (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
            uint32_t a = src >> 24;
            if (a) row[x] = mix(row[x], src, a);
        }
    }
}

bool Canvas::save_png(const std::string& path) const {
#if !defined(__3DS__) || defined(ANKI3DS_SELFTEST)
    std::vector<uint8_t> rgb((size_t)w_ * h_ * 3);
    for (size_t i = 0; i < px_.size(); i++) {
        rgb[i * 3] = (px_[i] >> 16) & 255;
        rgb[i * 3 + 1] = (px_[i] >> 8) & 255;
        rgb[i * 3 + 2] = px_[i] & 255;
    }
    return stbi_write_png(path.c_str(), w_, h_, 3, rgb.data(), w_ * 3) != 0;
#else
    (void)path;
    return false;
#endif
}

}  // namespace anki
