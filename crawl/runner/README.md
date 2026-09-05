# Background runner

The runner rotates bounded creator enrichment, source-tag enrichment, due link
checks and weekly catalogue refreshes. Missing tags use `enrich --missing-tags`.
Automatic catalogue refresh covers Poly Haven, ambientCG and game-icons; the large
OGA HTML crawl and curated Kenney catalogue refresh remain explicit operations.
A source that fails is deferred for a day, and other queues can keep progressing.
Each executed job is followed by a validated atomic export, including valid partial
progress. The runner does not bypass network caps to finish a queue.

```powershell
python runner.py --dry-run
python runner.py --status
.\install-task.ps1
```

Task Scheduler runs every two hours, on mains power, with no overlapping scheduled
instances. Quiet hours are 09:00–18:00 local. Installation uses the cmdlet's default
stop-on-battery behavior; it does not pass an unsupported parameter.

## Limits and stopping

- 120 requests per tick, 500/day, 3000/calendar week (Monday start, local date).
- Request counters come from the SQLite ledger, including manual commands. State
  files no longer estimate spend by booking a whole batch.
- All adapters call the C fetcher. Robots requests, redirects and retries count.
- `STOP.cmd` prevents subsequent requests in active work as well as future ticks.
  A request already in flight may finish. `RESUME.cmd` clears the stop file.
- The runner supplies an absolute STOP path and a deadline to the entire child
  chain. Waiting for pacing is interruptible; a timeout does not grant fresh budget.
- 429/503 persist a host cooldown. The next run cannot erase that cooldown.
- Failure/incomplete status propagates from adapters to the runner's status page.

The runner reads the index with SQLite read-only mode. A missing, corrupt or
unreadable index is an error, never a reason to invent a job. Run a current Magpie
command once to migrate an older index before enabling the runner.

## Link health and retry queues

Only 404/410 are hidden as dead. Other HTTP statuses remain visible and are
inconclusive. Dead links become due again after seven days, successful checks after
30 days, and inconclusive checks after one day. Previously unchecked links are due
immediately. Parse failures during enrichment have a one-day retry delay.

`status.html` reads `status.json`; `state.json` retains last-run/error information.
`runner.log` records decisions. The status page's unchecked count now means URLs due
for checking, including rechecks. It is a snapshot, not a live process supervisor.

## Tests

From `crawl/`, build and run `python -X utf8 tests/run.py`. Tests cover cancellation,
request caps, persistent cooldown, actionable source selection, retries and partial
failures without contacting source sites or registering a scheduled task.

Browser and publication tests: see [PRODUCT_IMPROVEMENTS.md](../PRODUCT_IMPROVEMENTS.md).
