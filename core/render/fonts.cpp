#include "fonts.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

namespace anki {

struct FontLib::Face {
    std::vector<uint8_t> data;
    stbtt_fontinfo info;
};

FontLib::FontLib() = default;
FontLib::~FontLib() = default;

uint32_t utf8_next(const char*& p, const char* end) {
    unsigned char c = (unsigned char)*p++;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && p < end && ((unsigned char)*p & 0xC0) == 0x80) cp = (cp << 6) | ((unsigned char)*p++ & 0x3F);
    return cp;
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

bool FontLib::load(const std::string& dir, std::string& err) {
    static const char* files[FACE_COUNT] = {
        "DejaVuSansCondensed.ttf",        "DejaVuSansCondensed-Bold.ttf", "DejaVuSansCondensed-Oblique.ttf",
        "DejaVuSansCondensed-BoldOblique.ttf", "DejaVuSansMono.ttf",   "DejaVuSerifCondensed.ttf"};
    faces_.clear();
    faces_.resize(FACE_COUNT);
    for (int i = 0; i < FACE_COUNT; i++) {
        auto f = std::make_unique<Face>();
        if (!read_file(dir + "/" + files[i], f->data)) {
            if (i == FACE_SANS) {
                err = "missing font " + dir + "/" + files[i];
                faces_.clear();
                return false;
            }
            continue;
        }
        if (!stbtt_InitFont(&f->info, f->data.data(), stbtt_GetFontOffsetForIndex(f->data.data(), 0))) continue;
        faces_[i] = std::move(f);
    }
    return true;
}

FaceId FontLib::resolve(FaceId face, uint32_t cp) {
    if (!faces_[face]) face = FACE_SANS;
    if (stbtt_FindGlyphIndex(&faces_[face]->info, (int)cp)) return face;
    for (int i = 0; i < FACE_COUNT; i++)
        if (faces_[i] && stbtt_FindGlyphIndex(&faces_[i]->info, (int)cp)) return (FaceId)i;
    return face;
}

FontMetricsPx FontLib::metrics(FaceId face, float px) {
    FontMetricsPx m;
    if (!loaded()) return m;
    if (!faces_[face]) face = FACE_SANS;
    const stbtt_fontinfo& info = faces_[face]->info;
    float scale = stbtt_ScaleForMappingEmToPixels(&info, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    m.ascent = asc * scale;
    m.descent = -desc * scale;
    m.line_gap = gap * scale;
    int x0, y0, x1, y1;
    if (stbtt_GetCodepointBox(&info, 'x', &x0, &y0, &x1, &y1)) m.x_height = y1 * scale;
    else m.x_height = px * 0.5f;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&info, '0', &adv, &lsb);
    m.zero_width = adv * scale;
    stbtt_GetCodepointHMetrics(&info, ' ', &adv, &lsb);
    m.space_width = adv * scale;
    return m;
}

const FontLib::Glyph& FontLib::glyph(FaceId face, float px, uint32_t cp, FaceId& used) {
    used = resolve(face, cp);
    int ipx = (int)std::lround(px * 4);  // quarter-pixel size buckets
    uint64_t key = ((uint64_t)used << 56) | ((uint64_t)(ipx & 0xFFFF) << 32) | cp;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    if (cache_bytes_ > 6 * 1024 * 1024) {  // keep the 3DS heap in check
        cache_.clear();
        cache_bytes_ = 0;
    }
    Glyph g;
    const stbtt_fontinfo& info = faces_[used]->info;
    float scale = stbtt_ScaleForMappingEmToPixels(&info, ipx / 4.0f);
    bool zero_width = cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || cp == 0x200B || (cp >= 0x1F3FB && cp <= 0x1F3FF);
    if (!stbtt_FindGlyphIndex(&info, (int)cp) && cp > 0x20) {
        g.missing = !zero_width;
        g.advance = zero_width ? 0 : ipx / 4.0f * 0.8f + 3;
        cache_bytes_ += 32;
        return cache_.emplace(key, std::move(g)).first->second;
    }
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&info, (int)cp, &adv, &lsb);
    g.advance = adv * scale;
    if (cp != ' ' && cp != 0xA0 && cp != '\t') {
        unsigned char* bmp = stbtt_GetCodepointBitmap(&info, scale, scale, (int)cp, &g.w, &g.h, &g.xoff, &g.yoff);
        if (bmp) {
            g.bitmap.assign(bmp, bmp + (size_t)g.w * g.h);
            stbtt_FreeBitmap(bmp, nullptr);
        }
    }
    cache_bytes_ += g.bitmap.size() + 32;
    return cache_.emplace(key, std::move(g)).first->second;
}

float FontLib::text_width(FaceId face, float px, const char* s, size_t len) {
    if (!loaded() || !s) return 0;
    if (len == (size_t)-1) len = strlen(s);
    const char* p = s;
    const char* end = s + len;
    float w = 0;
    while (p < end) {
        uint32_t cp = utf8_next(p, end);
        FaceId used;
        w += glyph(face, px, cp, used).advance;
    }
    return w;
}

float FontLib::draw(Canvas& c, FaceId face, float px, float x, float y, const char* s, uint32_t argb, size_t len) {
    if (!loaded() || !s) return 0;
    if (len == (size_t)-1) len = strlen(s);
    FontMetricsPx m = metrics(face, px);
    float baseline = y + m.ascent;
    const char* p = s;
    const char* end = s + len;
    float pen = x;
    while (p < end) {
        uint32_t cp = utf8_next(p, end);
        FaceId used;
        const Glyph& g = glyph(face, px, cp, used);
        if (!g.bitmap.empty()) {
            int gx = (int)std::lround(pen) + g.xoff;
            int gy = (int)std::lround(baseline) + g.yoff;
            for (int yy = 0; yy < g.h; yy++)
                for (int xx = 0; xx < g.w; xx++) {
                    uint8_t cov = g.bitmap[(size_t)yy * g.w + xx];
                    if (cov) c.blend(gx + xx, gy + yy, argb, cov);
                }
        } else if (g.missing) {
            // No bundled font has this glyph (usually an emoji): draw a soft
            // rounded token so placeholders like Malleus's hidden clozes show.
            int bw = (int)(px * 0.8f), bh = (int)(m.ascent * 0.75f);
            int bx = (int)pen + 1, by = (int)(baseline - bh);
            c.fill_round_rect(bx, by, bw, bh, bh / 3, (argb & 0x00FFFFFF) | 0x66000000);
        }
        pen += g.advance;
    }
    return pen - x;
}

}  // namespace anki
