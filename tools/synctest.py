#!/usr/bin/env python3
"""End-to-end sync test: anki3ds-cli against Anki's own sync server.

Everything runs on copies in a temp folder, against a local server; AnkiWeb and
your real collection are never touched.

  1. start `anki.syncserver` locally with a throwaway test account
  2. "desktop" (the anki Python package) full-uploads a copy of your collection
  3. 3DS client logs in and full-downloads it
  4. 3DS reviews cards; desktop reviews *other* cards and edits a note
  5. desktop syncs, 3DS syncs, desktop syncs again
  6. both sides must agree on every card, note and review-log row
"""
import os
import random
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLI = os.environ.get("ANKI3DS_CLI") or os.path.join(ROOT, "build", "host", "anki3ds-cli")
sys.path.insert(0, HERE)
from difftest import DEFAULT_COL, snapshot  # noqa: E402

from anki.collection import Collection  # noqa: E402
from anki.sync_pb2 import SyncAuth  # noqa: E402

PORT = 27701
ENDPOINT = f"http://127.0.0.1:{PORT}/"


def wait_port(port, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def cli(*args, stdin=None):
    out = subprocess.run([CLI, *args], input=stdin, capture_output=True, text=True)
    print(f"  $ anki3ds-cli {args[0]} -> {out.stdout.strip()} {out.stderr.strip()}")
    if out.returncode != 0:
        raise SystemExit(f"anki3ds-cli {args[0]} failed")
    return out.stdout


def desktop_sync(col, auth):
    out = col.sync_collection(auth, False)
    req = out.required
    if req not in (out.NO_CHANGES, out.NORMAL_SYNC):
        raise SystemExit(f"desktop sync wanted a full sync: {out}")
    return out


def unicase(a, b):
    a, b = a.casefold(), b.casefold()
    return (a > b) - (a < b)


def connect(path):
    db = sqlite3.connect(path)
    db.create_collation("unicase", unicase)
    return db


def deck_jsons(col):
    return {d["id"]: d for d in col.decks.all()}


def table_rows(path):
    db = connect(path)
    cards = sorted(db.execute("select id, nid, did, ord, type, queue, due, ivl, factor, reps, lapses, left, odue, odid, flags, data from cards"))
    notes = sorted(db.execute("select id, guid, mid, tags, flds from notes"))
    revlog = sorted(db.execute("select id, cid, ease, ivl, lastIvl, factor, time, type from revlog"))
    db.close()
    return cards, notes, revlog


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_COL
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, SYNC_USER1="test:test", SYNC_BASE=os.path.join(tmp, "server"),
                   SYNC_HOST="127.0.0.1", SYNC_PORT=str(PORT), RUST_LOG="anki=warn")
        server = subprocess.Popen([sys.executable, "-m", "anki.syncserver"], env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            if not wait_port(PORT):
                raise SystemExit("sync server did not start")
            print("1. local sync server up")
            desk_path = os.path.join(tmp, "desktop.anki2")
            snapshot(src, desk_path)
            desk = Collection(desk_path)
            decks_before = deck_jsons(desk)
            auth = desk.sync_login("test", "test", ENDPOINT)
            desk.close_for_full_sync()
            desk.full_upload_or_download(auth=auth, server_usn=None, upload=True)
            desk.reopen(after_full_sync=True)
            print("2. desktop full-uploaded")

            hkey = cli("login", ENDPOINT, "test", "test").strip()
            ds_path = os.path.join(tmp, "3ds.anki2")
            cli("sync", ds_path, ENDPOINT, hkey, "--full-download")
            print("3. 3DS full-downloaded")

            # 3DS reviews some due cards
            db = sqlite3.connect(ds_path)
            due = [r[0] for r in db.execute(
                "select id from cards where queue = 2 and due <= (select 3000) order by due limit 40")]
            db.close()
            random.seed(1)
            ds_cards = due[:20]
            desk_cards = due[20:40]
            os.environ["ANKI3DS_DECK_STATS"] = "1"
            cli("replay", ds_path, stdin="\n".join(f"{c} {random.choice([1,3,3,3,4])} 5000" for c in ds_cards))
            # desktop reviews different cards and edits a note
            time.sleep(1.5)  # keep review-log ids (millisecond timestamps) distinct
            # desktop studies from its own queue (may overlap the 3DS cards: then
            # the newer card wins and both review-log rows are kept)
            desk_cards = []
            for _ in range(20):
                q = desk.sched.get_queued_cards()
                if not q.cards:
                    break
                qc = q.cards[0]
                card = desk.get_card(qc.card.id)
                card.start_timer()
                desk.sched.answer_card(desk.sched.build_answer(card=card, states=qc.states, rating=2))
                desk_cards.append(card.id)
            note = desk.get_note(desk.get_card(desk_cards[0]).nid)
            note.fields[0] = note.fields[0] + " (edited on desktop)"
            note.tags.append("synctest")
            desk.update_note(note)
            print(f"4. 3DS answered {len(ds_cards)} cards; desktop answered {len(desk_cards)} and edited a note")

            desktop_sync(desk, auth)
            cli("sync", ds_path, ENDPOINT, hkey)
            desktop_sync(desk, auth)
            cli("sync", ds_path, ENDPOINT, hkey)
            print("5. desktop -> 3DS -> desktop -> 3DS synced")

            # media: give desktop a handful of real files used by one deck, sync
            # them to the server, then have the 3DS fetch just that deck's media
            src_media = os.path.join(os.path.dirname(src), "collection.media")
            media_ok = True
            if os.path.isdir(src_media):
                db = sqlite3.connect(ds_path)
                did = db.execute("select did from cards c join notes n on n.id = c.nid where n.flds like '%<img%' "
                                 "group by did having count() < 60 order by count() desc limit 1").fetchone()[0]
                flds = [r[0] for r in db.execute(f"select flds from notes where id in (select nid from cards where did = {did})")]
                db.close()
                import re as _re
                names = sorted({m for f in flds for m in _re.findall(r'src="([^"/]+)"', f)})
                names = [n for n in names if os.path.exists(os.path.join(src_media, n))][:30]
                for n in names:
                    shutil.copy(os.path.join(src_media, n), os.path.join(desk.media.dir(), n))
                desk.sync_media(auth)
                for _ in range(300):
                    if not desk.media_sync_status().active:
                        break
                    time.sleep(0.2)
                ds_media = os.path.join(tmp, "3ds-media")
                os.makedirs(ds_media, exist_ok=True)
                cli("media", ds_path, ENDPOINT, hkey, str(did), ds_media)
                got = sorted(os.listdir(ds_media))
                same = all(open(os.path.join(ds_media, n), "rb").read() == open(os.path.join(src_media, n), "rb").read()
                           for n in got)
                media_ok = set(names) <= set(got) and same
                print(f"6. media: desktop uploaded {len(names)} files; 3DS downloaded {len(got)}; identical={same}")
            # decks: everything except today's counters must be unchanged, and
            # both sides must agree on the counters
            decks_after = deck_jsons(desk)
            COUNTERS = {"lrnToday", "revToday", "newToday", "timeToday", "mod", "usn"}
            deck_ok = True
            for did, before in decks_before.items():
                after = decks_after.get(did)
                if after is None:
                    deck_ok = False
                    print(f"  deck {did} missing after sync")
                    continue
                diff = {k for k in set(before) | set(after) if k not in COUNTERS and before.get(k) != after.get(k)}
                if diff:
                    deck_ok = False
                    print(f"  deck {before['name']}: unexpected changes in {sorted(diff)}")
            ds_col = sqlite3.connect(ds_path)
            studied = {r[0] for r in ds_col.execute(f"select did from cards where id in ({','.join(map(str, ds_cards))})")}
            ds_col.close()
            desk.close()
            # compare every deck as desktop sees it with the 3DS copy
            ds_view = Collection(ds_path)
            ds_decks = deck_jsons(ds_view)
            ds_view.close()
            mismatch = [d for d in decks_after if decks_after[d] != ds_decks.get(d)]
            changed = [decks_after[d]["name"] for d in decks_after
                       if decks_after[d]["revToday"] != decks_before[d]["revToday"] or decks_after[d]["newToday"] != decks_before[d]["newToday"]]
            for d in mismatch[:3]:
                a_, b_ = decks_after[d], ds_decks.get(d) or {}
                print(f"  deck {a_['name']}: " + "; ".join(f"{k}: desktop={a_.get(k)!r} 3ds={b_.get(k)!r}" for k in set(a_) | set(b_) if a_.get(k) != b_.get(k)))
            deck_ok = deck_ok and not mismatch
            print(f"  decks: {'only today counters changed' if deck_ok else 'PROBLEM'}; {len(mismatch)} differ between desktop and 3DS; "
                  f"counters updated in: {changed[:6]}")
            a = table_rows(desk_path)
            b = table_rows(ds_path)
            ok = True
            for name, x, y in zip(("cards", "notes", "revlog"), a, b):
                if x != y:
                    ok = False
                    dx, dy = {r[0]: r for r in x}, {r[0]: r for r in y}
                    diff = [k for k in set(dx) | set(dy) if dx.get(k) != dy.get(k)]
                    print(f"  {name}: {len(diff)} rows differ")
                    for k in diff[:5]:
                        a_, b_ = dx.get(k), dy.get(k)
                        if a_ and b_:
                            cols = [i for i in range(len(a_)) if a_[i] != b_[i]]
                            print(f"    id {k}: " + "; ".join(f"col{i} desktop={a_[i]!r} 3ds={b_[i]!r}" for i in cols))
                        else:
                            print(f"    id {k}: desktop={a_} 3ds={b_}")
                else:
                    print(f"  {name}: identical ({len(x)} rows)")
            # the 3DS reviews must have reached desktop
            dbd = sqlite3.connect(desk_path)
            n = dbd.execute(f"select count() from revlog where cid in ({','.join(map(str, ds_cards))})").fetchone()[0]
            dbd.close()
            print(f"  desktop has {n} review-log rows for the 3DS-reviewed cards")
            # integrity check of the 3DS file
            chk = connect(ds_path).execute("pragma integrity_check").fetchone()[0]
            print(f"  3DS collection integrity: {chk}")
            ok = ok and chk == "ok" and media_ok and deck_ok
            print("PASS" if ok else "FAIL")
            sys.exit(0 if ok else 1)
        finally:
            server.terminate()
            try:
                out = server.communicate(timeout=5)[0].decode(errors="replace")
                if "error" in out.lower():
                    print("server log:\n" + out[-3000:])
            except Exception:
                pass


if __name__ == "__main__":
    main()
