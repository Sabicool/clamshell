#!/usr/bin/env python3
"""Runs the real 3DS build inside the Azahar emulator against a local sync server.

  tools/.venv/bin/python tools/emutest.py <path to Azahar.app> [collection]

A copy of the collection is uploaded to a throwaway local Anki sync server; the
self-test build (make -C 3ds selftest) signs in, downloads, studies, syncs and
saves screenshots read back from the emulated framebuffer to build/emu-shots/.
Finally "desktop" syncs and checks the reviews made on the 3DS arrived.
The emulator must be in portable mode (a `user` folder next to Azahar.app).
"""
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from difftest import DEFAULT_COL, snapshot  # noqa: E402
from synctest import PORT, wait_port  # noqa: E402

from anki.collection import Collection  # noqa: E402

# EMU_SHOTS and EMU_SCRIPT (a file of self-test commands) override the defaults;
# tools/screenshots.py uses them for the README pictures.
SHOTS = os.environ.get("EMU_SHOTS") or os.path.join(ROOT, "build", "emu-shots")
ENDPOINT = f"http://127.0.0.1:{PORT}/"

SCRIPT = f"""
shot 01-setup
type {ENDPOINT}
type test
type test
key A
shot 02-decks
select Malleus Clinical Medicine
shot 03-deck-selected
key A
shot 04-question
key A
shot 05-answer
touch 207 19
shot 05z-zoom
drag 200 200 120 80
shot 05z-zoom-moved
key B
key ZR
shot 05b-extra
key A
shot 06-next-question
key A
key B
shot 07-after-again
touch 280 19
shot 07b-menu
touch 230 166
drag 60 120 260 200
shot 07c-scratchpad
touch 280 19
touch 230 166
key A
key A
key A
key Y
key START
shot 08-decks-after
key X
shot 09-after-sync
key SELECT
shot 10-settings
key B
"""


def main():
    app = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_COL
    exe = os.path.join(app, "Contents", "MacOS", "azahar")
    sd = os.path.join(os.path.dirname(app), "user", "sdmc", "3ds", "clamshell")
    shutil.rmtree(sd, ignore_errors=True)
    os.makedirs(sd)
    os.makedirs(SHOTS, exist_ok=True)
    for f in os.listdir(SHOTS):
        os.remove(os.path.join(SHOTS, f))
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, SYNC_USER1="test:test", SYNC_BASE=os.path.join(tmp, "server"),
                   SYNC_HOST="127.0.0.1", SYNC_PORT=str(PORT), RUST_LOG="anki=warn")
        server = subprocess.Popen([sys.executable, "-m", "anki.syncserver"], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        emu = None
        try:
            wait_port(PORT)
            desk_path = os.path.join(tmp, "desktop.anki2")
            snapshot(src, desk_path)
            desk = Collection(desk_path)
            src_media = os.path.join(os.path.dirname(src), "collection.media")
            if os.path.isdir(src_media):
                names = set()
                # images for everything due today, plus some new cards, as a real server would have
                today = desk.sched.today
                for (flds,) in desk.db.execute(
                        "select flds from notes where id in (select nid from cards where queue in (1,3)"
                        f" or (queue = 2 and due <= {today}) or (queue = 0 and due < 2000))"):
                    names.update(re.findall(r'src="([^"/]+)"', flds))
                for n in names:
                    p = os.path.join(src_media, n)
                    if os.path.exists(p):
                        shutil.copy(p, os.path.join(desk.media.dir(), n))
            auth = desk.sync_login("test", "test", ENDPOINT)
            desk.close_for_full_sync()
            desk.full_upload_or_download(auth=auth, server_usn=None, upload=True)
            desk.reopen(after_full_sync=True)
            desk.sync_media(auth)
            for _ in range(600):
                if not desk.media_sync_status().active:
                    break
                time.sleep(0.2)
            revlog_before = desk.db.scalar("select count() from revlog")
            print("server ready; desktop review log rows:", revlog_before)

            script = SCRIPT
            if os.environ.get("EMU_SCRIPT"):
                script = open(os.environ["EMU_SCRIPT"]).read().replace("{ENDPOINT}", ENDPOINT)
            open(os.path.join(sd, "selftest.txt"), "w").write(script)
            # Azahar fails to load paths containing spaces, so copy the build
            selftest = os.path.join(tmp, "clamshell-selftest.3dsx")
            shutil.copy(os.path.join(ROOT, "3ds", "clamshell-selftest.3dsx"), selftest)
            # launch through LaunchServices like a normal app (a bare child process stalls)
            subprocess.run(["open", "-n", app, "--args", selftest], check=True)
            emu = None
            log = os.path.join(sd, "selftest.log")
            start = time.time()
            last = ""
            while time.time() - start < 1500:
                if os.path.exists(log):
                    txt = open(log).read()
                    if txt != last:
                        new = txt[len(last):]
                        for line in new.splitlines():
                            print(f"  [{time.time() - start:5.0f}s] {line}")
                        last = txt
                    if "done" in txt.splitlines():
                        break
                time.sleep(2)
            time.sleep(2)
            subprocess.run(["pkill", "-f", "Azahar.app/Contents/MacOS/azahar"])
            for f in os.listdir(sd):
                if f.startswith("shot-") or f in ("selftest.log", "log.txt", "settings.json", "timing.txt"):
                    shutil.copy(os.path.join(sd, f), os.path.join(SHOTS, f))
            # did the 3DS reviews reach the server?
            desk.sync_collection(auth, False)
            revlog_after = desk.db.scalar("select count() from revlog")
            desk.close()
            print("desktop review log rows after syncing:", revlog_after, f"(+{revlog_after - revlog_before})")
            if os.path.exists(os.path.join(sd, "log.txt")):
                print("app error log:\n" + open(os.path.join(sd, "log.txt")).read())
            print("screenshots in", SHOTS)
        finally:
            subprocess.run(["pkill", "-f", "Azahar.app/Contents/MacOS/azahar"])
            server.terminate()


if __name__ == "__main__":
    main()
