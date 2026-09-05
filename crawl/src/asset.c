#include "asset.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void asset_init(asset *a)
{
    memset(a, 0, sizeof *a);
    a->rigged     = TRI_UNKNOWN;
    a->tileable   = TRI_UNKNOWN;
    a->polycount  = -1;
    a->price      = -1;
    a->popularity = -1;
    a->rank_hint  = -1;
}

void asset_free(asset *a)
{
    xfree((void **)&a->id);
    xfree((void **)&a->source);
    xfree((void **)&a->title);
    xfree((void **)&a->author);
    xfree((void **)&a->source_url);
    xfree((void **)&a->thumb_url);
    xfree((void **)&a->licence_url);
    xfree((void **)&a->formats);
    xfree((void **)&a->tags);
    xfree((void **)&a->tags_auto);
    asset_init(a);
}

/* ---- enums -------------------------------------------------------------- */

static const struct { asset_type t; const char *s; } TYPES[] = {
    { AT_2D_SPRITE, "2d_sprite" }, { AT_3D_MODEL, "3d_model" },
    { AT_TEXTURE,   "texture"   }, { AT_AUDIO,    "audio"    },
    { AT_MUSIC,     "music"     }, { AT_FONT,     "font"     },
    { AT_SHADER,    "shader"    }, { AT_VFX,      "vfx"      },
    { AT_UI,        "ui"        },
    { AT_UNCLASSIFIABLE, "unclassifiable" },
    { AT_UNKNOWN,  "unknown"  },
};

