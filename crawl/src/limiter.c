#include "limiter.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HOSTS   32
#define BACKOFF_CAP_MS (5 * 60 * 1000)

typedef struct {
    char      host[128];
    int       interval_ms;
    int       budget;        /* <= 0 = unlimited */
    int       spent;
    long long next_ok_ms;    /* earliest time the next request may go out */
    int       backoff_ms;    /* current penalty, doubles on repeat 429 */
    int       throttles;     /* times this host has said slow down */
} hostrule;

struct limiter {
    hostrule rules[MAX_HOSTS];
    int      n;
    int      def_interval_ms;
    int      def_budget;
    int      total;
    unsigned rng;
};

limiter *limiter_new(int default_interval_ms, int default_budget)
{
    limiter *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->def_interval_ms = default_interval_ms;
    l->def_budget      = default_budget;
    l->rng             = 0x9e3779b9u ^ (unsigned)now_ms();
    return l;
}

void limiter_free(limiter *l) { free(l); }

static hostrule *find(limiter *l, const char *host, int create)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (str_ieq(l->rules[i].host, host)) return &l->rules[i];
    if (!create || l->n >= MAX_HOSTS) return NULL;

    {
        hostrule *r = &l->rules[l->n++];
        memset(r, 0, sizeof *r);
        snprintf(r->host, sizeof r->host, "%s", host);
        r->interval_ms = l->def_interval_ms;
        r->budget      = l->def_budget;
        return r;
    }
}

void limiter_set(limiter *l, const char *host, int interval_ms, int budget)
{
    hostrule *r = find(l, host, 1);
    if (!r) return;
    r->interval_ms = interval_ms;
    r->budget      = budget;
}

/* xorshift, only used for backoff jitter */
static unsigned rng_next(limiter *l)
{
    unsigned x = l->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (l->rng = x);
}

int limiter_acquire(limiter *l, const char *host)
{
    hostrule *r = find(l, host, 1);
    long long now;

    if (!r) return -1;  /* fail closed */

    if (r->budget > 0 && r->spent >= r->budget) {
        fprintf(stderr, "  [limit] %s: per-run budget of %d requests spent, "
                        "stopping this source\n", host, r->budget);
        return -1;
    }

    now = now_ms();
    if (now < r->next_ok_ms) {
        int wait = (int)(r->next_ok_ms - now);
        if (wait > 1000)
            fprintf(stderr, "  [limit] %s: waiting %.1fs\n", host, wait / 1000.0);
        if (sleep_interruptible(wait)) return -1;
    }

    r->spent++;
    l->total++;
    r->next_ok_ms = now_ms() + r->interval_ms;
    return 0;
}

void limiter_penalize(limiter *l, const char *host, int retry_after_s)
{
    hostrule *r = find(l, host, 1);
    int       pen;

    if (!r) return;
    r->throttles++;

    if (retry_after_s > 0) {
        pen = retry_after_s > 2147483 ? 2147483000 : retry_after_s * 1000;
    } else {
        /* No header: exponential from the interval, plus jitter so repeated
         * runs never line up into a herd. */
        pen = r->backoff_ms ? r->backoff_ms * 2 : (r->interval_ms * 4 + 1000);
        pen += (int)(rng_next(l) % 1000u);
    }
    if (retry_after_s <= 0 && pen > BACKOFF_CAP_MS) pen = BACKOFF_CAP_MS;

    r->backoff_ms = pen;
    r->next_ok_ms = now_ms() + pen;
    fprintf(stderr, "  [limit] %s: throttled, backing off %.1fs\n",
            host, pen / 1000.0);
}

void limiter_ok(limiter *l, const char *host)
{
    hostrule *r = find(l, host, 0);
    if (r) r->backoff_ms = 0;
}

int limiter_spent(limiter *l, const char *host)
{
    hostrule *r = find(l, host, 0);
    return r ? r->spent : 0;
}

int limiter_total(limiter *l) { return l->total; }

int limiter_throttles(limiter *l, const char *host)
{
    hostrule *r = find(l, host, 0);
    return r ? r->throttles : 0;
}
