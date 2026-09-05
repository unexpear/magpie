# Background runner

**Status: built.** See [crawl/runner/](crawl/runner/) — `runner.py`, `status.html`,
`STOP.cmd`, `install-task.ps1`. The budget lives inside `magpie` as `--max-requests`,
not in the wrapper.

Two things the design got wrong, found by running it:

- **A link check is not a crawl.** At the 1 request/sec used for crawling, a HEAD sweep
  drew repeated 429s from ambientcg.com and game-icons.net — hosts the API crawls never
  troubled. A crawl touches a host a handful of times; a check touches it once per
  indexed asset. `magpie check` now runs at 2s and **gives up on a host after 3
  throttles**, which is this document's own "429 means we were already too fast" rule
  finally enforced in code rather than only written down.
- **The adaptive `5 × latency` delay is still not implemented.** The limiter has no
  latency feedback, so the floor is doing all the work. Stated here rather than left to
  look finished.

The idea: `magpie check` and `magpie enrich` are inherently slow — OpenGameArt asks for 10s
between requests, so enriching its 4,135 rows is ~11 hours and link-checking 9,387 URLs
is hours more. That work should not be a thing you sit and watch. It should be a trickle
running quietly on the PC, with a control panel you can understand in two seconds.

## The one principle

**The scarce resource is not CPU, bandwidth, or time. It is social licence.**

Nobody at OpenGameArt agreed to us indexing them. Their robots.txt is the closest thing
to a contract we have, and it costs them real money to serve us. The whole design target
is that a sysadmin there never notices we exist. Every other decision falls out of that:

- Slow is not a limitation to engineer around. Slow **is the feature**.
- Throughput is not a metric we optimise. Requests-not-made is.
- Unattended means every failure mode must degrade to *doing nothing*, never to
  *retrying harder*.

A background process is more dangerous than a foreground one precisely because nobody is
watching it. A bug in a script you run by hand wastes your afternoon. The same bug in a
nightly job hammers someone's server for a week before you notice.

## What it runs

Only the slow, resumable, already-built commands. Nothing new, nothing clever:

| Job | Cadence | Why it is safe to leave alone |
|---|---|---|
| `magpie check --limit N` | nightly | HEAD only, resumable, no writes to anyone else |
| `magpie enrich --limit N` | nightly | one page per asset, bounded batch, resumable |
| `magpie crawl` | weekly | 4 requests for the fast sources |
| `magpie reclassify` | after enrich | local only, zero network |
| `magpie export` | after any change | local only |

It never invents work. If there is nothing unchecked and nothing unenriched, it sleeps.

**Lead with the uncreditable backlog.** `magpie stats` reports how many assets demand a
credit we cannot supply — 3,389 of 19,359 at the time of writing, almost all OpenGameArt.
Those are the rows whose licence the index cannot currently honour, and enrichment now
targets them first (decisions.md D15). At the 10s crawl delay that backlog is **9.4
hours**, versus 27.5 to enrich everything.

That makes a far better goal than "keep enriching": it is measurable, it maps onto the
promise the tool makes, and — unlike the general backlog — **it ends**. Show it as the
headline number in the window, counting down.

## Controls

One small window (or tray icon). Everything visible without scrolling, everything
reversible with one click.

```
┌────────────────────────────────────────────────┐
│  magpie background             ●  RUNNING      │
│                                                │
│     [  ■  STOP EVERYTHING  ]                   │
│                                                │
│  Now:  enrich · opengameart                    │
│        next request in 12s                     │
│                                                │
│  Today  ▓▓▓▓▓▓▓░░░░░░░░░░░░  142 / 500         │
│  Week   ▓▓▓▓░░░░░░░░░░░░░░░  890 / 3000        │
│                                                │
│  polyhaven    ▶ on    1/s     12 today         │
│  ambientcg    ▶ on    1/s      3 today         │
│  kenney       ▶ on    n/a      0 today         │
│  opengameart  ▶ on    1/10s  127 today         │
│                                                │
│  Quiet hours  09:00–18:00   [edit]             │
│  Last error   none                             │
│                                                │
│  [ what did it do?  ]   [ open web UI ]        │
└────────────────────────────────────────────────┘
```

