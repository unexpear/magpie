# magpie - Crawl milestone

Scraping controls were repaired in September 2026. See [SCRAPING_FIXES.md](SCRAPING_FIXES.md)
for the changes and [ADAPTER_PROTOCOL.md](ADAPTER_PROTOCOL.md) for the current request contract.
Counts and timings below describe the bundled August data snapshot; fresh crawls also
count robots requests, redirects and retries. ambientCG now uses documented 250-row pages.


Search 9,387 game assets from one command line, filtered by what you can actually ship.
C99 core, SQLite FTS5 index, adapters in any language.

This is the **Crawl** stage from [../architecture.md](../architecture.md): no web UI,
no hosting, no server. Just a binary and a database file.

```
> magpie search barrel --type 3d_model --limit 2

 1. Barrel_01
    polyhaven  3d_model   cc0
    commercial ok, no credit needed
    by Jorge Camacho
    .blend,.fbx,.gltf
    2682 tris
    https://polyhaven.com/a/Barrel_01

 2. Barrel Stove
    polyhaven  3d_model   cc0
    commercial ok, no credit needed
    by MP
    .blend,.fbx,.gltf
    11404 tris
    https://polyhaven.com/a/barrel_stove
```

## Build

Needs `gcc` on PATH. Developed against MSYS2 UCRT64. Nothing else to install —
HTTP comes from WinHTTP, which ships with Windows.

```powershell
.\fetch-deps.ps1     # downloads sqlite3 + yyjson into vendor/ (one time)
.\build.ps1          # produces magpie.exe
```

Linux/macOS: `make deps && make` (uses libcurl instead of WinHTTP).

## Use

```powershell
.\magpie.exe crawl                       # fetch everything into index.sqlite
.\magpie.exe crawl polyhaven             # just one source
.\magpie.exe crawl --offline             # rebuild from cache, zero requests

.\magpie.exe search rusty metal
.\magpie.exe search barrel --type 3d_model --commercial --no-credit
.\magpie.exe search dungeon --style pixel --why      # show classifier evidence
.\magpie.exe search --tag unclassifiable --limit 5   # find rows needing better data

.\magpie.exe check --limit 200           # HEAD urls, record dead links (resumable)
.\magpie.exe enrich --limit 50           # fetch author/tags (resumable)
.\magpie.exe reclassify                  # re-run the classifier, no network
.\magpie.exe export                      # write web/data.json
.\magpie.exe stats
.\magpie.exe sources
```

## Web UI

`magpie export` writes `web/data.json`; `web/index.html` is a single self-contained
page that loads it. No build step, no dependencies, nothing to install. Serve the
`web/` folder from anything:

```bash
python -m http.server 8231 --directory web
```

At ~9k assets the whole index is a 3.4 MB JSON (about 700 KB gzipped), which the
browser filters instantly. That is well inside GitHub Pages' limits. If the index
grows past roughly 100k assets, swap the JSON for `sql.js-httpvfs` over a static
SQLite (see [../hosting.md](../hosting.md)) — until then a JSON file is simpler
and has fewer sharp edges.

### The credits basket

The feature the whole UI is arranged around. You gather CC-BY assets over weeks;
at ship time you have to produce an accurate credits file, and reconstructing that
from browser history is exactly how attribution silently gets skipped.

Every result has **+ add to credits**. The basket persists in `localStorage`, so it
survives closing the tab, and **Build credits file** emits ready-to-paste Markdown
split into *Required attribution* / *Public domain* / *⚠ Check before shipping*.

It is loud about what it does not know. An asset that needs credit but has no
recorded author gets an **author unknown** badge in the results and, in the output,
a literal `**AUTHOR UNKNOWN — open the link and fill this in**`. That is currently
most OpenGameArt rows, since they are un-enriched — a credits line with no name does
not satisfy the licence, and quietly emitting one would be worse than emitting
nothing.

### Showing an asset honestly

The preview is the asset, so how it is drawn is a correctness question, not a
styling one.

- **Pixel art is rendered nearest-neighbour** (`image-rendering: pixelated`).
  Bilinear smoothing shows an asset you would never actually ship. Applied to
  anything the classifier calls `pixel` — 296 of the 2D rows. Declaration order
  matters: `crisp-edges` first, `pixelated` last, so the better-defined property
  wins where it exists.
- **Sprites, icons, UI and VFX are shown whole** on a checkerboard
  (`object-fit: contain`); textures and models still fill the frame. Cropping a
  tileset to a 4:3 box throws away the thing you are trying to judge.
