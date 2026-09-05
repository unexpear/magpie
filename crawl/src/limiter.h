/* limiter.h - per-host rate limiting.
 *
 * The crawl is single-threaded (decisions.md D5), so "concurrency 1 per host"
 * is satisfied by construction and this only has to enforce a minimum gap
 * between requests, a per-run request budget, and 429 backoff.
 */
#ifndef LIMITER_H
#define LIMITER_H

typedef struct limiter limiter;

/* default_interval_ms applies to any host without an explicit rule. */
limiter *limiter_new(int default_interval_ms, int default_budget);
void     limiter_free(limiter *l);

/* Per-host override. budget <= 0 means unlimited. */
void limiter_set(limiter *l, const char *host, int interval_ms, int budget);

/* Blocks until this host is allowed another request.
 * Returns 0 if allowed, -1 if the per-run budget for that host is spent. */
int limiter_acquire(limiter *l, const char *host);

/* Call on 429/503. retry_after_s of 0 means "no header, pick a backoff".
 * Applies exponential backoff with jitter, capped at 5 minutes. */
void limiter_penalize(limiter *l, const char *host, int retry_after_s);

/* Call on a successful response to decay the backoff. */
void limiter_ok(limiter *l, const char *host);

/* Requests actually issued this run, for the crawl summary. */
int limiter_spent(limiter *l, const char *host);
int limiter_total(limiter *l);

/* How many times this host has told us to slow down (429/503) this run.
 * Backing off is not enough on its own: being throttled at all means we were
 * already too fast, so callers use this to stop touching the host entirely. */
int limiter_throttles(limiter *l, const char *host);

#endif
