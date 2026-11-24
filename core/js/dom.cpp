#include "dom.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

#include "gumbo.h"

namespace anki::js {

const std::string* Node::attr(const std::string& name) const {
    for (auto& a : attrs)
        if (a.first == name) return &a.second;
    return nullptr;
}

void Node::set_attr(const std::string& name, const std::string& value) {
    for (auto& a : attrs)
        if (a.first == name) {
            a.second = value;
            return;
        }
    attrs.emplace_back(name, value);
}

void Node::remove_attr(const std::string& name) {
    attrs.erase(std::remove_if(attrs.begin(), attrs.end(), [&](auto& a) { return a.first == name; }), attrs.end());
}

bool Node::has_class(const std::string& cls) const {
    const std::string* c = attr("class");
    if (!c) return false;
    size_t i = 0;
    while (i < c->size()) {
        while (i < c->size() && isspace((unsigned char)(*c)[i])) i++;
        size_t j = i;
        while (j < c->size() && !isspace((unsigned char)(*c)[j])) j++;
        if (j > i && c->compare(i, j - i, cls) == 0 && cls.size() == j - i) return true;
        i = j;
    }
    return false;
}

Dom::Dom() {
    nodes_.clear();
    Node* doc = make(Node::DOCUMENT);
    (void)doc;
}

Node* Dom::make(Node::Type t) {
    auto n = std::make_unique<Node>();
    n->type = t;
    n->id = (int)nodes_.size();
    nodes_.push_back(std::move(n));
    return nodes_.back().get();
}

Node* Dom::create_element(const std::string& tag) {
    Node* n = make(Node::ELEMENT);
    n->tag = tag;
    for (auto& ch : n->tag) ch = (char)tolower((unsigned char)ch);
    return n;
}

Node* Dom::create_text(const std::string& text) {
    Node* n = make(Node::TEXT);
    n->text = text;
    return n;
}

void Dom::append(Node* parent, Node* child, Node* before) {
    if (!parent || !child || child == parent) return;
    if (child->parent) remove(child);
    child->parent = parent;
    auto it = before ? std::find(parent->children.begin(), parent->children.end(), before) : parent->children.end();
    parent->children.insert(it, child);
    version++;
}

void Dom::remove(Node* child) {
    if (!child || !child->parent) return;
    auto& v = child->parent->children;
    v.erase(std::remove(v.begin(), v.end(), child), v.end());
    child->parent = nullptr;
    version++;
}

static void convert(Dom& dom, Node* parent, GumboNode* g, const std::function<Node*(Node::Type)>& make,
                    std::vector<Node*>* html_body);

void Dom::parse_into(Node* parent, const std::string& html) {
    GumboOutput* out = gumbo_parse_with_options(&kGumboDefaultOptions, html.c_str(), html.size());
    // gumbo always produces <html><head/><body>…</body></html>; take body's children
    GumboNode* root = out->root;
    GumboNode* body = nullptr;
    GumboNode* head = nullptr;
    for (unsigned i = 0; i < root->v.element.children.length; i++) {
        GumboNode* c = (GumboNode*)root->v.element.children.data[i];
        if (c->type == GUMBO_NODE_ELEMENT && c->v.element.tag == GUMBO_TAG_BODY) body = c;
        if (c->type == GUMBO_NODE_ELEMENT && c->v.element.tag == GUMBO_TAG_HEAD) head = c;
    }
    auto mk = [this](Node::Type t) { return make(t); };
    // <style>/<script> at the start of a fragment end up in <head>
    if (head)
        for (unsigned i = 0; i < head->v.element.children.length; i++)
            convert(*this, parent, (GumboNode*)head->v.element.children.data[i], mk, nullptr);
    if (body)
        for (unsigned i = 0; i < body->v.element.children.length; i++)
            convert(*this, parent, (GumboNode*)body->v.element.children.data[i], mk, nullptr);
    gumbo_destroy_output(&kGumboDefaultOptions, out);
    version++;
}

static void convert(Dom& dom, Node* parent, GumboNode* g, const std::function<Node*(Node::Type)>& make,
                    std::vector<Node*>*) {
    Node* n = nullptr;
    switch (g->type) {
        case GUMBO_NODE_ELEMENT:
        case GUMBO_NODE_TEMPLATE: {
            n = make(Node::ELEMENT);
            GumboElement& e = g->v.element;
            if (e.tag == GUMBO_TAG_UNKNOWN) {
                GumboStringPiece tag = e.original_tag;
                gumbo_tag_from_original_text(&tag);
                n->tag.assign(tag.data, tag.length);
            } else {
                n->tag = gumbo_normalized_tagname(e.tag);
            }
            for (auto& ch : n->tag) ch = (char)tolower((unsigned char)ch);
            for (unsigned i = 0; i < e.attributes.length; i++) {
                GumboAttribute* a = (GumboAttribute*)e.attributes.data[i];
                n->attrs.emplace_back(a->name, a->value);
            }
            n->parent = parent;
            parent->children.push_back(n);
            for (unsigned i = 0; i < e.children.length; i++)
                convert(dom, n, (GumboNode*)e.children.data[i], make, nullptr);
            return;
        }
        case GUMBO_NODE_TEXT:
        case GUMBO_NODE_WHITESPACE:
        case GUMBO_NODE_CDATA:
            n = make(Node::TEXT);
            n->text = g->v.text.text;
            break;
        case GUMBO_NODE_COMMENT:
            n = make(Node::COMMENT);
            n->text = g->v.text.text;
            break;
        default:
            return;
    }
    n->parent = parent;
    parent->children.push_back(n);
}

void Dom::parse(const std::string& html) {
    nodes_.clear();
    make(Node::DOCUMENT);
    html_ = create_element("html");
    html_->parent = document();
    document()->children.push_back(html_);
    Node* head = create_element("head");
    head->parent = html_;
    html_->children.push_back(head);
    body_ = create_element("body");
    body_->parent = html_;
    html_->children.push_back(body_);
    parse_into(body_, html);
}

void Dom::set_inner_html(Node* el, const std::string& html) {
    if (!el) return;
    for (Node* c : el->children) c->parent = nullptr;
    el->children.clear();
    parse_into(el, html);
}

void Dom::set_text(Node* el, const std::string& text) {
    if (!el) return;
    if (el->type == Node::TEXT || el->type == Node::COMMENT) {
        el->text = text;
        version++;
        return;
    }
    for (Node* c : el->children) c->parent = nullptr;
    el->children.clear();
    if (!text.empty()) append(el, create_text(text));
    version++;
}

// ---------------------------------------------------------------- serialisation

static std::string escape_text(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

static std::string escape_attr(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

static bool is_void(const std::string& t) {
    static const char* v[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"};
    for (auto x : v)
        if (t == x) return true;
    return false;
}

static void serialise(const Node* n, std::string& out, bool raw_text) {
    switch (n->type) {
        case Node::TEXT: out += raw_text ? n->text : escape_text(n->text); return;
        case Node::COMMENT: out += "<!--" + n->text + "-->"; return;
        case Node::DOCUMENT:
            for (auto c : n->children) serialise(c, out, false);
            return;
        case Node::ELEMENT: {
            out += "<" + n->tag;
            for (auto& a : n->attrs) out += " " + a.first + "=\"" + escape_attr(a.second) + "\"";
            out += ">";
            if (is_void(n->tag)) return;
            bool raw = n->tag == "script" || n->tag == "style";
            for (auto c : n->children) serialise(c, out, raw);
            out += "</" + n->tag + ">";
            return;
        }
    }
}

std::string Dom::inner_html(const Node* n) const {
    std::string out;
    if (!n) return out;
    bool raw = n->tag == "script" || n->tag == "style";
    for (auto c : n->children) serialise(c, out, raw);
    return out;
}

std::string Dom::outer_html(const Node* n) const {
    std::string out;
    if (n) serialise(n, out, false);
    return out;
}

std::string Dom::text_content(const Node* n) const {
    if (!n) return "";
    if (n->type == Node::TEXT) return n->text;
    if (n->type == Node::COMMENT) return "";
    std::string out;
    for (auto c : n->children) out += text_content(c);
    return out;
}

// ---------------------------------------------------------------- selectors

namespace {

struct Simple {
    std::string tag;  // empty = any
    std::string id;
    std::vector<std::string> classes;
    struct Attr {
        std::string name, op, value;
    };
    std::vector<Attr> attrs;
    std::vector<std::string> pseudos;
    bool never = false;  // unsupported pseudo
};

struct Complex {
    std::vector<Simple> parts;      // left to right
    std::vector<char> combinators;  // between parts: ' ', '>', '+', '~'
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r"), b = s.find_last_not_of(" \t\n\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool parse_compound(const std::string& s, size_t& i, Simple& out) {
    auto ident = [&]() {
        std::string r;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_' || (unsigned char)s[i] >= 0x80))
            r += s[i++];
        return r;
    };
    bool any = false;
    if (i < s.size() && s[i] == '*') {
        i++;
        any = true;
    } else if (i < s.size() && (isalpha((unsigned char)s[i]))) {
        out.tag = ident();
        for (auto& ch : out.tag) ch = (char)tolower((unsigned char)ch);
        any = true;
    }
    while (i < s.size()) {
        char c = s[i];
        if (c == '#') {
            i++;
            out.id = ident();
        } else if (c == '.') {
            i++;
            out.classes.push_back(ident());
        } else if (c == '[') {
            size_t e = s.find(']', i);
            if (e == std::string::npos) return false;
            std::string inner = s.substr(i + 1, e - i - 1);
            i = e + 1;
            Simple::Attr a;
            size_t op = inner.find_first_of("=~|^$*");
            if (op == std::string::npos) {
                a.name = trim(inner);
            } else {
                a.name = trim(inner.substr(0, op));
                size_t eq = inner.find('=', op);
                a.op = inner.substr(op, eq - op + 1);
                std::string v = trim(inner.substr(eq + 1));
                if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'')) v = v.substr(1, v.size() - 2);
                a.value = v;
            }
            out.attrs.push_back(a);
        } else if (c == ':') {
            i++;
            if (i < s.size() && s[i] == ':') i++;
            std::string p = ident();
            if (i < s.size() && s[i] == '(') {
                int depth = 0;
                size_t st = i;
                while (i < s.size()) {
                    if (s[i] == '(') depth++;
                    else if (s[i] == ')' && --depth == 0) break;
                    i++;
                }
                p += s.substr(st, i - st + 1);
                i++;
            }
            out.pseudos.push_back(p);
        } else {
            break;
        }
        any = true;
    }
    return any;
}

std::vector<Complex> parse_selector(const std::string& sel) {
    std::vector<Complex> groups;
    // split on top-level commas
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : sel) {
        if (c == '(' || c == '[') depth++;
        if (c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    for (auto& p0 : parts) {
        std::string p = trim(p0);
        Complex cx;
        size_t i = 0;
        char pending = 0;
        while (i < p.size()) {
            while (i < p.size() && isspace((unsigned char)p[i])) {
                i++;
                if (!pending && !cx.parts.empty()) pending = ' ';
            }
            if (i >= p.size()) break;
            if (p[i] == '>' || p[i] == '+' || p[i] == '~') {
                pending = p[i++];
                continue;
            }
            Simple s;
            if (!parse_compound(p, i, s)) {
                i++;
                continue;
            }
            if (!cx.parts.empty()) cx.combinators.push_back(pending ? pending : ' ');
            pending = 0;
            cx.parts.push_back(s);
        }
        if (!cx.parts.empty()) groups.push_back(cx);
    }
    return groups;
}

const Node* prev_element(const Node* n) {
    if (!n->parent) return nullptr;
    const Node* prev = nullptr;
    for (auto c : n->parent->children) {
        if (c == n) return prev;
        if (c->type == Node::ELEMENT) prev = c;
    }
    return nullptr;
}

bool match_simple(const Node* n, const Simple& s) {
    if (n->type != Node::ELEMENT) return false;
    if (!s.tag.empty() && n->tag != s.tag) return false;
    if (!s.id.empty()) {
        const std::string* id = n->attr("id");
        if (!id || *id != s.id) return false;
    }
    for (auto& c : s.classes)
        if (!n->has_class(c)) return false;
    for (auto& a : s.attrs) {
        const std::string* v = n->attr(a.name);
        if (!v) return false;
        if (a.op.empty()) continue;
        if (a.op == "=" && *v != a.value) return false;
        if (a.op == "*=" && v->find(a.value) == std::string::npos) return false;
        if (a.op == "^=" && v->compare(0, a.value.size(), a.value) != 0) return false;
        if (a.op == "$=" && (v->size() < a.value.size() || v->compare(v->size() - a.value.size(), a.value.size(), a.value) != 0))
            return false;
        if (a.op == "~=") {
            Node tmp;
            tmp.attrs = {{"class", *v}};
            if (!tmp.has_class(a.value)) return false;
        }
        if (a.op == "|=" && !(*v == a.value || v->compare(0, a.value.size() + 1, a.value + "-") == 0)) return false;
    }
    for (auto& p : s.pseudos) {
        if (p == "first-child") {
            if (prev_element(n)) return false;
        } else if (p == "last-child") {
            if (!n->parent) return false;
            const Node* last = nullptr;
            for (auto c : n->parent->children)
                if (c->type == Node::ELEMENT) last = c;
            if (last != n) return false;
        } else if (p.compare(0, 4, "not(") == 0) {
            std::string inner = p.substr(4, p.size() - 5);
            if (matches(n, inner)) return false;
        } else if (p == "checked" || p == "hover" || p == "focus" || p == "active") {
            return false;
        } else {
            // unknown pseudo-classes/elements never match rather than over-match
            return false;
        }
    }
    return true;
}

bool match_complex(const Node* n, const Complex& cx, int idx) {
    if (!match_simple(n, cx.parts[idx])) return false;
    if (idx == 0) return true;
    char comb = cx.combinators[idx - 1];
    if (comb == '>') return n->parent && match_complex(n->parent, cx, idx - 1);
    if (comb == ' ') {
        for (const Node* a = n->parent; a; a = a->parent)
            if (match_complex(a, cx, idx - 1)) return true;
        return false;
    }
    if (comb == '+') {
        const Node* p = prev_element(n);
        return p && match_complex(p, cx, idx - 1);
    }
    if (comb == '~') {
        for (const Node* p = prev_element(n); p; p = prev_element(p))
            if (match_complex(p, cx, idx - 1)) return true;
        return false;
    }
    return false;
}

}  // namespace

bool matches(const Node* n, const std::string& selector) {
    for (auto& cx : parse_selector(selector))
        if (match_complex(n, cx, (int)cx.parts.size() - 1)) return true;
    return false;
}

std::vector<Node*> Dom::query(Node* scope, const std::string& selector, bool first_only) {
    std::vector<Node*> out;
    auto groups = parse_selector(selector);
    if (groups.empty() || !scope) return out;
    std::function<bool(Node*)> walk = [&](Node* n) {
        for (Node* c : n->children) {
            if (c->type != Node::ELEMENT) continue;
            for (auto& cx : groups)
                if (match_complex(c, cx, (int)cx.parts.size() - 1)) {
                    out.push_back(c);
                    if (first_only) return true;
                    break;
                }
            if (walk(c)) return true;
        }
        return false;
    };
    walk(scope);
    return out;
}

Node* Dom::by_id(const std::string& id) {
    std::function<Node*(Node*)> walk = [&](Node* n) -> Node* {
        for (Node* c : n->children) {
            if (c->type != Node::ELEMENT) continue;
            const std::string* v = c->attr("id");
            if (v && *v == id) return c;
            if (Node* r = walk(c)) return r;
        }
        return nullptr;
    };
    return walk(document());
}

}  // namespace anki::js
