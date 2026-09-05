/* adapter.h - one source in, normalized assets out.
 *
 * Native adapters are C functions. External adapters are subprocesses in any
 * language that print JSONL. The core does not care which. See
 * ADAPTER_PROTOCOL.md and decisions.md D2.
 */
#ifndef ADAPTER_H
#define ADAPTER_H

#include "asset.h"
#include "fetch.h"

typedef struct {
    fetcher    *f;
    store      *st;
    const char *source;   /* expected source slug, for validation */
    int         emitted;
    int         skipped;
    int         unclassified;  /* type came back unclassifiable  */
    int         unstyled;      /* style came back unclassifiable */
} adapter_ctx;

/* Validate, fill in guessable fields, derive licence facts, write. The asset
 * is not consumed - the caller still owns and frees it. */
int adapter_emit(adapter_ctx *ctx, asset *a);

typedef int (*adapter_fn)(adapter_ctx *ctx);

typedef enum { AD_NATIVE, AD_EXTERNAL } adapter_kind;

typedef struct {
    const char  *name;
    const char  *host;         /* for the rate limiter; NULL if no HTTP */
    adapter_kind kind;
    adapter_fn   fn;           /* AD_NATIVE  */
    const char  *cmd;          /* AD_EXTERNAL */
    int          interval_ms;  /* minimum gap between requests to host */
    int          budget;       /* max requests per run, 0 = unlimited */
    const char  *note;
    /* Optional second mode: fill in fields the listing could not carry.
     * The job-file path is appended to this command. NULL = not supported. */
    const char  *enrich_cmd;
    const char  *site;         /* human-facing home page, for the UI */
} adapter_def;

extern const adapter_def ADAPTERS[];
extern const int         N_ADAPTERS;

const adapter_def *adapter_find(const char *name);
int  adapter_run(const adapter_def *d, adapter_ctx *ctx);

/* Extra flags appended to external adapter commands, from `crawl -- <args>`.
 * Lets one source be crawled deeper without editing the registry:
 *   magpie crawl opengameart -- --type 2d_sprite --max-pages 8
 * Pass NULL to clear. Ignored by native adapters. */
void adapter_set_extra_args(const char *args);

/* Hand the adapter up to `limit` assets that are missing author/tags and
 * apply whatever it sends back. Returns the number enriched, or -1. */
int  adapter_enrich(const adapter_def *d, store *st, int limit, int *applied_out);

/* Native adapters. */
int adapter_polyhaven(adapter_ctx *ctx);
int adapter_ambientcg(adapter_ctx *ctx);

#endif
