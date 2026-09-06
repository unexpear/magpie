# Magpie

**A Yeggi-style search engine for game assets.** Find sprites, 3D models, textures, UI, music and sound across multiple sites, compare licence terms, and keep a reusable credits list.

**[Search the catalogue](https://unexpear.github.io/magpie/)**

Magpie indexes metadata from **OpenGameArt, game-icons.net, ambientCG, Poly Haven and Kenney**. Search runs in your browser against a static catalogue. Asset downloads stay on the original sites; previews appear only for sources covered by the publication policy.

The current bundled catalogue contains **19,441 assets**. API/repository sources were refreshed on September 6, 2026; other source observations date from August 2026; the site shows observation and crawl dates separately from export generation time.

## What you can do

- Search by title, tags, author and source; narrow by type, style and licence.
- Turn off inferred tags in search and tag filters; the choice is preserved in shared search links. Type and style remain Magpie classifications.
- Use a conservative commercial filter or a CC0 filter, then inspect the exact source terms. Attribution, ShareAlike and other obligations are shown explicitly.
- Collect assets into a credits list and download `CREDITS.md`.
- Save full attribution notices, creator corrections, modification notes and source-review evidence with each project asset.
- Back up and import projects as JSON, including saved metadata and revision history.
- Review changes to saved authors or licences without silently overwriting your original record.
- See source freshness, incomplete crawls and missing metadata.
- Inspect recorded update dates and source-relative popularity ranks without treating them as publication dates or a global download ranking.

Magpie reports indexed information; a filter is not a substitute for checking the source licence and any asset-specific instructions. Some creator names and tags are still missing. The interface identifies missing required credits rather than inventing them.

## Try the browser

Node and Python 3 can stage and serve the included catalogue locally:

```powershell
node crawl/publish.cjs
python -m http.server 8231 --bind 127.0.0.1 --directory crawl/build/site
```

Open **http://127.0.0.1:8231**. No account, API key, npm installation or search server is required. Browser storage keeps your credits between visits; use **Back up project** to keep an independent copy.

## Build and maintain the index

The crawler is C99 with SQLite FTS5. Native API adapters and Python adapters share request accounting, robots policy, caching, throttling and cancellation.

On Windows, install GCC and Python 3, then:

```powershell
cd crawl
.\fetch-deps.ps1
.\build.ps1
.\magpie.exe search barrel --type 3d_model --why
.\magpie.exe export
```

The included catalogue can be searched without a local SQLite database. To create your own database, run a bounded crawl, for example `magpie.exe crawl polyhaven --max-requests 20`, before using the CLI search/export commands. An export replaces the previous browser data only after validation.

The [background runner](crawl/runner/README.md) rotates creator enrichment, source-tag enrichment, link checks and periodic API refreshes under shared request caps. Task installation is an explicit local setup step; it is not enabled by opening the website.

## Verification

Verification covers Python regressions, C policy/accounting checks, publication exclusions and isolated browser scenarios. Tests use temporary databases and local fixtures, not live asset crawls. Pages CI also runs the staged site's browser suite on Linux/Chromium.

```powershell
python -X utf8 -B crawl/tests/run.py
cd crawl
npm install
npm test
```

Node and Playwright are development dependencies for browser tests only. See [test setup and implementation details](crawl/PRODUCT_IMPROVEMENTS.md).

## Project documentation

- [Architecture](architecture.md): the implemented crawler, storage and browser design.
- [Hosting](hosting.md): publishing the static site independently of crawling.
- [Runner](crawl/runner/README.md): bounded maintenance jobs and STOP controls.
- [Adapter protocol](crawl/ADAPTER_PROTOCOL.md): adding a source through the shared fetch contract.
- [Scraping fixes](crawl/SCRAPING_FIXES.md): repaired failure paths and verification.
- [Browser and product improvements](crawl/PRODUCT_IMPROVEMENTS.md): credits, exports, freshness and regression coverage.

The other design notes in this repository record earlier exploration; their historical counts and proposals may differ from the current implementation.
