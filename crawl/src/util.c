#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/time.h>
#  include <sys/statvfs.h>
#  include <unistd.h>
#endif

void sbuf_init(sbuf *b) { b->buf = NULL; b->len = 0; b->cap = 0; }

void sbuf_free(sbuf *b) { free(b->buf); b->buf = NULL; b->len = b->cap = 0; }

void sbuf_reset(sbuf *b) { b->len = 0; if (b->buf) b->buf[0] = '\0'; }

static int sbuf_grow(sbuf *b, size_t need)
{
    size_t cap;
    char  *p;

    if (b->cap >= need) return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) return -1;
        cap *= 2;
    }
    p = realloc(b->buf, cap);
    if (!p) return -1;
    b->buf = p;
    b->cap = cap;
    return 0;
}

int sbuf_append(sbuf *b, const char *data, size_t n)
{
    if (sbuf_grow(b, b->len + n + 1) != 0) return -1;
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return 0;
}

int sbuf_appendz(sbuf *b, const char *s)
{
    return s ? sbuf_append(b, s, strlen(s)) : 0;
}

int sbuf_printf(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    int     n;
    char    tmp[1024];

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;

    if ((size_t)n < sizeof tmp) return sbuf_append(b, tmp, (size_t)n);

    /* Rare long case: allocate exactly. */
    {
        char *big = malloc((size_t)n + 1);
        int   rc;
        if (!big) return -1;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        rc = sbuf_append(b, big, (size_t)n);
        free(big);
        return rc;
    }
}

int sbuf_json_str(sbuf *b, const char *s)
{
    if (!s) return sbuf_appendz(b, "null");
    if (sbuf_appendz(b, "\"") != 0) return -1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  sbuf_appendz(b, "\\\""); break;
        case '\\': sbuf_appendz(b, "\\\\"); break;
        case '\n': sbuf_appendz(b, "\\n");  break;
        case '\r': sbuf_appendz(b, "\\r");  break;
        case '\t': sbuf_appendz(b, "\\t");  break;
        default:
            if (c < 0x20) sbuf_printf(b, "\\u%04x", c);
            else          sbuf_append(b, (const char *)&c, 1);
        }
    }
    return sbuf_appendz(b, "\"");
}