Rules for the UI itself:

- **STOP EVERYTHING is the biggest thing on screen.** It stops mid-request and does not
  resume until you say so. No confirmation dialog — stopping must never be harder than
  starting.
- **The budget bar is the main readout**, not a throughput number. You should be able to
  glance at it and know whether today was quiet.
- **Per-site rows each have their own toggle.** If one site is having a bad day you can
  pause exactly that one without stopping the rest.
- **Tray icon colour is the whole status at a glance:** grey idle · blue working ·
  amber paused by a guard · red stopped by an error. If you have to open the window to
  know whether something is wrong, the icon has failed.
- **"What did it do?"** opens a plain readable log: timestamp, host, URL, status. This is
  the thing you show someone who asks what your PC was doing at 3am.
- No settings buried in menus. If a knob is not on this window, it does not exist.

## Industry practice (checked Aug 2026)

Worth grounding this in what real crawlers do, because the instinct to pad a fixed number
turns out to be the wrong shape entirely.

| Crawler / standard | What it actually does |
|---|---|
| **RFC 9309** (2022) | **Deliberately omits `Crawl-delay`** — there was no consistent real-world behaviour to standardise |
| **Googlebot** | Ignores `Crawl-delay` outright; argues crawl rate should come from *dynamic server-capacity signals*, not a static line in a text file |
| **Mercator** (the canonical crawler paper) | Delay before the next request to a host = **10× the time the last fetch from that host took**. Explicitly to consume "a bounded fraction" of the server's resources |
| **Scrapy AutoThrottle** | `target_delay = latency / target_concurrency` (default concurrency **1.0**). Next delay = mean of current and target. Floor `DOWNLOAD_DELAY`, ceiling **60s**, start **5s** |
| **Common Crawl (CCBot)** | "Waits a few seconds"; adaptive back-off on **429 or 5xx**; obeys `Crawl-delay` |
| **Heritrix** (Internet Archive) | **Serialised — one request at a time per host** — plus multi-second pauses |

Three things fall out of this:

**1. The consensus is adaptive, not a fixed number.** Every serious crawler scales its
delay to the server's *observed response time*. `Crawl-delay` is a floor those crawlers
respect (or, for Google, ignore) — not a target to pad.

**2. Latency-proportional beats uniform padding on both axes.** A fixed +50% is slower
than necessary when the host is healthy and *no safer* when it is struggling. Scaling to
latency automatically backs off exactly when the server starts hurting, which is the
only moment extra delay actually buys anything.

**3. Scrapy's subtlest rule is one this design would have got wrong:** *latencies of
non-200 responses must never be allowed to decrease the delay.* Error pages are fast —
a 429 or a 503 returns almost instantly — so naive latency-proportional throttling
**speeds up precisely when a server is rejecting you.** That is a real bug class, and
it is now an explicit rule below.

## Hard limits

```
global      500 requests/day     persisted in the database, hard stop
per host    max(robots.txt Crawl-delay, 5 × last observed latency), cap 60s
concurrency 1 request in flight, globally
batch       enrich 50/night, check 200/night
```

**The daily figure is kept in SQLite and charged per request**, so it binds a human
typing commands exactly as much as the scheduler. It had to be: a version where the
daily count lived in the runner let hand-run batches reset it every time, and ~1,208
requests reached OpenGameArt on a day capped at 500. Every batch was properly paced;
the day was not. `magpie budget` shows the ledger. See decisions.md D16.

**Per-host delay** is latency-proportional, floored by whatever `robots.txt` asks for and
capped at Scrapy's 60s ceiling.

**The multiplier is 5** — the midpoint of the two industry anchors, which disagree by a
factor of ten: Mercator waits 10× the last fetch time, Scrapy AutoThrottle effectively
waits 1× (`latency / target_concurrency`, default concurrency 1.0). Neither is wrong;
they were built for different jobs. Mercator was indexing the whole web and could afford
to be maximally deferential to any single host. Scrapy is usually pointed at one site by
someone who wants their data today.

We are neither. 5× splits it. (The log-space midpoint would be ~3, which is arguably the
more principled way to average a ratio — but 5 is the plain reading of "halfway", and it
is the slower of the two, which is the right way to break a tie here.)

