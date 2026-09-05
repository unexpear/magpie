#include "adapter.h"
#include "jsonutil.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define POPEN  _popen
#  define PCLOSE _pclose
#else
#  define POPEN  popen
#  define PCLOSE pclose
#endif

/* ---- the registry ------------------------------------------------------- */

const adapter_def ADAPTERS[] = {
    { "polyhaven", "api.polyhaven.com", AD_NATIVE, adapter_polyhaven, NULL,
      1000, 50,
      "Whole catalogue in one data request, plus cached robots policy. CC0.",
      NULL, "https://polyhaven.com" },

    { "ambientcg", "ambientcg.com", AD_NATIVE, adapter_ambientcg, NULL,
      1000, 50,
      "CC0 catalogue, documented 250-row pages, sorted alphabetically.",
      NULL, "https://ambientcg.com" },

    { "kenney", NULL, AD_EXTERNAL, NULL, "python adapters/kenney.py",
      0, 0,
      "External adapter. Hand-maintained pack list; enrich pulls the preview "
      "image, real tags and tile size off each pack page.",
      "python adapters/kenney.py --enrich", "https://kenney.nl" },

    { "gameicons", NULL, AD_EXTERNAL, NULL, "python adapters/gameicons.py",
      0, 0,
      "External adapter. 4,239 icons in ONE request; author comes from the "
      "file path, so every CC-BY credit line is complete.",
      NULL, "https://game-icons.net" },

    { "opengameart", NULL, AD_EXTERNAL, NULL, "python adapters/opengameart.py",
      0, 0,
      "External adapter. Scrapes; honours robots.txt Crawl-delay: 10, so it is "
      "slow by design. Mixed licences. Capped by default - pass --full inside "
      "the command for the whole site.",
      "python adapters/opengameart.py --enrich", "https://opengameart.org" },
};

const int N_ADAPTERS = (int)(sizeof ADAPTERS / sizeof ADAPTERS[0]);

const adapter_def *adapter_find(const char *name)
{
    int i;
    for (i = 0; i < N_ADAPTERS; i++)
        if (str_ieq(ADAPTERS[i].name, name)) return &ADAPTERS[i];
    return NULL;
}

/* ---- emit --------------------------------------------------------------- */

int adapter_emit(adapter_ctx *ctx, asset *a)
{
    const char    *why = NULL;
    classification c;

    if (!a->source && ctx->source) a->source = xstrdup(ctx->source);
    if(ctx->source && (!str_ieq(a->source,ctx->source) || !a->id ||
       strncmp(a->id,ctx->source,strlen(ctx->source)) || a->id[strlen(ctx->source)]!=':')) {
        ctx->skipped++; return -1;
    }

    if (asset_validate(a, &why) != 0) {
        fprintf(stderr, "  skip (%s): %s\n", why, a->id ? a->id : "<no id>");
        ctx->skipped++;
        return -1;
    }

    /* The classifier keeps whatever the adapter declared and only fills gaps,
     * then folds its verdict into the tags so it is searchable. */
    classify(a, &c);
    classify_apply(a, &c);

    if (c.type  == AT_UNCLASSIFIABLE) ctx->unclassified++;
    if (c.style == ST_UNCLASSIFIABLE) ctx->unstyled++;

    if (store_upsert(ctx->st, a, &c) != 0) { ctx->skipped++; return -1; }
    ctx->emitted++;
    return 0;
}

/* ---- JSON -> asset (the external adapter protocol) ---------------------- */

