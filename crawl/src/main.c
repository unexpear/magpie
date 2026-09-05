/* Magpie - game asset search.
 *
 * crawl / search / stats / sources / check / enrich / export
 */
#include "adapter.h"
#include "asset.h"
#include "classify.h"
#include "fetch.h"
#include "http.h"
#include "limiter.h"
#include "store.h"
#include "util.h"
#include <errno.h>
#include <limits.h>
#include "yyjson.h"
#ifndef _WIN32
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#  include <io.h>
#endif

#define VERSION "0.3"

/* Politeness defaults, deliberately tighter than anything a source enforces. */
#define DEFAULT_INTERVAL_MS 1000
#define DEFAULT_BUDGET      500

static const char *UA =
    "Magpie/" VERSION
    " (+https://github.com/unexpear/magpie; game asset metadata index)";

static const char *db_path = "index.sqlite";
/* Process-wide network request ceiling from --max-requests; 0 = unlimited. */
static int g_max_requests;

/* Requests per day across *all* runs, held in the database. The per-run
 * ceiling above bounds one invocation; this is the one that actually protects
 * a source, because running the command again does not reset it. */
#define DEFAULT_DAILY_CAP 500
static int g_daily_cap = DEFAULT_DAILY_CAP;

/* Returns requests left today, or -1 when uncapped. Prints and returns 0 when
 * the day is spent. */
static long long daily_remaining(store *st)
{
    long long used, left;

    if (g_daily_cap <= 0) return -1;
    used = store_budget_used_today(st);
    if (used < 0) { fprintf(stderr,"request ledger unavailable\n"); return 0; }
    left = g_daily_cap - used;
    if (left <= 0) {
        char day[11];
        store_budget_today_key(day);
        printf("daily budget spent: %lld of %d requests used today (%s).\n",
               used, g_daily_cap, day);
        printf("It resets at midnight. Everything done so far is saved.\n");
        printf("Override deliberately with --daily-cap <n>, or 0 for no limit.\n");
        return 0;
    }
    return left;
}

static int nonnegative(const char *value)
{
    char *end; long n; errno=0; n=strtol(value,&end,10);
    if(errno || !*value || *end || n<0 || n>INT_MAX) {
        fprintf(stderr,"expected a nonnegative integer, got: %s\n",value); exit(2);
    }
    return (int)n;
}

/* Recognised by every networked subcommand. */
static int parse_common(char **argv, int *i, int argc)
{
    if (strcmp(argv[*i], "--db") == 0 && *i + 1 < argc) {
        db_path = argv[++(*i)];
        return 1;
    }
    if (strcmp(argv[*i], "--max-requests") == 0 && *i + 1 < argc) {
        g_max_requests = nonnegative(argv[++(*i)]);
        return 1;
    }
    if (strcmp(argv[*i], "--daily-cap") == 0 && *i + 1 < argc) {
        g_daily_cap = nonnegative(argv[++(*i)]);
        return 1;
    }
    return 0;
}

static void usage(void)
{
    puts(
"magpie " VERSION " - search game assets across sites\n"
"\n"
"  magpie crawl [source...]      fetch sources into the index (default: all)\n"
"  magpie search <words...>      search the index\n"
"  magpie stats                  what is in the index\n"
"  magpie sources                sites we index, and their health\n"
"  magpie check [source]         HEAD every url, record dead links\n"
"  magpie enrich [source]        fill in author/tags a listing could not carry\n"
"  magpie reclassify             re-run the classifier over the index, no network\n"
"  magpie budget                 requests used today, and recent days\n"
"  magpie export [--out DIR]     write validated browser JSON (atomic replacement)\n"
"\n"
"crawl:\n"
"  --offline        cached responses only, no network\n"
"  --db <path>      index file (default index.sqlite)\n"
"  -- <args>        pass the rest to the external adapter, e.g.\n"
"                   magpie crawl opengameart -- --type 2d_sprite --max-pages 8\n"
"\n"
"search:\n"
"  --commercial     commercial filter (review licence obligations)\n"
"  --no-credit      only assets needing no attribution\n"
"  --type <t>       2d_sprite 3d_model texture audio music font shader vfx ui\n"
"                   unclassifiable\n"
"  --licence <l>    cc0 cc_by cc_by_sa oga_by gpl store_eula personal_only\n"
"  --style <s>      pixel lowpoly pbr handpainted flat voxel unclassifiable\n"
"  --source <s>     polyhaven ambientcg kenney opengameart\n"
"  --tag <t>        exact tag match\n"
"  --limit <n>      default 25       --offset <n>\n"
"  --urls           print urls only  --why  show classifier evidence\n"
"  --include-dead   do not hide urls that failed their last check\n"
"\n"
"check / enrich:\n"
"  --limit <n>      how many to process this run (both are resumable)\n"
"  --redo-blank     (enrich) retry rows that were enriched but still have no\n"
"                   author - i.e. ones an adapter bug silently dropped\n"
"\n"
"any networked command:\n"
"  --max-requests <n>  ceiling for THIS run\n"
"  --daily-cap <n>     ceiling for the whole day across every run (default 500,\n"
"                      0 = none). Held in the database, so running the command\n"
"                      again does not reset it. This is the one that protects\n"
"                      a source from a long afternoon of well-meaning batches.\n"
"\n"
"examples:\n"
"  magpie search rusty metal --commercial --no-credit\n"
"  magpie search barrel --type 3d_model --style lowpoly --why\n"
"  magpie search --tag unclassifiable --limit 5\n"
"  magpie check --limit 200\n");
}

