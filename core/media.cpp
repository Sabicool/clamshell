#include "media.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <set>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace anki {

namespace {

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

std::string html_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            static const std::pair<const char*, char> ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                                                                {"&quot;", '"'}, {"&#39;", '\''}};
            bool hit = false;
            for (auto& [e, c] : ents)
                if (s.compare(i, strlen(e), e) == 0) {
                    out += c;
                    i += strlen(e) - 1;
                    hit = true;
                    break;
                }
            if (!hit) out += s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

void scan_field(const std::string& f, std::set<std::string>& out) {
    // <img src="...">, <source src>, [sound:...], url(...) in style attributes
    size_t p = 0;
    while ((p = f.find("src=", p)) != std::string::npos) {
        size_t q = p + 4;
        if (q >= f.size()) break;
        char quote = f[q];
        size_t st = (quote == '"' || quote == '\'') ? q + 1 : q;
        size_t en = (quote == '"' || quote == '\'') ? f.find(quote, st) : f.find_first_of(" >", st);
        if (en == std::string::npos) break;
        std::string name = url_decode(html_unescape(f.substr(st, en - st)));
        if (!name.empty() && name.find("://") == std::string::npos && name.find('/') == std::string::npos)
            out.insert(name);
        p = en;
    }
    p = 0;
    while ((p = f.find("[sound:", p)) != std::string::npos) {
        size_t e = f.find(']', p);
        if (e == std::string::npos) break;
        // sounds aren't played on the 3DS yet; skip downloading them
        p = e;
    }
}

uint32_t rd32(const std::string& z, size_t o) {
    return (uint32_t)(uint8_t)z[o] | ((uint32_t)(uint8_t)z[o + 1] << 8) | ((uint32_t)(uint8_t)z[o + 2] << 16) |
           ((uint32_t)(uint8_t)z[o + 3] << 24);
}
uint16_t rd16(const std::string& z, size_t o) { return (uint16_t)((uint8_t)z[o] | ((uint8_t)z[o + 1] << 8)); }

}  // namespace

std::vector<std::string> missing_media_for_deck(Collection& col, int64_t deck_id, const std::string& media_dir) {
    std::vector<int64_t> dids = col.child_deck_ids(deck_id);
    std::string ids;
    for (size_t i = 0; i < dids.size(); i++) ids += (i ? "," : "") + std::to_string(dids[i]);
    std::string sql = "select flds from notes where id in (select nid from cards where did in (" + ids +
                      ") or odid in (" + ids + "))";
    std::set<std::string> names;
    Stmt s(col.db(), sql.c_str());
    while (s.step()) scan_field(s.text(0), names);
    std::vector<std::string> out;
    for (auto& n : names)
        if (!file_exists(media_dir + "/" + n)) out.push_back(n);
    return out;
}

bool extract_media_zip(const std::string& z, const std::string& media_dir, int& extracted, std::string& err) {
    extracted = 0;
    // End of central directory
    if (z.size() < 22) {
        err = "media zip too short";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = z.size() - 22 + 1; i-- > 0;) {
        if (rd32(z, i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (z.size() - i > 70000) break;
    }
    if (eocd == std::string::npos) {
        err = "media zip has no directory";
        return false;
    }
    uint16_t count = rd16(z, eocd + 10);
    size_t cd = rd32(z, eocd + 16);
    struct Entry {
        std::string name;
        size_t offset, size;
        uint16_t method;
    };
    std::vector<Entry> entries;
    for (uint16_t i = 0; i < count; i++) {
        if (cd + 46 > z.size() || rd32(z, cd) != 0x02014b50) {
            err = "bad media zip directory";
            return false;
        }
        Entry e;
        e.method = rd16(z, cd + 10);
        e.size = rd32(z, cd + 20);
        uint16_t nlen = rd16(z, cd + 28), xlen = rd16(z, cd + 30), clen = rd16(z, cd + 32);
        size_t local = rd32(z, cd + 42);
        e.name = z.substr(cd + 46, nlen);
        if (local + 30 > z.size()) {
            err = "bad media zip entry";
            return false;
        }
        e.offset = local + 30 + rd16(z, local + 26) + rd16(z, local + 28);
        entries.push_back(e);
        cd += 46 + nlen + xlen + clen;
    }
    json meta;
    for (auto& e : entries)
        if (e.name == "_meta") meta = json::parse(z.substr(e.offset, e.size), nullptr, false);
    if (!meta.is_object()) {
        err = "media zip missing _meta";
        return false;
    }
    for (auto& e : entries) {
        if (e.name == "_meta") continue;
        if (e.method != 0) {
            err = "compressed media zips aren't supported";
            return false;
        }
        auto it = meta.find(e.name);
        if (it == meta.end() || !it->is_string()) continue;
        std::string real = it->get<std::string>();
        if (real.find('/') != std::string::npos || real.find('\\') != std::string::npos || real == ".." ) continue;
        std::string path = media_dir + "/" + real;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            err = "can't write " + path;
            return false;
        }
        fwrite(z.data() + e.offset, 1, e.size, f);
        fclose(f);
        extracted++;
    }
    return true;
}

bool download_media(SyncClient& client, const std::vector<std::string>& names, const std::string& media_dir,
                    MediaDownloadResult& result, std::string& err, const std::function<void(int, int)>& progress) {
    result = MediaDownloadResult();
    result.requested = (int)names.size();
    mkdir(media_dir.c_str(), 0755);
    std::string out;
    json begin = {{"v", client.client_version}};
    if (!client.request_path("msync/begin", begin.dump(), out, err)) return false;
    for (size_t i = 0; i < names.size(); i += 25) {
        json files = json::array();
        for (size_t j = i; j < names.size() && j < i + 25; j++) files.push_back(names[j]);
        json req = {{"files", files}};
        if (!client.request_path("msync/downloadFiles", req.dump(), out, err)) return false;
        int n = 0;
        if (!extract_media_zip(out, media_dir, n, err)) return false;
        result.downloaded += n;
        if (progress) progress((int)std::min(names.size(), i + 25), (int)names.size());
    }
    return true;
}

}  // namespace anki
