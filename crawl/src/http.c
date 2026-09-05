#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>

static char g_ua[256];

/* =======================================================================
 *  Windows / WinHTTP
 * ======================================================================= */
#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_OPTION_DECOMPRESSION
#  define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#  define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif

static HINTERNET g_session;

static wchar_t *utf8_to_wide(const char *s)
{
    int      n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *wide_to_utf8(const wchar_t *w)
{
    int   n;
    char *s;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

int http_init(const char *user_agent)
{
    wchar_t *wua;
    DWORD    opt = WINHTTP_DECOMPRESSION_FLAG_ALL;

    snprintf(g_ua, sizeof g_ua, "%s", user_agent);
    wua = utf8_to_wide(user_agent);
    if (!wua) return -1;

    g_session = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    free(wua);
    if (!g_session) return -1;

    /* Ask for gzip. Costs the source less bandwidth; ignored on old Windows. */
    WinHttpSetOption(g_session, WINHTTP_OPTION_DECOMPRESSION, &opt, sizeof opt);

    {
        DWORD timeout = 30000;
        WinHttpSetTimeouts(g_session, timeout, timeout, timeout, timeout);
    }
    return 0;
}

void http_cleanup(void)
{
    if (g_session) { WinHttpCloseHandle(g_session); g_session = NULL; }
}

static void query_header(HINTERNET req, DWORD which, char *out, size_t outsz)
{
    wchar_t buf[1024];
    DWORD   len = sizeof buf;

    out[0] = '\0';
    if (WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, buf, &len,
                            WINHTTP_NO_HEADER_INDEX)) {
        char *u = wide_to_utf8(buf);
        if (u) { snprintf(out, outsz, "%s", u); free(u); }
    }
}

static int win_request(const wchar_t *method, const char *url,
                       const char *etag_in, const char *lastmod_in,
                       int want_body, http_resp *out)
{
    URL_COMPONENTS  uc;
    wchar_t         host[256], path[2048], *wurl = NULL;
    HINTERNET       conn = NULL, req = NULL;
    DWORD           flags = 0, status = 0, slen = sizeof status;
    int             rc = -1;

    memset(out, 0, sizeof *out);
    sbuf_init(&out->body);

    wurl = utf8_to_wide(url);
    if (!wurl) { snprintf(out->error, sizeof out->error, "bad url encoding"); goto done; }

    memset(&uc, 0, sizeof uc);
    uc.dwStructSize      = sizeof uc;
    uc.lpszHostName      = host;  uc.dwHostNameLength      = sizeof host / sizeof host[0];
    uc.lpszUrlPath       = path;  uc.dwUrlPathLength       = sizeof path / sizeof path[0];
    /* Extra info (query string) is folded into path below. */
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(out->error, sizeof out->error, "WinHttpCrackUrl failed (%lu)",
                 (unsigned long)GetLastError());
        goto done;
    }

    conn = WinHttpConnect(g_session, host, uc.nPort, 0);
    if (!conn) {
        snprintf(out->error, sizeof out->error, "connect failed (%lu)",
                 (unsigned long)GetLastError());
        goto done;
    }

    if (uc.nScheme == INTERNET_SCHEME_HTTPS) flags |= WINHTTP_FLAG_SECURE;
    req = WinHttpOpenRequest(conn, method, path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        snprintf(out->error, sizeof out->error, "open request failed (%lu)",
                 (unsigned long)GetLastError());
        goto done;
    }

    {
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof disable)) {
            snprintf(out->error, sizeof out->error, "cannot disable redirects"); goto done;
        }
    }

    if (etag_in && *etag_in) {
        char     hdr[512];
        wchar_t *w;
        snprintf(hdr, sizeof hdr, "If-None-Match: %s", etag_in);
        if ((w = utf8_to_wide(hdr))) {
            WinHttpAddRequestHeaders(req, w, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            free(w);
        }
    }
    if (lastmod_in && *lastmod_in) {
        char     hdr[512];
        wchar_t *w;
        snprintf(hdr, sizeof hdr, "If-Modified-Since: %s", lastmod_in);
        if ((w = utf8_to_wide(hdr))) {
            WinHttpAddRequestHeaders(req, w, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            free(w);
        }
    }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        snprintf(out->error, sizeof out->error, "request failed (%lu)",
                 (unsigned long)GetLastError());
        goto done;
    }

    WinHttpQueryHeaders(req,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                        WINHTTP_NO_HEADER_INDEX);
    out->status = (long)status;

    query_header(req, WINHTTP_QUERY_LOCATION, out->location, sizeof out->location);
    query_header(req, WINHTTP_QUERY_ETAG,          out->etag,          sizeof out->etag);
    query_header(req, WINHTTP_QUERY_LAST_MODIFIED, out->last_modified, sizeof out->last_modified);
    {
        char ra[64];
        query_header(req, WINHTTP_QUERY_RETRY_AFTER, ra, sizeof ra);
        out->retry_after = http_retry_after(ra);
    }

    if (want_body) {
        for (;;) {
            DWORD avail = 0, got = 0;
            char  chunk[16384];

            if (!WinHttpQueryDataAvailable(req, &avail)) {
                snprintf(out->error, sizeof out->error, "body query failed (%lu)", (unsigned long)GetLastError()); goto done;
            }
            if (avail == 0) break;
            if (avail > sizeof chunk) avail = sizeof chunk;
            if (!WinHttpReadData(req, chunk, avail, &got)) {
                snprintf(out->error, sizeof out->error, "body read failed (%lu)", (unsigned long)GetLastError()); goto done;
            }
            if (got == 0) break;
            if (out->body.len + got > 64 * 1024 * 1024 ||
                sbuf_append(&out->body, chunk, got) != 0) {
                snprintf(out->error, sizeof out->error, "out of memory");
                goto done;
            }
        }
    }

    if (want_body) {
        char length[64], encoding[128], *end;
        unsigned long long expected;
        query_header(req, WINHTTP_QUERY_CONTENT_LENGTH, length, sizeof length);
        query_header(req, WINHTTP_QUERY_CONTENT_ENCODING, encoding, sizeof encoding);
        if (*length && (!*encoding || str_ieq(encoding,"identity"))) {
            expected=strtoull(length,&end,10);
            if (*end || expected != out->body.len) {
                snprintf(out->error,sizeof out->error,"incomplete response body"); goto done;
            }
        }
    }

    rc = 0;

done:
    if (req)  WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    free(wurl);
    return rc;
}

