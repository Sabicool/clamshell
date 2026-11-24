#!/usr/bin/env python3
"""Differential test: anki-3ds scheduler vs the real Anki engine.

Uses the official `anki` Python package (same Rust core as desktop Anki) as the
reference. Works on COPIES of a collection; the original is never opened for
writing.

  tools/.venv/bin/python tools/difftest.py [path/to/collection.anki2] [--cards N] [--answers N]

Test 1 (states): for a sample of cards, compare the four next states (interval,
  steps, ease, lapses, FSRS stability/difficulty) with no load balancer.
Test 2 (replay): study a real deck in Anki (load balancer on, fuzz on),
  record each (card, rating), replay the same answers through anki3ds-cli on a
  fresh copy, then compare every touched card row and revlog entry.
"""
import argparse
import json
import os
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLI = os.environ.get("ANKI3DS_CLI") or os.path.join(ROOT, "build", "host", "anki3ds-cli")
DEFAULT_COL = os.path.expanduser("~/Library/Application Support/Anki2/Sabiqul/collection.anki2")

os.environ.pop("ANKI_TEST_MODE", None)  # would disable fuzz in the reference

from anki.collection import Collection  # noqa: E402
from anki.scheduler_pb2 import CardAnswer  # noqa: E402


def snapshot(src: str, dst: str) -> None:
    """Consistent copy even if Anki desktop has the collection open (WAL)."""
    try:
        s = sqlite3.connect(f"file:{src}?mode=ro", uri=True)
        d = sqlite3.connect(dst)
        s.backup(d)
        d.close()
        s.close()
    except sqlite3.Error:
        # read-only open of a WAL database can fail; copy the files instead
        shutil.copy(src, dst)
        if os.path.exists(src + "-wal"):
            shutil.copy(src + "-wal", dst + "-wal")
    c = sqlite3.connect(dst)
    c.execute("pragma journal_mode=delete")
    c.close()


def mem(m):
    return [m.stability, m.difficulty] if m.ByteSize() or m.stability else None


def py_state(st):
    which = st.WhichOneof("kind")
    if which == "filtered":
        f = st.filtered
        if f.WhichOneof("kind") == "preview":
            return {"kind": "preview", "scheduled_secs": f.preview.scheduled_secs, "finished": f.preview.finished}
        n = f.rescheduling.original_state
        filtered = True
    else:
        n = st.normal
        filtered = False
    k = n.WhichOneof("kind")
    out = {"filtered": filtered}
    if k == "new":
        out.update(kind="new", position=n.new.position)
    elif k == "learning":
        l = n.learning
        out.update(kind="learning", remaining_steps=l.remaining_steps, scheduled_secs=l.scheduled_secs,
                   memory=mem(l.memory_state) if l.HasField("memory_state") else None)
    elif k == "review":
        r = n.review
        out.update(kind="review", scheduled_days=r.scheduled_days, elapsed_days=r.elapsed_days,
                   ease=r.ease_factor, lapses=r.lapses, leeched=r.leeched,
                   memory=mem(r.memory_state) if r.HasField("memory_state") else None)
    elif k == "relearning":
        l, r = n.relearning.learning, n.relearning.review
        out.update(kind="relearning", remaining_steps=l.remaining_steps, scheduled_secs=l.scheduled_secs,
                   memory=mem(l.memory_state) if l.HasField("memory_state") else None,
                   scheduled_days=r.scheduled_days, ease=r.ease_factor, lapses=r.lapses, leeched=r.leeched)
    return out


def norm(d):
    # float32 values arrive as doubles on both sides; compare at f32 precision
    import struct

    def f(v):
        if isinstance(v, float):
            return struct.unpack("f", struct.pack("f", v))[0]
        if isinstance(v, list):
            return [f(x) for x in v]
        if isinstance(v, dict):
            return {k: f(x) for k, x in v.items() if k != "elapsed_secs"}
        return v

    return f(d)


