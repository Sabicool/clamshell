// AnkiWeb / self-hosted sync client: a port of the client half of
// rslib/src/sync (protocol version 11: zstd-compressed JSON over HTTP POST).
//
// Safety rules for this client:
//  - It never performs a full *upload*. If the server says a full sync is
//    required, only a full download is offered.
//  - It never sends decks, presets or note types (the 3DS can't edit them);
//    if any are pending locally the sync stops rather than guess.
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "collection.h"

namespace anki {

enum class SyncStatus { NoChanges, Synced, FullSyncRequired, Downloaded, Failed };
const char* sync_status_name(SyncStatus s);

struct SyncResult {
    SyncStatus status = SyncStatus::Failed;
    std::string message;  // server message or explanation
    bool download_ok = false;
    std::optional<std::string> new_endpoint;  // set if the server redirected us
};

class SyncClient {
public:
    explicit SyncClient(std::string endpoint = "https://sync.ankiweb.net/");
    std::string endpoint;
    std::string hkey;
    std::string client_version = "anki3ds,0.1.0,3ds";
    std::function<void(const std::string&)> on_progress;  // human-readable stage text
    std::function<void(size_t, size_t)> on_bytes;          // download progress

    std::optional<std::string> login(const std::string& user, const std::string& pass, std::string& err);
    SyncResult normal_sync(Collection& col, std::string& err);
    // Replaces the local collection with the server copy (after a temp-file
    // download + integrity check). The collection is reopened afterwards.
    SyncResult full_download(Collection& col, std::string& err);

private:
    std::string session_;
    bool request(const std::string& method, const std::string& json_body, std::string& out, std::string& err,
                 int* status_out = nullptr);
    void progress(const std::string& s) {
        if (on_progress) on_progress(s);
    }
};

// zstd helpers (exposed for tests)
std::string zstd_compress(const std::string& in);
bool zstd_decompress(const std::string& in, std::string& out);

}  // namespace anki