- **Audio gets a type glyph**, not a grey box. 1,717 rows have no preview image
  anywhere, and they used to render identically.
- A thumbnail that 404s degrades to "preview unavailable" rather than a broken
  image icon.
- **`referrerpolicy="no-referrer"` on every thumbnail.** Kenney's CDN serves a
  plain request happily but returns nothing to a browser sending a cross-origin
  `Referer` — so every Kenney preview was silently blank, and no `onerror` fired,
  so the failure was invisible to DOM tests. It also stops telling each source
  what page someone is browsing.
- **Use the source's own sized variant, not its full-size art.** Kenney's pack
  pages only expose full previews (19–192 KB, ~5 MB for one screen); its *listing*
  pages reference `-400x` variants, halving that to ~57 KB each. The suffix cannot
  be derived — it 404s on some files and is *larger* than the original on others —
  so `kenney.py --thumbs` reads the URLs Kenney already publishes.

### Detail view

Clicking a card opens a panel instead of throwing you onto another site
mid-comparison — the title link still goes out. It shows licence terms, formats,
triangle count, popularity, and **tags split by provenance**: the source's own
words as solid chips, ours as dashed ones, each clickable to filter. Missing
authors on credit-required assets are called out in red here too.

### Other UX decisions worth knowing

- **Licence is one three-way control**, not two checkboxes: *Anything* / *Can ship
  commercially* / *…and no credit needed*. That is the actual question, asked once.
- **Filters live in the URL**, so a search is shareable and survives a reload.
  `?q=barrel&use=nocredit&style=lowpoly` is a link you can send someone.
- **Facet counts respect the other filters.** The number beside `3d_model` is what
  you would get if you clicked it — counts computed against an unfiltered index are
  how people click into empty result sets.
- **The empty state names the culprit.** It works out which single filter is costing
  you the most results and offers one button to drop it, rather than shrugging.
- **Refine is multi-select and ANDed.** "isometric *and* animated" is a real
  question; a single-select facet cannot ask it. Counts show what would remain if
  you added that tag, with already-chosen tags excluded from the list.
- **Keyboard**: `/` search · `j`/`k` or arrows walk the grid · `Enter` opens
  details · `a` adds the focused card to credits · `Esc` closes. Walking off the
  end loads the next page instead of dead-ending.
- **On narrow screens** the header stops being sticky — it cost a quarter of the
  screen once the licence control wrapped to two lines, and there is no `/`
  shortcut on a touch device to justify keeping it. The licence control becomes a
  single swipeable row, and results move above the sidebar.

## What a full crawl actually costs

| Source | Assets | Requests | Time |
|---|---:|---:|---:|
| opengameart | 9,927 | 101 | ~17 min |
| gameicons | 4,180 | **1** | ~3s |
| ambientcg | 2,877 | 3 | ~3s |
| polyhaven | 2,296 | 1 | instant |
| kenney | 79 | 0 | instant |
| **total** | **19,359** | | |

The spread between sources is the whole story: game-icons hands over 4,180 assets in
**one** request, while OpenGameArt costs ten seconds per 144. Prefer sources that
publish a manifest.

The first three are 4 requests and about 5 seconds; on a re-crawl Poly Haven answers
`304 Not Modified` so it drops to three. You could run those hourly forever and nobody
would notice — which is the point (see [../crawler.md](../crawler.md)).

OpenGameArt is the outlier and deliberately so: its robots.txt asks for a 10-second
crawl delay, so 45 requests take 7.5 minutes no matter what. That is the polite floor,
not a performance problem to fix.

## Licence spread

```
cc0 5944   cc_by 1103   gpl 918   cc_by_sa 873   oga_by 549
9387 assets, 8469 commercially safe, 5944 needing no credit
```

This is what the project is for. `magpie search dungeon` returns GPL music you cannot ship
as its top hit; adding `--commercial --no-credit` swaps the entire result set for CC0.

## Layout

```
src/
  main.c            CLI: crawl search stats sources check enrich reclassify export
  asset.[ch]        the schema everything normalizes into
  classify.[ch]     type/style inference: weighs evidence, reports confidence,
                    returns unclassifiable rather than guessing
  store.[ch]        SQLite: assets, FTS5, ranking, link health, http cache
  http.[ch]         WinHTTP (Windows) / libcurl (else), GET + HEAD
  limiter.[ch]      per-host token bucket, budget, 429 backoff
  fetch.[ch]        the only path to the network: limit + cache + retry
  adapter.[ch]      registry, JSONL protocol, subprocess runner, enrich driver
  jsonutil.[ch]     yyjson accessors
  adapters/
    polyhaven.c     native
    ambientcg.c     native
adapters/
  kenney.py         external (Python) - curated list, no scraping
  opengameart.py    external (Python) - a real scrape; HTML is where C is
                    the wrong tool, which is why this boundary exists
  data/kenney-packs.json
web/
  index.html        the UI - search bar, facets, sources panel
  data.json         written by `magpie export`
```