/* A mistyped source name must never look like "that source had nothing to do".
 * Returns 0 if known, else prints the valid list and returns -1. */
static int check_source_name(const char *name)
{
    int k;
    if (adapter_find(name)) return 0;

    fprintf(stderr, "unknown source: '%s'\n  valid: ", name);
    for (k = 0; k < N_ADAPTERS; k++)
        fprintf(stderr, "%s%s", k ? " " : "", ADAPTERS[k].name);
    fprintf(stderr, "\n");
    return -1;
}

/* ---- crawl -------------------------------------------------------------- */

static int cmd_crawl(int argc, char **argv)
{
    store   *st;
    limiter *lim;
    fetcher  f;
    int      i, offline = 0, n_named = 0;
    char    *named[16];
    int      total_emitted = 0, total_skipped = 0, total_unclass = 0, failures = 0;
    long long t0 = now_ms();

    for (i = 0; i < argc; i++) {
        /* Everything after `--` is handed to the external adapter untouched,
         * so one source can be crawled deeper without editing the registry. */
        if (strcmp(argv[i], "--") == 0) {
            sbuf extra;
            sbuf_init(&extra);
            for (i++; i < argc; i++)
            {
                if(extra.len) sbuf_appendz(&extra," ");
                if(shell_arg(&extra,argv[i])) { fprintf(stderr,"unsupported adapter argument\n"); sbuf_free(&extra); return 2; }
            }
            adapter_set_extra_args(extra.buf);
            sbuf_free(&extra);
            break;
        }
        if (strcmp(argv[i], "--offline") == 0) offline = 1;
        else if (parse_common(argv, &i, argc)) { /* --db, --max-requests */ }
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown flag %s\n", argv[i]); return 2; }
        else if (n_named < 16) named[n_named++] = argv[i];
    }

    for (i = 0; i < n_named; i++)
        if (check_source_name(named[i]) != 0) return 2;

    st = store_open(db_path);
    if (!st) return 1;
    if (http_init(UA) != 0) { fprintf(stderr, "http init failed\n"); store_close(st); return 1; }

    lim = limiter_new(DEFAULT_INTERVAL_MS, DEFAULT_BUDGET);
    fetch_init(&f, lim, st);
    f.offline      = offline;
    f.max_requests = g_max_requests;
    if (!offline && fetch_arm_daily(&f, g_daily_cap) == 0) {
        daily_remaining(st);
        limiter_free(lim); http_cleanup(); store_close(st);
        return 1;
    }

    printf("crawling into %s%s\n", db_path, offline ? " (offline, cache only)" : "");

    fetch_export_env(&f);
    for (i = 0; i < N_ADAPTERS; i++) {
        const adapter_def *d = &ADAPTERS[i];
        adapter_ctx        ctx;
        int                rc;

        if (n_named) {
            int j, want = 0;
            for (j = 0; j < n_named; j++)
                if (str_ieq(named[j], d->name)) { want = 1; break; }
            if (!want) continue;
        }

        memset(&ctx, 0, sizeof ctx);
        ctx.f  = &f;
        ctx.st = st;

        printf("\n[%s]\n", d->name);
        if (!offline && store_crawl_state(st,d->name,1,0)) { failures++; continue; }
        rc = adapter_run(d, &ctx);
        if (!offline && store_crawl_state(st,d->name,rc ? 3 : 2,ctx.emitted)) rc=-1;

        if (rc != 0) {
            printf("  incomplete/failed - saved rows for '%s' are left alone\n",
                   d->name);
            failures++;
        } else {
            printf("  %d assets", ctx.emitted);
            if (ctx.skipped)      printf(", %d skipped", ctx.skipped);
            if (ctx.unclassified) printf(", %d unclassifiable", ctx.unclassified);
            printf("\n");
        }
        total_emitted += ctx.emitted;
        total_skipped += ctx.skipped;
        total_unclass += ctx.unclassified;
    }

    printf("\nranking and search index...\n");
    if (store_recompute_popularity(st) || store_rebuild_fts(st)) failures++;

    printf("\ndone in %.1fs\n", (now_ms() - t0) / 1000.0);
    printf("  %d assets indexed", total_emitted);
    if (total_skipped) printf(", %d skipped", total_skipped);
    if (total_unclass) printf(", %d unclassifiable", total_unclass);
    printf("\n  %d network requests, %d native cache hits\n",
           (int)store_run_used(st,f.run_id), f.n_cached);
    if (failures) printf("  %d source(s) failed\n", failures);

    {
        store_stats s;
        store_get_stats(st, &s);
        printf("  index holds %lld assets (%lld commercial filter eligible, "
               "%lld need no credit)\n",
               s.total, s.commercial_ok, s.no_attribution);
        if (s.unchecked)
            printf("  %lld urls never link-checked - run: magpie check\n", s.unchecked);
    }

    limiter_free(lim);
    http_cleanup();
    store_close(st);
    return failures ? 1 : 0;
}