def test_states(src, ncards, workdir):
    path = os.path.join(workdir, "states.anki2")
    snapshot(src, path)
    col = Collection(path)
    cids = [r[0] for r in col.db.all("select id from cards where queue >= 0")]
    random.seed(42)
    sample = cids if ncards <= 0 or ncards >= len(cids) else random.sample(cids, ncards)
    ref = {}
    skipped = 0
    for cid in sample:
        try:
            s = col._backend.get_scheduling_states(cid)
        except Exception:
            skipped += 1
            continue
        ref[cid] = {k: py_state(getattr(s, k)) for k in ("current", "again", "hard", "good", "easy")}
    col.close()
    out = subprocess.run([CLI, "states", path], input="\n".join(map(str, ref)), capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr)
        return False
    mismatches = 0
    shown = 0
    got = {}
    for line in out.stdout.splitlines():
        j = json.loads(line)
        got[j["cid"]] = {k: j[k] for k in ("current", "again", "hard", "good", "easy")}
    for cid, r in ref.items():
        g = got.get(cid)
        if g is None:
            mismatches += 1
            continue
        for k in ("again", "hard", "good", "easy"):
            a, b = norm(r[k]), norm(g[k])
            a.pop("elapsed_days", None) if a.get("kind") != "review" else None
            if a != b:
                mismatches += 1
                if shown < 8:
                    shown += 1
                    print(f"  MISMATCH card {cid} {k}:\n    anki : {a}\n    3ds  : {b}\n    current: {r['current']}")
                break
    print(f"[states] compared {len(ref)} cards ({skipped} skipped by Anki): {mismatches} mismatching")
    return mismatches == 0


def test_replay(src, nanswers, workdir):
    a_path = os.path.join(workdir, "replay_anki.anki2")
    b_path = os.path.join(workdir, "replay_3ds.anki2")
    snapshot(src, a_path)
    shutil.copy(a_path, b_path)
    col = Collection(a_path)
    # pick the deck with the most due cards
    best = None
    for node in col.sched.deck_due_tree().children:
        due = node.review_count + node.learn_count + node.new_count
        if best is None or due > best[1]:
            best = (node.deck_id, due, node.name)
    if not best or best[1] == 0:
        print("[replay] nothing due; skipped")
        return True
    col.decks.select(best[0])
    print(f"[replay] studying '{best[2]}' ({best[1]} due) in Anki")
    pattern = [3, 3, 1, 3, 4, 3, 2, 3, 3, 1, 4, 3]
    log = []
    for i in range(nanswers):
        q = col.sched.get_queued_cards()
        if not q.cards:
            break
        qc = q.cards[0]
        card = col.get_card(qc.card.id)
        card.start_timer()
        rating = pattern[i % len(pattern)]
        ans = col.sched.build_answer(card=card, states=qc.states, rating=rating - 1)  # enum is 0-based
        ans.milliseconds_taken = 4000 + 100 * (i % 7)
        col.sched.answer_card(ans)
        log.append((card.id, rating, ans.milliseconds_taken))
    col.close()
    inp = "\n".join(f"{c} {r} {m}" for c, r, m in log)
    out = subprocess.run([CLI, "replay", b_path], input=inp, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr)
        return False
    touched = sorted({c for c, _, _ in log})
    qs = ",".join(map(str, touched))
    cols = "id, did, type, queue, due, ivl, factor, reps, lapses, left, odue, odid, data"

    def rows(p):
        db = sqlite3.connect(p)
        cards = {r[0]: r for r in db.execute(f"select {cols} from cards where id in ({qs})")}
        rev = {}
        for r in db.execute(f"select cid, ease, ivl, lastIvl, factor, time, type from revlog where cid in ({qs}) order by id"):
            rev.setdefault(r[0], []).append(r[1:])
        db.close()
        return cards, rev

    ca, ra = rows(a_path)
    cb, rb = rows(b_path)
    bad = 0
    for cid in touched:
        a, b = list(ca[cid]), list(cb[cid])
        da, db_ = json.loads(a[-1] or "{}"), json.loads(b[-1] or "{}")
        lrt_a, lrt_b = da.pop("lrt", None), db_.pop("lrt", None)
        a[-1], b[-1] = da, db_
        if a[3] == 1 and lrt_a and lrt_b:  # intraday learning: due is a timestamp
            a[4] -= lrt_a
            b[4] -= lrt_b
        if a != b or ra.get(cid) != rb.get(cid):
            bad += 1
            if bad <= 8:
                print(f"  MISMATCH card {cid}:\n    anki : {a}\n    3ds  : {b}")
                if ra.get(cid) != rb.get(cid):
                    print(f"    revlog anki: {ra.get(cid)[-3:]}\n    revlog 3ds : {rb.get(cid)[-3:]}")
    print(f"[replay] {len(log)} answers over {len(touched)} cards: {bad} mismatching")
    return bad == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("collection", nargs="?", default=DEFAULT_COL)
    ap.add_argument("--cards", type=int, default=3000)
    ap.add_argument("--answers", type=int, default=150)
    args = ap.parse_args()
    if not os.path.exists(CLI):
        sys.exit("build the CLI first: make -f Makefile.host")
    with tempfile.TemporaryDirectory() as tmp:
        ok1 = test_states(args.collection, args.cards, tmp)
        ok2 = test_replay(args.collection, args.answers, tmp)
    print("PASS" if ok1 and ok2 else "FAIL")
    sys.exit(0 if ok1 and ok2 else 1)


if __name__ == "__main__":
    main()
