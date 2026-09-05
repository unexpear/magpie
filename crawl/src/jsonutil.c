#include "jsonutil.h"
#include "util.h"

#include <stdlib.h>

char *ju_str(yyjson_val *o, const char *k)
{
    yyjson_val *v = o ? yyjson_obj_get(o, k) : NULL;
    const char *s = v ? yyjson_get_str(v) : NULL;
    return s ? xstrdup(s) : NULL;
}

long long ju_int(yyjson_val *o, const char *k, long long dflt)
{
    yyjson_val *v = o ? yyjson_obj_get(o, k) : NULL;
    if (!v || yyjson_is_null(v)) return dflt;
    if (yyjson_is_int(v)) return yyjson_get_sint(v);
    if (yyjson_is_num(v)) return (long long)yyjson_get_num(v);
    return dflt;
}

double ju_num(yyjson_val *o, const char *k, double dflt)
{
    yyjson_val *v = o ? yyjson_obj_get(o, k) : NULL;
    if (!v || yyjson_is_null(v) || !yyjson_is_num(v)) return dflt;
    return yyjson_get_num(v);
}

tribool ju_tri(yyjson_val *o, const char *k)
{
    yyjson_val *v = o ? yyjson_obj_get(o, k) : NULL;
    if (!v || yyjson_is_null(v)) return TRI_UNKNOWN;
    if (yyjson_is_bool(v)) return yyjson_get_bool(v) ? TRI_YES : TRI_NO;
    return TRI_UNKNOWN;
}

char *ju_arr_join(yyjson_val *o, const char *k, int lowercase)
{
    yyjson_val     *arr = o ? yyjson_obj_get(o, k) : NULL;
    yyjson_val     *it;
    yyjson_arr_iter ai;
    sbuf            b;

    if (!arr || !yyjson_is_arr(arr)) return NULL;
    sbuf_init(&b);
    yyjson_arr_iter_init(arr, &ai);
    while ((it = yyjson_arr_iter_next(&ai))) {
        const char *s = yyjson_get_str(it);
        if (!s || !*s) continue;
        if (b.len) sbuf_appendz(&b, ",");
        sbuf_appendz(&b, s);
    }
    if (lowercase && b.buf) str_lower(b.buf);
    return b.buf;
}

char *ju_first_key(yyjson_val *o, const char *k)
{
    yyjson_val     *obj = o ? yyjson_obj_get(o, k) : NULL;
    yyjson_obj_iter it;
    yyjson_val     *key;

    if (!obj || !yyjson_is_obj(obj)) return NULL;
    yyjson_obj_iter_init(obj, &it);
    key = yyjson_obj_iter_next(&it);
    if (!key) return NULL;
    return xstrdup(yyjson_get_str(key));
}
