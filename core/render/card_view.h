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
};

class CardContainer;

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

private:
    FontLib& fonts_;
    std::unique_ptr<CardContainer> container_;
    Canvas canvas_;
    int content_h_ = 0;
    int answer_y_ = -1;
};

}  // namespace anki
