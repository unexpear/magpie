# Scraping fixes — 2026-09-05

Implemented the 15 findings in [the original review](../SCRAPING_REVIEW.md). The optimized Windows executable has been rebuilt. The production index, exported catalogue, and scheduled task were not changed during verification.

## Findings and changes

| Review | Correction |
| --- | --- |
| 1. External adapters bypass controls | Python adapters use `adapters/network.py`, which calls the native `magpie fetch` command. Offline mode, cached responses, robots policy and request limits now use the same implementation. |
| 2. Retries exceed caps | Reserve allowance before every HTTP attempt, including retries, robots requests and redirect hops. |
| 3. Charges roll back with asset batches | Commit request reservations independently before network I/O; refuse network reservations inside asset transactions. Asset writes use short transactions after fetching. SQLite errors stop requests. |
| 4. Enrichment resets budgets | All child processes inherit a shared run identifier; SQLite enforces shared run, host, daily and weekly limits atomically. |
| 5. STOP does not interrupt batches | Check STOP and the runner deadline before requests and during pacing/backoff waits. An already active transport request can finish or time out. |
| 6. OGA loses preferred licences | Deduplicate within each licence traversal, allowing a later, more permissive verified option to replace the earlier one. |
| 7. Robots rules ignored | Added group-aware Allow/Disallow matching, wildcard/end matching, encoding normalization, cached policies and failure handling. Matched Crawl-delay can increase pacing. |
| 8. Throttling ignored | Parse seconds and HTTP-date Retry-After values; persist host cooldowns across processes. Throttled sources fail/defer instead of continuing through the queue. |
| 9. Transient HEAD failures hide assets | Only 404/410 hide assets. Inconclusive checks retry after a day, dead links after seven days, and successful links after thirty days. |
| 10. Empty patches marked enriched | Validate meaningful enrichment; failed/empty patches remain retryable with a one-day defer. Migration requeues the existing OGA row with a missing required author. |
| 11. Partial work reports success | Preserve valid partial rows while returning failure through adapter, CLI and runner. Detect malformed output, truncated GitHub trees, incomplete pages and page ceilings. |
| 12. Invalid scheduler parameter | Removed the unsupported switch; verified the supported default stops on battery and disallows starting on battery. |
| 13. Kenney refresh loses metadata | Preserve curated fields and unseen entries, reject incomplete refreshes, and atomically replace the catalogue only after successful enumeration. |
| 14. Truncated WinHTTP bodies cached | Treat read failures and incomplete uncompressed Content-Length bodies as transport errors. Bound response size and do not cache partial responses. |
| 15. Runner starves checking | Select work from actionable per-source enrichment queues and due link checks; read actual request ledger usage instead of booking guessed costs. |

Related corrections include unique enrichment job files, fresh authoritative tags replacing older tags, atomic FTS rebuilds, strict licence alias parsing, cache validator refresh, RFC URL resolution and build flag/header dependency tracking. ambientCG pagination now uses the documented 250-item limit and checks the reported total. Placeholder contact details were removed from the default User-Agent; deployments can provide `MAGPIE_USER_AGENT` (retain the Magpie product token used by robots policy).

## Plan and implementation scope

The changes preserve the native/Python adapter split. Network policy and accounting were fixed first, then parsing/data preservation, then runner behavior. See [ADAPTER_PROTOCOL.md](ADAPTER_PROTOCOL.md) for the shared fetch contract and [runner/README.md](runner/README.md) for operation.

Existing database upgrades are versioned and transactional. They apply when the rebuilt executable next opens the index. Historical licence classifications require a subsequent successful crawl to receive newly discovered alternatives; no licence was guessed from existing rows.

## Verification

- Optimized Windows build completed successfully.
- `python -X utf8 -B crawl/tests/run.py`: all **34 Python regression tests passed**, plus the C policy, URL, date, licence and durable-accounting assertions.
- Tests use temporary databases, synthetic adapter responses and a loopback HTTP server. Coverage includes concurrent daily limits, weekly limits, GET/HEAD retry caps, offline external crawling, robots failures and redirects, STOP during waits, persistent throttling, incomplete and gzip responses, cache validators, enrichment budgets, dual licences, partial refresh preservation and runner scheduling.
- Migrated a SQLite backup of the real index: **19,359 assets and 19,359 FTS rows preserved**, request ledger unchanged, `quick_check=ok`, schema version 0→1. Exactly one invalid OGA enrichment was requeued. The original database was opened read-only for the backup.
- Task Scheduler settings were constructed and inspected without registering a task: battery restrictions enabled and overlapping instances ignored.

No live asset crawl was run. Current remote HTML layouts, taxonomy IDs and hardcoded source metadata therefore remain subject to live validation. Linux/libcurl runtime behavior was not exercised on this Windows host. Synthetic regression coverage establishes the repaired failure paths, not exhaustive correctness of every remote source.

## Research

Implementation was checked against [SQLite transactions](https://www.sqlite.org/lang_transaction.html), [RFC 9309 robots policy](https://www.rfc-editor.org/rfc/rfc9309.html), [RFC 9110 Retry-After](https://www.rfc-editor.org/rfc/rfc9110.html#name-retry-after), [RFC 3986 URL resolution](https://www.rfc-editor.org/rfc/rfc3986.html), [WinHttpReadData](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpreaddata), [libcurl options](https://curl.se/libcurl/c/curl_easy_setopt.html), [Python subprocess](https://docs.python.org/3/library/subprocess.html), [ambientCG pagination](https://docs.ambientcg.com/api/v2/full_json/), and [Task Scheduler settings](https://learn.microsoft.com/en-us/powershell/module/scheduledtasks/new-scheduledtasksettingsset).
