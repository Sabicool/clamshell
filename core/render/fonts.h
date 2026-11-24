// TrueType text via stb_truetype, with a glyph cache and per-codepoint fallback
// across the bundled DejaVu faces.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "canvas.h"

namespace anki {

enum FaceId { FACE_SANS = 0, FACE_SANS_BOLD, FACE_SANS_ITALIC, FACE_SANS_BOLD_ITALIC, FACE_MONO, FACE_SERIF, FACE_COUNT };

struct FontMetricsPx {
    float ascent = 0, descent = 0, line_gap = 0, x_height = 0, zero_width = 0, space_width = 0;
    float height() const { return ascent + descent; }
};

class FontLib {
public:
    FontLib();
    ~FontLib();
    // Loads faces from a directory containing the DejaVu files shipped in romfs.
    bool load(const std::string& dir, std::string& err);
    bool loaded() const { return !faces_.empty() && faces_[FACE_SANS] != nullptr; }

    FontMetricsPx metrics(FaceId face, float px);
    float text_width(FaceId face, float px, const char* utf8, size_t len = (size_t)-1);
    // Draws with the top of the line box at y. Returns the advance.
    float draw(Canvas& c, FaceId face, float px, float x, float y, const char* utf8, uint32_t argb,
               size_t len = (size_t)-1);

private:
    struct Face;
    struct Glyph {
        int w = 0, h = 0, xoff = 0, yoff = 0;
        float advance = 0;
        std::vector<uint8_t> bitmap;
    };
    std::vector<std::unique_ptr<Face>> faces_;
    std::unordered_map<uint64_t, Glyph> cache_;
    size_t cache_bytes_ = 0;
    const Glyph& glyph(FaceId face, float px, uint32_t cp, FaceId& used);
    FaceId resolve(FaceId face, uint32_t cp);
};

uint32_t utf8_next(const char*& p, const char* end);

}  // namespace anki
