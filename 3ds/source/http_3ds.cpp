// HTTP/1.1 POST over BSD sockets (libctru soc:u) with TLS from mbedTLS.
// The console's own HTTP service has outdated TLS and root certificates, so we
// bring our own (Mozilla CA bundle in romfs:/cacert.pem).
#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "http.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

extern "C" int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
    (void)data;
    if (R_FAILED(PS_GenerateRandomBytes(output, len))) return -1;
    *olen = len;
    return 0;
}

extern "C" mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)osGetTime(); }

namespace anki {

namespace {

const char* ca_bundle_path() {
#ifdef ANKI3DS_HOST_TEST
    static std::string p = std::string(getenv("ANKI3DS_ROMFS") ? getenv("ANKI3DS_ROMFS") : "3ds/romfs") + "/cacert.pem";
    return p.c_str();
#else
    return "romfs:/cacert.pem";
#endif
}

struct Tls {
    bool ready = false;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    std::string error;

    bool init() {
        if (ready) return true;
        psa_crypto_init();
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&ca);
        const char* pers = "anki3ds";
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers))) {
            error = "random number generator failed to start";
            return false;
        }
        FILE* f = fopen(ca_bundle_path(), "rb");
        if (!f) {
            error = "missing romfs:/cacert.pem";
            return false;
        }
        std::string pem;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) pem.append(buf, n);
        fclose(f);
        pem.push_back('\0');
        int rc = mbedtls_x509_crt_parse(&ca, (const unsigned char*)pem.data(), pem.size());
        if (rc < 0) {
            error = "could not load CA certificates";
            return false;
        }
        ready = true;
        return true;
    }
};

Tls g_tls;

int sock_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    int n = send(fd, buf, len, 0);
    if (n < 0) return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return n;
}

int sock_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    int n = recv(fd, buf, len, 0);
    if (n < 0) return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return n;
}

struct Conn {
    int fd = -1;
    bool tls = false;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    ~Conn() {
        if (tls) {
            mbedtls_ssl_close_notify(&ssl);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
        }
        if (fd >= 0) closesocket(fd);
    }

    int write_all(const char* p, size_t n) {
        while (n > 0) {
            int w = tls ? mbedtls_ssl_write(&ssl, (const unsigned char*)p, n) : send(fd, p, n, 0);
            if (tls && (w == MBEDTLS_ERR_SSL_WANT_WRITE || w == MBEDTLS_ERR_SSL_WANT_READ)) continue;
            if (w <= 0) return -1;
            p += w;
            n -= (size_t)w;
        }
        return 0;
    }

