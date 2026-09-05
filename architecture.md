# Architecture

Magpie is a metadata crawler and a static browser application. It does not query source APIs when a user searches.

## Data path

1. Native C adapters and external Python adapters fetch source metadata through the same C fetcher.
2. SQLite stores normalized assets, source tags, inferred tags, cached HTTP responses, request reservations and source health.
3. A validated export writes browser JSON from a consistent database snapshot and replaces the previous file after successful completion.
4. The browser filters and ranks that catalogue locally. Preview images and download links point to the original sources.

## Crawler

`crawl/src/fetch.c` enforces cached robots policy, redirect checks, request caps, retries, persistent host cooldowns and STOP/deadline cancellation. SQLite reserves each actual request before network I/O. Asset transactions do not contain network requests.

`crawl/adapters/network.py` lets external adapters use this same path through `magpie fetch`; they do not issue independent HTTP requests. See the [adapter contract](crawl/ADAPTER_PROTOCOL.md).

The classifier records source and inferred tags separately. CLI search uses SQLite FTS5. Browser search uses shared query matches, cached facet subsets and one relevance calculation per result; it does not use SQLite HTTP-vfs.

## Data and trust

Stable IDs namespace each asset by source. Licence categories are conservative indexing policy; exact stored licence links and source pages carry the terms. Unknown facts remain unknown. Saved credits preserve their original metadata and can retain earlier revisions when the user accepts an update.

Source health separates observation time, attempted crawl time, successful complete crawl time and link-check time. A newly generated export does not imply newly fetched metadata. Only HTTP 404/410 hide records as dead; due rechecks allow recovery.

## Maintenance

The local Python runner rotates creator/tag enrichment, due link checks and weekly API catalogue refreshes. It uses the database's actual request ledger, defers failures and exports valid progress safely. OGA's large HTML catalogue crawl and Kenney's curated pack-list refresh remain explicit operations.

The public site consists of `crawl/web/index.html`, `model.js` and `data.json`. It needs a static HTTP host. Crawling and publishing are separate operations; persistent crawler state must survive between runs.
