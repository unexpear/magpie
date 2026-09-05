# Seven free improvements — implementation

Implemented on 2026-09-05 following [the review](../FREE_IMPROVEMENTS.md).

1. **Saved credits and backups.** Exports include stable IDs. Saved projects retain original metadata, detect changes against the current index, and offer explicit adoption of current metadata while keeping revision history. Source URL changes do not create a second selection for the same ID. JSON project import/export preserves editable records and history; invalid imports leave the existing project intact. Existing browser baskets remain readable.
2. **Safe publication.** Export reads one SQLite snapshot, writes a unique file beside the destination, checks database iteration and output errors, flushes output, validates JSON, and replaces the destination only on success. Failed exports preserve the previous file. Source/asset timestamps and IDs are exported with the records.
3. **Metadata maintenance.** The runner rotates missing-creator enrichment, missing-source-tag enrichment, due link checks and weekly API catalogue refreshes. `enrich --missing-tags` targets eligible unprocessed rows without source tags. Automatic full refreshes cover Poly Haven, ambientCG and game-icons; OGA's large HTML catalogue and Kenney's curated pack-list refresh remain explicit maintenance operations. Per-source failures defer that source for a day. Existing request caps, host cooldowns, STOP and deadlines remain enforced. Completed or partial valid database changes are exported safely after each job.
4. **Licence obligations.** Cards show ShareAlike/copyleft markers; details and generated credits explain attribution, change notices and applicable licence obligations. Exact stored licence links remain available. Commercial filter labels describe a conservative filter rather than an unconditional clearance. No historical licence or version was guessed or silently replaced.
5. **Search work.** Query matching is shared across facets, each subset is cached within a render, and relevance is calculated once per matching record before sorting. Existing substring matching and ranking weights are preserved. The browser still works entirely against static local data.
6. **Freshness.** Source summaries distinguish latest observation, last complete crawl, latest attempted crawl and completion state. Per-asset source/check dates appear in details. Export generation time is explicitly separate. Older data with no crawl-history record says so. Interrupted attempts remain visible as “running or interrupted”; an export cannot manufacture a successful crawl date.
7. **Regression coverage.** Native/Python tests cover export failure preservation, exported identity/freshness, tag-queue targeting and job rotation alongside scraping controls. Browser tests cover credits updates/history, licence-change review, stable identity, backup import/reload, bad imports, filters, obligations, modal keyboard isolation, freshness, malformed indexes and the full catalogue. Tests also found and fixed closed dialogs remaining visible because of a CSS display override.

## Run checks

Build the executable with `crawl/build.ps1`, then from the project root:

```powershell
python -X utf8 -B crawl/tests/run.py
cd crawl
npm install
npm test
```

The browser suite uses installed Microsoft Edge on Windows. Elsewhere install Playwright Chromium (`npx playwright install chromium`); `MAGPIE_BROWSER_CHANNEL` can select another installed supported channel. The pinned development dependency is only for tests; the website has no npm/runtime dependency. All browser requests are intercepted with local fixtures, including the full existing index. External thumbnails are deliberately blocked in tests.

## Verification results

- Optimized Windows executable rebuilt successfully.
- 40 Python regression cases passed across the full run and the focused final export checks, plus the C policy/accounting assertions.
- All 11 isolated Edge browser scenarios passed, including the full 19,359-asset index and a 390px mobile viewport.
- Full-browser search/render checks for `dungeon` and `stone wall` took roughly 13–17 ms on this host. Broad queries remained around 100–140 ms. These are local observations, not cross-device performance guarantees.
- The browser data file was regenerated from a read-only backup of the existing database and validated using the browser's actual schema validator before replacement. All 19,359 assets have stable IDs. The original database remains at schema version 0 with its records unchanged; its normal next open by the native executable applies pending schema changes.
- The new export is 8,825,557 bytes, or 876,459 bytes with local gzip. Actual hosting compression still depends on the server.
- No live asset requests or scheduled-task changes were made. Linux runtime behavior was not tested here.

## Operational scope

The metadata backlog is not instantaneously filled by these changes: source requests still need the normal paced runner jobs. No paid API, backend, hosting account, deployment, or new scheduled task is needed. The runner's existing installation picks up its updated code. No broad live crawl was run during verification.

Research references: [SQLite snapshot isolation](https://www.sqlite.org/isolation.html), [Windows replacement semantics](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw), [HTML dialog behavior](https://developer.mozilla.org/en-US/docs/Web/HTML/Reference/Elements/dialog), [Playwright assertions](https://playwright.dev/docs/test-assertions), [CC BY-SA obligations](https://creativecommons.org/licenses/by-sa/4.0/), and [GNU commercial-use FAQ](https://www.gnu.org/licenses/gpl-faq.html#DoesTheGPLAllowMoney).