Worked through for OpenGameArt: they respond in roughly 0.3–0.5s, so 5× is 1.5–2.5s and
their stated 10s floor wins — we sit at 10s, and the 11-hour job stays 11 hours. The
adaptive rule only takes over once their latency passes **2s**, at which point something
is genuinely wrong on their end and backing off is exactly right. Under Mercator's 10×
that trigger sat at 1s, which would have had us slowing down for ordinary page-load
jitter.

The earlier "+50%" rule would have parked us at 15s permanently: slower than needed on a
good day, and no safer on a bad one.

Note this leaves us *stricter than Googlebot*, which ignores `Crawl-delay` entirely. That
is the right side to err on for a hobby project with no crawl budget to defend.

**Global concurrency 1** is stricter than the industry norm — Heritrix serialises per
host but runs many hosts at once; Scrapy defaults to 8 per domain. Being honest about
why: this is not politeness, it is blast radius. We have no deadline, and one request in
flight is impossible to get wrong. Revisit only if the runner ever has real work to do.

**500/day** is ~83 minutes of continuous crawling at a 10s delay. Against a site that
serves Googlebot without noticing, this is nothing — which is the intent.

## Fail-closed rules

Every one of these ends with *stop*, never *retry harder*:

- **429 or 503 → that host is done for 24 hours.** Not backoff-and-continue. If a site
  told us to slow down once, we have already been too fast, and nobody is watching to
  catch a second mistake. (Common Crawl merely backs off here; we stop, because they have
  an operations team and we do not.)
- **A non-200 response may never shorten the delay.** Errors return fast, so feeding
  their latency into the 5×-latency rule would make us speed up exactly when a server is
  rejecting us. Straight from Scrapy's AutoThrottle, and worth stating as a rule rather
  than hoping the arithmetic works out.
- **robots.txt unreadable → do not crawl that host at all.** A 5xx, a timeout or a DNS
  failure means we could not ask, and not being able to ask is not permission.
  **A clean 404 is different**: RFC 9309 says an absent robots.txt means no restrictions,
  so that host is fair game at our normal limits. kenney.nl is the live example — it
  serves no robots.txt at all. Worth keeping the two apart, because collapsing them
  either blocks a site that never objected or waves through one that is merely broken
  today.
- **robots.txt changed since last run → pause that host, tell the user.** They may have
  just added a Disallow that covers us.
- **3 consecutive errors on a host → disable it and surface it.**
- **Unexpected redirect off-origin → stop, do not follow.**
- **Budget exhausted → sleep until tomorrow.** Never borrow against it.
- **Clock jumped, config unreadable, DB locked → do nothing this cycle.**
- **Any unhandled exception → exit, do not restart automatically.** A crash loop is a
  DDoS with extra steps.

## Carefulness: robots, terms, law

*Not legal advice. These are the house rules; revisit them properly before anything is
published publicly or run at real scale.*

**robots.txt is the contract.** Fetch it, cache it for 24h, obey `Disallow` and
`Crawl-delay`. The adapter already does this at runtime rather than hardcoding — keep it
that way, so a site can throttle us without asking.

**Terms of service are separate from robots.txt** and can forbid automated access even
where robots.txt is permissive. Record the ToS position per source in
[sources.md](sources.md), with the date checked. An unchecked source does not get a
background job — manual runs only.

**Identify honestly.** Real User-Agent with a contact URL, so a sysadmin can email us
instead of blocking us. Answer that address.

**If a site asks us to stop, stop the same day** and remove their rows. Worth deciding in
advance that this is unconditional, because in the moment there will be a temptation to
negotiate.

**Metadata only, never mirror the asset.** Already a project rule ([concept.md](concept.md));
the background runner must not become the thing that quietly breaks it. Deep-link to the
human page, never to a direct download.

**Thumbnails remain the grey area** — see [holes.md](holes.md). Caching someone's image
is the one place we hold their file. Until that is settled, the runner should not
bulk-download thumbnails; hotlinking spends their bandwidth and caching copies their
work, and "we haven't decided" is a reason to do neither at scale.

