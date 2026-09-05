#!/usr/bin/env python3
"""Kenney (kenney.nl) external adapter.

Emits JSONL on stdout, one asset per line. See ../ADAPTER_PROTOCOL.md.

Kenney has no API, and per decisions.md D3 we deliberately do NOT scrape on
every crawl: the catalogue is ~100 packs and changes a few times a year, so a
hand-maintained list is more reliable than a scraper and costs zero requests.

    (default)    read data/kenney-packs.json, emit, zero network requests
    --check      HEAD every URL and report dead ones (maintenance)
    --refresh    scrape the listing and rewrite the JSON (maintenance)

--check and --refresh are for a human to run occasionally and commit the
result. The crawler only ever uses the default path.
"""

import argparse
import json
import os
import re
import sys
import urllib.parse
import tempfile
from network import fetch, FetchError

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data", "kenney-packs.json")

BASE = "https://kenney.nl"

# Politeness: the core's rate limiter cannot see requests made in here, so this
# adapter enforces its own. See ../../crawler.md.
REQUEST_INTERVAL = 2.0
def log(msg):
    print(msg, file=sys.stderr, flush=True)


class Response:
    def __init__(self, doc):
        self.status = doc["status"]
        self.body = doc["body"]

    def read(self):
        return self.body.encode("utf-8")


def polite_get(url, method="GET"):
    return Response(fetch(url, method))


def save_catalogue(doc):
    directory = os.path.dirname(DATA)
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".kenney-", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=2)
            fh.write("\n")
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, DATA)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


# --- type inference -------------------------------------------------------
# Kenney packs are named consistently enough that the name carries the type.

TYPE_RULES = [
    (("audio", "sounds", "sfx", "jingles", "voiceover", "music"), "audio"),
    (("font",), "font"),
    (("ui pack", "interface", "input prompts", "crosshair"), "ui"),
    (("particle",), "vfx"),
    (("kit", "3d", "blocky", "voxel", "characters", "prototype textures"), "3d_model"),
]

STYLE_RULES = [
    (("pixel", "1-bit", "micro", "tiny"), "pixel"),
    (("voxel", "blocky"), "voxel"),
    (("isometric",), "flat"),
]


def guess_type(name, explicit=None):
    if explicit:
        return explicit
    low = name.lower()
    for words, t in TYPE_RULES:
        if any(w in low for w in words):
            return t
    return "2d_sprite"


def guess_style(name, explicit=None):
    if explicit:
        return explicit
    low = name.lower()
    for words, s in STYLE_RULES:
        if any(w in low for w in words):
            return s
    return "unknown"


# --- default path: emit from the curated list -----------------------------

