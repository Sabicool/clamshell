// Converters from Anki's legacy ("schema 11") JSON objects, which the sync
// protocol still uses for note types, decks and deck presets, into the
// protobuf rows stored in a schema-18 collection. Port of the From<...Schema11>
// impls in rslib/src/{notetype,decks,deckconfig}/schema11.rs.
#pragma once
#include <string>

#include "collection.h"
#include "nlohmann/json.hpp"

namespace anki {

// Each returns false and sets err on malformed input. They follow rslib's merge
// rules: an incoming object only replaces a local one if it is at least as new.
bool merge_notetype_json(Collection& col, const nlohmann::json& nt, std::string& err);
bool merge_deck_json(Collection& col, const nlohmann::json& deck, std::string& err);
bool merge_deck_config_json(Collection& col, const nlohmann::json& conf, std::string& err);

// Local -> legacy JSON. The 3DS only ever changes a deck's "studied today"
// counters, and never edits presets or note types.
nlohmann::json all_config_json(Collection& col);
nlohmann::json deck_to_json(const Deck& d, int usn);

// Note helpers used when merging notes from the server.
std::string strip_html_preserving_media(const std::string& html);
int64_t field_checksum(const std::string& first_field_stripped);

}  // namespace anki