**Author names are personal data** in some jurisdictions. We store them because CC-BY
attribution is impossible without them, which is a decent purpose-limitation argument —
but it means: store only what is publicly published, never emails or profile scrapes, and
have a way to remove a person on request. Write that path down before anyone needs it.

**Nothing behind a login.** No accounts, no auth, no session cookies, no CAPTCHA solving,
no rate-limit evasion, no rotating IPs or user agents. If access requires pretending to
be something we are not, we do not have access.

## Never

- Run faster because "it's just a small backfill this once"
- Retry a 429 within the same day
- Crawl a host whose robots.txt we could not read
- Download asset files or bulk-download thumbnails
- Auto-restart after a crash
- Hide what it is doing behind a spinner with no detail
- Make stopping harder than starting

## Implementation sketch (Windows)

The jobs already exist and are resumable, so the runner is a scheduler and a window —
not new crawling logic. Resist putting any request-making code in it.

- **Scheduling:** Task Scheduler is the boring correct answer — survives reboots, no
  service to install, and the OS already solved "run this nightly". A tray app that
  shells out to `magpie.exe` is the alternative if the live status readout matters more.
- **State:** the SQLite index is already the source of truth for what is left to do
  (`last_checked IS NULL`, `enriched = 0`). The runner needs almost no state of its own —
  just today's request count and the pause flags. One small JSON file.
- **Budget accounting belongs in `magpie`, not the runner.** A `--max-requests N` flag that
  the C code enforces means the limit holds even when someone runs the command by hand.
  A limit that only exists in the wrapper is not a limit.
- **The log is the SQLite `http_cache` table plus a plain text request log.** Append-only,
  human-readable, no rotation cleverness.

## Open questions

- **Tray app or just Task Scheduler + a status page?** The web UI already exists; a
  `runner.json` written each cycle and rendered on a tab of the existing page might beat
  a native window for a tenth of the work. Probably start there.
- **Quiet hours in whose timezone?** Ours is easy and wrong; theirs is right and needs a
  guess per host. Maybe just avoid a fixed UTC window that covers most of the US/EU
  working day.
- ~~**Does the +50% crawl delay padding actually help?**~~ **Resolved:** no — it was the
  wrong shape. Fixed padding is not what any serious crawler does. Replaced with
  Mercator's latency-proportional rule floored by `robots.txt`, which is both the
  industry norm and, for OpenGameArt specifically, *faster* (10s rather than 15s) while
  being genuinely safer when their server is struggling. See Industry practice above.
- **What is the honest completion target?** Enriching all of OpenGameArt at these limits
  is weeks of calendar time. That may be fine — but it should be a stated expectation,
  not a surprise. At 50/night it is ~82 nights for the 4,118 rows still unenriched.
- ~~**Should the adaptive multiplier be 10 (Mercator) or ~1 (Scrapy)?**~~ **Settled at 5**,
  the midpoint. Still worth knowing that it is **untested in practice**: every source we
  currently index publishes a `Crawl-delay` that dominates the calculation, so the
  multiplier does nothing until we add a source without one. First such source is the
  time to check the number against reality rather than against two papers.
- **Should the runner ever crawl *new* sources unattended**, or only maintain existing
  ones? Leaning strongly toward: new sources are always a manual, watched, first run.

## Sources

- [RFC 9309 — Robots Exclusion Protocol](https://datatracker.ietf.org/doc/html/rfc9309) — the 2022 standard, and what it left out
- [Mercator: High-Performance Web Crawling](https://www.cs.cornell.edu/courses/cs685/2002fa/mercator.pdf) — Heydon & Najork, the 10×-latency politeness rule
- [Scrapy AutoThrottle](https://docs.scrapy.org/en/latest/topics/autothrottle.html) — algorithm and defaults ([source .rst](https://github.com/scrapy/scrapy/blob/master/docs/topics/autothrottle.rst))
- [Common Crawl FAQ](https://commoncrawl.org/faq) — CCBot's adaptive back-off and Crawl-delay support
- [Heritrix requirements](https://github.com/internetarchive/heritrix3/wiki/Internet-Archive-Crawler-Requirements-Analysis) — Internet Archive, serialised per-host politeness
- [Does Google respect Crawl-delay?](https://devendergupta.netlify.app/blog/crawl-delay-googlebot/) — background on why Google declined it
