// libcurl implementation of the sync transport for desktop testing.
#include "version.h"
#include <curl/curl.h>

#include <algorithm>
#include <cctype>

#include "http.h"

namespace anki {

namespace {
struct Ctx {
    HttpResponse* resp;
    const HttpSink* sink;
    const HttpProgress* progress;
    size_t received = 0;
    size_t total = 0;
};

size_t on_body(char* ptr, size_t size, size_t n, void* ud) {
    Ctx* c = (Ctx*)ud;
    size_t len = size * n;
    c->received += len;
    if (c->sink && *c->sink) {
        if (!(*c->sink)(ptr, len)) return 0;
    } else {
        c->resp->body.append(ptr, len);
    }
    if (c->progress && *c->progress && !(*c->progress)(c->received, c->total)) return 0;
    return len;
}

size_t on_header(char* buf, size_t size, size_t n, void* ud) {
    Ctx* c = (Ctx*)ud;
    std::string line(buf, size * n);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string k = line.substr(0, colon);
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ')) v.erase(0, 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        c->resp->headers[k] = v;
        if (k == "content-length") c->total = (size_t)std::stoull(v);
    }
    return size * n;
}
}  // namespace

bool http_post(const std::string& url, const std::map<std::string, std::string>& headers, const std::string& body,
               HttpResponse& resp, std::string& err, const HttpSink& sink, const HttpProgress& progress) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        err = "curl init failed";
        return false;
    }
    Ctx ctx{&resp, &sink, &progress};
    struct curl_slist* hl = nullptr;
    for (auto& [k, v] : headers) hl = curl_slist_append(hl, (k + ": " + v).c_str());
    hl = curl_slist_append(hl, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, APP_ID "/" ANKI3DS_VERSION);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    resp.status = (int)code;
    curl_slist_free_all(hl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        return false;
    }
    return true;
}

}  // namespace anki
