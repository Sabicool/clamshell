#include "template.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace anki {

// ---------------------------------------------------------------- cloze.rs

namespace {

struct ClozeNode {
    bool is_text = true;
    std::string text;
    std::vector<int> ordinals;
    std::vector<ClozeNode> nodes;
    bool has_hint = false;
    std::string hint;
};

bool parse_open(const std::string& s, size_t i, size_t& end, std::vector<int>& ords) {
    if (s.compare(i, 3, "{{c") != 0) return false;
    size_t j = i + 3;
    size_t start = j;
    while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == ',')) j++;
    std::set<int> set;
    std::string digits = s.substr(start, j - start);
    size_t p = 0;
    while (p <= digits.size()) {
        size_t q = digits.find(',', p);
        std::string part = digits.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (!part.empty() && part.size() < 6) set.insert(std::stoi(part));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    if (set.empty()) return false;
    if (s.compare(j, 2, "::") != 0) return false;
    ords.assign(set.begin(), set.end());
    end = j + 2;
    return true;
}

std::vector<ClozeNode> parse_clozes(const std::string& s) {
    std::vector<ClozeNode> output;
    std::vector<ClozeNode> open;
    size_t i = 0;
    auto push_text = [&](const std::string& t) {
        if (!open.empty()) {
            ClozeNode n;
            n.text = t;
            if (t.compare(0, 16, "image-occlusion:") != 0) {
                size_t p = t.find("::");
                if (p != std::string::npos) {
                    n.text = t.substr(0, p);
                    open.back().has_hint = true;
                    open.back().hint = t.substr(p + 2);
                }
            }
            open.back().nodes.push_back(n);
        } else {
            ClozeNode n;
            n.text = t;
            output.push_back(n);
        }
    };
    while (i < s.size()) {
        size_t end;
        std::vector<int> ords;
        if (parse_open(s, i, end, ords)) {
            if (open.size() < 10) {
                ClozeNode c;
                c.is_text = false;
                c.ordinals = ords;
                open.push_back(c);
            }
            i = end;
        } else if (s.compare(i, 2, "}}") == 0) {
            if (!open.empty()) {
                ClozeNode c = std::move(open.back());
                open.pop_back();
                if (!open.empty()) open.back().nodes.push_back(std::move(c));
                else output.push_back(std::move(c));
            } else {
                ClozeNode n;
                n.text = "}}";
                output.push_back(n);
            }
            i += 2;
        } else {
            // normal text up to the next token
            size_t j = i + 1;
            while (j < s.size()) {
                size_t e2;
                std::vector<int> o2;
                if (s.compare(j, 2, "}}") == 0 || parse_open(s, j, e2, o2)) break;
                j++;
            }
            push_text(s.substr(i, j - i));
            i = j;
        }
    }
    return output;
}

std::string ords_str(const std::vector<int>& o) {
    std::string s;
    for (size_t i = 0; i < o.size(); i++) {
        if (i) s += ",";
        s += std::to_string(o[i]);
    }
    return s;
}

