#include "buttons.h"

#include "app.h"

namespace anki {

const std::vector<AssignableButton>& assignable_buttons() {
    static const std::vector<AssignableButton> b = {
        {"L", "L", K_L},     {"R", "R", K_R},         {"ZL", "ZL", K_ZL},
        {"ZR", "ZR", K_ZR},  {"LEFT", "◀", K_LEFT},  {"RIGHT", "▶", K_RIGHT},
    };
    return b;
}

static const char* FLAG_NAMES[] = {"red", "orange", "green", "blue", "pink", "turquoise", "purple"};

const std::vector<std::string>& button_actions() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> v = {"none",       "page_up", "page_down", "extra", "undo", "menu",
                                      "scratchpad", "bury",    "suspend",   "mark"};
        for (int i = 1; i <= 7; i++) v.push_back("flag:" + std::to_string(i));
        for (const char* k : {"ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Space", "Enter", "Escape"})
            v.push_back(std::string("key:") + k);
        for (char c = 'A'; c <= 'Z'; c++) v.push_back(std::string("key:Key") + c);
        for (char c = '0'; c <= '9'; c++) v.push_back(std::string("key:Digit") + c);
        for (char c = '0'; c <= '9'; c++) v.push_back(std::string("key:Alt+Digit") + c);
        return v;
    }();
    return list;
}

std::string action_short_label(const std::string& a) {
    if (a.rfind("key:", 0) == 0) {
        std::string k = a.substr(4), prefix;
        if (k.rfind("Alt+", 0) == 0) {
            prefix = "Alt+";
            k = k.substr(4);
        }
        if (k == "ArrowLeft") k = "←";
        else if (k == "ArrowRight") k = "→";
        else if (k == "ArrowUp") k = "↑";
        else if (k == "ArrowDown") k = "↓";
        else if (k == "Escape") k = "Esc";
        else if (k.rfind("Key", 0) == 0 || k.rfind("Digit", 0) == 0) k = k.substr(k[0] == 'K' ? 3 : 5);
        return prefix + k;
    }
    if (a == "page_up") return "page up";
    if (a == "page_down") return "page down";
    if (a == "extra") return "extra";
    if (a == "scratchpad") return "scratchpad";
    if (a.rfind("flag:", 0) == 0) return std::string("flag ") + FLAG_NAMES[(a[5] - '1') % 7];
    return a == "none" ? "" : a;
}

std::string action_label(const std::string& a) {
    if (a == "none") return "Nothing";
    if (a == "page_up") return "Page up";
    if (a == "page_down") return "Page down";
    if (a == "extra") return "Show/hide extra sections";
    if (a == "undo") return "Undo";
    if (a == "menu") return "Open the More menu";
    if (a == "scratchpad") return "Scratchpad on/off";
    if (a == "bury") return "Bury card";
    if (a == "suspend") return "Suspend card";
    if (a == "mark") return "Mark/unmark note";
    if (a.rfind("flag:", 0) == 0) return std::string("Flag ") + FLAG_NAMES[(a[5] - '1') % 7];
    if (a.rfind("key:", 0) == 0) return "Card key " + action_short_label(a);
    return a;
}

// Works for Malleus out of the box: ◀ ▶ hide and reveal one-by-one clozes on
// the front, ZL sends N, which reveals on the front and opens the next resource on the back.
const ButtonMap& default_buttons() {
    static const ButtonMap m = {{"L", "page_up"},     {"R", "page_down"},          {"ZL", "key:KeyN"},
                                {"ZR", "extra"},      {"LEFT", "key:ArrowLeft"}, {"RIGHT", "key:ArrowRight"}};
    return m;
}

}  // namespace anki
