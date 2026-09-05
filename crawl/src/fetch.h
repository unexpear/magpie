/* fetch.h - the only way anything in this program touches the network.
 *
 * Wraps http_get with: per-host rate limiting, conditional requests against
 * the SQLite cache, robots policy, durable limits and host cooldown. No adapter gets its own
 * HTTP client. See crawler.md.
 */
#ifndef FETCH_H
#define FETCH_H

#include "limiter.h"
#include "store.h"
#include "util.h"

typedef struct {
    limiter *lim;
    store   *st;
    int      offline;      /* 1 = never hit the network, cache only */
    int      max_retries;
    int      n_requests;   /* network requests actually issued */
    int      n_cached;     /* served from cache or a 304          */
    /* Hard ceiling on network requests for this logical run, from --max-requests.
     * 0 = unlimited. Lives here rather than in the background runner so the
     * limit holds even when someone runs the command by hand - a budget only
     * the wrapper knows about is not a budget. */
    int      max_requests;
    int      budget_hit;   /* set once a ceiling stopped a request */

    /* Daily ceiling, persisted in the database and shared by every process.
     * This is the one that actually protects a source: --max-requests only
     * bounds a single run, so running the command repeatedly by hand walked
     * straight past it. */
    int      daily_cap;    /* 0 = no daily limit */
    char     day[11];      /* YYYY-MM-DD */
    long long day_used;    /* spent today, including previous runs */
    char run_id[80];
    int interval_ms;
    int host_cap;
    int weekly_cap;
    int last_status;
} fetcher;

/* Load today's spend from the ledger and arm the daily cap. Call after
 * fetch_init. Returns requests remaining today. */
long long fetch_arm_daily(fetcher *f, int daily_cap);

void fetch_init(fetcher *f, limiter *lim, store *st);

/* Appends the response body to *body. Returns 0 on success. */
int fetch_url(fetcher *f, const char *url, sbuf *body);

/* Status only, rate limited the same way. Returns the HTTP status, or 0 if
 * the request could not be made at all (which is not the same as a dead link
 * and must not be recorded as one). */
int fetch_status(fetcher *f, const char *url);
/* Configure inherited adapter/helper settings for this logical run. */
void fetch_export_env(fetcher *f);

#endif