/* ---- search ------------------------------------------------------------- */

/* A mistyped filter must never look like "there are no such assets". */
static int bad_value(const char *flag, const char *got, const char *valid)
{
    fprintf(stderr, "unknown value for %s: '%s'\n", flag, got);
    fprintf(stderr, "  valid: %s\n", valid);
    return 2;
}

typedef struct { int n; int urls_only; int why; } printer;

static void print_hit(const search_hit *h, void *ud)
{
    printer     *p = ud;
    const asset *a = &h->a;

    if (p->urls_only) { printf("%s\n", a->source_url); p->n++; return; }

    printf("\n%2d. %s\n", ++p->n, a->title ? a->title : "(untitled)");
    printf("    %-12s %-15s %s\n",
           a->source ? a->source : "?",
           asset_type_str(a->type),
           licence_str(a->licence));

    printf("    %s%s",
           licence_commercial_ok(a->licence) ? "commercial ok"
                                             : "NOT cleared for commercial use",
           licence_needs_attribution(a->licence) ? ", credit required"
                                                 : ", no credit needed");
    if (h->pop_pct >= 0) printf("   popularity %d%%", h->pop_pct);
    printf("\n");

    if (a->author && *a->author)   printf("    by %s\n", a->author);
    if (a->style != ST_UNKNOWN && a->style != ST_UNCLASSIFIABLE)
        printf("    style %s\n", style_str(a->style));
    if (a->formats && *a->formats) printf("    %s\n", a->formats);
    if (a->polycount > 0)          printf("    %lld tris\n", a->polycount);
    if (a->tags && *a->tags)       printf("    tags: %s\n", a->tags);
    /* Kept visually separate: these came off the title, not the source. */
    if (a->tags_auto && *a->tags_auto)
        printf("    mined: %s\n", a->tags_auto);

    if (p->why)
        printf("    [why] type=%s via %s (%d%%), style=%s via %s (%d%%)\n",
               asset_type_str(a->type), h->type_ev ? h->type_ev : "?", h->type_conf,
               style_str(a->style), h->style_ev ? h->style_ev : "?", h->style_conf);

    if (h->http_status == 404 || h->http_status == 410) printf("    LINK DEAD (HTTP %d)\n", h->http_status);
    printf("    %s\n", a->source_url);
}

static int cmd_search(int argc, char **argv)
{
    store       *st;
    search_query q;
    printer      p;
    sbuf         text;
    int          i, rc, total;

    memset(&q, 0, sizeof q);
    q.type_filter = q.licence_filter = q.style_filter = -1;
    q.limit = 25;
    memset(&p, 0, sizeof p);

    sbuf_init(&text);
    for (i = 0; i < argc; i++) {
        if      (strcmp(argv[i], "--commercial")   == 0) q.commercial_only = 1;
        else if (strcmp(argv[i], "--no-credit")    == 0) q.no_attribution  = 1;
        else if (strcmp(argv[i], "--urls")         == 0) p.urls_only       = 1;
        else if (strcmp(argv[i], "--why")          == 0) p.why             = 1;
        else if (strcmp(argv[i], "--include-dead") == 0) q.include_dead    = 1;
        else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db_path = argv[++i];
        else if (strcmp(argv[i], "--limit")  == 0 && i + 1 < argc) q.limit  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) q.offset = atoi(argv[++i]);
        else if (strcmp(argv[i], "--type")   == 0 && i + 1 < argc) {
            if ((q.type_filter = asset_type_parse_strict(argv[++i])) < 0)
                return bad_value("--type", argv[i], asset_type_values());
        }
        else if ((strcmp(argv[i], "--licence") == 0 || strcmp(argv[i], "--license") == 0)
                 && i + 1 < argc) {
            if ((q.licence_filter = licence_parse_strict(argv[++i])) < 0)
                return bad_value("--licence", argv[i], licence_values());
        }
        else if (strcmp(argv[i], "--style")  == 0 && i + 1 < argc) {
            if ((q.style_filter = style_parse_strict(argv[++i])) < 0)
                return bad_value("--style", argv[i], style_values());
        }
        else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            q.source_filter = argv[++i];
            if (!adapter_find(q.source_filter)) {
                sbuf names;
                int  k;
                sbuf_init(&names);
                for (k = 0; k < N_ADAPTERS; k++)
                    sbuf_printf(&names, "%s%s", k ? " " : "", ADAPTERS[k].name);
                bad_value("--source", q.source_filter, names.buf);
                sbuf_free(&names);
                sbuf_free(&text);
                return 2;
            }
        }
        else if (strcmp(argv[i], "--tag")    == 0 && i + 1 < argc) q.tag_filter    = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown flag %s\n", argv[i]);
            sbuf_free(&text);
            return 2;
        } else {
            if (text.len) sbuf_appendz(&text, " ");
            sbuf_appendz(&text, argv[i]);
        }
    }
    q.text = text.buf;

    st = store_open(db_path);
    if (!st) { sbuf_free(&text); return 1; }

    total = store_count(st, &q);
    rc = store_search(st, &q, print_hit, &p);
    if (rc < 0) { store_close(st); sbuf_free(&text); return 1; }

    if (!p.urls_only) {
        if (p.n == 0) {
            store_stats s;
            store_get_stats(st, &s);
            printf("no matches");
            if (q.commercial_only || q.no_attribution) printf(" with those licence filters");
            printf("\n");
            if (s.total == 0) printf("(the index is empty - run: magpie crawl)\n");
        } else {
            printf("\nshowing %d of %d match%s\n",
                   p.n, total, total == 1 ? "" : "es");
        }
    }

    store_close(st);
    sbuf_free(&text);
    return 0;
}

