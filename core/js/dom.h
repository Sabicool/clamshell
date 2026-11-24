// A small mutable DOM for running card scripts. HTML is parsed with gumbo,
// scripts change the tree through QuickJS, and the result is serialised back
// to HTML for litehtml to lay out.
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace anki::js {

struct Node {
    enum Type { ELEMENT, TEXT, COMMENT, DOCUMENT } type = ELEMENT;
    int id = 0;  // index into Dom::nodes
    std::string tag;  // lower-case
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;  // TEXT / COMMENT
    Node* parent = nullptr;
    std::vector<Node*> children;

    const std::string* attr(const std::string& name) const;
    void set_attr(const std::string& name, const std::string& value);
    void remove_attr(const std::string& name);
    bool has_class(const std::string& cls) const;
};

class Dom {
public:
    Dom();
    // Parses a fragment (the card body). The document has <html><body>…</body></html>.
    void parse(const std::string& html);
    Node* document() { return nodes_[0].get(); }
    Node* body() { return body_; }
    Node* html() { return html_; }
    Node* node(int id) { return id >= 0 && id < (int)nodes_.size() ? nodes_[id].get() : nullptr; }
    Node* create_element(const std::string& tag);
    Node* create_text(const std::string& text);
    void append(Node* parent, Node* child, Node* before = nullptr);
    void remove(Node* child);
    void set_inner_html(Node* el, const std::string& html);
    void set_text(Node* el, const std::string& text);

    std::string inner_html(const Node* n) const;
    std::string outer_html(const Node* n) const;
    std::string text_content(const Node* n) const;
    std::string body_html() const { return inner_html(body_); }

    std::vector<Node*> query(Node* scope, const std::string& selector, bool first_only);
    Node* by_id(const std::string& id);

    int version = 0;  // bumped on every mutation

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    Node* html_ = nullptr;
    Node* body_ = nullptr;
    Node* make(Node::Type t);
    void parse_into(Node* parent, const std::string& html);
};

// Selector matching (exposed for tests).
bool matches(const Node* n, const std::string& selector);

}  // namespace anki::js
