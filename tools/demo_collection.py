#!/usr/bin/env python3
"""Builds a small demo collection for screenshots, so none come from a real collection.

  tools/.venv/bin/python tools/demo_collection.py build/demo/collection.anki2

Uses Anki's stock note types, a few cards written for the demo, a generated
chart image, and a mix of new and due cards so the deck list has counts.
"""
import math
import os
import sys

from anki.collection import Collection
from PIL import Image, ImageDraw


def chart(path):
    """A simple sigmoid curve, standing in for a textbook figure."""
    w, h = 360, 220
    im = Image.new("RGB", (w, h), "white")
    d = ImageDraw.Draw(im)
    d.line([(40, 10), (40, h - 30), (w - 10, h - 30)], fill="black", width=2)
    pts = []
    for x in range(0, 300):
        p = x / 300 * 100  # 0-100 on the x axis
        y = 1 / (1 + math.exp(-(p - 27) / 6))
        pts.append((40 + x, h - 30 - y * (h - 50)))
    d.line(pts, fill=(200, 40, 40), width=3)
    d.line([(40 + 81, h - 30), (40 + 81, h - 30 - 0.5 * (h - 50))], fill=(120, 120, 120), width=1)
    d.text((45, 12), "saturation", fill="black")
    d.text((w - 70, h - 22), "pressure", fill="black")
    d.text((40 + 70, h - 22), "P50", fill=(90, 90, 90))
    im.save(path)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/demo/collection.anki2"
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    col = Collection(out)
    chart(os.path.join(col.media.dir(), "demo-curve.png"))

    def deck(name):
        return col.decks.id(name)

    basic = col.models.by_name("Basic")
    cloze = col.models.by_name("Cloze")

    def add(model, did, fields, tags=()):
        n = col.new_note(model)
        for k, v in fields.items():
            n[k] = v
        n.tags = list(tags)
        col.add_note(n, did)
        return n

    pharm = deck("Demo::Pharmacology")
    phys = deck("Demo::Physiology")
    chem = deck("Demo::Chemistry")
    lang = deck("Languages::Spanish")

    due = []
    n = add(cloze, pharm, {"Text": "Elimination half-life: {{c1::\\(t_{1/2} = \\dfrac{0.693 \\times V_d}{CL}\\)}}",
                           "Back Extra": "Doubling the volume of distribution doubles the half-life if clearance is unchanged."})
    due += n.card_ids()
    clin = deck("Demo::Clinical")
    n = add(cloze, clin, {"Text": "A drug with a {{c1::high}} hepatic extraction ratio has clearance limited mostly by "
                                   "{{c2::liver blood flow}}."})
    due += n.card_ids()
    n = add(basic, phys, {"Front": "What does a right shift of the oxyhaemoglobin curve mean?",
                          "Back": "Haemoglobin gives up oxygen more easily (higher P50).<br><br>"
                                  "<img src=\"demo-curve.png\">"})
    due += n.card_ids()
    n = add(basic, phys, {"Front": "Normal arterial blood gas values",
                          "Back": "<table style='margin:auto;border-collapse:collapse'>"
                                  "<tr><th style='border:1px solid #999;padding:2px 8px'>Value</th>"
                                  "<th style='border:1px solid #999;padding:2px 8px'>Range</th></tr>"
                                  "<tr><td style='border:1px solid #999;padding:2px 8px'>pH</td>"
                                  "<td style='border:1px solid #999;padding:2px 8px'>7.35–7.45</td></tr>"
                                  "<tr><td style='border:1px solid #999;padding:2px 8px'>\\(P_aCO_2\\)</td>"
                                  "<td style='border:1px solid #999;padding:2px 8px'>35–45 mmHg</td></tr>"
                                  "<tr><td style='border:1px solid #999;padding:2px 8px'>\\(HCO_3^-\\)</td>"
                                  "<td style='border:1px solid #999;padding:2px 8px'>22–26 mmol/L</td></tr></table>"})
    due += n.card_ids()
    add(cloze, chem, {"Text": "Water dissociates as {{c1::\\(\\ce{H2O <=> H+ + OH-}\\)}}."})
    add(basic, chem, {"Front": "Avogadro's number", "Back": "\\(6.022 \\times 10^{23}\\) per mole"})
    for word, meaning in [("el corazón", "the heart"), ("la sangre", "the blood"), ("el pulmón", "the lung"),
                          ("la vacuna", "the vaccine"), ("el riñón", "the kidney")]:
        add(basic, lang, {"Front": f"<div style='font-size:36px'>{word}</div>", "Back": meaning})

    # some cards due as reviews today, the rest stay new
    col.sched.set_due_date(due, "0")
    col.close()
    print("wrote", out)


if __name__ == "__main__":
    main()
