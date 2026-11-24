// HTTP transport used by the sync client. The desktop build implements it with
// libcurl (host/http_curl.cpp); the 3DS build uses mbedTLS over the console's
// sockets (3ds/source/http_3ds.cpp).
#pragma once
#include <functional>
#include <map>
#include <string>

namespace anki {

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-cased names
    std::string body;
};

// Called with bytes received so far and total (0 if unknown). Return false to cancel.
using HttpProgress = std::function<bool(size_t done, size_t total)>;

// Receives response body chunks when streaming to a file (full download).
using HttpSink = std::function<bool(const char* data, size_t len)>;

// POSTs `body` to `url`. If `sink` is set, body bytes go there instead of resp.body.
bool http_post(const std::string& url, const std::map<std::string, std::string>& headers, const std::string& body,
               HttpResponse& resp, std::string& err, const HttpSink& sink = nullptr,
               const HttpProgress& progress = nullptr);

}  // namespace anki
