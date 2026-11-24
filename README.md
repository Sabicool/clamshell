# Anki for 3DS

An unofficial Anki client for the New Nintendo 3DS (homebrew). It reviews your real Anki collection with the same FSRS scheduling as desktop Anki, renders cards' HTML and CSS (including tables, images, image occlusion masks and simple MathJax), and syncs with an Anki sync server.

Status: first draft. Everything below "Verified" was tested on a Mac against the real Anki 26.08 engine; nothing has run on 3DS hardware yet.

## Verified on the Mac

- **Scheduling matches desktop Anki exactly.** `tools/difftest.py` compares this code with the official `anki` 26.08 Python package (the same Rust engine as desktop). On a copy of a real 17,000-card FSRS collection, all 9,335 active cards produce identical next states (intervals, stability, difficulty, steps), with fuzz on. Studying every due card in the Malleus deck (65 answers, load balancer on) produced identical card rows and review-log rows.
- **Sync round trip.** `tools/synctest.py` runs Anki's own sync server locally: desktop uploads, the 3DS client downloads, both sides study and edit, both sync. Afterwards all cards, notes and review-log rows are identical on both sides and the 3DS file passes SQLite's integrity check. Per-deck image download is checked byte for byte.
- **Deck counts** (new/learning/due) match desktop for all 432 decks in the test collection.
- **Card rendering**: `anki3ds-cli shot` renders cards to PNG with the same code the 3DS uses. Malleus cloze cards, tables, images and Image Occlusion Enhanced cards render correctly.
- **The app's screens and flows** run in a desktop simulator (`tools/uitest.py`): sign in, download, deck list, study, answer, sync, settings. Screenshots go to `build/ui-shots/`.
- The 3DS build (`make -C 3ds`) compiles and links with devkitARM r68.

## Important: AnkiWeb

