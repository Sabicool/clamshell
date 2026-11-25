#include "card_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unordered_map>
#include <vector>

#include "litehtml.h"
#include "mathlite.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb/stb_image.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#include "webp/decode.h"
#include "webp/demux.h"

namespace anki {

using namespace litehtml;

namespace {

struct FontHandle {
    FaceId face;
    float px;
    int decoration;
    FontMetricsPx m;
};

uint32_t to_argb(const web_color& c) { return rgba(c.red, c.green, c.blue, c.alpha); }

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Scripts can't run here (no JS engine yet), and litehtml mis-measures line
// boxes that contain <script> elements, so remove them before layout.
std::string strip_scripts(const std::string& html) {
    std::string out;
    size_t i = 0;
    while (i < html.size()) {
        size_t p = i;
        while (true) {
            p = html.find('<', p);
            if (p == std::string::npos) break;
            if (html.size() - p >= 7 && strncasecmp(html.c_str() + p, "<script", 7) == 0) break;
            p++;
        }
        if (p == std::string::npos) {
            out += html.substr(i);
            break;
        }
        out += html.substr(i, p - i);
        size_t e = p;
        bool found = false;
        while ((e = html.find('<', e + 1)) != std::string::npos)
            if (html.size() - e >= 9 && strncasecmp(html.c_str() + e, "</script", 8) == 0) {
                found = true;
                break;
            }
        if (!found) break;
        size_t close = html.find('>', e);
        i = close == std::string::npos ? html.size() : close + 1;
    }
    return out;
}

bool webp_info(const std::string& path, int& w, int& h) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[64];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n >= 30 && WebPGetInfo(buf, n, &w, &h);
}

// Anki's built-in Image Occlusion draws its masks with JavaScript onto a canvas,
// reading hidden <div class="cloze" data-shape=...> markers. Without JS we move
// those markers into the image container as absolutely positioned boxes.
std::string io_fallback(const std::string& html, const std::string& media_dir) {
    size_t cont = html.find("id=\"image-occlusion-container\"");
    if (cont == std::string::npos || html.find("data-shape=") == std::string::npos) return html;
    // natural image size (for older notes that store pixel coordinates)
    float img_w = 0, img_h = 0;
    size_t img = html.find("<img", cont);
    if (img != std::string::npos) {
        size_t src = html.find("src=\"", img);
        size_t end = src == std::string::npos ? std::string::npos : html.find('"', src + 5);
        if (end != std::string::npos) {
            std::string name = url_decode(html.substr(src + 5, end - src - 5));
            int w, h, n;
            std::string ipath = media_dir + "/" + name;
            if (stbi_info(ipath.c_str(), &w, &h, &n) || webp_info(ipath, w, h)) {
                img_w = (float)w;
                img_h = (float)h;
            }
        }
    }
    auto attr = [](const std::string& tag, const char* name) -> std::string {
        std::string key = std::string(name) + "=\"";
        size_t p = tag.find(key);
        if (p == std::string::npos) return "";
        size_t e = tag.find('"', p + key.size());
        return e == std::string::npos ? "" : tag.substr(p + key.size(), e - p - key.size());
    };
    std::string shapes;
    std::string out;
    size_t i = 0;
    while (true) {
        size_t p = html.find("<div class=\"cloze", i);
        if (p == std::string::npos) break;
        size_t e = html.find("</div>", p);
        if (e == std::string::npos) break;
        std::string tag = html.substr(p, html.find('>', p) - p + 1);
        if (tag.find("data-shape=") == std::string::npos) {
            out += html.substr(i, e + 6 - i);
            i = e + 6;
            continue;
        }
        out += html.substr(i, p - i);
        i = e + 6;
        std::string shape = attr(tag, "data-shape");
        std::string cls = tag.find("cloze-inactive") != std::string::npos ? "io-inactive"
                          : tag.find("cloze-highlight") != std::string::npos ? "io-highlight"
                                                                             : "io-active";
        float l = 0, t = 0, w = 0, h = 0;
        auto num = [&](const char* n) { std::string v = attr(tag, n); return v.empty() ? 0.0f : (float)atof(v.c_str()); };
        if (shape == "rect" || shape == "text") {
            l = num("data-left"); t = num("data-top"); w = num("data-width"); h = num("data-height");
        } else if (shape == "ellipse") {
            l = num("data-left"); t = num("data-top"); w = 2 * num("data-rx"); h = 2 * num("data-ry");
        } else if (shape == "polygon") {
            std::string pts = attr(tag, "data-points");
            float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
            const char* c = pts.c_str();
            while (*c) {
                char* endp;
                float x = strtof(c, &endp);
                if (endp == c) { c++; continue; }
                c = endp;
                while (*c == ',' || *c == ' ') c++;
                float y = strtof(c, &endp);
                if (endp == c) break;
                c = endp;
                while (*c == ' ') c++;
                minx = std::min(minx, x); maxx = std::max(maxx, x);
                miny = std::min(miny, y); maxy = std::max(maxy, y);
            }
            if (maxx >= minx) { l = minx; t = miny; w = maxx - minx; h = maxy - miny; }
        }
        bool normalized = l <= 1.0f && t <= 1.0f && w <= 1.0f && h <= 1.0f;
        if (!normalized) {
            if (img_w <= 0) continue;
            l /= img_w; w /= img_w; t /= img_h; h /= img_h;
        }
        char style[200];
        snprintf(style, sizeof style, "left:%.3f%%;top:%.3f%%;width:%.3f%%;height:%.3f%%%s", l * 100, t * 100, w * 100,
                 h * 100, shape == "ellipse" ? ";border-radius:50%" : "");
        if (shape != "text") shapes += "<div class=\"io-mask " + cls + "\" style=\"" + style + "\"></div>";
    }
    out += html.substr(i);
    size_t canvas = out.find("<canvas id=\"image-occlusion-canvas\"");
    if (canvas == std::string::npos) canvas = out.find("</div>", out.find("id=\"image-occlusion-container\""));
    if (canvas != std::string::npos) out.insert(canvas, shapes);
    return out;
}

// Base styles: what Anki's reviewer adds around every card.
const char* BASE_CSS = R"CSS(
html { background: #ffffff; }
[hidden] { display: none !important; }
body { margin: 6px; overflow-wrap: break-word; }
.card { font-family: sans-serif; font-size: 20px; text-align: center; color: black; background-color: white; }
img { max-width: 100%; height: auto; }
.cloze { font-weight: bold; color: #0000ff; }
.cloze-inactive { }
hr#answer { margin: 8px 0; }
table { border-collapse: collapse; }
sub, sup { font-size: 75%; }
.mjx { white-space: nowrap; }
.mjx-frac { display: inline-table; vertical-align: middle; border-collapse: collapse; font-size: 85%; margin: 0 2px; }
.mjx-frac .mjx-frac { font-size: 100%; }
.mjx-dfrac { font-size: 100%; }
.mjx-row { display: table-row; }
.mjx-num, .mjx-den { display: table-cell; text-align: center; padding: 0 2px; line-height: 1.2; white-space: nowrap; }
.mjx-num { border-bottom: 1px solid; }
a { color: #1a5fb4; text-decoration: none; }
#image-occlusion-container { position: relative; display: inline-block; }
#image-occlusion-container img { display: block; max-width: 100%; }
#image-occlusion-canvas { display: none; }
.io-mask { position: absolute; border: 1px solid #212121; }
.io-active { background-color: #ff8e8e; }
.io-inactive { background-color: #ffeba2; }
.io-highlight { background-color: transparent; border: 2px solid #ff8e8e; }
)CSS";

// Optional: sections normally opened by JavaScript buttons.
const char* REVEAL_CSS = R"CSS(
.ExtraResourceContent, .hints, div.hint, .hidden-content, [data-anki3ds-reveal] { display: block !important; }
)CSS";

// Applied after the note type's CSS: controls that only work with JavaScript.
const char* COMPAT_CSS = R"CSS(
button, input, select, textarea { display: none !important; }
)CSS";

const char* NIGHT_CSS = R"CSS(
html, .card { background: #2b2b2b !important; color: #e8e8e8 !important; }
.cloze { color: #7cb7ff; }
)CSS";

}  // namespace

class CardContainer : public document_container {
public:
    FontLib& fonts;
    CardViewOptions opt;
    std::unordered_map<std::string, std::shared_ptr<Image>> images;
    std::vector<std::unique_ptr<FontHandle>> handles;

    explicit CardContainer(FontLib& f) : fonts(f) {}

    std::shared_ptr<Image> image(const std::string& src) {
        auto it = images.find(src);
        if (it != images.end()) return it->second;
        std::shared_ptr<Image> img;
        std::string name = url_decode(src);
        if (name.find("://") == std::string::npos && !opt.media_dir.empty()) {
            std::string path = opt.media_dir + "/" + name;
            int w = 0, h = 0, n;
            unsigned char* data = nullptr;
            bool svg = name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".svg") == 0;
            bool webp = false;
            if (svg) {
                // Image Occlusion masks are SVG; rasterise them with nanosvg.
                NSVGimage* doc = nsvgParseFromFile(path.c_str(), "px", 96.0f);
                if (doc && doc->width > 0 && doc->height > 0) {
                    float scale = doc->width > 800 ? 800.0f / doc->width : 1.0f;
                    w = (int)(doc->width * scale);
                    h = (int)(doc->height * scale);
                    data = (unsigned char*)malloc((size_t)w * h * 4);
                    NSVGrasterizer* rast = nsvgCreateRasterizer();
                    nsvgRasterize(rast, doc, 0, 0, scale, data, w, h, w * 4);
                    nsvgDeleteRasterizer(rast);
                }
                if (doc) nsvgDelete(doc);
            } else {
                data = stbi_load(path.c_str(), &w, &h, &n, 4);
                if (!data) {
                    // WebP (common in shared decks like Malleus) isn't handled by stb_image
                    FILE* f = fopen(path.c_str(), "rb");
                    if (f) {
                        std::vector<uint8_t> buf;
                        fseek(f, 0, SEEK_END);
                        long sz = ftell(f);
                        fseek(f, 0, SEEK_SET);
                        if (sz > 12 && sz < 32 * 1024 * 1024) {
                            buf.resize((size_t)sz);
                            if (fread(buf.data(), 1, buf.size(), f) == buf.size() &&
                                memcmp(buf.data(), "RIFF", 4) == 0 && memcmp(buf.data() + 8, "WEBP", 4) == 0) {
                                data = WebPDecodeRGBA(buf.data(), buf.size(), &w, &h);
                                webp = data != nullptr;
                                if (!data) {
                                    // animated WebP: show the first frame
                                    WebPData wd{buf.data(), buf.size()};
                                    WebPAnimDecoderOptions o;
                                    WebPAnimDecoderOptionsInit(&o);
                                    o.color_mode = MODE_RGBA;
                                    if (WebPAnimDecoder* dec = WebPAnimDecoderNew(&wd, &o)) {
                                        WebPAnimInfo info;
                                        uint8_t* frame = nullptr;
                                        int ts = 0;
                                        if (WebPAnimDecoderGetInfo(dec, &info) && WebPAnimDecoderGetNext(dec, &frame, &ts)) {
                                            w = (int)info.canvas_width;
                                            h = (int)info.canvas_height;
                                            data = (unsigned char*)malloc((size_t)w * h * 4);
                                            memcpy(data, frame, (size_t)w * h * 4);
                                            svg = true;  // free() with free(), like the SVG path
                                        }
                                        WebPAnimDecoderDelete(dec);
                                    }
                                }
                            }
                        }
                        fclose(f);
                    }
                }
            }
            if (data) {
                // Downscale big images up front: the 3DS screen is 400px wide.
                int maxw = 800;
                int tw = w, th = h;
                if (w > maxw) {
                    tw = maxw;
                    th = std::max(1, (int)((int64_t)h * maxw / w));
                }
                img = std::make_shared<Image>();
                img->w = tw;
                img->h = th;
                img->px.resize((size_t)tw * th);
                for (int y = 0; y < th; y++) {
                    int sy = (int)((int64_t)y * h / th);
                    for (int x = 0; x < tw; x++) {
                        int sx = (int)((int64_t)x * w / tw);
                        const unsigned char* p = data + ((size_t)sy * w + sx) * 4;
                        img->px[(size_t)y * tw + x] = rgba(p[0], p[1], p[2], p[3]);
                    }
                }
                if (svg) free(data);
                else if (webp) WebPFree(data);
                else stbi_image_free(data);
            }
        }
        if (!img && getenv("ANKI3DS_DEBUG_IMG")) fprintf(stderr, "image failed: %s\n", name.c_str());
        images[src] = img;
        return img;
    }

    uint_ptr create_font(const font_description& d, const document*, font_metrics* fm) override {
        std::string fam = d.family;
        for (auto& ch : fam) ch = (char)tolower((unsigned char)ch);
        bool bold = d.weight >= 600;
        bool italic = d.style == font_style_italic;
        FaceId face = FACE_SANS;
        if (fam.find("mono") != std::string::npos || fam.find("courier") != std::string::npos ||
            fam.find("consol") != std::string::npos)
            face = FACE_MONO;
        else if (bold && italic) face = FACE_SANS_BOLD_ITALIC;
        else if (bold) face = FACE_SANS_BOLD;
        else if (italic) face = FACE_SANS_ITALIC;
        auto h = std::make_unique<FontHandle>();
        h->face = face;
        h->px = std::max(6.0f, d.size * opt.text_zoom);
        h->decoration = d.decoration_line;
        h->m = fonts.metrics(face, h->px);
        if (fm) {
            fm->font_size = h->px;
            fm->ascent = std::ceil(h->m.ascent);
            fm->descent = std::ceil(h->m.descent);
            fm->height = fm->ascent + fm->descent;
            fm->x_height = h->m.x_height;
            fm->ch_width = h->m.zero_width;
            fm->draw_spaces = (d.decoration_line != text_decoration_line_none);
            fm->sub_shift = h->px * 0.25f;
            fm->super_shift = h->px * 0.35f;
        }
        handles.push_back(std::move(h));
        return (uint_ptr)handles.back().get();
    }
    void delete_font(uint_ptr) override {}
    pixel_t text_width(const char* text, uint_ptr hf) override {
        FontHandle* h = (FontHandle*)hf;
        return fonts.text_width(h->face, h->px, text);
    }
    void draw_text(uint_ptr hdc, const char* text, uint_ptr hf, web_color color, const position& pos) override {
        Canvas* c = (Canvas*)hdc;
        FontHandle* h = (FontHandle*)hf;
        uint32_t col = to_argb(color);
        float y = pos.y + (pos.height - (std::ceil(h->m.ascent) + std::ceil(h->m.descent)));
        float adv = fonts.draw(*c, h->face, h->px, pos.x, y, text, col);
        if (h->decoration & text_decoration_line_underline)
            c->fill_rect((int)pos.x, (int)(y + h->m.ascent + 1), (int)std::ceil(adv), 1, col);
        if (h->decoration & text_decoration_line_line_through)
            c->fill_rect((int)pos.x, (int)(y + h->m.ascent - h->m.x_height / 2), (int)std::ceil(adv), 1, col);
        if (h->decoration & text_decoration_line_overline)
            c->fill_rect((int)pos.x, (int)y, (int)std::ceil(adv), 1, col);
    }
    pixel_t pt_to_px(float pt) const override { return pt * 96.0f / 72.0f; }
    pixel_t get_default_font_size() const override { return 16; }
    const char* get_default_font_name() const override { return "sans-serif"; }

    void draw_list_marker(uint_ptr hdc, const list_marker& m) override {
        Canvas* c = (Canvas*)hdc;
        uint32_t col = to_argb(m.color);
        int cx = (int)(m.pos.x + m.pos.width / 2), cy = (int)(m.pos.y + m.pos.height / 2);
        int r = std::max(2, (int)(m.pos.width / 2));
        switch (m.marker_type) {
            case list_style_type_circle:
                c->fill_circle(cx, cy, r, col);
                c->fill_circle(cx, cy, std::max(1, r - 1), 0xFFFFFFFF);
                break;
            case list_style_type_square:
                c->fill_rect((int)m.pos.x, (int)m.pos.y, (int)m.pos.width, (int)m.pos.height, col);
                break;
            case list_style_type_decimal:
            case list_style_type_lower_alpha:
            case list_style_type_upper_alpha:
            case list_style_type_lower_roman:
            case list_style_type_upper_roman: {
                std::string label;
                if (m.marker_type == list_style_type_decimal) label = std::to_string(m.index) + ".";
                else if (m.marker_type == list_style_type_lower_alpha) label = std::string(1, (char)('a' + (m.index - 1) % 26)) + ".";
                else if (m.marker_type == list_style_type_upper_alpha) label = std::string(1, (char)('A' + (m.index - 1) % 26)) + ".";
                else label = std::to_string(m.index) + ".";
                if (m.font) {
                    FontHandle* h = (FontHandle*)m.font;
                    float w = fonts.text_width(h->face, h->px, label.c_str());
                    float y = m.pos.y + m.pos.height / 2 - h->m.ascent * 0.6f;
                    fonts.draw(*c, h->face, h->px, m.pos.x + m.pos.width - w, y, label.c_str(), col);
                }
                break;
            }
            case list_style_type_none:
                break;
            default:
                c->fill_circle(cx, cy, r, col);
        }
    }
    void load_image(const char* src, const char*, bool) override { image(src); }
    void get_image_size(const char* src, const char*, size& sz) override {
        auto img = image(src);
        if (img) {
            sz.width = (pixel_t)img->w * (img->w > opt.width ? 1.0f : 1.0f);
            sz.height = (pixel_t)img->h;
        } else {
            sz.width = sz.height = 0;
        }
    }
    void draw_image(uint_ptr hdc, const background_layer& layer, const std::string& url, const std::string&) override {
        Canvas* c = (Canvas*)hdc;
        auto img = image(url);
        if (!img) return;
        const position& b = layer.origin_box.width > 0 ? layer.origin_box : layer.border_box;
        c->push_clip({(int)layer.clip_box.x, (int)layer.clip_box.y, (int)std::ceil(layer.clip_box.width),
                      (int)std::ceil(layer.clip_box.height)});
        c->draw_image(*img, (int)b.x, (int)b.y, (int)std::lround(b.width), (int)std::lround(b.height));
        c->pop_clip();
    }
    void draw_solid_fill(uint_ptr hdc, const background_layer& layer, const web_color& color) override {
        Canvas* c = (Canvas*)hdc;
        const position& b = layer.border_box;
        position cl = layer.clip_box;
        c->push_clip({(int)cl.x, (int)cl.y, (int)std::ceil(cl.width), (int)std::ceil(cl.height)});
        int radius = (int)layer.border_radius.top_left_x;
        c->fill_round_rect((int)b.x, (int)b.y, (int)std::ceil(b.width), (int)std::ceil(b.height), radius, to_argb(color));
        c->pop_clip();
    }
    void draw_gradient_fallback(uint_ptr hdc, const background_layer& layer, const background_layer::gradient_base& g) {
        if (g.color_points.empty()) return;
        web_color mid = g.color_points[g.color_points.size() / 2].color;
        draw_solid_fill(hdc, layer, mid);
    }
    void draw_linear_gradient(uint_ptr hdc, const background_layer& l, const background_layer::linear_gradient& g) override {
        draw_gradient_fallback(hdc, l, g);
    }
    void draw_radial_gradient(uint_ptr hdc, const background_layer& l, const background_layer::radial_gradient& g) override {
        draw_gradient_fallback(hdc, l, g);
    }
    void draw_conic_gradient(uint_ptr hdc, const background_layer& l, const background_layer::conic_gradient& g) override {
        draw_gradient_fallback(hdc, l, g);
    }
    void draw_borders(uint_ptr hdc, const borders& b, const position& p, bool) override {
        Canvas* c = (Canvas*)hdc;
        auto side = [&](const border& bd, int x, int y, int w, int h) {
            if (bd.width <= 0 || bd.style == border_style_none || bd.style == border_style_hidden) return;
            c->fill_rect(x, y, w, h, to_argb(bd.color));
        };
        int x = (int)p.x, y = (int)p.y, w = (int)std::ceil(p.width), h = (int)std::ceil(p.height);
        side(b.top, x, y, w, (int)std::ceil(b.top.width));
        side(b.bottom, x, y + h - (int)std::ceil(b.bottom.width), w, (int)std::ceil(b.bottom.width));
        side(b.left, x, y, (int)std::ceil(b.left.width), h);
        side(b.right, x + w - (int)std::ceil(b.right.width), y, (int)std::ceil(b.right.width), h);
    }
    void set_caption(const char*) override {}
    void set_base_url(const char*) override {}
    void link(const std::shared_ptr<document>&, const element::ptr&) override {}
    void on_anchor_click(const char*, const element::ptr&) override {}
    void on_mouse_event(const element::ptr&, mouse_event) override {}
    void set_cursor(const char*) override {}
    void transform_text(string& text, text_transform tt) override {
        if (tt == text_transform_uppercase)
            for (auto& ch : text) ch = (char)toupper((unsigned char)ch);
        else if (tt == text_transform_lowercase)
            for (auto& ch : text) ch = (char)tolower((unsigned char)ch);
    }
    void import_css(string&, const string&, string&) override {}
    void set_clip(const position& pos, const border_radiuses&) override {
        clip_stack_canvas->push_clip({(int)pos.x, (int)pos.y, (int)std::ceil(pos.width), (int)std::ceil(pos.height)});
    }
    void del_clip() override { clip_stack_canvas->pop_clip(); }
    void get_viewport(position& vp) const override {
        vp.x = 0;
        vp.y = 0;
        vp.width = (pixel_t)opt.width;
        vp.height = (pixel_t)opt.viewport_height;
    }
    element::ptr create_element(const char*, const string_map&, const std::shared_ptr<document>&) override {
        return nullptr;
    }
    void get_media_features(media_features& m) const override {
        m.type = media_type_screen;
        m.width = m.device_width = (pixel_t)opt.width;
        m.height = m.device_height = (pixel_t)opt.viewport_height;
        m.color = 8;
        m.color_index = 0;
        m.monochrome = 0;
        m.resolution = 96;
    }
    void get_language(string& lang, string& culture) const override {
        lang = "en";
        culture = "";
    }

    Canvas* clip_stack_canvas = nullptr;
};

CardView::CardView(FontLib& fonts) : fonts_(fonts), container_(std::make_unique<CardContainer>(fonts)) {}
CardView::~CardView() = default;

static void find_answer_anchor(const element::ptr& el, int& y) {
    if (!el || y >= 0) return;
    const char* id = el->get_attr("id");
    if (id && std::string(id) == "answer") {
        y = (int)el->get_placement().y;
        return;
    }
    for (auto& ch : el->children()) find_answer_anchor(ch, y);
}

void CardView::render(const std::string& body_html, const std::string& css, int ord, const CardViewOptions& opt) {
    container_->opt = opt;
    container_->handles.clear();
    std::string body = render_mathjax_in_html(io_fallback(strip_scripts(body_html), opt.media_dir));
    if (body.compare(0, 13, "<div id=\"qa\">") != 0) body = "<div id=\"qa\">" + body + "</div>";
    // "mobile" like AnkiDroid/AnkiMobile, so templates use their small-screen layout
    std::string doc_html = "<html class=\"mobile\"><head><style>" + std::string(BASE_CSS) + css + COMPAT_CSS +
                           (opt.reveal_hidden ? std::string(REVEAL_CSS) : std::string()) +
                           (opt.night_mode ? std::string(NIGHT_CSS) : std::string()) +
                           "</style></head><body class=\"card card" + std::to_string(ord + 1) +
                           (opt.night_mode ? " nightMode night_mode" : "") + "\">" + body + "</body></html>";
    auto doc = document::createFromString(doc_html, container_.get());
    doc->render((pixel_t)opt.width);
    int h = std::max((int)std::ceil(doc->height()), opt.viewport_height);
    h = std::min(h, opt.max_height);
    content_h_ = h;
    uint32_t bg = opt.night_mode ? rgba(0x2b, 0x2b, 0x2b) : 0xFFFFFFFF;
    canvas_.resize(opt.width, h, bg);
    container_->clip_stack_canvas = &canvas_;
    position clip(0, 0, (pixel_t)opt.width, (pixel_t)h);
    doc->draw((uint_ptr)&canvas_, 0, 0, &clip);
    answer_y_ = -1;
    find_answer_anchor(doc->root(), answer_y_);
}

}  // namespace anki
