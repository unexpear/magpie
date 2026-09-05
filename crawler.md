# Crawler politeness + rate limiting

## Reality check on the numbers

Before designing anything, count the actual load. A full Tier-1 crawl:

| Source | Requests per full crawl | Why |
|---|---|---|
| Poly Haven | **1** | `/assets` returns the entire catalogue in one response |
| ambientCG | **1** | `/full_json` returns everything in one response |
| Kenney | ~5 | One listing page + pagination |
| Quaternius | ~3 | Small site |
| **Tier 1 total** | **~10 requests/week** | |

Ten requests. Per week. You could not overload these sites if you tried. The bulk-JSON
endpoints exist precisely so people stop hammering per-asset endpoints — use them.

The heavy ones are Tier 2/3:

| Source | Requests | Strategy |
|---|---|---|
| Freesound | ~200 for 30k sounds | 150/page, well inside the daily cap |
| OpenGameArt | ~1,500 | Scrape, paginated, spread over nights |
| Sketchfab | ~5,000+ | 24/page — must be spread across multiple days |
| itch.io | large | Don't build until you've asked them |

## Published limits (verified Aug 2026)

| Source | Documented limit | Auth |
|---|---|---|
| **Freesound** | **60/min, 2000/day** per key; 429 on breach | Free API key |
| Poly Haven | **None published.** Requires a unique `User-Agent` | None |
| ambientCG | None published | None |
| Sketchfab | None published; 429 exists | OAuth/token |
| GitHub API | 5000/hr authenticated | Token |

Only Freesound states a number. **Absence of a published limit is not permission** — it
means the ceiling is your own judgment, and if you're the reason they add a limit, you
were the problem.

This is also why `Crawl-delay` in robots.txt is worth honouring even though **RFC 9309
deliberately omits it and Googlebot ignores it**: it is the only number a site
volunteers, and we have no crawl budget to defend. Treat it as a *floor*, then scale
above it with the server's observed latency — see
[background-runner.md](background-runner.md) for the industry comparison.

## Self-imposed defaults

Tighter than anything they'd enforce. These cost you nothing because the crawl is offline.

```
default per-domain:
  concurrency        1          # never parallelize against one host
  rate               1 req/sec  # 0.5 req/sec for unknown/small sites
  budget per run     500 req    # hard stop, then resume next run
  timeout            30s
```

Per-source overrides live next to the adapter, not in a global config.

## Rules

**Token bucket per domain.** Not global. One slow site must not starve the others, and a
burst against one host is the thing that actually hurts.

**Concurrency 1 per host, always.** Parallelism across *different* hosts is fine and free.
Parallelism against *one* host is the entire problem.

**Conditional requests.** Send `If-None-Match` / `If-Modified-Since`, store the ETag.
A `304 Not Modified` costs the source almost nothing. After the first crawl, most of a
re-crawl should be 304s — this is the single biggest politeness lever available.

**Honour `Retry-After`.** On 429 or 503, sleep exactly what they ask, then exponential
backoff with jitter (2s, 4s, 8s… cap 5 min). Jitter matters — no thundering herd on retry.

**Circuit breaker.** 3 consecutive failures → disable that adapter for 24h and log it
loudly. A broken adapter retrying in a loop is how you get IP-banned.

**A link check is not a crawl, even at the same rate.** Measured Aug 2026: a HEAD sweep
at 1 request/sec drew repeated 429s from **ambientcg.com** and **game-icons.net**, which
the API crawls never provoked. A crawl touches a host a handful of times; a check touches
it once per indexed asset — thousands. `magpie check` therefore runs at 2s, and **stops
touching a host entirely after it has throttled us 3 times**. Backing off and carrying on
is not enough when several thousand more requests to the same host are queued behind you.

**Honest User-Agent.** Poly Haven *requires* a unique one. Include a contact URL so a
sysadmin can email you instead of blocking you:

```
Magpie/0.1 (+https://github.com/<you>/<repo>; contact@example.com)
```

**robots.txt.** Fetch it, cache it, obey it — including `Crawl-delay`. For any scraped
(non-API) source this is non-negotiable.

**Cache raw responses to disk.** During development you will re-run the crawler fifty
times. Every one of those hitting the live API is pure waste and pure rudeness. Cache
raw responses locally with a long TTL and iterate against the cache.

**Incremental after the first sweep.** Cursor on `updated_at`, pull only what changed.
Full re-crawl monthly at most, and only for sources without a change feed.

**Prefer the feed.** ambientCG has `/rss`, many sites have change feeds. A feed poll is
one cheap request and tells you whether a crawl is needed at all.

## Sketch

```python
class DomainLimiter:
    # token bucket + single-slot semaphore, per host
    # .acquire() blocks until a token is free
    # .penalize(retry_after) drains the bucket on 429

async def fetch(url, limiter, cache):
    if hit := cache.get(url):          # dev cache, long TTL
        return hit
    await limiter.acquire()
    headers = {"User-Agent": UA}
    if etag := cache.etag(url):
        headers["If-None-Match"] = etag
    r = await client.get(url, headers=headers)
    if r.status_code == 304:
        return cache.get_stale(url)     # free win
    if r.status_code == 429:
        limiter.penalize(r.headers.get("Retry-After", 60))
        raise Retryable()
    cache.put(url, r)
    return r
```

Every adapter goes through this. No adapter gets its own HTTP client.

## The load test that matters

Not "can my site handle 10k users" — static hosting handles that trivially. The real
test is: **does source load stay flat as users grow?** With the crawl model it does, by
construction. If you ever find yourself adding a live API call to a user request path,
that's the design breaking.
