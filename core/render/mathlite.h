// Lightweight stand-in for MathJax: rewrites \( ... \) and \[ ... \] blocks
// into HTML using Unicode symbols, <sub>/<sup>, and inline fractions. It covers
// the symbols, Greek letters, scripts, fractions, roots and \ce{} chemistry that
// medical decks typically use; anything it doesn't know is shown as-is.
#pragma once
#include <string>

namespace anki {

std::string tex_to_html(const std::string& tex);
std::string render_mathjax_in_html(const std::string& html);

}  // namespace anki