## The classifier

Type and style are inferred by weighing evidence, not by taking the first keyword
that hits. Rules, in priority order:

| Evidence | Confidence | Example |
|---|---:|---|
| declared by the source | 100 | OpenGameArt's own "Art Type: 3D Art" |
| file format | 85 | `.ogg` is audio, whatever the title says |
| tag | 70 | tagged `voxel` |
| polycount | 60 | a 900-triangle mesh is low-poly regardless of labels |
| title word | 50 | "Pixel Platformer" |
| source habits | 35 | ambientCG mostly publishes textures |

Anything below **50** returns `unclassifiable` — a real answer, not a shrug.
Source habits sit deliberately under the line: "this site mostly publishes
textures" is not evidence about one asset.

Two distinctions the schema keeps separate:

- **`unknown`** — nobody looked yet.
- **`unclassifiable`** — we looked, and the data genuinely isn't there.

Collapsing those hides a data-quality problem. You can find every weak row with
`magpie search --tag unclassifiable` and go fix the source; you cannot fix a
confident wrong guess you never knew was a guess.

`--why` shows the reasoning for any hit:

```
style=pixel via tag (70%)
```

`magpie reclassify` re-runs the whole thing over the stored index with no network,
so you can tune a rule and see the effect in seconds. Values a *source* declared
survive it — provenance is tracked per row, and re-deriving them would destroy
real data.

### Mined tags

Most assets arrive with no tags at all — OpenGameArt's listing pages publish none,
and fetching thousands of node pages costs hours. But the titles are full of
structure, so the classifier mines it, with **zero network requests**:

| | rows | with structural tags |
|---|---:|---:|
| 2D sprites | 935 | 7 → **595 (63%)** |
| 3D models | 1,452 | 556 → **900 (61%)** |
| Audio + music | 1,717 | 10 → **690 (40%)** |

The vocabulary is **gated by type**, because the same word means different things:
`rock` is a stone on a model and a genre on a track, so mixing the tables produces
nonsense.

**Everything** — `character` `animated` `pack` `medieval` `fantasy` `scifi` `horror`

**Visual only** — `tileset` `spritesheet` `icon` `background` `gui` `portrait`
`isometric` `sideview` `topdown` `lpc` `roguelike`, plus tile size (`16x16`,
`32x32`, any `NxN` bounded 4–256)

**3D only** — `rigged` `modular` `vehicle` `weapon` `building` `prop` `furniture`
`nature` `terrain` `container` `tool` `photoscan`, plus **poly buckets**

**Audio only** — `loopable` `sfx` `theme` `ambience` `chiptune` `orchestral`
`electronic` `piano` `percussion` `voice` `battle` `menu` `jingle` `impact`
`footsteps`

Three of these carry most of the weight, and all three are attributes that decide
whether an asset is usable *at all* while no source exposes them as a field:

- **Tile size** (2D) — you cannot mix 16px and 32px art in one game.
- **Poly bucket** (3D) — this index holds everything from a 182-triangle prop to a
  17-million-triangle photoscan. `under-1k` 17 · `1k-10k` 226 · `10k-100k` 212 ·
  `over-100k` 66.
- **`loopable`** (audio) — a track that does not loop is useless as background
  music. 86 found.

`lpc` deserves its own note: 149 assets belong to the Liberated Pixel Cup, a
collection whose whole point is a shared style and compatible dimensions. It was
completely invisible before, and recognising it is most of why pixel-style
detection went from 216 to 377 index-wide.

These are facets, not claims about type. Being wrong about `isometric` costs a user
one bad search result; being wrong about a licence costs them a takedown. The two
are held to different standards on purpose.

### Source tags and mined tags are kept apart

`tags` holds only what the site published. `tags_auto` holds everything we derived.
The classifier reads the first and writes only the second.

They were one merged list until it became clear the merge hid something: Poly Haven
genuinely publishes `vintage`, `worn`, `antique`, while `nature` on the same row was
our inference — and afterwards nobody could tell which was which. That made a
headline like "61% of 3D rows have structural tags" unauditable, since part was real
source data and part was our own guessing in unknown proportion.