std::string encode_attribute(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

bool contains(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

void reveal(const ClozeNode& c, int ord, bool question, bool& found, std::string& buf) {
    bool active = contains(c.ordinals, ord);
    found |= active;
    if (!c.nodes.empty() && c.nodes[0].is_text && c.nodes[0].text.compare(0, 16, "image-occlusion:") == 0) {
        // Image occlusion shape: keep data so the renderer can draw masks.
        std::string data = c.nodes[0].text.substr(16);
        std::string attrs;
        size_t p = 0;
        std::string shape;
        while (p < data.size()) {
            size_t q = data.find(':', p);
            std::string part = data.substr(p, q == std::string::npos ? std::string::npos : q - p);
            size_t eq = part.find('=');
            if (eq == std::string::npos) shape = part;
            else attrs += " data-" + part.substr(0, eq) + "=\"" + encode_attribute(part.substr(eq + 1)) + "\"";
            if (q == std::string::npos) break;
            p = q + 1;
        }
        std::string cls = (question && active) || contains(c.ordinals, 0) ? "cloze"
                          : !active                                       ? "cloze-inactive"
                                                                          : "cloze-highlight";
        buf += "<div class=\"" + cls + "\" data-ordinal=\"" + ords_str(c.ordinals) + "\" data-shape=\"" + shape +
               "\"" + attrs + " ></div>";
        return;
    }
    if (question && active) {
        std::string content;
        for (const auto& n : c.nodes) {
            if (n.is_text) content += n.text;
            else reveal(n, ord, question, found, content);
        }
        buf += "<span class=\"cloze\" data-cloze=\"" + encode_attribute(content) + "\" data-ordinal=\"" +
               ords_str(c.ordinals) + "\">[" + (c.has_hint ? c.hint : "...") + "]</span>";
    } else {
        buf += active ? "<span class=\"cloze\" data-ordinal=\"" : "<span class=\"cloze-inactive\" data-ordinal=\"";
        buf += ords_str(c.ordinals) + "\">";
        for (const auto& n : c.nodes) {
            if (n.is_text) buf += n.text;
            else reveal(n, ord, question, found, buf);
        }
        buf += "</span>";
    }
}

void reveal_text_only(const ClozeNode& n, int ord, bool question, std::vector<std::string>& out) {
    if (n.is_text) return;
    if (contains(n.ordinals, ord)) {
        if (question) out.push_back(n.has_hint ? n.hint : "...");
        else {
            std::string t;
            std::function<void(const ClozeNode&)> collect = [&](const ClozeNode& x) {
                for (auto& y : x.nodes) {
                    if (y.is_text) t += y.text;
                    else collect(y);
                }
            };
            collect(n);
            out.push_back(t);
        }
    }
    for (auto& c : n.nodes) reveal_text_only(c, ord, question, out);
}

}  // namespace

std::string reveal_cloze_text(const std::string& text, int ord, bool question) {
    std::string buf;
    bool found = false;
    for (const auto& n : parse_clozes(text)) {
        if (n.is_text) buf += n.text;
        else reveal(n, ord, question, found, buf);
    }
    return found ? buf : std::string();
}

// ---------------------------------------------------------------- text helpers

std::string strip_html(const std::string& html) {
    std::string out;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            size_t e = html.find('>', i);
            if (e == std::string::npos) break;
            i = e + 1;
        } else if (html[i] == '&') {
            static const std::pair<const char*, const char*> ents[] = {
                {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}};
            bool hit = false;
            for (auto& [e, r] : ents)
                if (html.compare(i, strlen(e), e) == 0) {
                    out += r;
                    i += strlen(e);
                    hit = true;
                    break;
                }
            if (!hit) out += html[i++];
        } else {
            out += html[i++];
        }
    }
    return out;
}

