#include "classify.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined below with the tag tables it belongs to. */
static int type_is_visual(asset_type t);

const char *evidence_str(evidence e)
{
    switch (e) {
    case EV_DECLARED:     return "declared";
    case EV_FORMAT:       return "format";
    case EV_TAG:          return "tag";
    case EV_POLYCOUNT:    return "polycount";
    case EV_TITLE:        return "title";
    case EV_SOURCE_PRIOR: return "source_prior";
    default:              return "none";
    }
}

/* ---- matching helpers --------------------------------------------------- */

/* Tags arrive comma-joined. Match per token so "cars" cannot satisfy a search
 * for "scar", while still allowing "low-poly" to match inside "lowpoly-tree". */
static int tag_has(const char *tags, const char *word)
{
    const char *p = tags;

    if (!tags || !word) return 0;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t      len = end ? (size_t)(end - p) : strlen(p);
        char        tok[128];

        if (len > 0 && len < sizeof tok) {
            memcpy(tok, p, len);
            tok[len] = '\0';
            if (str_icontains(tok, word)) return 1;
        }
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

static int text_has(const char *text, const char *word)
{
    return text && word && str_icontains(text, word);
}

/* Whole-word match, so short tokens are usable. "lpc" as a substring would hit
 * "helpcentre"; "ui" would hit almost everything.
 *
 * Phrases are boundary-checked too, not substring-matched: "top down" as a
 * bare substring matches "laptop download". */
static int word_in(const char *text, const char *word)
{
    size_t wl;
    const char *p;

    if (!text || !word || !*word) return 0;

    wl = strlen(word);
    for (p = text; *p; p++) {
        size_t i = 0;
        if (p != text && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        while (i < wl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)word[i])) i++;
        if (i == wl && !(isalnum((unsigned char)p[i]) || p[i] == '_')) return 1;
    }
    return 0;
}

/* Sources write compound words three different ways and all of them appear
 * here: "sci-fi" (19), "sci fi" (68), "scifi". Same for 8-bit / 8 bit / 8bit,
 * top-down / top down / topdown, sprite sheet / spritesheet. Generating the
 * variants from one rule beats hand-listing them, which is how "8 Bit Music"
 * got missed in the first place. */
static int word_in_flex(const char *text, const char *word)
{
    char   v[64];
    size_t i, j, n;

    if (word_in(text, word)) return 1;
    if (!strpbrk(word, "- ")) return 0;
    n = strlen(word);
    if (n + 1 > sizeof v) return 0;

    for (i = j = 0; i < n; i++)                       /* joined: "scifi"  */
        if (word[i] != '-' && word[i] != ' ') v[j++] = word[i];
    v[j] = '\0';
    if (word_in(text, v)) return 1;

    for (i = 0; i < n; i++)                           /* spaced: "sci fi" */
        v[i] = (word[i] == '-') ? ' ' : word[i];
    v[n] = '\0';
    if (word_in(text, v)) return 1;

    for (i = 0; i < n; i++)                           /* hyphen: "sci-fi" */
        v[i] = (word[i] == ' ') ? '-' : word[i];
    v[n] = '\0';
    return word_in(text, v);
}

/* Does the comma-joined format list contain this extension? */
static int fmt_has(const char *formats, const char *ext)
{
    return tag_has(formats, ext);
}

/* ---- candidate accumulation --------------------------------------------- */

typedef struct {
    int best_conf;
    int best_val;
    evidence best_ev;
} vote;

static void vote_init(vote *v) { v->best_conf = 0; v->best_val = 0; v->best_ev = EV_NONE; }

static void vote_add(vote *v, int val, int conf, evidence ev)
{
    if (conf > v->best_conf) { v->best_conf = conf; v->best_val = val; v->best_ev = ev; }
}

/* ---- type --------------------------------------------------------------- */

