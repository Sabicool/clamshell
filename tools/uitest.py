#!/usr/bin/env python3
"""UI walkthrough in the desktop simulator against a local Anki sync server.

Signs in, downloads the collection, opens the deck list, studies a few cards,
syncs, and saves screenshots of every screen to build/ui-shots/.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from difftest import DEFAULT_COL, snapshot  # noqa: E402
from synctest import ENDPOINT, PORT, wait_port  # noqa: E402

from anki.collection import Collection  # noqa: E402

SIM = os.path.join(ROOT, "build", "host", "anki3ds-sim")
SHOTS = os.path.join(ROOT, "build", "ui-shots")

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
touch 160 129
shot 05b-extra-sections
touch 160 129
hold DOWN 30
shot 06-answer-scrolled
key A
shot 07-next-question
key A
key B
shot 08-after-again
key SELECT
shot 08b-after-undo
key START
shot 09-decks-after
key X
shot 10-after-sync
key SELECT
shot 11-settings
key B
"""


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_COL
    os.makedirs(SHOTS, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, SYNC_USER1="test:test", SYNC_BASE=os.path.join(tmp, "server"),
                   SYNC_HOST="127.0.0.1", SYNC_PORT=str(PORT), RUST_LOG="anki=warn")
        server = subprocess.Popen([sys.executable, "-m", "anki.syncserver"], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_port(PORT)
            desk_path = os.path.join(tmp, "desktop.anki2")
            snapshot(src, desk_path)
            desk = Collection(desk_path)
            # give the server the media for the Malleus deck's first few hundred notes
            src_media = os.path.join(os.path.dirname(src), "collection.media")
            if os.path.isdir(src_media):
                import re
                names = set()
                for (flds,) in desk.db.execute(
                        "select flds from notes where id in (select nid from cards where queue in (2,3) and due <= 3000) limit 400"):
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
            import time
            for _ in range(600):
                if not desk.media_sync_status().active:
                    break
                time.sleep(0.2)
            desk.close()
            data = os.path.join(tmp, "sd")
            os.makedirs(data)
            script = os.path.join(tmp, "script.txt")
            open(script, "w").write(SCRIPT)
            r = subprocess.run([SIM, data, script, SHOTS], capture_output=True, text=True)
            print(r.stderr[-3000:])
            print("screenshots in", SHOTS)
            sys.exit(r.returncode)
        finally:
            server.terminate()


if __name__ == "__main__":
    main()