def load_packs():
    if not os.path.exists(DATA):
        log(f"missing {DATA} - run with --refresh to build it")
        return []
    with open(DATA, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    return doc.get("packs", [])


def emit():
    packs = load_packs()
    n = 0
    for p in packs:
        slug = p.get("slug")
        name = p.get("name") or slug
        if not slug:
            continue

        asset = {
            "id": f"kenney:{slug}",
            "source": "kenney",
            "title": name,
            "author": "Kenney",
            "source_url": f"{BASE}/assets/{slug}",
            "licence": "cc0",
            "licence_url": "https://creativecommons.org/publicdomain/zero/1.0/",
            "asset_type": guess_type(name, p.get("type")),
            "style": guess_style(name, p.get("style")),
            "tags": p.get("tags", []),
            "price": 0,
        }
        if p.get("thumb"):
            asset["thumb_url"] = p["thumb"]
        if p.get("formats"):
            asset["formats"] = p["formats"]

        print(json.dumps(asset), flush=True)
        n += 1

    log(f"emitted {n} packs from the curated list (0 network requests)")
    return 0


# --- maintenance: --check -------------------------------------------------

def check():
    packs = load_packs()
    dead, ok, uncertain = [], 0, 0
    log(f"checking {len(packs)} urls at {REQUEST_INTERVAL}s intervals...")

    for p in packs:
        url = f"{BASE}/assets/{p['slug']}"
        try:
            resp = polite_get(url, method="HEAD")
            if resp.status == 200:
                ok += 1
            elif resp.status in (404, 410):
                dead.append((p["slug"], resp.status))
            else:
                uncertain += 1
                log(f"  inconclusive HTTP {resp.status}: {url}")
        except FetchError as e:
            log(str(e))
            return 1
        except Exception as e:
            log(f"  inconclusive: {url}: {e}")
            return 1

    log(f"\n{ok} ok, {len(dead)} dead")
    for slug, why in dead:
        log(f"  DEAD {slug} ({why})")
    if dead:
        log("\nreview these confirmed missing pages before removing their curated entries")
    return 1 if dead or uncertain else 0


# --- maintenance: --refresh ----------------------------------------------

HREF = re.compile(r'href="/assets/([a-z0-9][a-z0-9\-]*)"')
TITLE = re.compile(r"<h[1-3][^>]*>\s*([^<]{2,80})\s*</h[1-3]>")


def refresh():
    """Scrape the listing pages and rewrite the JSON. Human reviews the diff."""
    found = {}
    page = 1

    while page <= 20:
        url = f"{BASE}/assets" if page == 1 else f"{BASE}/assets/page:{page}"
        try:
            html = polite_get(url).read().decode("utf-8", "replace")
        except FetchError as e:
            if e.code == 404:
                break
            log(f"  {url} -> HTTP {e.code}, leaving catalogue unchanged")
            return 1
        except Exception as e:                                  # noqa: BLE001
            log(f"  {url} -> {e}, leaving catalogue unchanged")
            return 1

        slugs = set(HREF.findall(html))
        if not slugs:
            log("unrecognized listing; leaving catalogue unchanged")
            return 1
        new = slugs - set(found)
        log(f"  page {page}: {len(slugs)} links, {len(new)} new")
        if not new:
            break
        for s in new:
            found[s] = {"slug": s, "name": s.replace("-", " ").title()}
        page += 1

    if page > 20:
        log("page ceiling reached; leaving catalogue unchanged")
        return 1
    if not found:
        log("found nothing - the page structure probably changed")
        return 1

    old_packs = {p["slug"]: p for p in load_packs()}
    old = set(old_packs)
    added = sorted(set(found) - old)
    gone = sorted(old - set(found))

    doc = {
        "_comment": "Curated Kenney pack list. See decisions.md D3. "
                    "Regenerate with: python kenney.py --refresh",
        "packs": [old_packs.get(s, found.get(s)) for s in sorted(old | set(found))],
    }
    save_catalogue(doc)

    log(f"\nwrote {len(found)} packs to {DATA}")
    if added:
        log(f"  added: {', '.join(added[:20])}{' ...' if len(added) > 20 else ''}")
    if gone:
        log(f"  not seen (retained for review):  {', '.join(gone[:20])}{' ...' if len(gone) > 20 else ''}")
    log("review the diff before committing - names come out slugified")
    return 0


# --- enrich mode ----------------------------------------------------------
#
# The curated list carries slug + name and nothing else, so every Kenney pack
# rendered with no preview image — and because they are CC0 and popular they
# rank high, so a whole screen of "no preview" was the first thing you saw.
#
# The pack pages themselves publish plenty: an og:image, real tags, a tile size
# and a category. kenney.nl serves no robots.txt at all (a clean 404, which
# RFC 9309 treats as allow-all), but it is one person's site, so this still
# paces itself.

RE_OG    = re.compile(r"<meta\s+property=['\"]og:image['\"]\s+content=['\"]([^'\"]+)")
RE_KTAG  = re.compile(r"href='[^']*?/assets/tag:([^']+)'")
RE_KSIZE = re.compile(r"href='[^']*?/assets/size:(\d+)'")
RE_KCAT  = re.compile(r"href='[^']*?/assets/category:([^']+)'")


def enrich(path):
    with open(path, encoding="utf-8") as fh:
        jobs = [ln.rstrip("\n").split("\t", 1) for ln in fh if "\t" in ln]

    log(f"enriching {len(jobs)} packs (~{len(jobs) * REQUEST_INTERVAL / 60:.1f} min)")
    done = failed = 0

    for asset_id, url in jobs:
        try:
            page = polite_get(url).read().decode("utf-8", "replace")
        except FetchError as e:
            log(str(e))
            if e.code in (404, 410):
                print(json.dumps({"id": asset_id, "retry": True}), flush=True)
                failed += 1
                continue
            return 1

        patch = {"id": asset_id}

        m = RE_OG.search(page)
        if m:
            patch["thumb_url"] = m.group(1)

        tags = [urllib.parse.unquote(t).strip().lower() for t in RE_KTAG.findall(page)]
        cat  = [urllib.parse.unquote(c).strip().lower() for c in RE_KCAT.findall(page)]
        # Tile size is the attribute you cannot mix and match, and Kenney is one
        # of the very few sources that states it outright.
        size = RE_KSIZE.search(page)
        if size:
            tags.append(f"{size.group(1)}x{size.group(1)}")
        tags += cat
        tags = [t for t in dict.fromkeys(tags) if t and "," not in t]
        if tags:
            patch["tags"] = tags

        if len(patch) == 1:
            log(f"  {url}: no recognized enrichment data")
            patch["retry"] = True
            failed += 1
        else:
            done += 1
        print(json.dumps(patch), flush=True)

    log(f"enriched {done}, failed {failed}")
    return 1 if failed else 0


# --- maintenance: --thumbs ------------------------------------------------
#
# Kenney's own listing pages reference pre-sized "-400x" variants. The pack
# page only offers full-size art (19-192 KB each, ~5 MB for one screen of
# results), and the -400x suffix cannot be derived reliably: it 404s for some
# files and is *larger* than the original for others. So take the URLs Kenney
# already publishes rather than constructing them.
#
# One or two requests for the whole catalogue, merged into the curated JSON so
# the crawl itself stays offline.

# The tile image is a CSS background on a .cover div, not an <img>, and the
# media URL carries the slug itself:
#   /media/pages/assets/<slug>/<hash>/preview-400x.png
# Parse the slug out of the URL rather than pairing it with a nearby <a>.
# Positional pairing silently mismatched — "Fish Pack" ended up showing a city —
# and nothing in the data looked wrong; it was only visible on screen.
RE_LIST_THUMB = re.compile(
    r"https://kenney\.nl/media/pages/assets/"
    r"([a-z0-9][a-z0-9\-]*)/[^'\")\s]+?-\d+x\.(?:png|jpg|webp)")


def thumbs():
    packs = {p["slug"]: p for p in load_packs()}
    found, page = {}, 1

    while page <= 20:
        url = f"{BASE}/assets" if page == 1 else f"{BASE}/assets/page:{page}"
        try:
            html = polite_get(url).read().decode("utf-8", "replace")
        except FetchError as e:
            if e.code != 404:
                log(f"  {url} -> HTTP {e.code}")
                return 1
            break
        except Exception as e:                                   # noqa: BLE001
            log(f"  {url} -> {e}")
            return 1

        hits = {m.group(1): m.group(0) for m in RE_LIST_THUMB.finditer(html)}
        new  = {s: i for s, i in hits.items() if s not in found}
        log(f"  page {page}: {len(hits)} thumbnails, {len(new)} new")
        if not new:
            break
        found.update(new)
        page += 1

    if page > 20 or not found:
        log("incomplete or unrecognized thumbnail listing")
        return 1
    matched = 0
    for slug, img in found.items():
        if slug in packs:
            packs[slug]["thumb"] = img
            matched += 1

    doc = {
        "_comment": "Curated Kenney pack list. See decisions.md D3.",
        "_validated": "thumbnails from kenney.nl listing (-400x variants)",
        "packs": [packs[s] for s in sorted(packs)],
    }
    save_catalogue(doc)

    missing = [s for s in packs if "thumb" not in packs[s]]
    log(f"\nmatched {matched} of {len(packs)} packs")
    if missing:
        log(f"no thumbnail for {len(missing)}: {', '.join(missing[:12])}"
            f"{' ...' if len(missing) > 12 else ''}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--thumbs", action="store_true",
                    help="scrape listing pages for -400x thumbnails, merge into json")
    ap.add_argument("--check", action="store_true",
                    help="HEAD every url, report dead ones")
    ap.add_argument("--refresh", action="store_true",
                    help="scrape the listing and rewrite the json")
    ap.add_argument("--enrich", metavar="FILE",
                    help="read id<TAB>url lines, emit thumbnail/tag patches")
    args = ap.parse_args()

    if args.thumbs:
        return thumbs()
    if args.check:
        return check()
    if args.refresh:
        return refresh()
    if args.enrich:
        return enrich(args.enrich)
    return emit()


if __name__ == "__main__":
    sys.exit(main())