int http_get(const char *url, const char *etag_in, const char *lastmod_in,
             http_resp *out)
{
    return win_request(L"GET", url, etag_in, lastmod_in, 1, out);
}

int http_head(const char *url, http_resp *out)
{
    return win_request(L"HEAD", url, NULL, NULL, 0, out);
}

/* =======================================================================
 *  Everything else / libcurl
 * ======================================================================= */
#else

#include <curl/curl.h>
#include <strings.h>

static CURL *g_curl;

int http_init(const char *user_agent)
{
    snprintf(g_ua, sizeof g_ua, "%s", user_agent);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
    return g_curl ? 0 : -1;
}

void http_cleanup(void)
{
    if (g_curl) { curl_easy_cleanup(g_curl); g_curl = NULL; }
    curl_global_cleanup();
}

static size_t on_body(char *p, size_t sz, size_t n, void *ud)
{
    sbuf *b = ud;
    if (sz * n > 64 * 1024 * 1024 - b->len || sbuf_append(b, p, sz * n) != 0) return 0;
    return sz * n;
}

static size_t on_header(char *p, size_t sz, size_t n, void *ud)
{
    http_resp  *r   = ud;
    size_t      len = sz * n;
    const char *v;

    if (len > 5 && strncasecmp(p, "ETag:", 5) == 0) {
        v = p + 5;
        while (*v == ' ') v++;
        snprintf(r->etag, sizeof r->etag, "%.*s", (int)(len - (size_t)(v - p)), v);
        str_trim(r->etag);
    } else if (len > 14 && strncasecmp(p, "Last-Modified:", 14) == 0) {
        v = p + 14;
        while (*v == ' ') v++;
        snprintf(r->last_modified, sizeof r->last_modified,
                 "%.*s", (int)(len - (size_t)(v - p)), v);
        str_trim(r->last_modified);
    } else if (len > 12 && strncasecmp(p, "Retry-After:", 12) == 0) {
        char value[128]; snprintf(value,sizeof value,"%.*s",(int)(len-12),p+12);
        r->retry_after = http_retry_after(str_trim(value));
    } else if (len > 9 && strncasecmp(p, "Location:", 9) == 0) {
        char value[4096]; snprintf(value,sizeof value,"%.*s",(int)(len-9),p+9);
        snprintf(r->location,sizeof r->location,"%s",str_trim(value));
    }
    return len;
}

