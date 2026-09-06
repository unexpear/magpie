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

## Populate the lightweight catalogue

`python runner.py --populate` runs one refresh of Poly Haven, ambientCG and the
game-icons repository. It does not fetch asset downloads or install a schedule.
The three sources share 120 requests and a 15-minute deadline; the durable
500/day and 3000/week ceilings, host cooldowns and STOP file still apply.
An explicit population run can run during the background runner's quiet hours.
Valid partial results are exported, with incomplete sources reported honestly.
A failed source is not repeatedly retried by this command.

The native store caps new records at 50,000, the database at 128 MiB (WAL/journal
files are additional), and cached response bodies at 32 MiB. Old response cache
entries are evicted as needed; searchable asset metadata and the request ledger
are retained. Before each network request, less than 2 GiB available on the
index drive, or inability to measure it, stops further requests. An in-flight
response can finish. SQLite reuses freed cache pages rather than continuously
shrinking its file.

Population refuses a metadata export over 24 MiB. The publication step independently
rejects more than 50,000 records or over 25 MiB across its four public files before
replacing staged files. These are conservative project limits, not provider quotas.
GitHub Pages currently limits sites to 1 GB and has a soft 100 GB/month bandwidth
limit: https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits
Crawler limits cannot measure or cap visitor bandwidth. No paid services or
billing upgrades are enabled by this command. A substantially larger catalogue
would need a different search-loading design before raising these limits.

The public site stores only metadata and links. Allowed previews load from their
original CDN/repository URLs in the visitor's browser; asset files stay on source
sites. The local SQLite index and bounded cache are needed to maintain the index;
zero storage would mean losing local search and crawl accounting. Git history,
build tools and the browser's own cache are separate from these catalogue limits.
