#!/usr/bin/env python3
"""OpenGameArt.org external adapter.

Emits JSONL on stdout, one asset per line. See ../ADAPTER_PROTOCOL.md.

OpenGameArt is Drupal 7 with no API, so this scrapes the advanced-search
listing. It is the reason the adapter boundary exists (decisions.md D2): the
parsing here is ~15 lines of Python and would be ~200 of lexbor in C.

Two things shape the whole design:

1. robots.txt says `Crawl-delay: 10`. Ten seconds per request is the polite
   floor, so requests are the scarce resource and every one has to earn itself.
   A full sweep of the site is ~113 pages ~= 19 minutes. We read the delay from
   robots.txt through the core fetcher rather than trusting a hardcoded policy.

2. The listing does NOT show a licence, and fetching each of 16,000 node pages
   to find one would take 45 hours. But licence is a *search filter*, so we
   crawl one licence at a time and every result in the set is known to carry it.
   Same trick for asset type. Zero node fetches.

Ordering matters: assets can be dual-licensed, and the core upserts by id, so
the LAST write wins. We therefore crawl least-permissive first and CC0 last, so
a dual-licensed asset ends up recorded as the most permissive option - which is
the one a user may actually rely on.

Author is not in the listing markup, so it is left null rather than guessed.
"""

import argparse
import html as htmllib
import json
import re
import sys
import urllib.parse
from network import get_text, FetchError

BASE = "https://opengameart.org"
SEARCH = BASE + "/art-search-advanced"

PER_PAGE = 144          # the maximum the form offers; 24 is the default
DEFAULT_DELAY = 10.0    # fallback if robots.txt cannot be read

# term id -> our licence enum. Ordered least permissive first: see docstring.
LICENCES = [
    (5,     "gpl",      "GPL 2.0",      "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html"),
    (6,     "gpl",      "GPL 3.0",      "https://www.gnu.org/licenses/gpl-3.0.html"),
    (3,     "cc_by_sa", "CC-BY-SA 3.0", "https://creativecommons.org/licenses/by-sa/3.0/"),
    (17982, "cc_by_sa", "CC-BY-SA 4.0", "https://creativecommons.org/licenses/by-sa/4.0/"),
    (10310, "oga_by",   "OGA-BY 3.0",   BASE + "/content/oga-by-30-faq"),
    (31772, "oga_by",   "OGA-BY 4.0",   BASE + "/content/oga-by-30-faq"),
    (2,     "cc_by",    "CC-BY 3.0",    "https://creativecommons.org/licenses/by/3.0/"),
    (17981, "cc_by",    "CC-BY 4.0",    "https://creativecommons.org/licenses/by/4.0/"),
    (4,     "cc0",      "CC0",          "https://creativecommons.org/publicdomain/zero/1.0/"),
]

# term id -> our asset_type. Concept Art (7273) and Document (11) are skipped:
# neither is a usable game asset, and at 10s a request they are not worth it.
TYPES = [
    (9,  "2d_sprite"),
    (10, "3d_model"),
    (14, "texture"),
    (12, "music"),
    (13, "audio"),
]

# <span class="art-preview-title"><a href="/content/SLUG">TITLE</a></span>
RE_ROW = re.compile(
    r'art-preview-title"><a href="/content/([^"]+)">(.*?)</a>', re.S)
# the thumbnail sits in the sibling field, keyed by the same slug
RE_THUMB = re.compile(
    r'<a href="/content/([^"]+)"><img src=[\'"]([^\'"]+)[\'"]')
RE_TOTAL = re.compile(r'Displaying\s+[\d,]+\s*-\s*[\d,]+\s+of\s+([\d,]+)')

# --- node page (enrich mode only) ---
# Tags are taxonomy links; the author sits in its own field. Anchor the author
# regex to that field or you pick up every commenter's profile link instead.
RE_TAGS = re.compile(r'href="/art-search-advanced\?field_art_tags_tid=([^"&]+)"')

