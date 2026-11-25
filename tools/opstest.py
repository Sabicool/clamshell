#!/usr/bin/env python3
"""Checks the review-menu actions and sibling burying against Anki itself.

  tools/.venv/bin/python tools/opstest.py

Builds a small collection with Anki, copies it, then does the same things in
Anki (copy A) and with the 3DS code via anki3ds-cli (copy B): answering a card
with sibling burying on, bury card/note, suspend card/note (including the
suspended-and-buried edge cases), flags and mark. Card and note rows must match
apart from modification times.
"""
import os
import shutil
import subprocess
import sys
import tempfile

from anki.collection import Collection

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "build", "host", "anki3ds-cli")


def build(path):
    col = Collection(path)
    m = col.models.by_name("Basic (and reversed card)")
    nids = []
    for i in range(4):
        n = col.new_note(m)
        n["Front"], n["Back"] = f"front {i}", f"back {i}"
        if i == 3:
            n.tags = ["Marked", "other"]
        col.add_note(n, 1)
        nids.append(n.id)
    conf = col.decks.config_dict_for_deck_id(1)
    conf["new"]["bury"] = True
    conf["rev"]["bury"] = True
    col.decks.save(conf)
    cids = [col.get_note(n).card_ids() for n in nids]
    col.close()
    return nids, cids


def answer_good(col):
    card = col.get_card(CIDS[0][0])
    card.start_timer()
    col.sched.answerCard(card, 3)


def rows(path):
    col = Collection(path)
    cards = col.db.all("select id, nid, type, queue, due, ivl, flags, usn from cards order by id")
    notes = col.db.all("select id, tags, usn from notes order by id")
    tags = sorted(col.db.list("select tag from tags"))
    col.close()
    return cards, notes, tags


def main():
    with tempfile.TemporaryDirectory() as tmp:
        base = os.path.join(tmp, "base.anki2")
        nids, cids = build(base)
        global CIDS
        CIDS = cids
        a, b = os.path.join(tmp, "a.anki2"), os.path.join(tmp, "b.anki2")
        shutil.copy(base, a)
        shutil.copy(base, b)

        # what each step does, in Anki and on the 3DS
        steps = [
            ("answer note 0 card 0 (siblings get buried)", answer_good,
             ["replay", f"{cids[0][0]} 3 5000"]),
            ("bury card", lambda c: c.sched.bury_cards([cids[1][0]], manual=True), ["op", cids[1][0], "bury"]),
            ("suspend card", lambda c: c.sched.suspend_cards([cids[2][0]]), ["op", cids[2][0], "suspend"]),
            ("bury note with a suspended card", lambda c: c.sched.bury_notes([nids[2]]), ["op", cids[2][1], "burynote"]),
            ("suspend note with a buried card", lambda c: c.sched.suspend_cards(cids[1]), ["op", cids[1][1], "suspendnote"]),
            ("flag red", lambda c: c.set_user_flag_for_cards(1, [cids[3][0]]), ["op", cids[3][0], "flag1"]),
            ("flag blue", lambda c: c.set_user_flag_for_cards(4, [cids[3][0]]), ["op", cids[3][0], "flag4"]),
            ("unflag", lambda c: c.set_user_flag_for_cards(0, [cids[3][0]]), ["op", cids[3][0], "flag0"]),
            ("unmark (tag was 'Marked')", lambda c: c.tags.bulk_remove([nids[3]], "marked"), ["op", cids[3][0], "mark"]),
            ("mark", lambda c: c.tags.bulk_add([nids[0]], "marked"), ["op", cids[0][1], "mark"]),
        ]
        ok = True
        for name, anki_step, cli in steps:
            col = Collection(a)
            anki_step(col)
            col.close()
            if cli[0] == "replay":
                subprocess.run([CLI, "replay", b], input=cli[1] + "\n", text=True, check=True,
                               stdout=subprocess.DEVNULL)
            else:
                subprocess.run([CLI, "op", b, str(cli[1]), cli[2]], check=True)
            ra, rb = rows(a), rows(b)
            same = ra == rb
            ok &= same
            print(("ok   " if same else "DIFF ") + name)
            if not same:
                for label, x, y in zip(("cards", "notes", "tags"), ra, rb):
                    if x != y:
                        print(f"  {label}\n    anki: {x}\n    3ds:  {y}")
        print("PASS" if ok else "FAIL")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