char *xstrdup(const char *s)
{
    size_t n;
    char  *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char  *p;
    size_t len;
    if (!s) return NULL;
    len = strnlen(s, n);
    p = malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

void xfree(void **p)
{
    if (p && *p) { free(*p); *p = NULL; }
}

void str_lower(char *s)
{
    if (!s) return;
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

char *str_trim(char *s)
{
    char *end;
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

int str_icontains(const char *hay, const char *needle)
{
    size_t nl;
    if (!hay || !needle) return 0;
    nl = strlen(needle);
    if (nl == 0) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

int str_ieq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

char *str_join(char **items, size_t n, const char *sep)
{
    sbuf   b;
    size_t i;
    char  *out;

    sbuf_init(&b);
    for (i = 0; i < n; i++) {
        if (!items[i]) continue;
        if (b.len) sbuf_appendz(&b, sep);
        sbuf_appendz(&b, items[i]);
    }
    out = b.buf ? b.buf : xstrdup("");
    return out; /* ownership transferred */
}

long long now_unix(void) { return (long long)time(NULL); }

long long now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

void sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* days_from_civil, Howard Hinnant's algorithm. Avoids timegm/_mkgmtime split. */
static long long days_from_civil(int y, int m, int d)
{
    long long yy  = y;
    long long era, yoe, doy, doe;
    yy -= m <= 2;
    era = (yy >= 0 ? yy : yy - 399) / 400;
    yoe = yy - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

long long parse_datetime(const char *s)
{
    int y, mo, d, h, mi, sec;
    if (!s) return 0;
    h = mi = sec = 0;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 6) {
        if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    return days_from_civil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL + sec;
}


long long wall_ms(void)
{
#ifdef _WIN32
    FILETIME ft; ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft); u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ULL - 11644473600000ULL);
#else
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}
int stop_requested(void)
{
    const char *path=getenv("MAGPIE_STOP_FILE"), *deadline=getenv("MAGPIE_DEADLINE");
    FILE *fp;
    if (deadline && *deadline && now_unix() >= atoll(deadline)) return 1;
    fp=fopen(path && *path ? path : "runner/STOP", "r");
    if (fp) { fclose(fp); return 1; }
    return 0;
}
int sleep_interruptible(int ms)
{
    while (ms>0) {
        int part=ms>200 ? 200 : ms;
        if (stop_requested()) return -1;
        sleep_ms(part); ms-=part;
    }
    return stop_requested() ? -1 : 0;
}
void env_set(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    setenv(name, value ? value : "", 1);
#endif
}
void unique_token(char out[80])
{
    static unsigned seq;
#ifdef _WIN32
    unsigned long pid=GetCurrentProcessId();
#else
    unsigned long pid=(unsigned long)getpid();
#endif
    snprintf(out,80,"%lld-%lu-%u",wall_ms(),pid,++seq);
}
int url_origin(const char *url, char *out, size_t n)
{
    const char *p, *e; size_t len;
    if (!url || !(strncmp(url,"http://",7)==0 || strncmp(url,"https://",8)==0)) return -1;
    p=strstr(url,"://")+3; e=p;
    while (*e && *e!='/' && *e!='?' && *e!='#') {
        if ((unsigned char)*e<=32 || *e=='@' || *e=='\\') return -1;
        e++;
    }
    len=(size_t)(e-url);
    if (e==p || len>=n) return -1;
    memcpy(out,url,len); out[len]=0; str_lower(out); return 0;
}
int url_host(const char *url, char *out, size_t n)
{
    char origin[4096]; const char *p,*e; size_t len;
    if (url_origin(url,origin,sizeof origin)) return -1;
    p=strstr(origin,"://")+3;
    if (*p=='[') { e=strchr(p,']'); if (!e) return -1; e++; }
    else { e=p; while (*e && *e!=':') e++; }
    len=(size_t)(e-p); if (!len || len>=n) return -1;
    memcpy(out,p,len); out[len]=0; return 0;
}
/* RFC 3986 relative references, including dot-segment removal. */
char *url_resolve(const char *base, const char *ref)
{
    char origin[4096]; sbuf raw, result; char *path,*query,*end; size_t prefix;
    if (url_origin(base,origin,sizeof origin) || !ref) return NULL;
    {
        const char *colon=strchr(ref,':'), *slash=strpbrk(ref,"/?#");
        if(colon && (!slash || colon<slash) &&
           strncmp(ref,"http://",7) && strncmp(ref,"https://",8)) return NULL;
    }
    sbuf_init(&raw); sbuf_init(&result);
    if (strstr(ref,"://")) sbuf_appendz(&raw,ref);
    else if (ref[0]=='/' && ref[1]=='/') sbuf_printf(&raw,"%.*s:%s",(int)(strchr(base,':')-base),base,ref);
    else {
        sbuf_appendz(&raw,origin);
        path=(char *)base+strlen(origin);
        end=strpbrk(path,"?#"); if (!end) end=path+strlen(path);
        if (*ref=='/') sbuf_appendz(&raw,ref);
        else if (*ref=='?' || !*ref || *ref=='#') {
            if (end==path) sbuf_appendz(&raw,"/"); else sbuf_append(&raw,path,(size_t)(end-path));
            if (*ref=='#' || !*ref) { if (*end=='?') { const char *h=strchr(end,'#'); sbuf_append(&raw,end,h?(size_t)(h-end):strlen(end)); } }
            sbuf_appendz(&raw,ref);
        } else {
            while (end>path && end[-1]!='/') end--;
            if (end==path) sbuf_appendz(&raw,"/"); else sbuf_append(&raw,path,(size_t)(end-path));
            sbuf_appendz(&raw,ref);
        }
    }
    if (!raw.buf || url_origin(raw.buf,origin,sizeof origin)) { sbuf_free(&raw); return NULL; }
    end=strchr(raw.buf,'#'); if(end) *end=0;
    sbuf_appendz(&result,origin); prefix=result.len;
    path=raw.buf+strlen(origin); query=strchr(path,'?'); if(query) *query++=0;
    if (!*path) sbuf_appendz(&result,"/");
    while (*path) {
        if (!strncmp(path,"/./",3)) { path+=2; continue; }
        if (!strcmp(path,"/.")) { sbuf_appendz(&result,"/"); break; }
        if (!strncmp(path,"/../",4) || !strcmp(path,"/..")) {
            size_t j=result.len;
            while (j>prefix && result.buf[j-1]!='/') j--;
            if (j>prefix) j--;
            result.len=j; result.buf[j]=0;
            if (!strcmp(path,"/..")) { sbuf_appendz(&result,"/"); break; }
            path+=3; continue;
        }
        end=strchr(path+(*path=='/'),'/'); if (!end) end=path+strlen(path);
        sbuf_append(&result,path,(size_t)(end-path)); path=end;
    }
    if(query) sbuf_printf(&result,"?%s",query);
    sbuf_free(&raw); return result.buf;
}
int shell_arg(sbuf *b, const char *arg)
{
#ifdef _WIN32
    /* Registry args are simple flags/values. Reject cmd.exe expansion syntax. */
    if (strpbrk(arg,"\"%!^&|<>\r\n")) return -1;
    return sbuf_printf(b,"\"%s\"",arg);
#else
    sbuf_appendz(b,"'");
    for (;*arg;arg++) {
        if (*arg=='\'') sbuf_appendz(b,"'\\''");
        else sbuf_append(b,arg,1);
    }
    return sbuf_appendz(b,"'");
#endif
}

/* Same-directory replacement: never remove the previous destination first. */
int atomic_replace(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t a[32768], b[32768];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,from,-1,a,32768) ||
       !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,to,-1,b,32768)) return -1;
    return MoveFileExW(a,b,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from,to);
#endif
}

/* Query the containing directory, including when the database is a relative path. */
long long disk_free_bytes(const char *file_path)
{
    char *dir=xstrdup(file_path), *last=NULL, *p;
    long long result=-1;
    if(!dir) return -1;
    for(p=dir;*p;p++) if(*p=='/' || *p=='\\') last=p;
    if(last) last[1]=0;
    else { free(dir); dir=xstrdup("."); if(!dir) return -1; }
#ifdef _WIN32
    { ULARGE_INTEGER available;
      if(GetDiskFreeSpaceExA(dir,&available,NULL,NULL)) result=(long long)available.QuadPart; }
#else
    { struct statvfs space;
      if(!statvfs(dir,&space)) result=(long long)space.f_bavail*space.f_frsize; }
#endif
    free(dir); return result;
}
