// Selective media download. Rather than mirroring the whole media folder
// (often gigabytes), the 3DS fetches only the files used by the decks you
// study, using the media sync endpoint that serves files by name.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "collection.h"
#include "sync.h"

namespace anki {

// File names referenced by notes with cards in `deck_id` (and its subdecks)
// that are not yet present in `media_dir`.
std::vector<std::string> missing_media_for_deck(Collection& col, int64_t deck_id, const std::string& media_dir);

// Which of `names` aren't in media_dir yet (one directory listing).
std::vector<std::string> missing_files(const std::vector<std::string>& names, const std::string& media_dir);

// Same, for the notes of the given cards (e.g. today's study queue).
std::vector<std::string> missing_media_for_cards(Collection& col, const std::vector<int64_t>& card_ids,
                                                 const std::string& media_dir);

struct MediaDownloadResult {
    int downloaded = 0;
    int requested = 0;
    int unavailable = 0;  // referenced by notes but not on the server
};

// Downloads `names` in batches of 25 into media_dir. `progress(done, total)`.
bool download_media(SyncClient& client, const std::vector<std::string>& names, const std::string& media_dir,
                    MediaDownloadResult& result, std::string& err,
                    const std::function<void(int, int)>& progress = nullptr);

// Exposed for tests: extract a stored (uncompressed) zip produced by Anki's media server.
bool extract_media_zip(const std::string& zip, const std::string& media_dir, int& extracted, std::string& err);

}  // namespace anki
