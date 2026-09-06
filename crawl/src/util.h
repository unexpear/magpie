/* util.h - small string helpers. Nothing clever. */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* Growable byte buffer. Always NUL-terminated at buf[len]. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} sbuf;

void  sbuf_init(sbuf *b);
void  sbuf_free(sbuf *b);
void  sbuf_reset(sbuf *b);
int   sbuf_append(sbuf *b, const char *data, size_t n);
int   sbuf_appendz(sbuf *b, const char *s);
int   sbuf_printf(sbuf *b, const char *fmt, ...);
/* Append s as a quoted, escaped JSON string. NULL becomes null. */
int   sbuf_json_str(sbuf *b, const char *s);

/* strdup that never returns a live pointer to freed memory; NULL in -> NULL out. */
char *xstrdup(const char *s);
/* strdup of at most n bytes. */
char *xstrndup(const char *s, size_t n);
/* free(*p) then *p = NULL. */
void  xfree(void **p);

/* Lowercase in place (ASCII only - deliberate, tags are ASCII). */
void  str_lower(char *s);
/* Trim ASCII whitespace in place, returns s. */
char *str_trim(char *s);
/* Case-insensitive substring test. */
int   str_icontains(const char *hay, const char *needle);
/* Case-insensitive equality. */
int   str_ieq(const char *a, const char *b);

/* Join a NULL-terminated array of strings with a comma. Caller frees. */
char *str_join(char **items, size_t n, const char *sep);

/* Monotonic-ish milliseconds since process start. */
long long now_ms(void);
/* Unix seconds. */
long long now_unix(void);
/* Sleep. Portable enough. */
void sleep_ms(int ms);

/* Parse "2026-08-08 20:00:00" (UTC) to unix seconds. 0 on failure. */
long long parse_datetime(const char *s);

/* Extract the host from a URL into out (size n). Returns 0 on success. */
int url_host(const char *url, char *out, size_t n);
long long wall_ms(void);
int stop_requested(void);
int sleep_interruptible(int ms);
void env_set(const char *name, const char *value);
void unique_token(char out[80]);
int url_origin(const char *url, char *out, size_t n);
char *url_resolve(const char *base, const char *ref);
int shell_arg(sbuf *b, const char *arg);

long long disk_free_bytes(const char *file_path);

int atomic_replace(const char *from, const char *to);

#endif