/* ---- stats / sources ---------------------------------------------------- */

static int cmd_stats(int argc, char **argv)
{
    store      *st;
    store_stats s;
    int         i;

    for (i = 0; i < argc; i++)
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db_path = argv[++i];

    st = store_open(db_path);
    if (!st) return 1;

    store_get_stats(st, &s);
    printf("index: %s\n", db_path);
    printf("  %-24s %8lld\n", "assets", s.total);
    printf("  %-24s %8lld\n", "commercial filter eligible", s.commercial_ok);
    printf("  %-24s %8lld\n", "no attribution needed", s.no_attribution);
    printf("  %-24s %8lld\n", "licence unknown", s.unknown_licence);
    printf("  %-24s %8lld\n", "type unclassifiable", s.unclassifiable);
    printf("  %-24s %8lld\n", "tagged by the source", s.source_tagged);
    printf("  %-24s %8lld\n", "tags mined by us only", s.auto_only);
    printf("  %-24s %8lld\n", "no tags at all", s.no_tags);
    printf("  %-24s %8lld  <- credit required, no creator recorded\n",
           "uncreditable", s.uncreditable);
    printf("  %-24s %8lld\n", "links dead", s.dead);
    printf("  %-24s %8lld\n", "links never checked", s.unchecked);
    store_print_breakdown(st);

    store_close(st);
    return 0;
}

static void print_source(const source_row *r, void *ud)
{
    const adapter_def *d = adapter_find(r->source);
    (void)ud;

    printf("  %-13s %7lld assets  %6lld commercial", r->source,
           r->assets, r->commercial_ok);
    if (r->dead)         printf("  %lld dead", r->dead);
    if (r->unclassified) printf("  %lld unclassified", r->unclassified);
    if (r->unchecked)    printf("  %lld unchecked", r->unchecked);
    printf("\n");
    if (d && d->site) printf("  %-13s %s\n", "", d->site);
}

static int cmd_sources(int argc, char **argv)
{
    store *st;
    int    i, n;

    for (i = 0; i < argc; i++)
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db_path = argv[++i];

    printf("sites we index\n\n");
    st = store_open(db_path);
    if (st) {
        n = store_list_sources(st, print_source, NULL);
        if (n == 0) printf("  (index empty - run: magpie crawl)\n");
        store_close(st);
    }

    printf("\nadapters registered\n\n");
    printf("  %-13s %-10s %s\n", "name", "kind", "notes");
    for (i = 0; i < N_ADAPTERS; i++) {
        const adapter_def *d = &ADAPTERS[i];
        printf("  %-13s %-10s %s\n", d->name,
               d->kind == AD_NATIVE ? "native" : "external",
               d->note ? d->note : "");
        if (d->kind == AD_EXTERNAL) printf("  %-13s %-10s -> %s\n", "", "", d->cmd);
        if (d->enrich_cmd)          printf("  %-13s %-10s -> %s\n", "", "enrich", d->enrich_cmd);
    }
    return 0;
}

/* ---- check -------------------------------------------------------------- */

typedef struct {
    fetcher *f;
    store   *st;
    int      ok, dead, skipped;
    /* Circuit breaker. A host that cannot be reached at all - DNS failure,
     * network block - would otherwise burn the whole run on retries and
     * backoff for every one of its URLs. crawler.md already demands this for
     * adapters; the checker needs it too. */
    char     bad_host[128];
    int      consecutive_fail;
    int      host_skipped;
} checker;

#define CHECK_GIVE_UP_AFTER 5
/* Being throttled at all means we were already going too fast. Backing off and
 * carrying on is not good enough for a job that will make thousands more
 * requests to the same host - stop touching it and come back another day.
 * background-runner.md states this rule; this is where it is enforced. */
#define CHECK_MAX_THROTTLES 3

