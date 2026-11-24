// Runs a card template's JavaScript with QuickJS against a small DOM, so
// templates that build or toggle content in script (Malleus one-by-one clozes,
// hint buttons, tag banners) work without a browser engine.
#pragma once
#include <map>
#include <memory>
#include <string>

namespace anki::js {

class Dom;

class CardScript {
public:
    CardScript();
    ~CardScript();
    // Parses `body_html`, runs its inline <script>s, timers and Anki's
    // onShownHook, and returns the resulting HTML. `storage` carries
    // sessionStorage/localStorage between the question and answer sides.
    std::string run(const std::string& body_html, std::map<std::string, std::string>& storage);
    // Shows the answer the way Anki's reviewer does: the page (globals, event
    // listeners, timers) stays, its body is replaced and the new scripts run.
    std::string show_answer(const std::string& body_html);
    // Sends a key press (DOM `code`, e.g. "ArrowRight") to the page. Returns
    // true if the page changed; then html() has the new markup.
    bool key(const std::string& code);
    std::string html() const;
    // Evaluates an expression and returns it as a string (tests and debugging).
    std::string eval(const std::string& expr);
    const std::string& log() const { return log_; }
    bool active() const { return ctx_ != nullptr; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void* ctx_ = nullptr;
    std::string log_;
    std::map<std::string, std::string>* storage_ = nullptr;
    void sync_storage_out();
    void run_scripts(bool first);
};

}  // namespace anki::js