int http_get(const char *url, const char *etag_in, const char *lastmod_in,
             http_resp *out)
{
    struct curl_slist *hdrs = NULL;
    CURLcode           res;
    long               status = 0;
    char               buf[512];

    memset(out, 0, sizeof *out);
    sbuf_init(&out->body);

    curl_easy_reset(g_curl);
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_USERAGENT, g_ua);
    curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(g_curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(g_curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &out->body);
    curl_easy_setopt(g_curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(g_curl, CURLOPT_HEADERDATA, out);

    if (etag_in && *etag_in) {
        snprintf(buf, sizeof buf, "If-None-Match: %s", etag_in);
        hdrs = curl_slist_append(hdrs, buf);
    }
    if (lastmod_in && *lastmod_in) {
        snprintf(buf, sizeof buf, "If-Modified-Since: %s", lastmod_in);
        hdrs = curl_slist_append(hdrs, buf);
    }
    if (hdrs) curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, hdrs);

    res = curl_easy_perform(g_curl);
    if (hdrs) curl_slist_free_all(hdrs);

    if (res != CURLE_OK) {
        snprintf(out->error, sizeof out->error, "%s", curl_easy_strerror(res));
        return -1;
    }
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &status);
    out->status = status;
    return 0;
}

int http_head(const char *url, http_resp *out)
{
    CURLcode res;
    long     status = 0;

    memset(out, 0, sizeof *out);
    sbuf_init(&out->body);

    curl_easy_reset(g_curl);
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_USERAGENT, g_ua);
    curl_easy_setopt(g_curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(g_curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(g_curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(g_curl, CURLOPT_HEADERDATA, out);

    res = curl_easy_perform(g_curl);
    if (res != CURLE_OK) {
        snprintf(out->error, sizeof out->error, "%s", curl_easy_strerror(res));
        return -1;
    }
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &status);
    out->status = status;
    return 0;
}

#endif

void http_resp_free(http_resp *r) { sbuf_free(&r->body); }


int http_retry_after(const char *value)
{
    char *end; long long seconds;
    if(!value) return 0;
    while(isspace((unsigned char)*value)) value++;
    if(isdigit((unsigned char)*value)) {
        seconds=strtoll(value,&end,10);
        while(isspace((unsigned char)*end)) end++;
        if(*end || seconds<0) return 0;
    } else {
#ifdef _WIN32
        wchar_t *w=utf8_to_wide(value); SYSTEMTIME st; FILETIME ft; ULARGE_INTEGER u;
        int ok=w && WinHttpTimeToSystemTime(w,&st) && SystemTimeToFileTime(&st,&ft);
        free(w); if(!ok) return 0;
        u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
        seconds=(long long)(u.QuadPart/10000000ULL)-11644473600LL-now_unix();
#else
        time_t t=curl_getdate(value,NULL); if(t==(time_t)-1) return 0;
        seconds=(long long)t-now_unix();
#endif
    }
    return seconds<=0 ? 0 : seconds>INT_MAX ? INT_MAX : (int)seconds;
}