static void check_one(const char *id, const char *url, void *ud)
{
    checker *c = ud;
    char     host[128];
    int      status;

    if (url_host(url, host, sizeof host) != 0) { c->skipped++; return; }

    if (c->bad_host[0] && str_ieq(host, c->bad_host)) {
        c->host_skipped++;
        c->skipped++;
        return;
    }

    if (limiter_throttles(c->f->lim, host) >= CHECK_MAX_THROTTLES) {
        snprintf(c->bad_host, sizeof c->bad_host, "%s", host);
        fprintf(stderr, "  [back off] %s asked us to slow down %d times - "
                        "leaving it alone for the rest of this run\n",
                host, CHECK_MAX_THROTTLES);
        c->host_skipped++;
        c->skipped++;
        return;
    }

    status = fetch_status(c->f, url);

    if (status == 0) {
        /* Could not get an answer. Leaving it unchecked is honest; recording
         * it as dead would quietly hide a live asset from every search. */
        c->skipped++;
        if(!c->f->budget_hit && !stop_requested()) store_set_status(c->st,id,0);
        if (++c->consecutive_fail >= CHECK_GIVE_UP_AFTER) {
            snprintf(c->bad_host, sizeof c->bad_host, "%s", host);
            fprintf(stderr, "  [give up] %s unreachable after %d tries - "
                            "skipping the rest of its urls this run\n",
                    host, CHECK_GIVE_UP_AFTER);
        }
        return;
    }
    c->consecutive_fail = 0;
    if (store_set_status(c->st, id, status)) { c->skipped++; return; }
    if (status == 404 || status == 410) {
        c->dead++;
        printf("  DEAD %d  %s\n", status, url);
    } else if (status >= 200 && status < 300) {
        c->ok++;
    } else {
        c->skipped++;
        printf("  inconclusive HTTP %d  %s\n",status,url);
    }
}

static int cmd_check(int argc, char **argv)
{
    store      *st;
    limiter    *lim;
    fetcher     f;
    checker     c;
    const char *source = NULL;
    int         i, limit = 200;
    store_stats s;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = nonnegative(argv[++i]);
        else if (parse_common(argv, &i, argc)) { /* --db, --max-requests */ }
        else if (argv[i][0] != '-') source = argv[i];
        else { fprintf(stderr,"unknown flag %s\n",argv[i]); return 2; }
    }

    if (source && check_source_name(source) != 0) return 2;
    if (limit==0) return 0;
    /* Whichever ceiling is lower wins. */
    if (g_max_requests > 0 && (limit <= 0 || g_max_requests < limit))
        limit = g_max_requests;

    st = store_open(db_path);
    if (!st) return 1;
    if (http_init(UA) != 0) { store_close(st); return 1; }

    /* 2s, not the 1s used elsewhere. Measured: a HEAD sweep at 1/sec drew
     * repeated 429s from ambientcg.com and game-icons.net. A crawl touches a
     * host a handful of times; a link check touches it thousands, and the same
     * rate is not equally polite in both cases. */
    lim = limiter_new(2000, 0);
    /* OpenGameArt asks for 10s between requests and that applies to HEAD too. */
    limiter_set(lim, "opengameart.org", 10000, 0);
    fetch_init(&f, lim, st);

    f.max_requests = g_max_requests;
    if (fetch_arm_daily(&f, g_daily_cap) == 0) {
        daily_remaining(st);
        limiter_free(lim); http_cleanup(); store_close(st);
        return 1;
    }
    memset(&c, 0, sizeof c);
    c.f = &f; c.st = st;

    store_get_stats(st, &s);
    printf("checking up to %d due urls (%lld never checked)\n", limit, s.unchecked);

    if (store_list_unchecked(st, source, limit, check_one, &c)<0) c.skipped++;

    printf("\n%d ok, %d dead", c.ok, c.dead);
    if (c.skipped) printf(", %d left unchecked (no answer)", c.skipped);
    printf("\n");
    if (c.host_skipped)
        printf("%d skipped because %s could not be reached at all\n",
               c.host_skipped, c.bad_host);

    store_get_stats(st, &s);
    if (s.unchecked)
        printf("%lld still unchecked - run again to continue\n", s.unchecked);
    if (s.dead)
        printf("%lld dead links are now hidden from search "
               "(--include-dead to see them)\n", s.dead);

    limiter_free(lim);
    http_cleanup();
    store_close(st);
    return c.skipped || f.budget_hit || stop_requested() ? 1 : 0;
}

/* ---- enrich ------------------------------------------------------------- */