static void vote_type_formats(vote *v, const char *f)
{
    if (!f || !*f) return;

    /* Unambiguous extensions. An .ogg is audio no matter what the title says. */
    if (fmt_has(f, ".wav") || fmt_has(f, ".ogg") || fmt_has(f, ".mp3") ||
        fmt_has(f, ".flac") || fmt_has(f, ".aiff"))
        vote_add(v, AT_AUDIO, CONF_FORMAT, EV_FORMAT);

    if (fmt_has(f, ".ttf") || fmt_has(f, ".otf") || fmt_has(f, ".woff"))
        vote_add(v, AT_FONT, CONF_FORMAT, EV_FORMAT);

    if (fmt_has(f, ".glb") || fmt_has(f, ".gltf") || fmt_has(f, ".fbx") ||
        fmt_has(f, ".obj") || fmt_has(f, ".blend") || fmt_has(f, ".dae"))
        vote_add(v, AT_3D_MODEL, CONF_FORMAT, EV_FORMAT);

    if (fmt_has(f, ".glsl") || fmt_has(f, ".hlsl") || fmt_has(f, ".shader"))
        vote_add(v, AT_SHADER, CONF_FORMAT, EV_FORMAT);

    if (fmt_has(f, ".hdr") || fmt_has(f, ".exr"))
        vote_add(v, AT_TEXTURE, CONF_FORMAT, EV_FORMAT);

    /* .png / .jpg are deliberately absent: they are equally a sprite sheet, a
     * texture, a UI atlas or a font sheet. Claiming otherwise from the
     * extension alone would be exactly the kind of confident-and-wrong the
     * threshold exists to prevent. */
}

static const struct { const char *word; asset_type t; } TYPE_WORDS[] = {
    { "hdri",        AT_TEXTURE   }, { "texture",  AT_TEXTURE   },
    { "material",    AT_TEXTURE   }, { "pbr",      AT_TEXTURE   },
    { "seamless",    AT_TEXTURE   }, { "tileable", AT_TEXTURE   },
    { "sprite",      AT_2D_SPRITE }, { "tileset",  AT_2D_SPRITE },
    { "spritesheet", AT_2D_SPRITE }, { "2d",       AT_2D_SPRITE },
    { "sfx",         AT_AUDIO     }, { "sound",    AT_AUDIO     },
    { "foley",       AT_AUDIO     },
    { "music",       AT_MUSIC     }, { "soundtrack", AT_MUSIC   },
    { "song",        AT_MUSIC     }, { "theme",    AT_MUSIC     },
    { "font",        AT_FONT      }, { "typeface", AT_FONT      },
    { "shader",      AT_SHADER    },
    { "vfx",         AT_VFX       }, { "particle", AT_VFX       },
    { "ui",          AT_UI        }, { "hud",      AT_UI        },
    { "icon",        AT_UI        }, { "button",   AT_UI        },
    { "3d",          AT_3D_MODEL  }, { "model",    AT_3D_MODEL  },
    { "mesh",        AT_3D_MODEL  }, { "lowpoly",  AT_3D_MODEL  },
};

static void vote_type_words(vote *v, const char *tags, const char *title)
{
    size_t i;
    /* Whole-word only. Substring matching here meant "ui" hit "building" and
     * "guide", quietly classifying models as interface art. */
    for (i = 0; i < sizeof TYPE_WORDS / sizeof TYPE_WORDS[0]; i++) {
        if (word_in_flex(tags, TYPE_WORDS[i].word))
            vote_add(v, TYPE_WORDS[i].t, CONF_TAG, EV_TAG);
        else if (word_in_flex(title, TYPE_WORDS[i].word))
            vote_add(v, TYPE_WORDS[i].t, CONF_TITLE, EV_TITLE);
    }
}

/* What a source publishes when we know nothing else. Weak by construction. */
static void vote_type_prior(vote *v, const char *source)
{
    if (!source) return;
    if (str_ieq(source, "ambientcg")) vote_add(v, AT_TEXTURE, CONF_PRIOR, EV_SOURCE_PRIOR);
}

/* ---- style -------------------------------------------------------------- */

