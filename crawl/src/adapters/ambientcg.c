/* ambientCG v2: documented 250-row pages in stable alphabetical order.
 * https://docs.ambientcg.com/api/v2/full_json/
 * Read type from each asset. Older cached 1000-row responses demonstrate past
 * behavior, but are not a contract for the current endpoint.
 */
#include "adapter.h"
#include "jsonutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACG_PAGE      250
#define ACG_MAX_PAGES 100     /* hard stop well above the real catalogue size */

static asset_type type_from_acg(const char *dt)
{
    if (!dt) return AT_UNKNOWN;
    if (str_ieq(dt, "3DModel")) return AT_3D_MODEL;
    /* Everything else ambientCG publishes - materials, decals, atlases,
     * terrain, HDRIs, substances, brushes - is image/material data. */
    return AT_TEXTURE;
}

static tribool tileable_from_acg(const char *dt)
{
    if (!dt) return TRI_UNKNOWN;
    if (str_ieq(dt, "Material") || str_ieq(dt, "PlainTexture") ||
        str_ieq(dt, "Terrain")  || str_ieq(dt, "Substance"))
        return TRI_YES;                          /* seamless by design */
    if (str_ieq(dt, "Decal") || str_ieq(dt, "Atlas") || str_ieq(dt, "3DModel"))
        return TRI_NO;
    return TRI_UNKNOWN;                          /* HDRI, Brush, ... */
}

static style_id style_from_acg(const char *dt)
{
    if (!dt) return ST_UNKNOWN;
    if (str_ieq(dt, "Material") || str_ieq(dt, "PlainTexture") ||
        str_ieq(dt, "Substance") || str_ieq(dt, "Terrain"))
        return ST_PBR;
    return ST_UNKNOWN;
}

/* previewImage is an object keyed "<size>-<format>". Prefer 256px. */
static char *pick_preview(yyjson_val *o)
{
    yyjson_val     *img = yyjson_obj_get(o, "previewImage");
    yyjson_obj_iter it;
    yyjson_val     *k;
    const char     *first = NULL;

    if (!img || !yyjson_is_obj(img)) return NULL;

    yyjson_obj_iter_init(img, &it);
    while ((k = yyjson_obj_iter_next(&it))) {
        const char *name = yyjson_get_str(k);
        const char *url  = yyjson_get_str(yyjson_obj_iter_get_val(k));
        if (!url || !*url) continue;
        if (!first) first = url;
        if (name && strstr(name, "256")) return xstrdup(url);
    }
    return first ? xstrdup(first) : NULL;
}

/* The dataType is genuinely useful to search on ("hdri", "decal"), and it is
 * not in the tag list, so fold it in. */
static char *tags_with_datatype(yyjson_val *item, const char *dt)
{
    char *tags = ju_arr_join(item, "tags", 1);
    sbuf  b;

    if (!dt || !*dt) return tags;

    sbuf_init(&b);
    if (tags && *tags) { sbuf_appendz(&b, tags); sbuf_appendz(&b, ","); }
    sbuf_appendz(&b, dt);
    if (b.buf) str_lower(b.buf);
    free(tags);
    return b.buf;
}

int adapter_ambientcg(adapter_ctx *ctx)
{
    int       page;
    long long total = -1;

    for (page = 0; page < ACG_MAX_PAGES; page++) {
        sbuf            body;
        yyjson_doc     *doc;
        yyjson_val     *root, *arr, *item;
        yyjson_arr_iter ai;
        char            url[512];
        size_t          n_page = 0;

        snprintf(url, sizeof url,
                 "https://ambientcg.com/api/v2/full_json"
                 "?limit=%d&offset=%d&include=imageData&sort=Alphabet",
                 ACG_PAGE, page * ACG_PAGE);

        sbuf_init(&body);
        if (fetch_url(ctx->f, url, &body) != 0) { sbuf_free(&body); return -1; }

        doc = yyjson_read(body.buf, body.len, 0);
        if (!doc) {
            fprintf(stderr, "  ambientcg: page %d was not JSON\n", page);
            sbuf_free(&body);
            return -1;
        }

        root = yyjson_doc_get_root(doc);
        if (total < 0) {
            total = ju_int(root, "numberOfResults", -1);
            if (total > 0)
                fprintf(stderr, "  catalogue reports %lld assets\n", total);
        }

        arr = yyjson_obj_get(root, "foundAssets");
        if (!arr || !yyjson_is_arr(arr)) {
            yyjson_doc_free(doc);
            sbuf_free(&body);
            fprintf(stderr,"  ambientcg: missing foundAssets array\n");
            return -1;
        }

        if (store_begin(ctx->st)) { yyjson_doc_free(doc); sbuf_free(&body); return -1; }
        yyjson_arr_iter_init(arr, &ai);
        while ((item = yyjson_arr_iter_next(&ai))) {
            asset  a;
            char  *aid, *dt, *rel;
            char   buf[512];

            n_page++;
            aid = ju_str(item, "assetId");
            if (!aid) continue;

            asset_init(&a);
            dt = ju_str(item, "dataType");

            snprintf(buf, sizeof buf, "ambientcg:%s", aid);
            a.id     = xstrdup(buf);
            a.source = xstrdup("ambientcg");

            a.title = ju_str(item, "customDisplayName");
            if (!a.title || !*a.title) {
                free(a.title);
                a.title = ju_str(item, "displayName");
            }
            if (!a.title) a.title = xstrdup(aid);

            a.source_url = ju_str(item, "shortLink");
            if (!a.source_url) {
                snprintf(buf, sizeof buf, "https://ambientcg.com/a/%s", aid);
                a.source_url = xstrdup(buf);
            }

            a.thumb_url = pick_preview(item);
            a.tags      = tags_with_datatype(item, dt);
            a.author    = xstrdup("ambientCG");

            a.type        = type_from_acg(dt);
            a.tileable    = tileable_from_acg(dt);
            a.style       = style_from_acg(dt);
            a.licence     = LIC_CC0;
            a.licence_url = xstrdup("https://docs.ambientcg.com/license/");
            a.price       = 0;
            a.popularity  = ju_int(item, "downloadCount", -1);

            rel = ju_str(item, "releaseDate");
            a.updated_at = parse_datetime(rel);
            free(rel);

            adapter_emit(ctx, &a);
            asset_free(&a);
            free(aid);
            free(dt);
        }

        if (store_commit(ctx->st)) { yyjson_doc_free(doc); sbuf_free(&body); return -1; }
        yyjson_doc_free(doc);
        sbuf_free(&body);

        if (n_page < (size_t)ACG_PAGE) {
            if(total>=0 && (long long)page*ACG_PAGE+(long long)n_page < total) {
                fprintf(stderr,"  ambientcg: incomplete page before reported total\n"); return -1;
            }
            break;
        }
        if (total > 0 && (long long)(page + 1) * ACG_PAGE >= total) break;
    }
    if(page==ACG_MAX_PAGES) { fprintf(stderr,"  ambientcg: page ceiling reached\n"); return -1; }
    return ctx->skipped ? -1 : 0;
}