static int parse_asset_json(yyjson_val *o, asset *a)
{
    char *s;

    asset_init(a);
    if (!yyjson_is_obj(o)) return -1;

    a->id          = ju_str(o, "id");
    a->source      = ju_str(o, "source");
    a->title       = ju_str(o, "title");
    a->author      = ju_str(o, "author");
    a->source_url  = ju_str(o, "source_url");
    a->thumb_url   = ju_str(o, "thumb_url");
    a->licence_url = ju_str(o, "licence_url");
    if (!a->licence_url) a->licence_url = ju_str(o, "license_url"); /* be kind */

    a->formats = ju_arr_join(o, "formats", 1);
    a->tags    = ju_arr_join(o, "tags", 1);

    s = ju_str(o, "asset_type"); a->type = asset_type_parse(s); free(s);

    s = ju_str(o, "licence");
    if (!s) s = ju_str(o, "license");
    a->licence = licence_parse(s);
    free(s);

    s = ju_str(o, "style"); a->style = style_parse(s); free(s);

    a->rigged     = ju_tri(o, "rigged");
    a->tileable   = ju_tri(o, "tileable");
    a->polycount  = ju_int(o, "polycount", -1);
    a->updated_at = ju_int(o, "updated_at", 0);
    a->price      = ju_num(o, "price", -1);
    a->popularity = ju_int(o, "popularity", -1);
    a->rank_hint  = ju_int(o, "rank_hint", -1);
    return 0;
}

/* ---- external adapters -------------------------------------------------- */

/* Read one line of unbounded length. Returns NULL at EOF. Caller frees. */
static char *read_line(FILE *fp)
{
    sbuf b;
    char chunk[4096];
    int  got = 0;

    sbuf_init(&b);
    while (fgets(chunk, sizeof chunk, fp)) {
        size_t n = strlen(chunk);
        got = 1;
        if (n && chunk[n - 1] == '\n') {
            if (n > 1 && chunk[n - 2] == '\r') n -= 2; else n -= 1;
            sbuf_append(&b, chunk, n);
            return b.buf ? b.buf : xstrdup("");
        }
        sbuf_append(&b, chunk, n);
    }
    if (!got) { sbuf_free(&b); return NULL; }
    return b.buf ? b.buf : xstrdup("");
}

static char g_extra_args[512];

void adapter_set_extra_args(const char *args)
{
    snprintf(g_extra_args, sizeof g_extra_args, "%s", args ? args : "");
}

static int run_external(const adapter_def *d, adapter_ctx *ctx)
{
    FILE *fp;
    char *line;
    char  cmd[1024];
    int   lineno = 0, bad = 0, rc;

    snprintf(cmd, sizeof cmd, "%s%s%s", d->cmd,
             g_extra_args[0] ? " " : "", g_extra_args);

    fprintf(stderr, "  running: %s\n", cmd);
    fp = POPEN(cmd, "r");
    if (!fp) {
        fprintf(stderr, "  [skip] could not start '%s'\n", cmd);
        return -1;
    }

    while ((line = read_line(fp))) {
        yyjson_doc *doc;
        char       *t = str_trim(line);

        lineno++;
        if (!*t) { free(line); continue; }

        doc = yyjson_read(t, strlen(t), 0);
        if (!doc) {
            /* One bad line must not kill a crawl. */
            if (bad < 5)
                fprintf(stderr, "  bad JSON on line %d, skipping\n", lineno);
            bad++;
            free(line);
            continue;
        }
        {
            asset a;
            if (parse_asset_json(yyjson_doc_get_root(doc), &a) == 0)
                adapter_emit(ctx, &a);
            else bad++;
            asset_free(&a);
        }
        yyjson_doc_free(doc);
        free(line);
    }

    rc = PCLOSE(fp);
    if (bad > 5) fprintf(stderr, "  ...and %d more bad lines\n", bad - 5);

    if (rc != 0) {
        if (ctx->emitted == 0) {
            /* Almost always a missing interpreter. Not fatal - a C build must
             * not break because Python is absent. */
            fprintf(stderr, "  [skip] '%s' exited %d and produced nothing "
                            "(is the interpreter installed?)\n", d->cmd, rc);
            return -1;
        }
        fprintf(stderr, "  warning: '%s' exited %d after %d assets; "
                        "preserving emitted rows but reporting failure\n", d->cmd, rc, ctx->emitted);
    }
    return bad || ctx->skipped || rc != 0 ? -1 : 0;
}

