// Card template rendering: a port of the parts of rslib/src/template.rs,
// template_filters.rs and cloze.rs that turn a note + card template into the
// question/answer HTML. The HTML is then laid out by the 3DS renderer.
#pragma once
#include <string>
#include <vector>

#include "collection.h"

namespace anki {

struct RenderedCard {
    std::string question;  // HTML body (without <html>/<style>)
    std::string answer;
    std::string css;
    int ord = 0;           // template ordinal; body gets class "card card{ord+1}"
    std::vector<std::string> media;  // media file names referenced (images, sounds)
};

RenderedCard render_card(Collection& col, const Card& card);

// Exposed for tests
std::string reveal_cloze_text(const std::string& text, int cloze_ord, bool question);
std::string strip_html(const std::string& html);
bool field_is_empty(const std::string& text);

}  // namespace anki
