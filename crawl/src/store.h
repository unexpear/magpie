/* store.h - SQLite index: assets, FTS5, link health, and the request cache. */
#ifndef STORE_H
#define STORE_H

#include "asset.h"
#include "classify.h"
#include "util.h"

typedef struct store store;

store *store_open(const char *path);
const char *store_path(store *s);
void   store_close(store *s);

/* Batch writes. Without a transaction, 10k inserts take minutes. */
int store_begin(store *s);
int store_commit(store *s);
int store_crawl_state(store *s, const char *source, int state, int emitted);

int store_upsert(store *s, const asset *a, const classification *c);

/* Rebuild the FTS index from the assets table. Cheap at this scale, and much
 * simpler than keeping triggers in sync. Run after any bulk change. */
int store_rebuild_fts(store *s);

/* Turn raw popularity / listing position into a 0-100 percentile within each
 * source, so a Poly Haven download count and an OpenGameArt favourite rank
 * become comparable. Run at the end of a crawl. */
int store_recompute_popularity(store *s);

/* Re-run the classifier over every stored row. No network. Use after changing
 * a classifier rule, or after `enrich` gives previously tag-less rows
 * something to reason about. Returns rows updated. */
int store_reclassify(store *s);

/* ---- search ---- */

typedef struct {
    const char *text;            /* NULL/empty = browse everything */
    int         type_filter;     /* asset_type, or -1 for any       */
    int         licence_filter;  /* licence_id, or -1 for any       */
    int         style_filter;    /* style_id, or -1 for any         */
    int         commercial_only;
    int         no_attribution;
    const char *source_filter;
    const char *tag_filter;      /* substring match on tags         */
    int         include_dead;    /* 1 = do not hide known-dead urls */
    int         limit;
    int         offset;
} search_query;

typedef struct {
    asset       a;
    int         type_conf;
    int         style_conf;
    const char *type_ev;
    const char *style_ev;
    int         pop_pct;         /* -1 unknown */
    long long   last_seen, last_checked;
    int         http_status;     /* 0 = never checked */
} search_hit;

typedef void (*search_cb)(const search_hit *h, void *ud);

int store_search(store *s, const search_query *q, search_cb cb, void *ud);
/* Total matches ignoring limit/offset. */
int store_count(store *s, const search_query *q);

/* ---- link health ---- */

typedef void (*url_cb)(const char *id, const char *url, void *ud);

/* URLs due for checking: new, inconclusive (1 day), dead (7 days), live (30 days). */
int store_list_unchecked(store *s, const char *source, int limit,
                         url_cb cb, void *ud);
int store_set_status(store *s, const char *id, int http_status);

/* ---- enrichment (fill author / tags a listing did not carry) ---- */

void store_enrich_tags_only(store *s, int enabled);
int store_list_unenriched(store *s, const char *source, int limit,
                          url_cb cb, void *ud);

/* Clear the enriched flag on rows that went through enrichment and still have
 * no author. Those are the ones a parser bug silently dropped: marked done,
 * never revisited, and permanently missing the one field their licence needs.
 * Returns how many were queued for another attempt. */
int store_enrich_retry(store *s, const char *id);
int store_redo_blank_authors(store *s, const char *source);
/* Fills fields a listing could not carry. Any argument may be NULL/empty,
 * meaning "learned nothing about this one". Marks the row enriched. */
int store_enrich(store *s, const char *id, const char *author,
                 const char *tags, const char *thumb_url);

/* ---- stats ---- */

typedef struct {
    long long total;
    long long commercial_ok;
    long long no_attribution;
    long long unknown_licence;
    long long unclassifiable;
    long long dead;
    long long unchecked;
    long long no_tags;         /* no tags from either provenance   */
    long long source_tagged;   /* the source published tags        */
    long long auto_only;       /* every tag we have was mined here */
    /* Licence demands a credit and we have no creator to name. These cannot
     * lawfully be shipped from what we hold, which makes this the sharpest
     * measure of whether the index keeps its promise. */
    long long uncreditable;
} store_stats;

int store_get_stats(store *s, store_stats *out);
int store_print_breakdown(store *s);

/* One row per source, for `magpie sources` and the web UI's sources panel. */
typedef struct {
    char      source[64];
    long long assets;
    long long dead;
    long long unchecked;
    long long unclassified;
    long long commercial_ok;
    long long last_seen;
    long long last_checked, last_attempt, last_success;
    int crawl_state; /* 0 unknown, 1 running, 2 complete, 3 incomplete */
    long long source_tagged, missing_authors;
} source_row;

typedef void (*source_cb)(const source_row *r, void *ud);
int store_list_sources(store *s, source_cb cb, void *ud);

/* ---- persistent request budget ----
 *
 * The daily ceiling lives here, in the database, not in whatever script
 * happened to launch the process. A limit that only exists in the wrapper is
 * not a limit: it evaporates the moment anyone runs the command by hand, which
 * is how ~1,000 requests reached one small site in a day that was supposed to
 * cap at 500.
 *
 * Written through on every request. A single-row UPDATE costs microseconds
 * against a network wait measured in seconds, so there is no reason to batch
 * it and risk losing the count in a crash. */

/* Requests already spent on `day` (YYYY-MM-DD). */
long long store_budget_used(store *s, const char *day);
/* Record n more. Returns 0 on success. */
int store_budget_add(store *s, const char *day, int n);

/* Today, in local time. Used by both the C fetcher and the enrichment path so
 * external adapters are charged to the same ledger - their requests are real
 * even though our fetcher never sees them. */
long long store_budget_used_today(store *s);
int       store_budget_add_today(store *s, int n);
void      store_budget_today_key(char out[11]);
/* Human-readable ledger for `magpie budget`. */
int store_budget_report(store *s, int days);

/* ---- conditional-request cache ---- */

int store_cache_get(store *s, const char *url,
                    char *etag, size_t etag_n,
                    char *lastmod, size_t lastmod_n,
                    sbuf *body /* may be NULL */);

int store_cache_put(store *s, const char *url, const char *etag,
                    const char *lastmod, const char *body, size_t len);
long long store_cache_time(store *s, const char *url);
/* Atomic request reservation, committed before network I/O. 0=reserved,
 * 1=budget/cooldown, 2=wait, -1=database error. Never within an asset transaction. */
int store_request_reserve(store *s, const char *run, const char *host,
                          int interval_ms, int daily_cap, int run_cap,
                          int host_cap, int weekly_cap, int *wait_ms);
int store_host_pause(store *s, const char *host, int seconds);
long long store_run_used(store *s, const char *run);

#endif
