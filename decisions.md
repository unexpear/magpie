# Decisions

## D1 — Core is C, not Python

**Decided.** Crawler + index + search are C99.

Why:
- SQLite *is* C. The amalgamation is one public-domain `.c` file. Not a binding — the real
  thing. This project is mostly "put rows in SQLite", so the core dep is a native fit.
- No dependency rot. This runs on cron for years. A Python project decays; a static binary
  doesn't.
- All libs permissive: sqlite3 (public domain), yyjson (MIT), WinHTTP (OS), libcurl (MIT-ish).
  No GPL anywhere.

Cost accepted: string munging and licence normalization are more tedious in C. That's the
bulk of the work, and it's the price.

The argument that nearly killed this was HTML parsing (lexbor/gumbo are miserable). See D2 —
that argument is now routed around instead of paid.

## D2 — Adapters may be written in any language, as subprocesses

**Decided.** Two kinds of adapter, one schema:

- **Native (C)** — compiled in. For clean JSON APIs. `polyhaven`, `ambientcg`.
- **External (any language)** — a program the core spawns. It prints JSONL to stdout, one
  asset per line. For anything where C is the wrong tool: HTML scraping, weird auth,
  sites with an official SDK in another language.

The core never knows or cares which is which. Same normalizer, same validation, same
SQLite writer.

Why this over "just write it all in C": HTML parsing in C is the one genuinely bad part of
the job, and it's confined to exactly the adapters that need it. Python's `html.parser` is
in the stdlib and does in 10 lines what lexbor does in 200. Meanwhile the hot path — HTTP,
JSON, SQLite, FTS — stays C.

Why this over "just write it all in Python": the core is the part that must not rot.
Adapters are expected to break and get rewritten; the core isn't.

Cost accepted: external adapters need their runtime installed. Mitigated — the core runs
fine with zero external adapters, and skips any whose command is missing rather than
failing the crawl.

Protocol: [crawl/ADAPTER_PROTOCOL.md](crawl/ADAPTER_PROTOCOL.md).

## D3 — Don't scrape Kenney; hand-maintain the list

**Decided.** Kenney is ~100 packs and changes rarely. A curated JSON file in the repo is
more reliable than a scraper, costs zero requests, and never breaks on a redesign.

`adapters/kenney.py` reads that list. It *can* also scrape to check for new packs, but
that's opt-in (`--check-new`), not the default path.

Generalizes: for any small, slow-moving, high-quality source, a hand-list beats a crawler.

## D4 — HTTP backend is WinHTTP on Windows, libcurl elsewhere

**Decided.** WinHTTP ships with Windows — no vcpkg, no MSYS2 package, no DLL shipping.
libcurl everywhere else. One `http.h` interface, two backends, picked at compile time.

Cost accepted: two implementations of ~200 lines to keep in sync. Worth it for a
zero-install build on the machine this is actually developed on.

## D6 — Get licence from the search filter, never from the asset page

**Decided.** When a source doesn't show licence in its listing but *does* let you filter
by it, crawl one licence at a time. Every result in the set is then known to carry that
licence, at zero extra cost.

OpenGameArt is the case that forced this. Its listing shows title and thumbnail only.
Fetching each of ~16,000 node pages to read a licence, at the 10s crawl-delay its
robots.txt demands, is **45 hours**. Crawling 9 licences × 5 types as filtered queries at
144 results per page is **~19 minutes** for the whole site, and the licence is exact
rather than parsed out of prose.

Same trick gives exact asset type instead of guessing from tags.

**Dual licensing falls out of the ordering.** An asset can carry both GPL and CC0. The
core upserts by id, so the last write wins — therefore crawl **least permissive first,
CC0 last**. A dual-licensed asset ends up recorded as the most permissive option, which
is the one a user may actually rely on.

Generalizes: before writing a per-item fetch loop, check whether the thing you want is
already a facet on the listing. Two orders of magnitude here.

## D7 — `oga_by` is its own licence, not an alias for `cc_by`

