/* http.h - one GET, two backends.
 *
 * WinHTTP on Windows (ships with the OS, nothing to install),
 * libcurl everywhere else. See decisions.md D4.
 */
#ifndef HTTP_H
#define HTTP_H

#include "util.h"

typedef struct {
    long status;              /* HTTP status, or 0 on transport failure */
    sbuf body;
    char etag[256];
    char last_modified[128];
    char location[4096];
    int  retry_after;         /* seconds, parsed from Retry-After; 0 if absent */
    char error[256];          /* transport error text when status == 0 */
} http_resp;

/* user_agent must include a contact URL. Poly Haven requires a unique UA. */
int  http_init(const char *user_agent);
void http_cleanup(void);

/* Conditional GET. Pass NULL/empty for etag_in / lastmod_in to skip.
 * Returns 0 if a response was obtained (including 304/404/429), -1 on
 * transport failure. Caller must http_resp_free either way. */
int  http_get(const char *url, const char *etag_in, const char *lastmod_in,
              http_resp *out);

/* Status only, no body. Used by the link checker: asking 10,000 sites for a
 * status code should not also download 10,000 pages. These primitives never
 * follow redirects; the fetcher checks policy and charges each hop. */
int  http_head(const char *url, http_resp *out);

void http_resp_free(http_resp *r);
int http_retry_after(const char *value);

#endif