- A tag **filter** searches both. A user wanting `tileset` does not care who said it.
- `magpie search` prints them on separate lines (`tags:` and `mined:`), `magpie stats`
  counts them separately, and the UI puts a **·** beside any tag that is mostly
  inferred, with a tooltip saying so.
- FTS indexes both, so search behaviour is unchanged.

It also deleted a bug class: a helper existed purely to strip a stale
`unclassifiable` tag off rows that later became classifiable. With `tags_auto`
rebuilt from nothing every run, a changed verdict is simply not written again, and
the workaround became dead code.

### Keyword variants are generated, not listed

One rule covers `sci-fi`, `sci fi` and `scifi`. Hand-listing forms is how "8 Bit
Music" slipped past a table that already had `8-bit` *and* `8bit`; an audit for
near-misses then found **"sci fi" in 68 titles**, the largest single miss in the
index, plus `16 bit`, `chip tune`, `rogue like`, `side scroller`.

Write each rule in its **most separated** form — the generator can join "tile set"
into "tileset", but cannot split "tileset" apart.

**Style is gated too.** Every value in `style_id` describes how something *looks*,
so asking which one a sound file is produces nonsense — 50 tracks were labelled
`pixel` because "8-Bit Battle Theme" contains a visual-style keyword. Audio and
music now decline to answer, and the genre lands in the tags instead. Fonts stay
visual: pixel fonts are real.

**Deliberately not done:** ranking does *not* boost assets for having more mined
tags. Tag count measures how well our own parser did on a title, not how good the
asset is, and dressing that up as a quality signal would be inventing a metric from
our own artifacts. The tags improve *filtering*, which is the actual win.

## Adding a source

Clean JSON API → write a native adapter in `src/adapters/`, add it to `ADAPTERS[]`
in `src/adapter.c`, add the file to `build.ps1`.

Anything messy — HTML scraping, an SDK in another language — → write an external
adapter that prints JSONL. See [ADAPTER_PROTOCOL.md](ADAPTER_PROTOCOL.md). The core
skips external adapters whose command is missing, so a C build never breaks because
Python isn't installed.

## Notes and gotchas

- **`build.ps1` tracks header changes.** It originally compared each `.c` only
  against its own `.o`, so editing a header rebuilt nothing that merely *included*
  it. Adding a field to the `asset` struct that way produced a binary containing
  two different struct layouts, and the symptom was a heap-corruption crash
  (`0xC0000374`) inside an adapter — which looks like a memory bug in your code and
  is nothing of the sort. It reproduced only at `-O2`, ran fine under `-O0`, and
  vanished after `-Clean`. If you ever see that combination, suspect stale objects
  before you suspect your pointers.
- **stdout is unbuffered.** Crawls are long; with the default full buffering, piping
  to a log showed nothing until exit — and nothing at all if the process died.
- **`--offline` is the dev loop.** Every response is cached in SQLite, so you can
  re-run the crawler as often as you like while working on a normalizer without
  sending a single request. A full offline rebuild takes 0.4s.
- **Never pass `?type=` to ambientCG.** Measured: unrecognised values are ignored
  and the API returns the *entire* catalogue, so enumerating types fetched
  everything three times over. Read `dataType` off each asset instead.
- **ambientCG doesn't send ETag/Last-Modified**, so its 3 requests can't 304. Poly
  Haven does.
- **Kenney's pack list is hand-maintained**, not scraped (decisions.md D3). Validate
  it with `python adapters/kenney.py --check`; regenerate with `--refresh`. The
  shipped list was checked — 16 of an initial 95 slugs were dead and were removed.
- **OpenGameArt is slow by design.** Its robots.txt asks for `Crawl-delay: 10`, and the
  adapter reads that at runtime and obeys it. Default is one page per licence×type
  combination (~8 min); `--full` sweeps the whole site (~19 min). It prints exactly
  which combinations were capped and roughly how many assets were left behind — a
  bounded crawl that doesn't say so reads as complete when it isn't.
- **Licence comes from the search filter, not the asset page** (decisions.md D6). That
  is a 45-hour vs 19-minute difference on OpenGameArt, and the answer is exact rather
  than parsed out of prose.
- **`commercial_ok` and `attribution` are derived in the core** from the licence,
  never taken from an adapter. A wrong value there is the one bug here that could
  cost someone real money, so `unknown` is treated as not-safe.

## Not done

No web UI, no thumbnails, no dedupe across sources, no Sketchfab/OpenGameArt/itch.
Those are Walk and Run.
