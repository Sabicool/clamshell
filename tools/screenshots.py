#!/usr/bin/env python3
"""Takes the README screenshots: the real 3DS build in Azahar, on the demo collection.

  tools/.venv/bin/python tools/screenshots.py <path to Azahar.app>

Builds tools/demo_collection.py's collection, runs tools/emutest.py with the
script below, and writes docs/screenshots/*.png (top screen above bottom screen).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "docs", "screenshots")

SCRIPT = """
shot setup
type {ENDPOINT}
type test
type test
key A
select Languages
key RIGHT
select Demo
key RIGHT
shot decks
select Pharmacology
key A
shot question
key A
shot answer-fraction
key A
key START
select Physiology
key A
key A
shot physio-1
touch 207 19
drag 160 120 210 60 10
shot zoom
key B
touch 280 19
shot menu
touch 230 166
drag 70 120 120 170 12
drag 120 170 250 90 20
drag 60 205 260 205 20
shot scratchpad
touch 280 19
touch 230 166
key A
key A
shot physio-2
touch 280 19
touch 160 214
shot buttons
key B
key START
key SELECT
key DOWN
key RIGHT
shot settings
key B
select Spanish
key A
key A
shot night
key START
key SELECT
key DOWN
key RIGHT
key B
"""

KEEP = ["decks", "question", "answer-fraction", "physio-1", "physio-2", "menu", "scratchpad", "zoom", "buttons",
        "settings", "night"]


def main():
    app = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        col = os.path.join(tmp, "demo", "collection.anki2")
        subprocess.run([sys.executable, os.path.join(HERE, "demo_collection.py"), col], check=True)
        script = os.path.join(tmp, "script.txt")
        open(script, "w").write(SCRIPT)
        shots = os.path.join(tmp, "shots")
        env = dict(os.environ, EMU_SCRIPT=script, EMU_SHOTS=shots)
        subprocess.run([sys.executable, os.path.join(HERE, "emutest.py"), app, col], env=env, check=True)
        os.makedirs(OUT, exist_ok=True)
        for name in KEEP:
            src = os.path.join(shots, f"shot-{name}.png")
            if os.path.exists(src):
                shutil.copy(src, os.path.join(OUT, f"{name}.png"))
            else:
                print("missing", name)
    # the two strips the README shows
    from PIL import Image

    def strip(names, out, scale=0.6, gap=16):
        ims = [Image.open(os.path.join(OUT, n + ".png")).convert("RGB") for n in names]
        ims = [i.resize((int(i.width * scale), int(i.height * scale)), Image.LANCZOS) for i in ims]
        w = sum(i.width for i in ims) + gap * (len(ims) - 1)
        o = Image.new("RGB", (w, max(i.height for i in ims)), (255, 255, 255))
        x = 0
        for i in ims:
            o.paste(i, (x, 0))
            x += i.width + gap
        o.save(os.path.join(OUT, out), optimize=True)

    strip(["decks", "answer-fraction", "physio-1", "night"], "overview.png")
    strip(["menu", "scratchpad", "zoom", "buttons"], "features.png")
    print("screenshots in", OUT)


if __name__ == "__main__":
    main()