const char *asset_type_str(asset_type t)
{
    size_t i;
    for (i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (TYPES[i].t == t) return TYPES[i].s;
    return "unknown";
}

asset_type asset_type_parse(const char *s)
{
    size_t i;
    if (!s) return AT_UNKNOWN;
    for (i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (str_ieq(TYPES[i].s, s)) return TYPES[i].t;
    return AT_UNKNOWN;
}

static const struct { licence_id l; const char *s; } LICS[] = {
    { LIC_CC0,           "cc0"           },
    { LIC_CC_BY,         "cc_by"         },
    { LIC_CC_BY_SA,      "cc_by_sa"      },
    { LIC_GPL,           "gpl"           },
    { LIC_STORE_EULA,    "store_eula"    },
    { LIC_PERSONAL_ONLY, "personal_only" },
    { LIC_OGA_BY,        "oga_by"        },
    { LIC_UNKNOWN,       "unknown"       },
};

const char *licence_str(licence_id l)
{
    size_t i;
    for (i = 0; i < sizeof LICS / sizeof LICS[0]; i++)
        if (LICS[i].l == l) return LICS[i].s;
    return "unknown";
}

licence_id licence_parse(const char *s)
{
    size_t i;
    if (!s) return LIC_UNKNOWN;
    for (i = 0; i < sizeof LICS / sizeof LICS[0]; i++)
        if (str_ieq(LICS[i].s, s)) return LICS[i].l;

    /* Only recognized complete spellings are safe. Substring matching would
     * turn CC-BY-NC into commercially usable CC-BY. */
    {
        static const struct { const char *name; licence_id id; } aliases[] = {
            {"CC0 1.0", LIC_CC0}, {"CC-BY 3.0", LIC_CC_BY},
            {"CC-BY 4.0", LIC_CC_BY}, {"CC BY 3.0", LIC_CC_BY},
            {"CC BY 4.0", LIC_CC_BY}, {"CC-BY-SA 3.0", LIC_CC_BY_SA},
            {"CC-BY-SA 4.0", LIC_CC_BY_SA}, {"OGA-BY 3.0", LIC_OGA_BY},
            {"OGA-BY 4.0", LIC_OGA_BY}, {"GPL 2.0", LIC_GPL}, {"GPL 3.0", LIC_GPL}
        };
        for(i=0;i<sizeof aliases/sizeof aliases[0];i++)
            if(str_ieq(s,aliases[i].name)) return aliases[i].id;
    }
    return LIC_UNKNOWN;
}

static const struct { style_id s; const char *n; } STYLES[] = {
    { ST_PIXEL, "pixel" }, { ST_LOWPOLY, "lowpoly" }, { ST_PBR, "pbr" },
    { ST_HANDPAINTED, "handpainted" }, { ST_FLAT, "flat" },
    { ST_VOXEL, "voxel" },
    { ST_UNCLASSIFIABLE, "unclassifiable" },
    { ST_UNKNOWN, "unknown" },
};

const char *style_str(style_id s)
{
    size_t i;
    for (i = 0; i < sizeof STYLES / sizeof STYLES[0]; i++)
        if (STYLES[i].s == s) return STYLES[i].n;
    return "unknown";
}

style_id style_parse(const char *s)
{
    size_t i;
    if (!s) return ST_UNKNOWN;
    for (i = 0; i < sizeof STYLES / sizeof STYLES[0]; i++)
        if (str_ieq(STYLES[i].n, s)) return STYLES[i].s;
    return ST_UNKNOWN;
}

/* ---- strict parsing for human input -------------------------------------- */

int asset_type_parse_strict(const char *s)
{
    size_t i;
    if (!s) return -1;
    for (i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (str_ieq(TYPES[i].s, s)) return (int)TYPES[i].t;
    return -1;
}

int licence_parse_strict(const char *s)
{
    size_t i;
    if (!s) return -1;
    for (i = 0; i < sizeof LICS / sizeof LICS[0]; i++)
        if (str_ieq(LICS[i].s, s)) return (int)LICS[i].l;
    return -1;
}

int style_parse_strict(const char *s)
{
    size_t i;
    if (!s) return -1;
    for (i = 0; i < sizeof STYLES / sizeof STYLES[0]; i++)
        if (str_ieq(STYLES[i].n, s)) return (int)STYLES[i].s;
    return -1;
}

/* Built from the same tables the parsers use, so an error message can never
 * drift out of date with what is actually accepted. */
static const char *join_values(char *buf, size_t n, const char *const *names,
                               size_t count)
{
    size_t i, used = 0;
    buf[0] = '\0';
    for (i = 0; i < count; i++) {
        int w = snprintf(buf + used, n - used, "%s%s", used ? " " : "", names[i]);
        if (w < 0 || (size_t)w >= n - used) break;
        used += (size_t)w;
    }
    return buf;
}

const char *asset_type_values(void)
{
    static char buf[256];
    const char *names[sizeof TYPES / sizeof TYPES[0]];
    size_t i;
    if (buf[0]) return buf;
    for (i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) names[i] = TYPES[i].s;
    return join_values(buf, sizeof buf, names, sizeof TYPES / sizeof TYPES[0]);
}

const char *licence_values(void)
{
    static char buf[256];
    const char *names[sizeof LICS / sizeof LICS[0]];
    size_t i;
    if (buf[0]) return buf;
    for (i = 0; i < sizeof LICS / sizeof LICS[0]; i++) names[i] = LICS[i].s;
    return join_values(buf, sizeof buf, names, sizeof LICS / sizeof LICS[0]);
}

const char *style_values(void)
{
    static char buf[256];
    const char *names[sizeof STYLES / sizeof STYLES[0]];
    size_t i;
    if (buf[0]) return buf;
    for (i = 0; i < sizeof STYLES / sizeof STYLES[0]; i++) names[i] = STYLES[i].n;
    return join_values(buf, sizeof buf, names, sizeof STYLES / sizeof STYLES[0]);
}

/* ---- derived licence facts ---------------------------------------------- */

/* Conservative on purpose. "unknown" is not commercially ok, because a wrong
 * yes here is the one bug in this project that can cost a user real money. */
int licence_commercial_ok(licence_id l)
{
    switch (l) {
    case LIC_CC0:
    case LIC_CC_BY:
    case LIC_CC_BY_SA:
    case LIC_OGA_BY:
        return 1;
    case LIC_GPL:           /* usable but viral; treated as not-safe by default */
    case LIC_STORE_EULA:    /* depends entirely on the store; do not assert */
    case LIC_PERSONAL_ONLY:
    case LIC_UNKNOWN:
    default:
        return 0;
    }
}

int licence_needs_attribution(licence_id l)
{
    switch (l) {
    case LIC_CC0:        return 0;
    case LIC_CC_BY:
    case LIC_CC_BY_SA:
    case LIC_OGA_BY:
    case LIC_GPL:        return 1;
    default:             return 1;  /* assume yes when unsure */
    }
}

/* ---- validation --------------------------------------------------------- */

int asset_validate(const asset *a, const char **why)
{
    if (!a->id     || !*a->id)     { *why = "missing id";         return -1; }
    if (!a->source || !*a->source) { *why = "missing source";     return -1; }
    if (!a->title  || !*a->title)  { *why = "missing title";      return -1; }
    if (!a->source_url || !*a->source_url) {
        *why = "missing source_url"; return -1;
    }
    if (strncmp(a->source_url, "https://", 8) != 0 && strncmp(a->source_url, "http://", 7) != 0) {
        *why = "source_url is not http(s)"; return -1;
    }
    if (!strchr(a->id, ':')) {
        *why = "id must be namespaced as source:local_id"; return -1;
    }
    return 0;
}
