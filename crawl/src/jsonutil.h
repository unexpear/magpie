/* jsonutil.h - thin yyjson accessors that return owned copies or defaults. */
#ifndef JSONUTIL_H
#define JSONUTIL_H

#include "asset.h"
#include "yyjson.h"

/* All return NULL / the default when the key is absent or the wrong type. */
char     *ju_str(yyjson_val *o, const char *k);            /* caller frees */
long long ju_int(yyjson_val *o, const char *k, long long dflt);
tribool   ju_tri(yyjson_val *o, const char *k);
double    ju_num(yyjson_val *o, const char *k, double dflt);

/* Join a JSON string array into "a,b,c". Caller frees. NULL if absent. */
char *ju_arr_join(yyjson_val *o, const char *k, int lowercase);

/* First key of an object, as a string. Caller frees. Used for shapes like
 * Poly Haven's "authors": {"Name": "role"}. */
char *ju_first_key(yyjson_val *o, const char *k);

#endif