bool field_is_empty(const std::string& text) {
    // rslib: ^[\s\u{a0}]*$ after removing <br>, <div>, </div>, &nbsp; etc.
    std::string t = text;
    for (const char* rm : {"<br>", "<br/>", "<br />", "<div>", "</div>", "&nbsp;", "<p>", "</p>"}) {
        size_t p;
        while ((p = t.find(rm)) != std::string::npos) t.erase(p, strlen(rm));
    }
    for (size_t i = 0; i < t.size(); i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c == 0xC2 && i + 1 < t.size() && (unsigned char)t[i + 1] == 0xA0) { i++; continue; }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- template.rs

namespace {

struct Ctx {
    std::map<std::string, std::string> fields;
    int card_ord = 0;
    bool question = true;
    std::string frontside;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string apply_filter(const std::string& filter, const std::string& text, const std::string& field_name,
                         const Ctx& ctx) {
    if (filter == "text") return strip_html(text);
    if (filter == "cloze") {
        std::string r = reveal_cloze_text(text, ctx.card_ord + 1, ctx.question);
        return r;
    }
    if (filter == "cloze-only") {
        std::vector<std::string> out;
        for (auto& n : parse_clozes(text)) reveal_text_only(n, ctx.card_ord + 1, ctx.question, out);
        std::string s;
        for (size_t i = 0; i < out.size(); i++) s += (i ? ", " : "") + out[i];
        return s;
    }
    if (filter == "hint") {
        if (field_is_empty(text)) return "";
        // Shown collapsed; the 3DS renderer expands hints when tapped.
        return "<a class=\"hint\" href=\"#\">" + field_name + "</a><div class=\"hint\" style=\"display:none\">" +
               text + "</div>";
    }
    if (filter == "type") return "";  // typing answers isn't supported on the 3DS
    if (filter == "kana" || filter == "kanji" || filter == "furigana") {
        // furigana syntax: 漢字[かんじ]. Keep base text or reading.
        std::string out;
        size_t i = 0;
        while (i < text.size()) {
            size_t lb = text.find('[', i);
            if (lb == std::string::npos) { out += text.substr(i); break; }
            size_t rb = text.find(']', lb);
            if (rb == std::string::npos) { out += text.substr(i); break; }
            size_t base_start = text.rfind(' ', lb);
            base_start = base_start == std::string::npos || base_start < i ? i : base_start + 1;
            out += text.substr(i, base_start - i);
            std::string base = text.substr(base_start, lb - base_start);
            std::string reading = text.substr(lb + 1, rb - lb - 1);
            if (filter == "kanji") out += base;
            else if (filter == "kana") out += reading;
            else out += "<ruby><rb>" + base + "</rb><rt>" + reading + "</rt></ruby>";
            i = rb + 1;
        }
        return out;
    }
    if (filter.compare(0, 3, "tts") == 0) return "";
    return text;  // unknown filters pass the text through
}

std::string render_field(const std::string& ref, const Ctx& ctx) {
    // filters are applied right-to-left: {{text:cloze:Field}}
    std::vector<std::string> parts;
    size_t p = 0;
    while (true) {
        size_t q = ref.find(':', p);
        parts.push_back(trim(ref.substr(p, q == std::string::npos ? std::string::npos : q - p)));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    std::string name = parts.back();
    parts.pop_back();
    std::string text;
    if (name == "FrontSide") text = ctx.frontside;
    else {
        auto it = ctx.fields.find(name);
        if (it == ctx.fields.end()) {
            if (parts.empty() || parts.back() != "type")
                return "<span style=\"color:#c00\">{{" + ref + "}}: unknown field</span>";
            return "";
        }
        text = it->second;
    }
    for (auto f = parts.rbegin(); f != parts.rend(); ++f) text = apply_filter(*f, text, name, ctx);
    return text;
}

bool cond_true(const std::string& name, const Ctx& ctx) {
    auto it = ctx.fields.find(name);
    if (it == ctx.fields.end()) return false;
    return !field_is_empty(it->second);
}

// Recursive-descent render over {{...}} tags.
std::string render_template(const std::string& tmpl, size_t& i, const Ctx& ctx, const std::string& until,
                            bool skip) {
    std::string out;
    while (i < tmpl.size()) {
        size_t open = tmpl.find("{{", i);
        if (open == std::string::npos) {
            if (!skip) out += tmpl.substr(i);
            i = tmpl.size();
            break;
        }
        if (!skip) out += tmpl.substr(i, open - i);
        size_t close = tmpl.find("}}", open + 2);
        if (close == std::string::npos) {
            if (!skip) out += tmpl.substr(open);
            i = tmpl.size();
            break;
        }
        std::string tag = trim(tmpl.substr(open + 2, close - open - 2));
        i = close + 2;
        if (tag.empty()) continue;
        if (tag[0] == '!') continue;  // comment
        if (tag[0] == '#' || tag[0] == '^') {
            std::string name = trim(tag.substr(1));
            bool truthy = cond_true(name, ctx);
            bool show = tag[0] == '#' ? truthy : !truthy;
            std::string inner = render_template(tmpl, i, ctx, name, skip || !show);
            if (!skip && show) out += inner;
            continue;
        }
        if (tag[0] == '/') {
            std::string name = trim(tag.substr(1));
            if (name == until) return out;
            continue;  // unbalanced close: ignore
        }
        if (!skip) out += render_field(tag, ctx);
    }
    return out;
}

std::string strip_av_tags(std::string s, std::vector<std::string>& media) {
    size_t p;
    while ((p = s.find("[sound:")) != std::string::npos) {
        size_t e = s.find(']', p);
        if (e == std::string::npos) break;
        media.push_back(s.substr(p + 7, e - p - 7));
        s.erase(p, e - p + 1);
    }
    return s;
}

void collect_images(const std::string& html, std::vector<std::string>& media) {
    size_t p = 0;
    while ((p = html.find("<img", p)) != std::string::npos) {
        size_t e = html.find('>', p);
        if (e == std::string::npos) break;
        std::string tag = html.substr(p, e - p);
        size_t s = tag.find("src=");
        if (s != std::string::npos) {
            char q = tag[s + 4];
            size_t st = (q == '"' || q == '\'') ? s + 5 : s + 4;
            size_t en = (q == '"' || q == '\'') ? tag.find(q, st) : tag.find_first_of(" />", st);
            if (en != std::string::npos) media.push_back(tag.substr(st, en - st));
        }
        p = e;
    }
}

}  // namespace

RenderedCard render_card(Collection& col, const Card& card) {
    RenderedCard r;
    auto note = col.get_note(card.nid);
    auto nt = note ? col.notetypes.find(note->mid) : col.notetypes.end();
    if (!note || nt == col.notetypes.end()) {
        r.question = r.answer = "<p>This card's note or note type is missing.</p>";
        return r;
    }
    const Notetype& t = nt->second;
    r.css = t.css;
    Ctx ctx;
    for (size_t i = 0; i < t.fields.size(); i++)
        ctx.fields[t.fields[i].name] = i < note->fields.size() ? note->fields[i] : std::string();
    std::string tags = note->tags;
    while (!tags.empty() && tags.front() == ' ') tags.erase(0, 1);
    while (!tags.empty() && tags.back() == ' ') tags.pop_back();
    ctx.fields["Tags"] = tags;
    ctx.fields["Type"] = t.name;
    const Deck* d = col.deck(card.odid ? card.odid : card.did);
    std::string deckname = d ? d->human_name() : "";
    ctx.fields["Deck"] = deckname;
    size_t last = deckname.rfind("::");
    ctx.fields["Subdeck"] = last == std::string::npos ? deckname : deckname.substr(last + 2);
    ctx.fields["CardFlag"] = card.flags & 7 ? "flag" + std::to_string(card.flags & 7) : "";
    ctx.fields["CardID"] = std::to_string(card.id);
    const NotetypeTemplate* tmpl = nullptr;
    int tord = t.is_cloze ? 0 : card.ord;
    for (auto& tt : t.templates)
        if (tt.ord == tord) tmpl = &tt;
    if (!tmpl && !t.templates.empty()) tmpl = &t.templates[0];
    if (!tmpl) {
        r.question = r.answer = "<p>No card template.</p>";
        return r;
    }
    ctx.fields["Card"] = tmpl->name;
    ctx.card_ord = card.ord;
    r.ord = card.ord;

    ctx.question = true;
    size_t i = 0;
    std::string q = render_template(tmpl->qfmt, i, ctx, "", false);
    if (t.is_cloze && q.find("class=\"cloze") == std::string::npos && q.find("cloze") == std::string::npos) {
        // rslib shows an explanation when the cloze number has no matching deletion
    }
    ctx.question = false;
    ctx.frontside = q;
    i = 0;
    std::string a = render_template(tmpl->afmt, i, ctx, "", false);
    r.question = strip_av_tags(q, r.media);
    r.answer = strip_av_tags(a, r.media);
    collect_images(r.question, r.media);
    collect_images(r.answer, r.media);
    std::sort(r.media.begin(), r.media.end());
    r.media.erase(std::unique(r.media.begin(), r.media.end()), r.media.end());
    return r;
}

}  // namespace anki