**Decided.** Added `LIC_OGA_BY` rather than folding OpenGameArt's attribution licence
into `cc_by`.

They behave identically for our derived fields (commercial ok, attribution required), so
aliasing would have been invisible and convenient. But licence is the one field where
this project's whole value sits, and telling a user something is CC-BY when it is
OGA-BY is a lie in exactly the place they came to us for the truth.

Watch the parse order: `str_icontains(s, "by")` swallows "CC-BY-SA 3.0" and "OGA-BY 3.0"
alike, so the `sa` and `oga` tests must both run first.

## D8 — The classifier reports confidence, and refuses when the evidence is thin

**Decided.** Type and style come from weighed evidence with an explicit confidence
threshold. Below it, the answer is `unclassifiable`.

The first version was a keyword ladder: first tag that matched won, and anything
unmatched became `unknown`. That is fake precision. It cannot tell you *why* it decided,
it silently ranks a wild guess equal to a certainty, and its failures are invisible.

Evidence weights: declared 100, file format 85, tag 70, polycount 60, title 50, source
habits 35. Threshold 50. Source habits sit under the line on purpose — "ambientCG mostly
publishes textures" is a fact about a site, not about an asset.

Two states, kept distinct:
- `unknown` — nobody looked yet.
- `unclassifiable` — we looked and the data isn't there.

Collapsing them buries a data-quality problem. `unclassifiable` is also added as a *tag*,
so `magpie search --tag unclassifiable` lists exactly the rows worth improving. A user can
fix a known gap; they cannot fix a confident wrong guess they never knew was a guess.
Unclassifiable rows also sink in ranking rather than being hidden.

Consequences worth knowing:
- Every row records how it was classified (`type_ev`, `type_conf`), so a bad call traces
  back to the rule that made it.
- `magpie reclassify` re-runs everything offline, so tuning a rule takes seconds.
- Provenance must be respected on re-run: values a source **declared** survive
  reclassification. Re-deriving them would delete real data — that was a live bug caught
  before it ran.
- `classify_apply` is idempotent *and* removes a stale `unclassifiable` tag when a row
  later becomes classifiable. Without that second half, a row that gains tags keeps
  claiming it has none forever.

## D9 — Enrichment is a separate, resumable pass