static const struct { const char *word; style_id s; } STYLE_WORDS[] = {
    { "pixel",          ST_PIXEL       }, { "8-bit",       ST_PIXEL       },
    { "16-bit",         ST_PIXEL       }, { "retro",       ST_PIXEL       },
    { "1-bit",          ST_PIXEL       },
    /* Tile dimensions in a tag are a strong pixel-art tell: nobody labels a
     * PBR material "16x16". Safe against "128x128" because the match is
     * substring-within-token and "128x128" does not contain "8x8". */
    { "8x8",            ST_PIXEL       }, { "16x16",       ST_PIXEL       },
    { "32x32",          ST_PIXEL       }, { "64x64",       ST_PIXEL       },
    { "24x24",          ST_PIXEL       }, { "24x32",       ST_PIXEL       },
    { "48x48",          ST_PIXEL       },
    /* "LPC" is the Liberated Pixel Cup — a large OpenGameArt collection whose
     * whole point is a shared pixel style and compatible sprite dimensions.
     * It appears in 149 titles here and was previously invisible. */
    { "lpc",            ST_PIXEL       },
    { "roguelike",      ST_PIXEL       },
    { "voxel",          ST_VOXEL       }, { "magicavoxel", ST_VOXEL       },
    { "lowpoly",        ST_LOWPOLY     }, { "low-poly",    ST_LOWPOLY     },
    { "pbr",            ST_PBR         }, { "photogrammetry", ST_PBR      },
    { "photoscan",      ST_PBR         }, { "realistic",   ST_PBR         },
    { "hand-painted",   ST_HANDPAINTED }, { "handpainted", ST_HANDPAINTED },
    { "stylized",       ST_HANDPAINTED }, { "cartoon",     ST_HANDPAINTED },
    { "toon",           ST_HANDPAINTED },
    { "flat",           ST_FLAT        }, { "vector",      ST_FLAT        },
    { "minimalist",     ST_FLAT        },
};

static void vote_style_words(vote *v, const char *tags, const char *title)
{
    size_t i;
    for (i = 0; i < sizeof STYLE_WORDS / sizeof STYLE_WORDS[0]; i++) {
        if (word_in_flex(tags, STYLE_WORDS[i].word))
            vote_add(v, STYLE_WORDS[i].s, CONF_TAG, EV_TAG);
        else if (word_in_flex(title, STYLE_WORDS[i].word))
            vote_add(v, STYLE_WORDS[i].s, CONF_TITLE, EV_TITLE);
    }
    /* "low poly" as two words never survives tokenising, so check the phrase. */
    if (text_has(title, "low poly")) vote_add(v, ST_LOWPOLY, CONF_TITLE, EV_TITLE);
}

/* Geometry density is a real signal, not a keyword. A 900-triangle mesh is
 * low-poly whatever anyone called it. */
static void vote_style_polycount(vote *v, const asset *a)
{
    if (a->type != AT_3D_MODEL || a->polycount < 0) return;
    if (a->polycount > 0 && a->polycount <= 2000)
        vote_add(v, ST_LOWPOLY, CONF_POLYCOUNT, EV_POLYCOUNT);
    else if (a->polycount >= 50000)
        vote_add(v, ST_PBR, CONF_POLYCOUNT, EV_POLYCOUNT);
}

/* ---- entry point -------------------------------------------------------- */

void classify(const asset *a, classification *out)
{
    vote tv, sv;

    memset(out, 0, sizeof *out);

    /* Rule 1: a declared value is final. */
    if (a->type != AT_UNKNOWN && a->type != AT_UNCLASSIFIABLE) {
        out->type      = a->type;
        out->type_from = EV_DECLARED;
        out->type_conf = CONF_DECLARED;
    } else {
        vote_init(&tv);
        vote_type_formats(&tv, a->formats);
        vote_type_words(&tv, a->tags, a->title);
        vote_type_prior(&tv, a->source);

        if (tv.best_conf >= CONF_THRESHOLD) {
            out->type      = (asset_type)tv.best_val;
            out->type_from = tv.best_ev;
            out->type_conf = tv.best_conf;
        } else {
            out->type      = AT_UNCLASSIFIABLE;
            out->type_from = tv.best_ev;      /* what little there was */
            out->type_conf = tv.best_conf;
        }
    }

    if (a->style != ST_UNKNOWN && a->style != ST_UNCLASSIFIABLE) {
        out->style      = a->style;
        out->style_from = EV_DECLARED;
        out->style_conf = CONF_DECLARED;
    } else if (!type_is_visual(out->type)) {
        /* Every value in style_id describes how something looks. Asking which
         * of them a sound file is produces nonsense, so decline rather than
         * guess - and the genre lands in the tags instead. */
        out->style      = ST_UNCLASSIFIABLE;
        out->style_from = EV_NONE;
        out->style_conf = 0;
    } else {
        asset probe = *a;
        probe.type = out->type;    /* polycount rule needs the settled type */

        vote_init(&sv);
        vote_style_words(&sv, a->tags, a->title);
        vote_style_polycount(&sv, &probe);

        if (sv.best_conf >= CONF_THRESHOLD) {
            out->style      = (style_id)sv.best_val;
            out->style_from = sv.best_ev;
            out->style_conf = sv.best_conf;
        } else {
            out->style      = ST_UNCLASSIFIABLE;
            out->style_from = sv.best_ev;
            out->style_conf = sv.best_conf;
        }
    }
}

