// HTTP for the build on Anki's backend: requests go through the bridge's
// curl + mbedTLS transport (Alex Molina's), so the app has one TLS stack.
// Used for Clamshell's per-deck image downloads; collection sync itself
// happens inside the backend.
#ifdef CLAMSHELL_ANKI

#include <cstring>

#include "http.h"
#include "nlohmann/json.hpp"

extern "C" {
char* clamshell_call(const char* method, const char* args);
void clamshell_free(char* s);
}

namespace anki {

namespace {
const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += B64[v >> 18];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += B64[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        if (i + 1 < in.size()) v |= (uint8_t)in[i + 1] << 8;
        out += B64[v >> 18];
        out += B64[(v >> 12) & 63];
        out += i + 1 < in.size() ? B64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string b64_decode(const std::string& in) {
    int8_t map[256];
    memset(map, -1, sizeof map);
    for (int k = 0; k < 64; k++) map[(uint8_t)B64[k]] = (int8_t)k;
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int8_t d = map[(uint8_t)c];
        if (d < 0) continue;
        acc = (acc << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}
}  // namespace

bool http_post(const std::string& url, const std::map<std::string, std::string>& headers, const std::string& body,
               HttpResponse& resp, std::string& err, const HttpSink& sink, const HttpProgress&) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        err = "bad URL " + url;
        return false;
    }
    size_t path_at = url.find('/', scheme + 3);
    std::string endpoint = path_at == std::string::npos ? url + "/" : url.substr(0, path_at + 1);
    std::string path = path_at == std::string::npos ? "/" : url.substr(path_at);
    auto h = headers.find("anki-sync");
    nlohmann::json args = {{"endpoint", endpoint},
                           {"path", path},
                           {"header", h == headers.end() ? "" : h->second},
                           {"body", b64_encode(body)}};
    std::string a = args.dump();
    char* out = clamshell_call("post", a.c_str());
    auto r = nlohmann::json::parse(out ? out : "{}", nullptr, false);
    clamshell_free(out);
    if (r.is_discarded() || r.contains("error")) {
        err = r.is_discarded() ? "unreadable reply from the network layer" : r["error"].get<std::string>();
        return false;
    }
    resp.status = r.value("status", 0);
    resp.body = b64_decode(r.value("body", ""));
    if (sink) {
        if (!sink(resp.body.data(), resp.body.size())) {
            err = "cancelled";
            return false;
        }
        resp.body.clear();
    }
    return true;
}

}  // namespace anki

#endif