**Decided.** Fields a listing cannot carry (OpenGameArt's author and tags) are filled by
`magpie enrich`, not by the crawl.

Crawling gets breadth cheaply; enrichment costs one request per asset. At OpenGameArt's
10s crawl delay that is 45 hours for the full site, so it cannot be part of a crawl that
otherwise takes minutes. Splitting them means the index is useful immediately and gets
better in the background.

The core hands the adapter a bounded batch, most-popular-first, so the assets people
actually see improve first. Adapters emit **patches**, not assets, and the update is
`COALESCE`-based — a patch can only fill blanks, never overwrite what a crawl
established. After enriching, the classifier re-runs, because rows that just gained real
tags can often be classified where they could not before.

## D10 — Lenient parsing for scraped data, strict parsing for human input

**Decided.** The same value can arrive from two places and must be treated differently.

`licence_parse()` is deliberately forgiving because it reads whatever a website
published — "CC-BY 3.0", "Creative Commons Zero", "GPLv2". Tolerance there is the whole
job. Using that same function on a command-line argument turned out to be a quiet
disaster:

- `--licence cczero` **matched CC0**, because the fuzzy fallback saw "zero". A typo
  produced a confident, wrong-but-plausible result set.
- `--type banana` returned "no matches" — indistinguishable from "we have no such
  assets".
- `--source polyhavn`, `crawl polyhavn`, `check polyhavn` all reported cheerful success:
  *0 assets indexed*, *0 ok, 0 dead*, exit code 0.

So: `*_parse_strict()` for anything a human typed, exact table match, `-1` on failure,
and an error listing the valid values (generated from the same table, so the message
cannot drift). Source names validate against the adapter registry on every subcommand.

The general rule: **a typo must never be indistinguishable from an empty result.** Wrong
input should fail loudly; only untrusted *data* earns the benefit of the doubt.

## D11 — Read the command line as UTF-16 on Windows

**Decided.** `main()` re-reads `GetCommandLineW()` via `CommandLineToArgvW` and converts
to UTF-8, and sets `SetConsoleOutputCP(CP_UTF8)`.

Windows hands `main(int, char**)` its arguments in the system ANSI codepage. The index
is UTF-8 and contains "Comfy Café", "Pfälzer Forest 01", "Herkulessäulen", "Дум" — every
one of which was **unfindable from the CLI** while being perfectly findable from SQLite
directly and from the web UI. The bug looked like a search-engine problem and was
actually an argument-encoding problem two layers earlier.

Worth remembering as a debugging lesson: the FTS index, the tokenizer and the browser
were all fine. Only the shell path was broken, so any test that went through Python or
JavaScript would have said everything worked.

## D12 — Mined tags live in their own column

**Decided.** `tags` holds only what the source published. Everything the classifier
derives goes in `tags_auto`. The classifier reads `tags` and never writes it.

Previously both were merged into one list, which meant nobody — not a user, not a later
version of us — could tell Poly Haven's `vintage` from a `nature` we inferred off a
title. That made a claim like "61% of 3D rows have structural tags" unauditable: part of
it was real source data and part was our own guessing, in unknown proportion.

It also makes a bad mining rule **findable and undoable**. Before, a wrong rule polluted
the same column as the good data permanently.

Consequences:
- A tag *filter* searches both columns — a user wanting `tileset` does not care who said
  it. The split is about trust and auditing, not filtering.
- `magpie stats` reports "tagged by the source" vs "tags mined by us only"; the UI marks
  predominantly-mined tags with a dot and an explanatory tooltip.
- FTS indexes both, so search behaviour is unchanged.
- It **deleted a bug class**. A `copy_without()` helper existed to strip a stale
  `unclassifiable` tag from rows that later became classifiable. With `tags_auto` rebuilt
  from nothing on every run, a changed verdict simply is not written again — the
  workaround became dead code.

Cost: the merged column could not be unmixed after the fact (`character` could have come
from either side), so the migration cleared it and re-crawled to repopulate from source.

## D13 — Match keyword variants by rule, not by listing them

**Decided.** `word_in_flex()` generates the hyphen, space and joined forms of a keyword,
so one table entry covers "sci-fi", "sci fi" and "scifi".

Hand-listing variants is how "8 Bit Music" slipped past a table that already had `8-bit`
and `8bit`. An audit for near-misses then found **"sci fi" in 68 titles** — the single
biggest miss in the index — plus `16 bit`, `chip tune`, `rogue like`, `side scroller`.

Rules must be written in their **most separated** form: the generator can join "tile set"
into "tileset" but cannot split "tileset" apart.

The same pass fixed a latent false positive: phrase matching had fallen back to raw
substring, so `top down` matched inside "lap**top down**load". Phrases are now
boundary-checked like single words.

## D14 — Pair scraped data by identifier, never by position

**Decided.** When extracting two things from a page, tie them together with something
intrinsic — an id inside the URL, a shared key — not "the next match after this one".

Kenney's listing renders each tile as a CSS background on a `.cover` div, and the first
regex paired each `/assets/<slug>` link with the next media URL in the document. It
matched 77 of 79 packs and every count looked healthy. It was also **wrong**: Fish Pack
showed a city, Generic Items showed a map. Nothing in the data revealed it — the URLs
were real, the slugs were real, the pairing was garbage.

The fix was to notice that the media URL already contains the slug
(`/media/pages/assets/<slug>/<hash>/preview-400x.png`) and parse it from there, which
cannot mismatch. 79 of 79, correct by construction.

Two lessons worth keeping:
- **Positional regex pairing is a silent-corruption machine.** Prefer a self-identifying
  field even when it means a second look at the markup.
- **Some bugs are only visible rendered.** Row counts, HTTP statuses and non-empty
  strings all passed. It took looking at the page to see that the pictures were wrong.

## D15 — Enrich by legal necessity, not popularity

**Decided.** `magpie enrich` processes assets whose licence demands a credit we cannot
supply *before* anything else. Popularity is the tie-break, not the sort key.

Measuring the index made the priority obvious. Of 19,359 assets, **7,461 require
attribution and 3,389 of those have no creator recorded** — almost all from OpenGameArt,
whose listing pages simply do not publish authors. Those assets cannot lawfully be shipped
from what we hold. A CC0 row with no tags is merely harder to find; a CC-BY row with no
author is a broken promise, and this project exists to answer "can I ship this".

It also reshapes the work. Enriching all of OpenGameArt is **27.5 hours** at their
10s crawl delay. Enriching only the rows that legally need it is **9.4**. Same politeness,
a third of the calendar time, and the part that matters finishes first.

`magpie stats` now reports `uncreditable` directly, and each enrich run prints how many
remain — so progress is measured against the promise rather than against row count.

Worth noting for [background-runner.md](background-runner.md): this is the job the runner
should lead with. "Retire the uncreditable backlog" is a far better goal than "enrich
everything", and it has a definite end.

**Refined once more, after measuring the backlog.** Of 2,964 uncreditable assets, **797
are GPL** — already flagged as not cleared for commercial use. Fetching their authors
completes a credit line for something most people still cannot ship, at a cost of 2.2
hours of OpenGameArt's server time. So the order is now:

1. credit missing **and** the asset is commercially usable — 2,167 rows, 6.0 h
2. credit missing at all — the remaining 797
3. popularity

Deprioritised, not skipped: the data is still worth having, just last. The principle
generalises past licences — **rank the backlog by what the missing field unlocks**, not
by how incomplete the row looks.

## D16 — The daily budget lives in the database, and it is the one that matters

**Decided.** `request_budget(day, used)` in SQLite, charged on every request, enforced by
`magpie` itself. Default 500/day, `--daily-cap` to override, `magpie budget` to inspect.

This one is embarrassing, and worth writing down precisely because of that. D-for-runner
already said: *"Budget accounting belongs in magpie, not the runner. A limit that only
exists in the wrapper is not a limit."* What then got built was `--max-requests`, which
bounds **one invocation**, plus a daily counter living in `runner.py`. So the daily
ceiling existed only in the wrapper — exactly the thing the rule warns against — and
every hand-run command started from zero.

The result: **~1,208 requests to OpenGameArt in a day capped at 500.** Each individual
batch was correctly paced at their 10s crawl delay, and each looked reasonable in
isolation. That is the trap. "Every request was polite" does not add up to "the day was
polite", and a per-run cap cannot see the difference between one batch and the fifth.

What makes the fix real rather than another good intention:

- **Written through on every request**, not batched. A single-row UPDATE is microseconds
  against a network wait measured in seconds, so there is no reason to risk losing the
  count in a crash.
- **External adapters are charged too.** OpenGameArt enrichment fetches pages inside a
  Python subprocess where our fetcher never sees them. It is one page per asset, so the
  batch is charged up front, before the adapter is handed the job file. Leaving those
  uncounted is precisely how the overrun happened.
- **Charged up front, so a crash over-counts.** Over-counting costs us a pause;
  under-counting costs someone else's server.
- **The ledger was backfilled** from evidence in the index rather than started at zero,
  because a cap that forgives the day it was written for is theatre.

Generalisable: a rate limit that resets when the process does is not a rate limit. If the
limit is meant to protect someone else, it has to outlive the process that respects it.

## D5 — Sequential crawl, per-host minimum interval

**Decided.** No threads. The crawler walks sources one at a time and sleeps to enforce a
minimum gap per host.

The politeness rule in [crawler.md](crawler.md) is "concurrency 1 per host" — going
sequential satisfies it by construction rather than by careful locking. Total Tier-1 crawl
is ~10 requests, so there is nothing to parallelize. Revisit only if a source needs
thousands of paginated calls.