static int cmd_enrich(int argc, char **argv)
{
    store      *st;
    const char *source = NULL;
    int         i, limit = 50, total = 0, redo_blank = 0, failures = 0, tags_only = 0;
    fetcher f;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = nonnegative(argv[++i]);
        else if (strcmp(argv[i], "--redo-blank") == 0) redo_blank = 1;
        else if (strcmp(argv[i], "--missing-tags") == 0) tags_only = 1;
        else if (parse_common(argv, &i, argc)) { /* --db, --max-requests */ }
        else if (argv[i][0] != '-') source = argv[i];
        else { fprintf(stderr,"unknown flag %s\n",argv[i]); return 2; }
    }

    if (limit==0) return 0;

    st = store_open(db_path);
    if (!st) return 1;

    store_enrich_tags_only(st,tags_only);
    if (source && check_source_name(source) != 0) { store_close(st); return 2; }

    if (redo_blank) {
        int n = store_redo_blank_authors(st, source);
        printf("queued %d row(s) that were enriched but still have no author\n",
               n < 0 ? 0 : n);
    }

    fetch_init(&f,NULL,st); f.max_requests=g_max_requests; f.daily_cap=g_daily_cap;
    fetch_export_env(&f);

    if (source && !adapter_find(source)->enrich_cmd) {
        /* Silently doing nothing here left the user unable to tell success
         * from a no-op. */
        printf("'%s' has no enrich mode - its listing already carries "
               "everything we store.\n", source);
        store_close(st);
        return 0;
    }

    for (i = 0; i < N_ADAPTERS; i++) {
        const adapter_def *d = &ADAPTERS[i];
        int                n, applied=0;

        if (!d->enrich_cmd) continue;
        if (source && !str_ieq(source, d->name)) continue;

        printf("[%s]\n", d->name);
        if(total>=limit || stop_requested()) break;
        n = adapter_enrich(d, st, limit-total, &applied);
        if(n<0) failures++;
        if(applied>0) { printf("  enriched %d\n",applied); total+=applied; }

        {
            /* The number that matters: rows whose licence demands a credit we
             * cannot supply. Enrichment targets these first. */
            store_stats s2;
            store_get_stats(st, &s2);
            printf("  %lld assets still need a credit we cannot supply\n",
                   s2.uncreditable);
        }
    }

    if (total) {
        /* Rows that just gained real tags can often be classified now. */
        int n = store_reclassify(st);
        printf("\nreclassified %d rows, rebuilding search index...\n", n);
        if(n<0 || store_rebuild_fts(st)) failures++;
    } else if (!source) {
        printf("nothing to do\n");
    }

    store_close(st);
    return failures || stop_requested() ? 1 : 0;
}

static int cmd_budget(int argc, char **argv)
{
    store    *st;
    int       i;
    long long used;
    char      day[11];

    for (i = 0; i < argc; i++) parse_common(argv, &i, argc);

    st = store_open(db_path);
    if (!st) return 1;

    store_budget_today_key(day);
    used = store_budget_used_today(st);

    printf("request budget\n");
    printf("  today (%s)   %lld of %d\n", day, used, g_daily_cap);
    if (g_daily_cap > 0)
        printf("  remaining      %lld\n",
               used >= g_daily_cap ? 0 : g_daily_cap - used);
    printf("\nrecent days\n");
    store_budget_report(st, 14);
    printf("\nEvery networked run charges this ledger, so the ceiling holds\n"
           "however magpie is invoked. --daily-cap <n> overrides it, 0 removes it.\n");

    store_close(st);
    return 0;
}

static int cmd_reclassify(int argc, char **argv)
{
    store *st;
    int    i, n;

    for (i = 0; i < argc; i++)
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db_path = argv[++i];

    st = store_open(db_path);
    if (!st) return 1;

    printf("re-running the classifier over the whole index (no network)...\n");
    n = store_reclassify(st);
    if (n < 0) { store_close(st); return 1; }
    store_rebuild_fts(st);

    {
        store_stats s;
        store_get_stats(st, &s);
        printf("reclassified %d rows; %lld are unclassifiable\n",
               n, s.unclassifiable);
    }
    store_close(st);
    return 0;
}

/* ---- export ------------------------------------------------------------- */

typedef struct { FILE *fp; int n, failed; } exporter;

static void export_hit(const search_hit *h, void *ud)
{
    exporter    *e = ud;
    const asset *a = &h->a;
    sbuf         b;
    const char *why=NULL;
    if(asset_validate(a,&why)) { e->failed=1; return; }

    sbuf_init(&b);
    sbuf_appendz(&b, e->n ? ",\n" : "");
    sbuf_appendz(&b, "{\"id\":"); sbuf_json_str(&b, a->id);
    sbuf_appendz(&b, ",\"t\":");   sbuf_json_str(&b, a->title);
    sbuf_appendz(&b, ",\"s\":");   sbuf_json_str(&b, a->source);
    sbuf_appendz(&b, ",\"u\":");   sbuf_json_str(&b, a->source_url);
    sbuf_appendz(&b, ",\"th\":");  sbuf_json_str(&b, a->thumb_url);
    sbuf_appendz(&b, ",\"ty\":");  sbuf_json_str(&b, asset_type_str(a->type));
    sbuf_appendz(&b, ",\"st\":");  sbuf_json_str(&b, style_str(a->style));
    sbuf_appendz(&b, ",\"l\":");   sbuf_json_str(&b, licence_str(a->licence));
    sbuf_appendz(&b, ",\"lu\":");  sbuf_json_str(&b, a->licence_url);
    sbuf_appendz(&b, ",\"a\":");   sbuf_json_str(&b, a->author);
    sbuf_appendz(&b, ",\"tg\":");  sbuf_json_str(&b, a->tags);
    sbuf_appendz(&b, ",\"tga\":"); sbuf_json_str(&b, a->tags_auto);
    sbuf_appendz(&b, ",\"f\":");   sbuf_json_str(&b, a->formats);
    sbuf_printf(&b, ",\"c\":%d",   licence_commercial_ok(a->licence));
    sbuf_printf(&b, ",\"n\":%d",   licence_needs_attribution(a->licence) ? 0 : 1);
    sbuf_printf(&b, ",\"p\":%d",   h->pop_pct < 0 ? 0 : h->pop_pct);
    sbuf_printf(&b, ",\"seen\":%lld,\"checked\":%lld",h->last_seen,h->last_checked);
    if (a->updated_at > 0) sbuf_printf(&b, ",\"up\":%lld", a->updated_at);
    if (a->polycount > 0)  sbuf_printf(&b, ",\"pc\":%lld", a->polycount);
    sbuf_appendz(&b, "}");

    fwrite(b.buf, 1, b.len, e->fp);
    sbuf_free(&b);
    e->n++;
}