# Anchor on the author field, then take the first username span. The link
# target is deliberately not constrained: an earlier version required
# href="/users/...", which silently dropped every creator who links their own
# site instead ("Justin Nichol" -> justinnichol.wordpress.com). Those rows were
# still marked enriched, so the author was lost for good rather than retried.
# Note the page also carries "(Submitted by X)" — matching the first username
# span gets the creator, not the uploader.
RE_AUTHOR = re.compile(
    r"field-name-author-submitter.*?<span class=['\"]username['\"]>\s*"
    r"<a [^>]*>([^<]+)</a>", re.S)
# Same field, but the name is plain text with no link at all.
RE_AUTHOR_PLAIN = re.compile(
    r"field-name-author-submitter.*?<span class=['\"]username['\"]>\s*([^<]{2,60})\s*</span>",
    re.S)

def log(msg):
    print(msg, file=sys.stderr, flush=True)


def get(url):
    return get_text(url)


def page_url(lic_tid, type_tid, page):
    return (f"{SEARCH}?field_art_licenses_tid%5B%5D={lic_tid}"
            f"&field_art_type_tid%5B%5D={type_tid}"
            f"&items_per_page={PER_PAGE}&sort_by=count&sort_order=DESC"
            f"&page={page}")


def parse_rows(page_html):
    """-> [(slug, title, thumb_or_None)] in page order."""
    thumbs = dict(RE_THUMB.findall(page_html))
    out = []
    for slug, title in RE_ROW.findall(page_html):
        title = htmllib.unescape(re.sub(r"<[^>]+>", "", title)).strip()
        if not title:
            continue
        out.append((slug, title, thumbs.get(slug)))
    return out


def crawl(max_pages, only_licence, only_type):

    seen = set()  # dedupe within a licence, retain later licence alternatives
    emitted = 0
    truncated = []          # (label, got, total) for anything we capped
    requests = 0

    combos = [(l, t) for l in LICENCES for t in TYPES
              if (not only_licence or l[1] == only_licence)
              and (not only_type or t[1] == only_type)]

    est = len(combos) * max_pages * DEFAULT_DELAY if max_pages else None
    log(f"{len(combos)} licence x type combinations, "
        f"up to {max_pages or 'all'} page(s) each"
        + (f" (~{est/60:.0f} min at {DEFAULT_DELAY:g}s/request)" if est else ""))

    for (lic_tid, lic, lic_label, lic_url), (type_tid, atype) in combos:
        label = f"{lic_label} / {atype}"
        page = 0
        got = 0
        total = None

        while max_pages is None or page < max_pages:
            try:
                page_html = get(page_url(lic_tid, type_tid, page))
                requests += 1
            except FetchError as e:
                log(f"  {label} page {page}: {e}; aborting source")
                return 1
            except Exception as e:                               # noqa: BLE001
                log(f"  {label} page {page}: {e}; aborting source")
                return 1

            if total is None:
                m = RE_TOTAL.search(page_html)
                total = int(m.group(1).replace(",", "")) if m else None

            rows = parse_rows(page_html)
            if not rows:
                if total and got < total:
                    log(f"  {label}: listing parsed empty before reported total")
                    return 1
                if total is None and not re.search(r"view-empty|no (?:results|artwork) found", page_html, re.I):
                    log(f"  {label}: unrecognized listing page")
                    return 1
                break

            for pos, (slug, title, thumb) in enumerate(rows):
                got += 1
                key = (lic_tid, slug)
                if key in seen:
                    continue          # repeated within the same licence
                seen.add(key)

                asset = {
                    "id": f"opengameart:{slug}",
                    "source": "opengameart",
                    "title": title,
                    "source_url": f"{BASE}/content/{slug}",
                    "licence": lic,
                    "licence_url": lic_url,
                    "asset_type": atype,
                    "tags": [],
                    # Listings are sorted by favourites, so position is the
                    # only popularity signal OGA gives us. Coarse - it restarts
                    # each combination - but "first page of favourites" still
                    # beats no signal at all.
                    "rank_hint": page * PER_PAGE + pos,
                }
                if thumb:
                    asset["thumb_url"] = thumb
                print(json.dumps(asset), flush=True)
                emitted += 1

            page += 1
            if total is not None and got >= total:
                break
            if len(rows) < PER_PAGE:
                if total is not None and got < total:
                    log(f"  {label}: short listing before reported total")
                    return 1
                break

        if total:
            log(f"  {label:24} {got:5}/{total:<6}"
                + ("  CAPPED" if got < total else ""))
            if got < total:
                truncated.append((label, got, total))

    # crawler.md: never silently truncate - say what was left behind.
    log(f"\nemitted {emitted} assets in {requests} requests "
        f"(~{requests * DEFAULT_DELAY / 60:.1f} min of crawl delay)")
    if truncated:
        missing = sum(t - g for _, g, t in truncated)
        log(f"CAPPED {len(truncated)} combination(s), ~{missing} assets not indexed.")
        log("Run with --full for everything (much slower), or raise --max-pages.")
    return 1 if truncated else 0


