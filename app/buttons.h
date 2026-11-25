// What the shoulder buttons and ◀ ▶ do while reviewing. Each can run an app
// action or send a keyboard key to the card's scripts, so decks with their own
// shortcuts (Malleus: N for the next resource, AnKing: C for tags) can be used.
// There's one set for all note types, and a note type can have its own.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace anki {

// The assignable buttons, in the order the settings screen lists them.
struct AssignableButton {
    const char* id;     // saved in settings.json
    const char* label;  // shown on screen
    uint32_t key;       // Key bit
};
const std::vector<AssignableButton>& assignable_buttons();

// Every choice, in the order ◀ ▶ step through them. Ids are "page_down",
// "extra", "key:KeyN", "key:Alt+Digit1" and so on.
const std::vector<std::string>& button_actions();
std::string action_label(const std::string& action);        // "Card key N"
std::string action_short_label(const std::string& action);  // "N", for hints

using ButtonMap = std::map<std::string, std::string>;  // button id -> action id
const ButtonMap& default_buttons();

}  // namespace anki