/* Exact token match, so appending is idempotent. tag_has() is a *contains*
 * test and would wrongly consider "3d_model" already present in a tag list
 * holding "3d_models". */
static int has_exact(const char *csv, const char *word)
{
    const char *p = csv;
    size_t      wl;

    if (!csv || !word) return 0;
    wl = strlen(word);
    while (*p) {
        const char *end = strchr(p, ',');
        size_t      len = end ? (size_t)(end - p) : strlen(p);
        if (len == wl && strncmp(p, word, wl) == 0) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

/* Set for the duration of classify_apply so add_tag can skip anything the
 * source already published. Single-threaded by design (decisions.md D5); a
 * parameter would have to be threaded through derive_tilesize's callback for
 * no benefit. */
static const char *g_source_tags;

static void add_tag(sbuf *b, const char *word)
{
    if (has_exact(b->buf, word)) return;          /* idempotent: safe to re-run */
    if (has_exact(g_source_tags, word)) return;   /* the source said it first */
    if (b->len) sbuf_appendz(b, ",");
    sbuf_appendz(b, word);
}

/* There used to be a copy_without() here, stripping a stale "unclassifiable"
 * tag from a row that had since become classifiable. Splitting derived tags
 * into their own column removed the need: tags_auto is rebuilt from nothing on
 * every run, so a verdict that changes is simply not written again. The
 * provenance split deleted a whole class of staleness bug rather than
 * relabelling it. */

/* ---- derived tags -------------------------------------------------------
 *
 * 928 of the 935 2D assets here carry no tags from their source at all — the
 * OpenGameArt listing pages simply don't publish them, and fetching 900 node
 * pages costs hours. But the titles are full of structure ("LPC Medieval
 * Tileset 16x16", "Isometric City Icons", "Animated Character"), and mining
 * that costs nothing and needs no network.
 *
 * These are facets, not claims about type. They stay searchable and
 * filterable, and being wrong about one is cheap in a way that being wrong
 * about a licence is not.
 */
typedef struct { const char *word; const char *tag; } tagrule;

/* Applies to everything. */
static const tagrule DERIVED_ANY[] = {
    { "character",   "character"   }, { "characters",  "character"   },
    { "enemy",       "character"   }, { "enemies",     "character"   },
    { "monster",     "character"   },
    { "animated",    "animated"    }, { "animation",   "animated"    },
    { "animations",  "animated"    }, { "walk cycle",  "animated"    },
    { "pack",        "pack"        }, { "kit",         "pack"        },
    { "medieval",    "medieval"    }, { "fantasy",     "fantasy"     },
    /* word_in_flex covers "sci fi" and "scifi" from this single entry. */
    { "sci-fi",      "scifi"       }, { "futuristic",  "scifi"       },
    { "horror",      "horror"      },
};

/* 2D, 3D, textures, UI, VFX, fonts - anything you look at. */
static const tagrule DERIVED_VISUAL[] = {
    /* Write every rule in its most-separated form: word_in_flex can join
     * "tile set" into "tileset", but cannot split "tileset" apart. */
    { "tile set",    "tileset"     },
    { "sprite sheet","spritesheet" },
    { "portrait",    "portrait"    }, { "icon",        "icon"        },
    { "icons",       "icon"        }, { "background",  "background"  },
    { "backgrounds", "background"  }, { "gui",         "gui"         },
    { "hud",         "gui"         }, { "button",      "gui"         },
    { "isometric",   "isometric"   }, { "iso",         "isometric"   },
    { "platformer",  "sideview"    }, { "side scroll", "sideview"    },
    { "side scroller","sideview"   },
    { "top down",    "topdown"     }, { "overhead",    "topdown"     },
    { "lpc",         "lpc"         }, { "rogue like",  "roguelike"   },
    { "seamless",    "tileable"    },
};

/* 3D only. "rigged" and "modular" are the two that decide whether a model is
 * usable for a given job at all: an unrigged character cannot be animated, and
 * a modular kit is meant to snap to its siblings. */
static const tagrule DERIVED_3D[] = {
    { "rigged",      "rigged"      }, { "rig",         "rigged"      },
    { "modular",     "modular"     },
    { "vehicle",     "vehicle"     }, { "car",         "vehicle"     },
    { "weapon",      "weapon"      }, { "gun",         "weapon"      },
    { "building",    "building"    }, { "house",       "building"    },
    { "prop",        "prop"        }, { "props",       "prop"        },
    { "furniture",   "furniture"   }, { "food",        "food"        },
    { "tree",        "nature"      }, { "trees",       "nature"      },
    { "plant",       "nature"      }, { "nature",      "nature"      },
    { "rock",        "nature"      }, { "rocks",       "nature"      },
    { "terrain",     "terrain"     }, { "animal",      "animal"      },
    { "container",   "container"   }, { "barrel",      "container"   },
    { "crate",       "container"   }, { "box",         "container"   },
    { "tool",        "tool"        }, { "scanned",     "photoscan"   },
    { "photogrammetry","photoscan" },
};

/* Audio and music only. Visual-style words must never reach these rows. */
static const tagrule DERIVED_AUDIO[] = {
    /* The single most load-bearing audio attribute: a track that does not loop
     * is unusable as background music, and nothing exposes it as a field. */
    { "loop",        "loopable"    }, { "loopable",    "loopable"    },
    { "looping",     "loopable"    }, { "seamless",    "loopable"    },
    { "sfx",         "sfx"         }, { "sound effect","sfx"         },
    { "sound effects","sfx"        }, { "foley",       "sfx"         },
    { "theme",       "theme"       }, { "soundtrack",  "theme"       },
    { "bgm",         "theme"       },
    { "ambient",     "ambience"    }, { "ambience",    "ambience"    },
    { "atmosphere",  "ambience"    },
    { "chip tune",   "chiptune"    }, { "8-bit",       "chiptune"    },
    { "16-bit",      "chiptune"    }, { "nes",         "chiptune"    },
    { "game boy",    "chiptune"    },
    { "orchestral",  "orchestral"  }, { "orchestra",   "orchestral"  },
    { "electronic",  "electronic"  }, { "synth",       "electronic"  },
    { "piano",       "piano"       }, { "guitar",      "guitar"      },
    { "drum",        "percussion"  }, { "percussion",  "percussion"  },
    { "voice",       "voice"       }, { "vocal",       "voice"       },
    { "announcer",   "voice"       },
    { "battle",      "battle"      }, { "boss",        "battle"      },
    { "combat",      "battle"      },
    { "menu",        "menu"        }, { "jingle",      "jingle"      },
    { "stinger",     "jingle"      },
    { "explosion",   "impact"      }, { "impact",      "impact"      },
    { "foot step",   "footsteps"   }, { "footsteps",   "footsteps"   },
};

/* Triangle count decides whether a model can be used at all - this index holds
 * everything from a 182-triangle prop to a 17-million-triangle photoscan, and
 * no source offers it as a filter. The 3D analogue of 2D's tile size. */
static const char *poly_bucket(long long n)
{
    if (n <= 0)        return NULL;
    if (n < 1000)      return "under-1k";
    if (n < 10000)     return "1k-10k";
    if (n < 100000)    return "10k-100k";
    return "over-100k";
}

/* Visual style is meaningless for audio: "8-Bit Battle Theme" is a music
 * genre, not pixel art, and 50 tracks were being labelled `pixel` because of
 * it. Fonts stay visual - pixel fonts are real. */
static int type_is_visual(asset_type t)
{
    return !(t == AT_AUDIO || t == AT_MUSIC);
}

/* Pull "16x16" style dimensions out of free text. For 2D work this is the
 * single most load-bearing attribute there is — you cannot mix 16px and 32px
 * art in one game, and no source exposes it as a field. */
static void derive_tilesize(sbuf *b, const char *text,
                            void (*add)(sbuf *, const char *))
{
    const char *p;
    if (!text) return;

    for (p = text; *p; p++) {
        const char *s = p;
        int a = 0, c2 = 0;

        if (!isdigit((unsigned char)*p)) continue;
        if (p != text && (isalnum((unsigned char)p[-1]))) continue;

        while (isdigit((unsigned char)*p)) { a = a * 10 + (*p - '0'); p++; }
        if (*p != 'x' && *p != 'X') { p = s; continue; }
        p++;
        if (!isdigit((unsigned char)*p)) { p = s; continue; }
        while (isdigit((unsigned char)*p)) { c2 = c2 * 10 + (*p - '0'); p++; }

        /* Sprite dimensions, not a texture resolution or a year. */
        if (a >= 4 && a <= 256 && c2 >= 4 && c2 <= 256) {
            char tag[16];
            snprintf(tag, sizeof tag, "%dx%d", a, c2);
            add(b, tag);
        }
        p--;
    }
}

char *classify_apply(asset *a, const classification *c)
{
    sbuf b;

    a->type  = c->type;
    a->style = c->style;

    /* Everything below lands in tags_auto. a->tags stays exactly as the source
     * published it, so the two are always separable afterwards. */
    sbuf_init(&b);
    g_source_tags = a->tags;

    if (c->type != AT_UNCLASSIFIABLE)
        add_tag(&b, asset_type_str(c->type));
    if (c->style != ST_UNCLASSIFIABLE && c->style != ST_UNKNOWN)
        add_tag(&b, style_str(c->style));
    if (c->type == AT_UNCLASSIFIABLE || c->style == ST_UNCLASSIFIABLE)
        add_tag(&b, "unclassifiable");

    /* Mine the title for structure the source never published. Which
     * vocabulary applies depends on the type - "rock" is a stone on a model
     * and a genre on a track, and mixing the tables produces nonsense. */
    {
        size_t i;
        const tagrule *sets[3] = { DERIVED_ANY, NULL, NULL };
        size_t         lens[3] = { sizeof DERIVED_ANY / sizeof DERIVED_ANY[0], 0, 0 };
        int            n = 1;

        if (type_is_visual(c->type)) {
            sets[n] = DERIVED_VISUAL;
            lens[n] = sizeof DERIVED_VISUAL / sizeof DERIVED_VISUAL[0];
            n++;
            if (c->type == AT_3D_MODEL) {
                sets[n] = DERIVED_3D;
                lens[n] = sizeof DERIVED_3D / sizeof DERIVED_3D[0];
                n++;
            }
        } else {
            sets[n] = DERIVED_AUDIO;
            lens[n] = sizeof DERIVED_AUDIO / sizeof DERIVED_AUDIO[0];
            n++;
        }

        {
            int k;
            for (k = 0; k < n; k++)
                for (i = 0; i < lens[k]; i++)
                    if (word_in_flex(a->title, sets[k][i].word) ||
                        word_in_flex(a->tags,  sets[k][i].word))
                        add_tag(&b, sets[k][i].tag);
        }

        if (type_is_visual(c->type) && c->type != AT_3D_MODEL)
            derive_tilesize(&b, a->title, add_tag);

        if (c->type == AT_3D_MODEL) {
            const char *pb = poly_bucket(a->polycount);
            if (pb) add_tag(&b, pb);
        }
    }

    g_source_tags = NULL;
    free(a->tags_auto);
    a->tags_auto = b.buf ? b.buf : xstrdup("");
    return a->tags_auto;
}