# --- enrich mode ----------------------------------------------------------

def enrich(path):
    """Fill in author and tags, which the listing pages do not carry.

    Reads "id<TAB>url" lines, fetches each node page, and emits patch objects
    {"id","author","tags"} - not full assets. One request per asset at the
    crawl delay, so this is deliberately incremental: the core hands us a
    batch, we do that batch, and the next run picks up where we stopped.
    """

    with open(path, encoding="utf-8") as fh:
        jobs = [ln.rstrip("\n").split("\t", 1) for ln in fh if "\t" in ln]

    log(f"enriching {len(jobs)} assets "
        f"(~{len(jobs) * DEFAULT_DELAY / 60:.1f} min at {DEFAULT_DELAY:g}s each)")

    done = failed = 0
    for asset_id, url in jobs:
        try:
            page = get(url)
        except FetchError as e:
            log(str(e))
            if e.code in (404, 410):
                print(json.dumps({"id": asset_id, "retry": True}), flush=True)
                failed += 1
                continue
            return 1
        if "field-name-author-submitter" not in page:
            log(f"  {url}: asset author field missing; retry later")
            print(json.dumps({"id": asset_id, "retry": True}), flush=True)
            failed += 1
            continue

        # Tags come out of an href, so they are percent-encoded: "black%20and%20white".
        # unquote_plus handles both %XX and the + form.
        tags = [htmllib.unescape(urllib.parse.unquote_plus(t)).strip().lower()
                for t in RE_TAGS.findall(page)]
        # dedupe, keep order
        tags = list(dict.fromkeys(t for t in tags if t and "," not in t))

        m = RE_AUTHOR.search(page) or RE_AUTHOR_PLAIN.search(page)
        author = htmllib.unescape(m.group(1)).strip() if m else None

        patch = {"id": asset_id}
        if author:
            patch["author"] = author
        if tags:
            patch["tags"] = tags
        if len(patch) == 1:
            log(f"  {url}: no usable enrichment data")
            patch["retry"] = True
            failed += 1
        else:
            done += 1
        print(json.dumps(patch), flush=True)

    log(f"enriched {done}, failed {failed}")
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--max-pages", type=int, default=1,
                    help="pages per licence x type combo (default 1 = 144 assets)")
    ap.add_argument("--full", action="store_true",
                    help="no page cap; the whole site, ~19 min")
    ap.add_argument("--licence", help="only this licence (cc0, cc_by, gpl, ...)")
    ap.add_argument("--type", dest="atype", help="only this asset type")
    ap.add_argument("--enrich", metavar="FILE",
                    help="read id<TAB>url lines, emit author/tag patches")
    args = ap.parse_args()

    if args.licence and args.licence not in {l[1] for l in LICENCES}:
        ap.error("unknown licence")
    if args.atype and args.atype not in {t[1] for t in TYPES}:
        ap.error("unknown asset type")
    if args.max_pages < 1:
        ap.error("--max-pages must be positive")
    if args.enrich:
        return enrich(args.enrich)
    return crawl(None if args.full else max(1, args.max_pages),
                 args.licence, args.atype)


if __name__ == "__main__":
    sys.exit(main())
