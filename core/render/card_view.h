// Lays out a card's HTML + CSS with litehtml and paints it into a Canvas.
#pragma once
#include <map>
#include <memory>
#include <string>

#include "canvas.h"
#include "fonts.h"

namespace litehtml {
class document;
}

namespace anki {

struct CardViewOptions {
    int width = 400;           // layout width in screen pixels
    int viewport_height = 240;
    float text_zoom = 0.8f;    // scales font sizes (cards are styled for big desktop screens)
    std::string media_dir;     // folder containing collection.media files
    bool night_mode = false;
    int max_height = 6000;     // cap for very long cards
    // Show sections that templates hide behind JavaScript buttons (Malleus
    // extra resources, AnKing hints), since scripts don't run here.
    bool reveal_hidden = false;
};

class CardContainer;

// Time spent in each stage of CardView::render, added up (for profiling)
struct RenderTimes {
    double prep_ms = 0, parse_ms = 0, layout_ms = 0, draw_ms = 0;
};
extern RenderTimes g_render_times;

// A media file's image, decoded and scaled for the screen; cached. Safe to
// call from another thread (the app decodes images ahead on its worker).
std::shared_ptr<Image> load_media_image(const std::string& media_dir, const std::string& name);
// Decodes the images a card's HTML refers to, so showing it later is quick.
void preload_media_images(const std::string& media_dir, const std::string& html);
// Frees the decoded images (before a sync or download, which need the memory).
void clear_media_image_cache();

class CardView {
public:
    explicit CardView(FontLib& fonts);
    ~CardView();
    // Lays out the card and paints it into an internal canvas.
    // `body_html` is the rendered question or answer; `ord` gives the card class.
    void render(const std::string& body_html, const std::string& css, int ord, const CardViewOptions& opt);
    const Canvas& canvas() const { return canvas_; }
    int content_height() const { return content_h_; }
    // y position (in canvas pixels) of the element with id="answer", or -1
    int answer_anchor() const { return answer_y_; }
    // Redraws part of the laid-out card at `scale` into `out` (keeping out's
    // size): the region starting at (src_x, src_y) in normal canvas pixels.
    // Text is drawn at the larger size and images from their full resolution.
    void draw_zoomed(Canvas& out, float scale, float src_x, float src_y) const;

private:
    FontLib& fonts_;
    std::unique_ptr<CardContainer> container_;
    Canvas canvas_;
    std::shared_ptr<litehtml::document> doc_;
    int content_h_ = 0;
    int answer_y_ = -1;
};

}  // namespace anki