/* ---- enrichment --------------------------------------------------------- */



typedef struct { FILE *fp; int n; } jobwriter;

static void write_job(const char *id, const char *url, void *ud)
{
    jobwriter *w = ud;
    fprintf(w->fp, "%s\t%s\n", id, url);
    w->n++;
}

int adapter_enrich(const adapter_def *d, store *st, int limit, int *applied_out)
{
    jobwriter w;
    FILE     *fp;
    char      cmd[512];
    char     *line;
    int       applied = 0, rc, bad = 0;
    char jobfile[120], token[80];
    *applied_out=0; unique_token(token);
    snprintf(jobfile,sizeof jobfile,".gas-enrich-%s.tmp",token);

    if (!d->enrich_cmd) {
        fprintf(stderr, "  '%s' has no enrich mode\n", d->name);
        return -1;
    }

    w.fp = fopen(jobfile, "w");
    if (!w.fp) { fprintf(stderr, "  cannot write %s\n", jobfile); return -1; }
    w.n = 0;
    rc=store_list_unenriched(st, d->name, limit, write_job, &w);
    if(fclose(w.fp) || rc<0) { remove(jobfile); return -1; }

    if (w.n == 0) {
        remove(jobfile);
        printf("  nothing left to enrich for '%s'\n", d->name);
        return 0;
    }

    snprintf(cmd, sizeof cmd, "%s %s", d->enrich_cmd, jobfile);
    fprintf(stderr, "  running: %s  (%d assets)\n", cmd, w.n);

    fp = POPEN(cmd, "r");
    if (!fp) {
        fprintf(stderr, "  [skip] could not start '%s'\n", cmd);
        remove(jobfile);
        return -1;
    }

    while ((line = read_line(fp))) {
        yyjson_doc *doc;
        char       *t = str_trim(line);

        if (!*t) { free(line); continue; }
        doc = yyjson_read(t, strlen(t), 0);
        if (!doc) { bad++; free(line); continue; }
        {
            /* Patches are {id, author, tags} - deliberately not full assets.
             * store_enrich preserves absent fields, so a patch cannot erase
             * something the crawl already knew. */
            yyjson_val *o     = yyjson_doc_get_root(doc);
            char       *id    = ju_str(o, "id");
            char       *auth  = ju_str(o, "author");
            char       *tags  = ju_arr_join(o, "tags", 1);
            char       *thumb = ju_str(o, "thumb_url");

            if (!id || strncmp(id,d->name,strlen(d->name)) || id[strlen(d->name)]!=':') bad++;
            else if (yyjson_is_true(yyjson_obj_get(o,"retry"))) { store_enrich_retry(st,id); bad++; }
            else if (!(auth && *auth) && !(tags && *tags) && !(thumb && *thumb)) { store_enrich_retry(st,id); bad++; }
            else if (store_enrich(st, id, auth, tags, thumb) == 0) applied++;
            else bad++;
            free(id); free(auth); free(tags); free(thumb);
        }
        yyjson_doc_free(doc);
        free(line);
    }

    rc = PCLOSE(fp);
    remove(jobfile);
    *applied_out=applied;
    if (rc != 0 || bad || applied < w.n) {
        fprintf(stderr, "  incomplete enrichment: %d of %d applied, child exit %d\n", applied, w.n, rc);
        return -1;
    }
    return applied;
}

/* ---- dispatch ----------------------------------------------------------- */

int adapter_run(const adapter_def *d, adapter_ctx *ctx)
{
    ctx->source = d->name;

    if (d->host && d->interval_ms > 0)
        limiter_set(ctx->f->lim, d->host, d->interval_ms, d->budget);

    if (d->kind == AD_NATIVE) { ctx->f->interval_ms=d->interval_ms; ctx->f->host_cap=d->budget; return d->fn(ctx); }
    return run_external(d, ctx);
}
