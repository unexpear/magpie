#!/usr/bin/env python3
"""game-icons.net adapter.

Emits JSONL on stdout, one asset per line. See ../ADAPTER_PROTOCOL.md.

The whole catalogue — 4,239 icons — arrives in **one** GitHub tree request, so
this is the cheapest source in the index by a wide margin: OpenGameArt costs
10 seconds per 144 assets, this costs one request for all of them.

Two properties make it unusually good data:

1. **Author is in the path.** Files live at `<contributor>/<icon>.svg`, so every
   asset has a creator without fetching anything extra. That is precisely what
   the OpenGameArt rows lack, and what makes a CC-BY credits line usable.
2. **Licence is knowable per asset.** license.txt puts everything under
   CC-BY 3.0 except two contributors explicitly marked CC0. That mapping is
   transcribed below rather than guessed.

Icons are white-on-black SVGs served with a correct image/svg+xml content type
straight from raw.githubusercontent, so no CDN or rasterising is involved.
"""

import json
import re
import sys
from network import get_text

REPO = "game-icons/icons"
TREE = f"https://api.github.com/repos/{REPO}/git/trees/master?recursive=1"
# Thumbnails come from jsDelivr, not raw.githubusercontent. A single raw URL
# loads fine, but a grid asks for ~50 at once and GitHub throttles that — every
# icon rendered blank, with no error event to notice it by. GitHub also asks
# people not to use raw as a CDN; jsDelivr is built for exactly this and takes
# the load off them.
RAW  = f"https://cdn.jsdelivr.net/gh/{REPO}@master/"
SITE = "https://game-icons.net/1x1/"

CC_BY = "https://creativecommons.org/licenses/by/3.0/"
CC_0  = "https://creativecommons.org/publicdomain/zero/1.0/"

# From license.txt: "Icons provided under the Creative Commons 3.0 BY or CC0 if
# mentioned below." Only these two contributors are marked CC0.
CC0_AUTHORS = {"viscious-speed", "zeromancer"}

# Directory name -> the name license.txt credits, since attribution must read
# "Icons made by {author}". Anything unlisted falls back to a tidied slug.
AUTHORS = {
    "lorc": "Lorc", "delapouite": "Delapouite", "john-colburn": "John Colburn",
    "felbrigg": "Felbrigg", "john-redman": "John Redman",
    "carl-olsen": "Carl Olsen", "sbed": "Sbed", "priorblue": "PriorBlue",
    "willdabeast": "Willdabeast", "viscious-speed": "Viscious Speed",
    "lord-berandas": "Lord Berandas", "irongamer": "Irongamer",
    "heavenly-dog": "HeavenlyDog", "lucasms": "Lucas",
    "faithtoken": "Faithtoken", "skoll": "Skoll",
    "andymeneely": "Andy Meneely", "cathelineau": "Cathelineau",
    "kier-heyl": "Kier Heyl", "aussiesim": "Aussiesim", "sparker": "Sparker",
    "zeromancer": "Zeromancer", "rihlsul": "Rihlsul", "quoting": "Quoting",
    "guard13007": "Guard13007", "darkzaitzev": "DarkZaitzev",
    "spencerdub": "SpencerDub", "generalace135": "GeneralAce135",
    "zajkonur": "Zajkonur", "catsu": "Catsu", "starseeker": "Starseeker",
    "pepijn-poolman": "Pepijn Poolman", "pierre-leducq": "Pierre Leducq",
    "caro-asercion": "Caro Asercion", "seregacthtuf": "SeregaCthtuf",
}

STOP = {"of", "the", "and", "a", "with", "in", "on"}

# A real folder whose icons have no single creator. CC-BY needs a name, and
# "Various Artists" is not one — emit no author so the row is flagged as
# needing a credit filled in by hand rather than shipping a bogus one.
UNATTRIBUTED = {"various-artists"}


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def titleise(slug):
    words = [w for w in re.split(r"[-_]+", slug) if w]
    return " ".join(w if w.isupper() else w.capitalize() for w in words)


def main():
    doc = json.loads(get_text(TREE))
    if not isinstance(doc.get("tree"), list) or not doc["tree"]:
        log("invalid/empty GitHub tree response")
        return 1

    if doc.get("truncated"):
        # Never pretend a partial listing is the whole catalogue.
        log("WARNING: GitHub truncated the tree; this run is incomplete")

    paths = [t["path"] for t in doc.get("tree", [])
             if t.get("type") == "blob" and t["path"].endswith(".svg")
             and "/" in t["path"]]

    n = 0
    unknown = set()
    for p in paths:
        folder, fname = p.split("/", 1)
        if folder in ("badges",):
            continue
        slug = fname[:-4]                      # strip .svg
        if folder not in AUTHORS and folder not in UNATTRIBUTED:
            unknown.add(folder)

        cc0    = folder in CC0_AUTHORS
        title  = titleise(slug)
        tags   = [w.lower() for w in re.split(r"[-_]+", slug)
                  if w and w.lower() not in STOP]

        asset = {
            "id": f"gameicons:{folder}/{slug}",
            "source": "gameicons",
            "title": title,
            "source_url": f"{SITE}{folder}/{slug}.html",
            "thumb_url": RAW + p,
            "licence": "cc0" if cc0 else "cc_by",
            "licence_url": CC_0 if cc0 else CC_BY,
            # Interface icons, not sprite sheets - `ui` is the honest type even
            # though they are 2D art. They carry an `icon` tag either way.
            "asset_type": "ui",
            "style": "flat",
            "formats": [".svg"],
            "tags": tags + ["icon"],
            "price": 0,
        }
        if folder not in UNATTRIBUTED:
            asset["author"] = AUTHORS.get(folder)

        print(json.dumps(asset), flush=True)
        n += 1

    log(f"emitted {n} icons in 1 request")
    if unknown:
        log(f"contributors missing from the author map: {', '.join(sorted(unknown))}")
        log("their author remains unknown - verify their credit and add them to AUTHORS")
    return 1 if doc.get("truncated") else 0


if __name__ == "__main__":
    sys.exit(main())
