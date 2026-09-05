# Adapter protocol

Native adapters remain C functions. External adapters emit UTF-8 JSONL assets on
stdout and diagnostics on stderr. Python is required only for external adapters.

## Requests and controls

External adapters must use `adapters/network.py`, which invokes `magpie fetch`.
Do not use a separate HTTP client. The core fetcher enforces:

- A shared per-run cap (`--max-requests`), daily cap (`--daily-cap`, default 500),
  and weekly cap (default 3000, configurable with `MAGPIE_WEEKLY_CAP`). Zero disables
  the specified cap. All commands using the same database share daily/week charges.
- Atomic, durable reservation before each request, including robots, redirects,
  retries, and enrichment. Interrupted network work can overcount, never refund.
- Per-host pacing across processes, robots group/path rules, a 24-hour robots
  cache, and persistent cooldown on 429/503. Explicit Retry-After is never shortened;
  the host is deferred rather than keeping a process asleep for hours.
- Cancellation before requests and during waits. `MAGPIE_STOP_FILE` defaults to
  `runner/STOP`; the runner also supplies a deadline.
- `--offline`: cached GET responses only. Missing cache is a failure, with no
  network fallback. The curated Kenney catalogue still requires no network.

The parent supplies `MAGPIE_EXE`, `MAGPIE_DB`, `MAGPIE_RUN_ID`,
`MAGPIE_MAX_REQUESTS`, `MAGPIE_DAILY_CAP`, and `MAGPIE_OFFLINE` to children.
Standalone maintenance commands establish their own run ID and use the same ledger.
`MAGPIE_USER_AGENT` can supply a project-specific UA with real contact information;
the default describes Magpie and contains no invented contact address.

```python
from network import get_text
page = get_text(url)
```

A missing core executable, malformed bridge response, or refused request raises
`FetchError`. Stop the source on throttling, cancellation, budget refusal, or
transport/policy failure. Do not continue with another URL on that host.

## Asset records

Required fields: `id`, `source`, `title`, `source_url`. IDs must be namespaced as
`source:local_id`, and the source must match the registered adapter. Optional fields
include `author`, `thumb_url`, `licence`, `licence_url`, `asset_type`, `style`,
`formats`, `tags`, `rigged`, `tileable`, `polycount`, `price`, `updated_at`,
`popularity`, and `rank_hint`. Formats and tags are arrays of strings.

`commercial_ok` and `attribution` are derived in C; never send them as authority.
Use recognized licence enums (`cc0`, `cc_by`, `cc_by_sa`, `oga_by`, `gpl`,
`store_eula`, `personal_only`, `unknown`). Unknown or unsupported spellings remain
unknown; never infer permission from a substring.

For dual-licensed sources, emit each verified licence alternative in the declared
least-to-most-permissive order. Deduplicate within a licence, not across licences.
The final upsert selects the last verified alternative.

Malformed records are skipped, but make the overall command fail. Partial valid
results remain saved. A nonzero child exit is propagated even after valid output.
A deliberately capped/incomplete OpenGameArt listing returns nonzero too.

## Enrichment

The core writes a unique job file containing `id<TAB>url` rows and calls the
registered enrichment command with its path. The adapter emits patches such as:

```json
{"id":"opengameart:example","author":"Creator","tags":["plant"]}
```

Only `author`, `tags`, and `thumb_url` are applied. A useful patch marks the asset
enriched; an empty patch is a failure. Validate the asset-page structure before
parsing. When a fetched page is missing or cannot be parsed, emit
`{"id":"opengameart:example","retry":true}` and finish with nonzero status.
Those rows become eligible again after 24 hours. Budget/cancellation failures leave
unprocessed jobs immediately eligible for a later run.

Enrichment can correct previously stored nonempty fields. A normal listing crawl
preserves enrichment when that listing carries no tags, author, or thumbnail.
Fresh native source tags replace older tags even when the new list is shorter.
The core reclassifies and rebuilds search after applying patches, including partial
successful batches. `--redo-blank` explicitly resets missing-author retry state.

## Verification

Build, then run `python -X utf8 tests/run.py`. Tests use temporary databases,
synthetic source responses, and a loopback HTTP server; no live source crawl occurs.