static void export_source(const source_row *r, void *ud)
{
    exporter          *e = ud;
    const adapter_def *d = adapter_find(r->source);
    sbuf               b;

    sbuf_init(&b);
    sbuf_appendz(&b, e->n ? ",\n" : "");
    sbuf_appendz(&b, "{\"name\":");  sbuf_json_str(&b, r->source);
    sbuf_appendz(&b, ",\"site\":");  sbuf_json_str(&b, d ? d->site : NULL);
    sbuf_appendz(&b, ",\"note\":");  sbuf_json_str(&b, d ? d->note : NULL);
    sbuf_printf(&b, ",\"assets\":%lld", r->assets);
    sbuf_printf(&b, ",\"commercial\":%lld", r->commercial_ok);
    sbuf_printf(&b, ",\"dead\":%lld", r->dead);
    sbuf_printf(&b, ",\"unchecked\":%lld", r->unchecked);
    sbuf_printf(&b, ",\"unclassified\":%lld", r->unclassified);
    sbuf_printf(&b, ",\"last_seen\":%lld,\"last_checked\":%lld,\"last_attempt\":%lld,\"last_success\":%lld,\"crawl_state\":%d,\"source_tagged\":%lld,\"missing_authors\":%lld",r->last_seen,r->last_checked,r->last_attempt,r->last_success,r->crawl_state,r->source_tagged,r->missing_authors);
    sbuf_appendz(&b, "}");

    fwrite(b.buf, 1, b.len, e->fp);
    sbuf_free(&b);
    e->n++;
}

static int cmd_export(int argc, char **argv)
{
    store       *st;
    search_query q;
    exporter     e;
    store_stats  s;
    const char  *out = "web/data.json";
    FILE        *fp;
    int          i, failed=0;
    char token[80], temporary[4096];

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db_path = argv[++i];
        else { fprintf(stderr,"unknown or incomplete export argument: %s\n",argv[i]); return 2; }
    }

    st = store_open(db_path);
    if (!st) return 1;

    unique_token(token);
    if (snprintf(temporary,sizeof temporary,"%s.%s.tmp",out,token)>=(int)sizeof temporary) { store_close(st); return 1; }
    fp = fopen(temporary, "wb");
    if (!fp) {
        fprintf(stderr, "cannot write %s (does the directory exist?)\n", out);
        store_close(st);
        return 1;
    }

    memset(&s,0,sizeof s);
    if (store_begin(st) || store_get_stats(st, &s)) failed=1;

    fprintf(fp, "{\n\"generated\":%lld,\n", now_unix());
    fprintf(fp, "\"stats\":{\"total\":%lld,\"commercial\":%lld,\"nocredit\":%lld,"
                "\"unclassifiable\":%lld,\"dead\":%lld,\"unchecked\":%lld,"
                "\"source_tagged\":%lld,\"auto_only\":%lld},\n",
            s.total, s.commercial_ok, s.no_attribution, s.unclassifiable,
            s.dead, s.unchecked, s.source_tagged, s.auto_only);

    memset(&e, 0, sizeof e);
    e.fp = fp;
    fprintf(fp, "\"sources\":[\n");
    if(store_list_sources(st, export_source, &e)<0) failed=1;
    fprintf(fp, "\n],\n");

    memset(&q, 0, sizeof q);
    q.type_filter = q.licence_filter = q.style_filter = -1;
    q.limit = 1000000;

    e.n = 0;
    fprintf(fp, "\"assets\":[\n");
    if(store_search(st, &q, export_hit, &e)<0 || e.failed) failed=1;
    fprintf(fp, "\n]}\n");

    if(ferror(fp) || fflush(fp)) failed=1;
#ifdef _WIN32
    if(!failed && _commit(_fileno(fp))) failed=1;
#else
    if(!failed && fsync(fileno(fp))) failed=1;
#endif
    if(fclose(fp)) failed=1;
    if(store_commit(st)) failed=1;
    yyjson_doc *doc=failed ? NULL : yyjson_read_file(temporary,0,NULL,NULL);
    if(!doc) failed=1;
    if(doc) yyjson_doc_free(doc);
    if(!failed && atomic_replace(temporary,out)) failed=1;
    if(failed) { remove(temporary); store_close(st); fprintf(stderr,"export failed; previous output preserved\n"); return 1; }
    printf("wrote %s: %d assets\n", out, e.n);
    printf("open web/index.html to use it\n");

    store_close(st);
    return 0;
}