AnkiWeb's [terms](https://ankiweb.net/account/terms) currently only allow Anki, AnkiMobile, AnkiDroid and AnkiUniversal to sync; other third-party clients aren't permitted. Exceptions are handled privately by the Anki team (see this [forum thread](https://forums.ankiweb.net/t/what-is-the-process-of-getting-new-clients-approved-for-ankiweb-sync/21350)). The protocol code works with AnkiWeb, but the app defaults to a self-hosted sync server, and the AnkiWeb button says it needs permission. It has never been pointed at AnkiWeb.

A self-hosted server is built into Anki and works with desktop Anki, AnkiDroid and this app ([manual](https://docs.ankiweb.net/sync-server.html)). NixOS also packages it as `services.anki-sync-server`.

## Trying it on your 3DS

You need a 3DS with the Homebrew Launcher (any custom firmware setup).

1. **Copy the app.** Put `3ds/anki3ds.3dsx` in `sdmc:/3ds/` on the SD card.
2. **Start a sync server on the Mac.** Run `tools/run-sync-server.sh` (first run asks you to choose a username and password). It prints a URL like `http://192.168.1.20:8080/`.
3. **Point desktop Anki at it.** In Anki: Preferences → Syncing → Self-hosted sync server → paste the URL. Sync, sign in with the username/password from step 2, and choose **Upload to server** when asked. (To go back to AnkiWeb later, clear that field and sync; Anki will ask which side to keep.)
4. **On the 3DS:** open Anki 3DS from the Homebrew Launcher → **Sign in to my sync server** → type the URL, username, password. It downloads your collection (a few minutes over 3DS Wi-Fi).
5. **Set the time zone** (Settings → Time zone) if the clock line on the deck list looks wrong. The 3DS clock has no time zone, and syncs are refused if the clocks disagree by more than 5 minutes.
6. Study a few cards, press X on the deck list to sync, then sync desktop Anki and check the reviews arrived (Browse → card info).

If something fails, `sdmc:/3ds/anki3ds/log.txt` records every error message shown.

Quicker look without a server: close desktop Anki, copy your `collection.anki2` to `sdmc:/3ds/anki3ds/collection.anki2` (and any images you want into `sdmc:/3ds/anki3ds/collection.media/`). The app opens it directly. Reviews made this way can't be synced back, so treat it as a demo copy.

## Controls

| Screen | Controls |
|---|---|
| Deck list | ↑↓ choose, ◀▶ collapse/expand, L/R page, **A** study, **X** sync, **Y** download images for the deck, **SELECT** settings, **START** quit |
| Question | **A** show answer, ↑↓ / circle pad / C-stick scroll, L/R page, **START** back to decks |
| Answer | **B** Again, **Y** Hard, **A** Good, **X** Easy (or tap the buttons) |
| Settings | ↑↓ choose, ◀▶ change, **A** select, **B** back |

## How it works

- `core/` is portable C++17: collection access (SQLite, schema 18), protobuf decoding, the scheduler, sync, templates and the renderer. It builds for the 3DS and for macOS/Linux.
- `core/states.cpp`, `core/fsrs.cpp`, `core/rng.cpp` are close ports of Anki 26.08's `rslib/src/scheduler/states`, the `fsrs` 6.6.1 crate's inference code, and the parts of Rust's `rand` 0.9 that Anki uses for fuzz and load balancing, so random choices are bit-identical.
- `core/sync.cpp` implements sync protocol v11 (zstd-compressed JSON over HTTP POST). It never does a full upload; if a full sync is needed it only offers to download.
- `core/render/` lays out card HTML with [litehtml](https://github.com/litehtml/litehtml), draws text with stb_truetype and the DejaVu fonts, decodes images with stb_image and SVG masks with nanosvg, and turns `\(...\)` MathJax into Unicode/HTML.
- `app/` is the UI, drawn in software into two canvases. `3ds/source/main.cpp` shows them on the screens; `host/sim.cpp` runs the same app on a Mac with scripted input.
- `3ds/source/http_3ds.cpp` does HTTP/HTTPS with mbedTLS and a bundled CA list, because the console's own TLS is too old for modern servers.

## Known limitations (first draft)

- **JavaScript doesn't run.** Card templates' scripts are ignored, so features like Malleus's one-by-one cloze reveal and tap-to-show hint buttons don't work; buttons and inputs are hidden. Embedding QuickJS with a small DOM bridge is the planned next step.
- **No audio, no typed answers, no undo** yet.
- **"Studied today" counts stay on the 3DS.** Daily new/review limits are tracked locally but aren't sent to the server, so desktop may offer more new cards that day than your limit.
- **The study order is a re-implementation** of Anki's queue builder. Counts match desktop exactly; the exact order in which cards appear may differ slightly. Each answer is still scheduled exactly as desktop would.
- **Cards that have never had FSRS memory states computed** (rare; usually suspended cards from before FSRS was turned on) show an error when answered. Opening the deck options on desktop and saving recomputes them.
- **Filtered decks** work for rescheduling decks; preview decks are untested.
- MathJax support is an approximation (symbols, Greek, sub/superscripts, fractions, roots, `\ce{}`); complex layouts show as plain text.
- On the 3DS, floating-point maths comes from newlib's libm rather than macOS's, so a 1-day difference in an interval is possible in rare edge cases (AnkiDroid and desktop have the same property).

## Building

Desktop tools and tests (macOS, needs Xcode command-line tools and curl):

```bash
make -f Makefile.host
```

```bash
tools/.venv/bin/python tools/difftest.py
```

```bash
tools/.venv/bin/python tools/synctest.py
```

```bash
tools/.venv/bin/python tools/uitest.py
```

(`python3 -m venv tools/.venv && tools/.venv/bin/pip install anki==26.08 pillow` creates the venv.)

3DS (needs devkitPro with devkitARM and libctru):

```bash
make -C 3ds
```

Render a card to PNG with the 3DS renderer:

```bash
build/host/anki3ds-cli shot path/to/collection.anki2 <card id> /tmp/card ~/Library/Application\ Support/Anki2/<profile>/collection.media
```

## Licences

This project ports logic from Anki (AGPL-3.0-or-later) and is therefore AGPL-3.0-or-later. Bundled third-party code: SQLite (public domain), zstd (BSD), mbedTLS (Apache-2.0), litehtml and gumbo (BSD/Apache-2.0), nlohmann/json (MIT), stb (public domain/MIT), nanosvg (zlib), DejaVu fonts (Bitstream Vera licence), Mozilla CA bundle (MPL-2.0).