    int read_some(char* p, size_t n) {
        while (true) {
            int r = tls ? mbedtls_ssl_read(&ssl, (unsigned char*)p, n) : recv(fd, p, n, 0);
            if (tls && (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
            if (tls && r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            return r;
        }
    }
};

bool parse_url(const std::string& url, bool& https, std::string& host, std::string& port, std::string& path) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    std::string scheme = url.substr(0, p);
    https = scheme == "https";
    size_t hs = p + 3;
    size_t ps = url.find('/', hs);
    std::string hostport = url.substr(hs, ps == std::string::npos ? std::string::npos : ps - hs);
    path = ps == std::string::npos ? "/" : url.substr(ps);
    size_t c = hostport.rfind(':');
    if (c != std::string::npos) {
        host = hostport.substr(0, c);
        port = hostport.substr(c + 1);
    } else {
        host = hostport;
        port = https ? "443" : "80";
    }
    return !host.empty();
}

}  // namespace

bool http_post(const std::string& url, const std::map<std::string, std::string>& headers, const std::string& body,
               HttpResponse& resp, std::string& err, const HttpSink& sink, const HttpProgress& progress) {
    bool https;
    std::string host, port, path;
    if (!parse_url(url, https, host, port, path)) {
        err = "bad URL: " + url;
        return false;
    }
    if (https && !g_tls.init()) {
        err = g_tls.error;
        return false;
    }
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        err = "could not look up " + host + " (is Wi-Fi on?)";
        return false;
    }
    Conn conn;
    conn.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (conn.fd < 0 || connect(conn.fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        err = "could not connect to " + host;
        return false;
    }
    freeaddrinfo(res);
    if (https) {
        conn.tls = true;
        mbedtls_ssl_init(&conn.ssl);
        mbedtls_ssl_config_init(&conn.conf);
        mbedtls_ssl_config_defaults(&conn.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_authmode(&conn.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conn.conf, &g_tls.ca, nullptr);
        mbedtls_ssl_conf_rng(&conn.conf, mbedtls_ctr_drbg_random, &g_tls.drbg);
        if (mbedtls_ssl_setup(&conn.ssl, &conn.conf) != 0 || mbedtls_ssl_set_hostname(&conn.ssl, host.c_str()) != 0) {
            err = "TLS setup failed";
            return false;
        }
        mbedtls_ssl_set_bio(&conn.ssl, &conn.fd, sock_send, sock_recv, nullptr);
        int rc;
        while ((rc = mbedtls_ssl_handshake(&conn.ssl)) != 0) {
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            char buf[128];
            mbedtls_strerror(rc, buf, sizeof buf);
            uint32_t flags = mbedtls_ssl_get_verify_result(&conn.ssl);
            err = std::string("secure connection failed: ") + buf;
            if (flags & MBEDTLS_X509_BADCERT_EXPIRED || flags & MBEDTLS_X509_BADCERT_FUTURE)
                err += " (check the 3DS date and time)";
            return false;
        }
    }

    std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n" +
                      "User-Agent: anki3ds/0.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    for (auto& [k, v] : headers) req += k + ": " + v + "\r\n";
    req += "\r\n";
    if (conn.write_all(req.data(), req.size()) || conn.write_all(body.data(), body.size())) {
        err = "connection lost while sending";
        return false;
    }

    // Read headers
    std::string buf;
    char tmp[16384];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        int n = conn.read_some(tmp, sizeof tmp);
        if (n <= 0) {
            err = "connection closed before a response arrived";
            return false;
        }
        buf.append(tmp, (size_t)n);
        header_end = buf.find("\r\n\r\n");
    }
    std::string head = buf.substr(0, header_end);
    std::string rest = buf.substr(header_end + 4);
    size_t sp = head.find(' ');
    resp.status = sp == std::string::npos ? 0 : atoi(head.c_str() + sp + 1);
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos && pos < head.size()) {
        size_t next = head.find("\r\n", pos + 2);
        std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            resp.headers[k] = v;
        }
        pos = next;
    }
    bool chunked = resp.headers.count("transfer-encoding") &&
                   resp.headers["transfer-encoding"].find("chunked") != std::string::npos;
    size_t total = resp.headers.count("content-length") ? (size_t)strtoull(resp.headers["content-length"].c_str(), nullptr, 10) : 0;
    size_t received = 0;
    bool ok_sink = true;
    auto emit = [&](const char* p, size_t n) {
        if (!n || !ok_sink) return;
        received += n;
        if (sink && resp.status == 200) ok_sink = sink(p, n);
        else resp.body.append(p, n);
        if (progress && !progress(received, total)) ok_sink = false;
    };

    // Body: chunked decoder state machine, or raw until close/length
    std::string pending = rest;
    size_t chunk_left = 0;
    bool in_chunk = false, done = false;
    auto process = [&]() {
        if (!chunked) {
            emit(pending.data(), pending.size());
            pending.clear();
            if (total && received >= total) done = true;
            return;
        }
        while (!done) {
            if (!in_chunk) {
                size_t eol = pending.find("\r\n");
                if (eol == std::string::npos) return;
                chunk_left = strtoul(pending.substr(0, eol).c_str(), nullptr, 16);
                pending.erase(0, eol + 2);
                if (chunk_left == 0) {
                    done = true;
                    return;
                }
                in_chunk = true;
            }
            size_t take = std::min(chunk_left, pending.size());
            emit(pending.data(), take);
            pending.erase(0, take);
            chunk_left -= take;
            if (chunk_left) return;
            if (pending.size() < 2) return;  // wait for the CRLF after the chunk
            pending.erase(0, 2);
            in_chunk = false;
        }
    };
    process();
    while (!done && ok_sink) {
        int n = conn.read_some(tmp, sizeof tmp);
        if (n <= 0) break;
        pending.append(tmp, (size_t)n);
        process();
    }
    if (!ok_sink) {
        err = "download cancelled or failed";
        return false;
    }
    if (chunked && !done) {
        err = "connection lost during download";
        return false;
    }
    return true;
}

}  // namespace anki
