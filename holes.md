# Holes

## Legal / ToS

- **Scraping the big sites.** itch.io is the highest-value source and has no public search
  API. Yeggi survives by linking out, but survival ≠ permission. Open question: ask itch.io
  directly before building that adapter? A "no" is cheaper to hear early.
- **Thumbnail caching.** Hotlinking steals their bandwidth; caching copies their image.
  Yeggi caches. Probably fine under fair use + it drives traffic to them, but it *is* the
  one place we hold someone else's file. Unresolved.
- **Being wrong about a licence.** If the site says CC0 and it isn't, and someone ships a
  game on our say-so — whose problem? Mitigation: never assert, always show the source's
  own licence string + link, and disclaim. Doesn't fully solve it.

## Product

- **Why not just Google?** Real question. Answer has to be the licence facet + the unified
  grid. If someone can get 80% of this from a Google query, the project is a toy. Needs
  testing on real searches before building much.
- **Cold start.** Yeggi works because 3D printing people already knew they had a search
  problem. Do gamedevs? Or has everyone already picked their two favourite sites and
  stopped looking?
- **Quality floor.** itch.io + OpenGameArt contain a *lot* of unusable junk. Aggregating
  everything may just aggregate the noise. Need a quality signal — downloads, ratings,
  curated-source weighting — or the results are worse than one good site.
- **2D vs 3D vs audio may not want one search box.** A pixel-art person and a PBR-texture
  person share almost no vocabulary. Maybe it's three products.

## The one that undercuts the whole premise

**3,389 assets require attribution and have no creator recorded** (of 7,461 that require
it). Their licence cannot be honoured from what the index holds, so the credits file
Magpie generates marks them `AUTHOR UNKNOWN` and tells you to go and look. Almost all are
OpenGameArt, whose listing pages don't publish authors.

This is the sharpest measure of whether the tool keeps its promise, and it is now tracked
as `uncreditable` in `magpie stats`. Enrichment targets these first (decisions.md D15);
clearing the backlog is ~9.4 hours of politely-paced requests.

Worth being honest that a *partial* answer here is still useful — the asset is findable
and its licence is correctly stated — but "findable" is not "shippable", and the gap
between those two is exactly what this project claims to close.

## Small, watched, not yet worth building for

- **A permanently-failing enrich URL would be retried forever.** Failures leave the row at
  `enriched = 0`, which is right for a timeout or a 502 — both seen, ~0.8% of requests, and
  both resolved on the next pass. But a page that is genuinely *gone* (404) would be picked
  every run and never resolve, burning one request each time. Not yet observed, so it is
  noted rather than engineered around; the fix would be an attempt counter that
  deprioritises repeat offenders, or letting the adapter say "this one is gone" so the row
  can be marked tried.

## Known gaps in the data (as built)

The first four below are all the same shape: work that is slow *by politeness*, not by
inefficiency. Proposed answer is [background-runner.md](background-runner.md) — trickle
them unattended rather than sitting and watching.

- **Most OpenGameArt rows still have no author or tags.** `magpie enrich` fixes this one
  batch at a time, but at their 10s crawl delay the full 4,135 rows is ~11 hours of
  wall clock. Only a handful are enriched so far. Until then those rows match on title
  alone and rank below tagged ones — which is correct behaviour, but it means the index
  is uneven by source.
- **Style is unclassifiable for ~5,900 assets**, overwhelmingly the un-enriched
  OpenGameArt rows. That number should fall as enrichment progresses; it is the single
  best measure of how much enrichment is left worth doing.
- **The default OpenGameArt crawl is capped** at one page per licence×type. It says so
  loudly, but the index is a sample (4,135) not the site (~37k more).
- **Nearly all links are unchecked.** `magpie check` is resumable and hides dead links from
  search, but at 1s/request for the fast sources and 10s for OpenGameArt, checking all
  9,387 is hours. No dead links found in the first 40.
- ~~**Redoing enrichment means clearing the flag by hand.**~~ **Fixed:**
  `magpie enrich --redo-blank` requeues rows that went through enrichment and still have
  no author — exactly the ones an adapter bug silently dropped. This kept mattering: it
  caught percent-encoded tags once, and then a regex that required `href="/users/..."`
  and so lost every creator who links their own site instead. Those rows were marked
  done and would never have been revisited.

## Technical

- **Style classification.** "pixel / low-poly / PBR" is the facet people actually want and
  almost no source tags it. Infer from tags? From the thumbnail with a small image model?
  Both are unreliable. Might have to ship without it.
- **Dedupe across mirrors** is fuzzy and will produce wrong groupings. Fail toward showing
  duplicates rather than hiding a real result.
- **Adapters rot.** Every scraped source breaks on a redesign. Needs per-adapter health
  checks or the index silently goes stale.

## Money

- Hosting + crawl is cheap; it's metadata and thumbnails. Fine as a hobby project.
- Monetization would be affiliate links to paid stores — which pulls ranking toward paid
  results and against the CC0 pitch. Conflict. Probably just don't monetize.

## Decide before building

1. Ask itch.io about indexing — yes/no changes the scope a lot.
2. Run 10 real asset searches by hand, time them, compare to the imagined tool. Is the
   pain real?
3. Pick: all asset types, or 3D-only v1?