/* JSON response for adapters; includes policy and accounting. */
static int cmd_fetch(int argc, char **argv)
{
    const char *url=NULL, *method="GET", *v; int i,rc,status; sbuf body,json;
    store *st; fetcher f;
    if((v=getenv("MAGPIE_DB")) && *v) db_path=v;
    if((v=getenv("MAGPIE_MAX_REQUESTS")) && *v) g_max_requests=nonnegative(v);
    if((v=getenv("MAGPIE_DAILY_CAP")) && *v) g_daily_cap=nonnegative(v);
    for(i=0;i<argc;i++) {
        if(!strcmp(argv[i],"--url") && i+1<argc) url=argv[++i];
        else if(!strcmp(argv[i],"--method") && i+1<argc) method=argv[++i];
        else if(parse_common(argv,&i,argc)) {}
        else return 2;
    }
    if(!url || (strcmp(method,"GET") && strcmp(method,"HEAD"))) return 2;
    st=store_open(db_path); if(!st) return 1;
    if(http_init(UA)) { store_close(st); return 1; }
    fetch_init(&f,NULL,st); f.max_requests=g_max_requests; f.daily_cap=g_daily_cap;
    v=getenv("MAGPIE_OFFLINE"); f.offline=v && !strcmp(v,"1");
    sbuf_init(&body); sbuf_init(&json);
    if(!strcmp(method,"HEAD")) { status=fetch_status(&f,url); rc=status?0:-1; }
    else { rc=fetch_url(&f,url,&body); status=f.last_status; }
    sbuf_printf(&json,"{\"ok\":%s,\"status\":%d,\"body\":",rc?"false":"true",status);
    sbuf_json_str(&json,body.buf?body.buf:""); sbuf_appendz(&json,"}\n");
    fwrite(json.buf,1,json.len,stdout);
    sbuf_free(&body); sbuf_free(&json); http_cleanup(); store_close(st);
    return rc?1:0;
}

/* ---- main --------------------------------------------------------------- */

#ifdef _WIN32
/* Windows hands main() its arguments in the system ANSI codepage, so anything
 * outside ASCII is mangled before it ever reaches SQLite. The index is UTF-8
 * and holds titles like "Comfy Café", "Pfalzer Forest" and "Дум";
 * searching for them from the shell silently returned nothing. Re-read the raw
 * command line as UTF-16 and convert it properly. */
static char **utf8_argv(int *argc_out)
{
    int       nw = 0, i;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &nw);
    char    **out;

    if (!wargv) return NULL;
    out = calloc((size_t)nw + 1, sizeof *out);
    if (!out) { LocalFree(wargv); return NULL; }

    for (i = 0; i < nw; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0) { out[i] = xstrdup(""); continue; }
        out[i] = malloc((size_t)n);
        if (out[i])
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, out[i], n, NULL, NULL);
        else
            out[i] = xstrdup("");
    }
    LocalFree(wargv);
    *argc_out = nw;
    return out;
}
#endif

int main(int argc, char **argv)
{
    /* Unbuffered: crawls are long, and when stdout is a pipe or a log file the
     * default full buffering means you see nothing until the process exits -
     * including nothing at all if it dies partway. */
    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
    /* ...and the same in reverse, so non-ASCII titles print readably. */
    SetConsoleOutputCP(CP_UTF8);
    {
        int    n = 0;
        char **a = utf8_argv(&n);
        if (a && n > 0) { argc = n; argv = a; }
    }
#endif

    {
        const char *configured=getenv("MAGPIE_USER_AGENT");
        static char custom_ua[1024];
        if(configured && *configured) {
            snprintf(custom_ua,sizeof custom_ua,"Magpie/" VERSION " (+https://github.com/unexpear/magpie) %s",configured);
            UA=custom_ua;
        }
#ifdef _WIN32
        wchar_t path[32768]; char utf8[32768];
        if(GetModuleFileNameW(NULL,path,32768) &&
           WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,sizeof utf8,NULL,NULL)) env_set("MAGPIE_EXE",utf8);
#else
        char resolved[4096];
        if(realpath(argv[0],resolved)) env_set("MAGPIE_EXE",resolved);
#endif
    }
    if (argc < 2) { usage(); return 2; }

    if (strcmp(argv[1], "fetch") == 0) return cmd_fetch(argc - 2, argv + 2);
    if (strcmp(argv[1], "crawl")   == 0) return cmd_crawl(argc - 2, argv + 2);
    if (strcmp(argv[1], "search")  == 0) return cmd_search(argc - 2, argv + 2);
    if (strcmp(argv[1], "stats")   == 0) return cmd_stats(argc - 2, argv + 2);
    if (strcmp(argv[1], "sources") == 0) return cmd_sources(argc - 2, argv + 2);
    if (strcmp(argv[1], "check")   == 0) return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "enrich")  == 0) return cmd_enrich(argc - 2, argv + 2);
    if (strcmp(argv[1], "export")  == 0) return cmd_export(argc - 2, argv + 2);
    if (strcmp(argv[1], "reclassify") == 0) return cmd_reclassify(argc - 2, argv + 2);
    if (strcmp(argv[1], "budget")     == 0) return cmd_budget(argc - 2, argv + 2);

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }

    fprintf(stderr, "unknown command: %s\n\n", argv[1]);
    usage();
    return 2;
}
