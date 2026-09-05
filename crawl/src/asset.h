/* asset.h - the one schema everything normalizes into.
 *
 * This struct is the whole point of the project. Every source, native or
 * external, ends up here. Get it right and the rest is plumbing.
 */
#ifndef ASSET_H
#define ASSET_H

/* AT_UNKNOWN means "nobody has looked yet".
 * AT_UNCLASSIFIABLE means "the classifier looked and there genuinely is not
 * enough information". Those are different facts and collapsing them hides a
 * data-quality problem behind a shrug. */
typedef enum {
    AT_UNKNOWN = 0,
    AT_2D_SPRITE,
    AT_3D_MODEL,
    AT_TEXTURE,
    AT_AUDIO,
    AT_MUSIC,
    AT_FONT,
    AT_SHADER,
    AT_VFX,
    AT_UI,
    AT_UNCLASSIFIABLE
} asset_type;

typedef enum {
    LIC_UNKNOWN = 0,
    LIC_CC0,
    LIC_CC_BY,
    LIC_CC_BY_SA,
    LIC_GPL,
    LIC_STORE_EULA,
    LIC_PERSONAL_ONLY,
    /* OpenGameArt's own attribution licence. Behaves like CC-BY, but it is a
     * distinct licence with its own text - labelling it cc_by would be a small
     * lie in the one field where lying is expensive. */
    LIC_OGA_BY
} licence_id;

typedef enum {
    ST_UNKNOWN = 0,
    ST_PIXEL,
    ST_LOWPOLY,
    ST_PBR,
    ST_HANDPAINTED,
    ST_FLAT,
    ST_VOXEL,
    ST_UNCLASSIFIABLE
} style_id;

/* -1 means "we do not know", which is different from 0/false. */
typedef signed char tribool;
#define TRI_UNKNOWN (-1)
#define TRI_NO       (0)
#define TRI_YES      (1)

typedef struct {
    char *id;          /* "source:local_id", unique          */
    char *source;      /* short slug                          */
    char *title;
    char *author;
    char *source_url;  /* human landing page, never a direct download */
    char *thumb_url;
    char *licence_url;
    char *formats;     /* comma-joined, ".glb,.fbx"           */
    /* What the source actually published. Never written by the classifier -
     * mixing the two made it impossible to tell Poly Haven's "vintage" from a
     * tag we guessed off a title, and an unfixable mining rule from a
     * legitimate one. */
    char *tags;        /* comma-joined, lowercase             */
    char *tags_auto;   /* derived by classify.c; same format  */

    asset_type type;
    licence_id licence;
    style_id   style;

    tribool rigged;
    tribool tileable;

    long long polycount;   /* -1 unknown */
    double    price;       /* -1 unknown, 0 free */
    long long updated_at;  /* unix seconds, 0 unknown */

    /* Raw downloads / favourites as the source reports them. Not comparable
     * across sources - store.c turns it into a per-source percentile. */
    long long popularity;  /* -1 unknown */
    /* Position in a popularity-sorted listing, when that is all a source
     * gives us (OpenGameArt). 0 = best. -1 unknown. */
    long long rank_hint;
} asset;

void asset_init(asset *a);
void asset_free(asset *a);

/* Enum <-> string. to_str never returns NULL.
 *
 * The plain parse() functions are lenient on purpose - they read messy strings
 * scraped off websites ("CC-BY 3.0", "Creative Commons Zero"). That tolerance
 * is wrong for command-line arguments, where a typo should be an error rather
 * than a confident wrong answer: `--licence cczero` used to match CC0 via a
 * substring rule, and `--type banana` silently returned an empty result set.
 * Use the _strict variants for anything a human typed. */
const char *asset_type_str(asset_type t);
asset_type  asset_type_parse(const char *s);
const char *licence_str(licence_id l);
licence_id  licence_parse(const char *s);
const char *style_str(style_id s);
style_id    style_parse(const char *s);

/* Exact table match only. Return -1 when the value is not recognised. */
int asset_type_parse_strict(const char *s);
int licence_parse_strict(const char *s);
int style_parse_strict(const char *s);

/* Space-separated list of accepted values, for error messages. */
const char *asset_type_values(void);
const char *licence_values(void);
const char *style_values(void);

/* Derived, never trusted from an adapter. See ADAPTER_PROTOCOL.md. */
int licence_commercial_ok(licence_id l);
int licence_needs_attribution(licence_id l);

/* Type and style inference lives in classify.h - it weighs evidence and
 * reports confidence, which a bare guess() cannot. */

/* Required fields present and sane? Returns 0 if ok, else a reason string
 * is written to *why (static storage, do not free). */
int asset_validate(const asset *a, const char **why);

#endif
