/* Poly Haven - https://api.polyhaven.com
 *
 * The entire catalogue comes back in ONE request. Everything is CC0.
 * Their API terms ask for a unique User-Agent (we send one) and that the UI
 * makes clear the assets came from Poly Haven.
 *
 * Verified response shape: top level is an OBJECT keyed by asset id.
 *   "ArmChair_01": { "name":..., "type":2, "tags":[...], "authors":{"X":"All"},
 *                    "polycount":5626, "thumbnail_url":"https://cdn...", ... }
 *   type: 0 = HDRI, 1 = texture, 2 = model
 */
#include "adapter.h"
#include "jsonutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PH_URL "https://api.polyhaven.com/assets"

static asset_type type_from_ph(long long t)
{
    switch (t) {
    case 0:  return AT_TEXTURE;   /* HDRI - an environment texture */
    case 1:  return AT_TEXTURE;
    case 2:  return AT_3D_MODEL;
    default: return AT_UNKNOWN;
    }
}

static const char *formats_for(long long t)
{
    switch (t) {
    case 0:  return ".hdr,.exr";
    case 1:  return ".png,.exr,.jpg";
    case 2:  return ".blend,.fbx,.gltf";
    default: return NULL;
    }
}

int adapter_polyhaven(adapter_ctx *ctx)
{
    sbuf            body;
    yyjson_doc     *doc;
    yyjson_val     *root, *key;
    yyjson_obj_iter it;
    int             rc = -1;

    sbuf_init(&body);
    if (fetch_url(ctx->f, PH_URL, &body) != 0) goto done;

    doc = yyjson_read(body.buf, body.len, 0);
    if (!doc) { fprintf(stderr, "  polyhaven: response was not JSON\n"); goto done; }

    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        fprintf(stderr, "  polyhaven: expected a JSON object at the top level\n");
        yyjson_doc_free(doc);
        goto done;
    }

    if(store_begin(ctx->st)) { yyjson_doc_free(doc); goto done; }
    yyjson_obj_iter_init(root, &it);
    while ((key = yyjson_obj_iter_next(&it))) {
        yyjson_val *o  = yyjson_obj_iter_get_val(key);
        const char *id = yyjson_get_str(key);
        asset       a;
        long long   phtype;
        char        buf[512];

        if (!id || !yyjson_is_obj(o)) continue;

        asset_init(&a);
        phtype = ju_int(o, "type", -1);

        snprintf(buf, sizeof buf, "polyhaven:%s", id);
        a.id     = xstrdup(buf);
        a.source = xstrdup("polyhaven");
        a.title  = ju_str(o, "name");
        if (!a.title) a.title = xstrdup(id);
        a.author = ju_first_key(o, "authors");

        snprintf(buf, sizeof buf, "https://polyhaven.com/a/%s", id);
        a.source_url = xstrdup(buf);
        a.thumb_url  = ju_str(o, "thumbnail_url");

        a.tags = ju_arr_join(o, "tags", 1);
        if (!a.tags) a.tags = ju_arr_join(o, "categories", 1);

        a.type    = type_from_ph(phtype);
        a.licence = LIC_CC0;
        a.licence_url = xstrdup("https://creativecommons.org/publicdomain/zero/1.0/");
        a.formats = xstrdup(formats_for(phtype));

        a.polycount  = ju_int(o, "polycount", -1);
        a.updated_at = ju_int(o, "date_published", 0);
        a.price      = 0;
        a.popularity = ju_int(o, "download_count", -1);

        /* Poly Haven textures are seamless by design; HDRIs and models are not
         * "tileable" in the same sense, so leave those unknown. */
        if (phtype == 1) a.tileable = TRI_YES;
        if (phtype == 1) a.style    = ST_PBR;

        adapter_emit(ctx, &a);
        asset_free(&a);
    }

    yyjson_doc_free(doc);
    rc = store_commit(ctx->st) || ctx->skipped ? -1 : 0;

done:
    sbuf_free(&body);
    return rc;
}
