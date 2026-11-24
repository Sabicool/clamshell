#include "mathlite.h"

#include <cctype>
#include <cstring>
#include <map>

namespace anki {

namespace {

const std::map<std::string, std::string>& symbols() {
    static const std::map<std::string, std::string> m = {
        // Greek
        {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"varepsilon", "ε"},
        {"zeta", "ζ"}, {"eta", "η"}, {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"},
        {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"}, {"pi", "π"}, {"rho", "ρ"}, {"sigma", "σ"},
        {"tau", "τ"}, {"upsilon", "υ"}, {"phi", "φ"}, {"varphi", "φ"}, {"chi", "χ"}, {"psi", "ψ"},
        {"omega", "ω"}, {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Xi", "Ξ"},
        {"Pi", "Π"}, {"Sigma", "Σ"}, {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
        // relations and operators
        {"ge", "≥"}, {"geq", "≥"}, {"geqslant", "≥"}, {"le", "≤"}, {"leq", "≤"}, {"leqslant", "≤"},
        {"neq", "≠"}, {"ne", "≠"}, {"approx", "≈"}, {"sim", "∼"}, {"simeq", "≃"}, {"equiv", "≡"},
        {"propto", "∝"}, {"pm", "±"}, {"mp", "∓"}, {"times", "×"}, {"div", "÷"}, {"cdot", "·"},
        {"cdots", "⋯"}, {"ldots", "…"}, {"dots", "…"}, {"infty", "∞"}, {"partial", "∂"}, {"nabla", "∇"},
        {"sum", "∑"}, {"prod", "∏"}, {"int", "∫"}, {"oint", "∮"}, {"ll", "≪"}, {"gg", "≫"},
        {"gt", ">"}, {"lt", "<"}, {"in", "∈"}, {"notin", "∉"}, {"subset", "⊂"}, {"cup", "∪"}, {"cap", "∩"},
        {"forall", "∀"}, {"exists", "∃"}, {"neg", "¬"}, {"land", "∧"}, {"lor", "∨"}, {"emptyset", "∅"},
        {"degree", "°"}, {"circ", "°"}, {"prime", "′"}, {"star", "⋆"}, {"ast", "∗"}, {"bullet", "•"},
        {"therefore", "∴"}, {"because", "∵"}, {"angle", "∠"}, {"perp", "⊥"}, {"parallel", "∥"},
        // arrows
        {"to", "→"}, {"rightarrow", "→"}, {"leftarrow", "←"}, {"gets", "←"}, {"leftrightarrow", "↔"},
        {"Rightarrow", "⇒"}, {"Leftarrow", "⇐"}, {"Leftrightarrow", "⇔"}, {"implies", "⇒"}, {"iff", "⇔"},
        {"uparrow", "↑"}, {"downarrow", "↓"}, {"updownarrow", "↕"}, {"rightleftharpoons", "⇌"},
        {"longrightarrow", "⟶"}, {"mapsto", "↦"}, {"uparrow", "↑"},
        // spacing and misc
        {",", " "}, {";", " "}, {":", " "}, {"!", ""}, {"quad", "  "}, {"qquad", "    "}, {" ", " "},
        {"%", "%"}, {"$", "$"}, {"#", "#"}, {"&", "&amp;"}, {"{", "{"}, {"}", "}"}, {"_", "_"},
        {"left", ""}, {"right", ""}, {"big", ""}, {"Big", ""}, {"bigg", ""}, {"Bigg", ""},
        {"displaystyle", ""}, {"textstyle", ""}, {"limits", ""}, {"nolimits", ""},
        {"rm", ""}, {"it", ""}, {"sf", ""}, {"tt", ""}, {"cal", ""}, {"scriptstyle", ""}, {"normalsize", ""},
        {"log", "log"}, {"ln", "ln"}, {"exp", "exp"}, {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"},
        {"max", "max"}, {"min", "min"}, {"lim", "lim"}, {"det", "det"},
        {"langle", "⟨"}, {"rangle", "⟩"}, {"lceil", "⌈"}, {"rceil", "⌉"}, {"lfloor", "⌊"}, {"rfloor", "⌋"},
        {"uparrow", "↑"}, {"backslash", "\\"}, {"vert", "|"}, {"|", "‖"}, {"mid", "|"},
    };
    return m;
}

std::string escape(char c) {
    switch (c) {
        case '<': return "&lt;";
        case '>': return "&gt;";
        case '&': return "&amp;";
        default: return std::string(1, c);
    }
}

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    std::string parse_all() {
        std::string out;
        while (i_ < s_.size()) out += parse_atom(true);
        return out;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skip_ws() {
        while (i_ < s_.size() && isspace((unsigned char)s_[i_])) i_++;
    }

    // A braced group or a single token.
    std::string parse_arg() {
        skip_ws();
        if (i_ >= s_.size()) return "";
        if (s_[i_] == '{') {
            i_++;
            std::string out;
            while (i_ < s_.size() && s_[i_] != '}') out += parse_atom(true);
            if (i_ < s_.size()) i_++;
            return out;
        }
        return parse_atom(false);
    }

    std::string raw_group() {
        skip_ws();
        if (i_ >= s_.size() || s_[i_] != '{') return "";
        int depth = 0;
        size_t start = ++i_;
        while (i_ < s_.size()) {
            if (s_[i_] == '{') depth++;
            else if (s_[i_] == '}') {
                if (depth == 0) break;
                depth--;
            }
            i_++;
        }
        std::string r = s_.substr(start, i_ - start);
        if (i_ < s_.size()) i_++;
        return r;
    }

    std::string parse_atom(bool allow_scripts) {
        std::string base = parse_primary();
        if (!allow_scripts) return base;
        while (i_ < s_.size() && (s_[i_] == '^' || s_[i_] == '_')) {
            char k = s_[i_++];
            std::string arg = parse_arg();
            if (k == '^') {
                if (arg == "′" || arg == "\\prime") base += "′";
                else base += "<sup>" + arg + "</sup>";
            } else {
                base += "<sub>" + arg + "</sub>";
            }
        }
        return base;
    }

    std::string chem(const std::string& t) {
        // \ce{H2O}, \ce{Na+}, \ce{SO4^2-}, \ce{A -> B}
        std::string out;
        for (size_t k = 0; k < t.size(); k++) {
            char c = t[k];
            if (t.compare(k, 2, "->") == 0) { out += " → "; k++; continue; }
            if (t.compare(k, 3, "<=>") == 0) { out += " ⇌ "; k += 2; continue; }
            if (isdigit((unsigned char)c) && k > 0 && (isalpha((unsigned char)t[k - 1]) || t[k - 1] == ')' || t[k - 1] == ']')) {
                std::string d;
                while (k < t.size() && isdigit((unsigned char)t[k])) d += t[k++];
                k--;
                out += "<sub>" + d + "</sub>";
                continue;
            }
            if (c == '^') {
                std::string sup;
                k++;
                if (k < t.size() && t[k] == '{') {
                    k++;
                    while (k < t.size() && t[k] != '}') sup += t[k++];
                } else {
                    while (k < t.size() && (isdigit((unsigned char)t[k]) || t[k] == '+' || t[k] == '-')) sup += t[k++];
                    k--;
                }
                out += "<sup>" + sup + "</sup>";
                continue;
            }
            if ((c == '+' || c == '-') && k > 0 && isalnum((unsigned char)t[k - 1]) &&
                (k + 1 == t.size() || t[k + 1] == ' ')) {
                out += std::string("<sup>") + (c == '-' ? "−" : "+") + "</sup>";
                continue;
            }
            out += escape(c);
        }
        return out;
    }

    std::string parse_primary() {
        skip_ws();
        if (i_ >= s_.size()) return "";
        char c = s_[i_];
        if (c == '{') {
            return parse_arg();
        }
        if (c == '}') {  // stray
            i_++;
            return "";
        }
        if (c != '\\') {
            i_++;
            if (c == '-') return "−";
            if (c == '*') return "∗";
            if (c == '~') return " ";
            if (isalpha((unsigned char)c)) return std::string("<i>") + c + "</i>";
            return escape(c);
        }
        // command
        i_++;
        std::string name;
        if (i_ < s_.size() && !isalpha((unsigned char)s_[i_])) {
            name = std::string(1, s_[i_++]);
        } else {
            while (i_ < s_.size() && isalpha((unsigned char)s_[i_])) name += s_[i_++];
        }
        if (name == "frac" || name == "dfrac" || name == "tfrac") {
            std::string a = parse_arg(), b = parse_arg();
            return "<span class=\"mjx-frac\"><sup>" + a + "</sup>⁄<sub>" + b + "</sub></span>";
        }
        if (name == "sqrt") {
            skip_ws();
            std::string idx;
            if (i_ < s_.size() && s_[i_] == '[') {
                size_t e = s_.find(']', i_);
                if (e != std::string::npos) {
                    idx = s_.substr(i_ + 1, e - i_ - 1);
                    i_ = e + 1;
                }
            }
            std::string a = parse_arg();
            return (idx.empty() ? "" : "<sup>" + idx + "</sup>") + "√<span style=\"border-top:1px solid\">" + a + "</span>";
        }
        if (name == "text" || name == "textrm" || name == "mathrm" || name == "operatorname" || name == "mbox" ||
            name == "textit" || name == "mathit") {
            std::string t = raw_group();
            std::string out;
            for (char ch : t) out += escape(ch);
            return name == "textit" || name == "mathit" ? "<i>" + out + "</i>" : out;
        }
        if (name == "mathbf" || name == "textbf" || name == "boldsymbol" || name == "bf") return "<b>" + parse_arg() + "</b>";
        if (name == "underline") return "<u>" + parse_arg() + "</u>";
        if (name == "overline" || name == "bar") return "<span style=\"border-top:1px solid\">" + parse_arg() + "</span>";
        if (name == "hat") return parse_arg() + "̂";
        if (name == "vec") return parse_arg() + "⃗";
        if (name == "dot") return parse_arg() + "̇";
        if (name == "ce" || name == "pu") return chem(raw_group());
        if (name == "color" || name == "textcolor") {
            std::string col = raw_group();
            return "<span style=\"color:" + col + "\">" + parse_arg() + "</span>";
        }
        if (name == "begin" || name == "end") {
            raw_group();
            return "";
        }
        if (name == "\\") return "<br>";
        auto it = symbols().find(name);
        if (it != symbols().end()) return it->second;
        return "\\" + name;  // unknown: show the source
    }
};

}  // namespace

std::string tex_to_html(const std::string& tex) {
    // MathJax input in Anki fields is HTML-escaped (&lt; etc.) and may contain <br>.
    std::string t;
    for (size_t i = 0; i < tex.size(); i++) {
        if (tex.compare(i, 4, "&lt;") == 0) { t += '<'; i += 3; }
        else if (tex.compare(i, 4, "&gt;") == 0) { t += '>'; i += 3; }
        else if (tex.compare(i, 5, "&amp;") == 0) { t += '&'; i += 4; }
        else if (tex.compare(i, 6, "&nbsp;") == 0) { t += ' '; i += 5; }
        else if (tex[i] == '<') {
            size_t e = tex.find('>', i);
            if (e == std::string::npos) break;
            i = e;
        } else t += tex[i];
    }
    Parser p(t);
    return "<span class=\"mjx\">" + p.parse_all() + "</span>";
}

std::string render_mathjax_in_html(const std::string& html) {
    // Only text between tags is rewritten; attribute values (e.g. data-cloze)
    // must keep their TeX untouched.
    std::string out;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            size_t e = html.find('>', i);
            if (e == std::string::npos) {
                out += html.substr(i);
                break;
            }
            out += html.substr(i, e - i + 1);
            i = e + 1;
            continue;
        }
        // text run up to the next tag; MathJax may span tags (e.g. <br>), so
        // look for the closing delimiter anywhere after the opener
        size_t next_tag = html.find('<', i);
        size_t a = html.find("\\(", i), b = html.find("\\[", i);
        size_t start = std::min(a, b);
        if (start == std::string::npos || (next_tag != std::string::npos && start > next_tag)) {
            size_t stop = next_tag == std::string::npos ? html.size() : next_tag;
            out += html.substr(i, stop - i);
            i = stop;
            continue;
        }
        bool display = start == b;
        const char* close = display ? "\\]" : "\\)";
        size_t end = html.find(close, start + 2);
        if (end == std::string::npos) {
            out += html.substr(i);
            break;
        }
        out += html.substr(i, start - i);
        std::string inner = tex_to_html(html.substr(start + 2, end - start - 2));
        out += display ? "<div style=\"text-align:center\">" + inner + "</div>" : inner;
        i = end + 2;
    }
    return out;
}

}  // namespace anki
