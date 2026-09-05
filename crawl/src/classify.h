/* classify.h - work out asset type and style, and say how sure we are.
 *
 * Design rules, in order of importance:
 *
 *  1. Never overwrite what a source told us. A declared value beats any
 *     inference, always.
 *  2. Weigh evidence rather than taking the first keyword that hits. Formats
 *     are stronger than tags, tags are stronger than titles, a source's
 *     general habits are weakest.
 *  3. When the evidence is too thin, return UNCLASSIFIABLE - not UNKNOWN, and
 *     not a coin flip dressed up as an answer. "Unknown" means nobody looked.
 *     "Unclassifiable" means we looked and the data isn't there. Users can
 *     filter for those and go fix them; they cannot fix a wrong guess they
 *     never knew was a guess.
 *  4. Report the evidence, so a wrong call can be traced to the rule that
 *     made it instead of being folded into a black box.
 */
#ifndef CLASSIFY_H
#define CLASSIFY_H

#include "asset.h"

typedef enum {
    EV_NONE = 0,      /* nothing to go on            */
    EV_DECLARED,      /* the source stated it        */
    EV_FORMAT,        /* file extensions             */
    EV_TAG,           /* tag list                    */
    EV_POLYCOUNT,     /* geometry density            */
    EV_TITLE,         /* words in the title          */
    EV_SOURCE_PRIOR   /* what this source mostly is  */
} evidence;

/* Confidence each evidence class carries, 0-100. */
#define CONF_DECLARED   100
#define CONF_FORMAT      85
#define CONF_TAG         70
#define CONF_POLYCOUNT   60
#define CONF_TITLE       50
#define CONF_PRIOR       35

/* Below this, we refuse to answer. CONF_PRIOR deliberately sits under it:
 * "ambientCG mostly publishes textures" is not evidence about one asset. */
#define CONF_THRESHOLD   50

typedef struct {
    asset_type type;
    style_id   style;
    evidence   type_from;
    evidence   style_from;
    int        type_conf;
    int        style_conf;
} classification;

const char *evidence_str(evidence e);

/* Fills *out. Does not modify the asset. */
void classify(const asset *a, classification *out);

/* Apply a classification and write every derived tag into a->tags_auto -
 * the verdict itself ("pixel", "3d_model", "unclassifiable") plus whatever the
 * title mining found ("tileset", "loopable", "16x16", "under-1k").
 *
 * a->tags is READ but never written: it holds what the source published, and
 * keeping the two apart is what lets a bad mining rule be found and undone
 * later. Tags already present in a->tags are not repeated here.
 *
 * Returns a->tags_auto, owned by the asset. Safe to call repeatedly. */
char *classify_apply(asset *a, const classification *c);

#endif
