#include "fetch.h"
#include "http.h"
#include "robots.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

static int env_int(const char *key, int fallback)
{
    const char *v=getenv(key); char *end; long n;
    if(!v || !*v) return fallback;
    errno=0; n=strtol(v,&end,10);
    return errno || *end || n<0 || n>INT_MAX ? fallback : (int)n;
}
void fetch_init(fetcher *f, limiter *lim, store *st)
{
    const char *run=getenv("MAGPIE_RUN_ID");
    memset(f,0,sizeof *f); f->lim=lim; f->st=st; f->max_retries=3;
    f->interval_ms=2000; f->daily_cap=500; f->host_cap=500;
    f->weekly_cap=env_int("MAGPIE_WEEKLY_CAP",3000);
    if(run && *run) snprintf(f->run_id,sizeof f->run_id,"%s",run);
    else unique_token(f->run_id);
}
void fetch_export_env(fetcher *f)
{
    char b[40];
    env_set("MAGPIE_RUN_ID",f->run_id); env_set("MAGPIE_DB",store_path(f->st));
    snprintf(b,sizeof b,"%d",f->max_requests); env_set("MAGPIE_MAX_REQUESTS",b);
    snprintf(b,sizeof b,"%d",f->daily_cap); env_set("MAGPIE_DAILY_CAP",b);
    snprintf(b,sizeof b,"%d",f->offline); env_set("MAGPIE_OFFLINE",b);
}
long long fetch_arm_daily(fetcher *f, int daily_cap)
{
    f->daily_cap=daily_cap; store_budget_today_key(f->day);
    f->day_used=store_budget_used(f->st,f->day);
    if(f->day_used<0) return 0;
    return daily_cap<=0 ? -1 : f->day_used>=daily_cap ? 0 : daily_cap-f->day_used;
}
static int reserve(fetcher *f, const char *host, int interval)
{
    for(;;) {
        int wait=0,rc;
        if(stop_requested()) { fprintf(stderr,"  stopped before request\n"); return -1; }
        rc=store_request_reserve(f->st,f->run_id,host,interval,f->daily_cap,
                                  f->max_requests,f->host_cap,f->weekly_cap,&wait);
        if(rc==2) { if(sleep_interruptible(wait)) return -1; continue; }
        if(rc) {
            f->budget_hit=1;
            fprintf(stderr,"  request refused: budget, host cooldown, or ledger unavailable (%s)\n",host);
            return -1;
        }
        f->n_requests++; return 0;
    }
}
static int request(fetcher *f, const char *url, int head, int policy,
                   int interval, http_resp *out);
static int policy_check(fetcher *f, const char *url, int *interval)
{
    char origin[4096], robots[4120]; sbuf text; long long stamp; int rc;
    if(url_origin(url,origin,sizeof origin)) return -1;
    snprintf(robots,sizeof robots,"%s/robots.txt",origin);
    sbuf_init(&text); stamp=store_cache_time(f->st,robots);
    if(stamp<0) return -1;
    if(stamp>0 && now_unix()-stamp<86400 &&
       store_cache_get(f->st,robots,NULL,0,NULL,0,&text)==0) {
        rc=robots_allowed(text.buf,url,interval); sbuf_free(&text); return rc==1 ? 0 : -1;
    }
    {
        http_resp r;
        rc=request(f,robots,0,0,*interval,&r);
        if(rc) { http_resp_free(&r); return -1; }
        if(r.status==200) {
            if(r.body.len>2*1024*1024) { http_resp_free(&r); return -1; }
            sbuf_append(&text,r.body.buf ? r.body.buf : "",r.body.len);
        } else if(r.status>=400 && r.status<500 && r.status!=429) {
            /* RFC 9309 unavailable: allow, with a bounded cache lifetime. */
            if(store_cache_put(f->st,robots,NULL,NULL,"",0)) { http_resp_free(&r); return -1; }
        } else { http_resp_free(&r); return -1; }
        http_resp_free(&r);
    }
    rc=robots_allowed(text.buf,url,interval); sbuf_free(&text);
    if(rc!=1) fprintf(stderr,"  robots policy disallows %s\n",url);
    return rc==1 ? 0 : -1;
}
static int request(fetcher *f, const char *url, int head, int policy,
                   int interval, http_resp *out)
{
    char *current=url_resolve(url,""); int redirects=0, attempt=0, rc=-1;
    memset(out,0,sizeof *out); sbuf_init(&out->body);
    if(!current) return -1;
    while(redirects<=5) {
        char host[256],etag[256]="",lastmod[128]=""; http_resp r; int gap=interval, not_modified=0;
        if(stop_requested() || url_host(current,host,sizeof host)) break;
        if(str_ieq(host,"opengameart.org") && gap<10000) gap=10000;
        if(policy && policy_check(f,current,&gap)) break;
        if(!head) store_cache_get(f->st,current,etag,sizeof etag,lastmod,sizeof lastmod,NULL);
        if(reserve(f,host,gap)) break;
        rc=head ? http_head(current,&r) : http_get(current,etag,lastmod,&r);
        if(rc) {
            fprintf(stderr,"  transport error: %s\n",r.error); http_resp_free(&r);
            if(attempt++>=f->max_retries || sleep_interruptible(1000*attempt)) { rc=-1; break; }
            continue;
        }
        f->last_status=(int)r.status;
        if(r.status==429 || r.status==503) {
            int delay=r.retry_after>0 ? r.retry_after : 300;
            if(store_host_pause(f->st,host,delay)) fprintf(stderr,"  cannot persist host cooldown\n");
            fprintf(stderr,"  HTTP %ld: deferring %s for at least %d seconds\n",r.status,host,delay);
            *out=r; rc=-1; break;
        }
        if(r.status==301 || r.status==302 || r.status==303 || r.status==307 || r.status==308) {
            char *next=url_resolve(current,r.location);
            if(r.retry_after>0) {
                store_host_pause(f->st,host,r.retry_after); free(next); http_resp_free(&r); rc=-1; break;
            }
            if(!r.location[0] || !next) { free(next); http_resp_free(&r); rc=-1; break; }
            free(current); current=next; http_resp_free(&r); redirects++; attempt=0; rc=-1; continue;
        }
        if(r.status>=500) {
            http_resp_free(&r);
            if(attempt++>=f->max_retries || sleep_interruptible(1000*attempt)) { rc=-1; break; }
            continue;
        }
        if(!head && r.status==304) {
            if(store_cache_get(f->st,current,NULL,0,NULL,0,&r.body)) { http_resp_free(&r); rc=-1; break; }
            r.status=200; not_modified=1; f->n_cached++;
        }
        if(!head && r.status==200 &&
           store_cache_put(f->st,current,r.etag[0]?r.etag:(not_modified?etag:NULL),
                            r.last_modified[0]?r.last_modified:(not_modified?lastmod:NULL),
                            r.body.buf?r.body.buf:"",r.body.len)) {
            http_resp_free(&r); rc=-1; break;
        }
        /* Also cache the original URL after a validated redirect. */
        if(!head && r.status==200 && strcmp(current,url) &&
           store_cache_put(f->st,url,NULL,NULL,r.body.buf?r.body.buf:"",r.body.len)) {
            http_resp_free(&r); rc=-1; break;
        }
        *out=r; rc=0; break;
    }
    free(current); return rc;
}
int fetch_url(fetcher *f, const char *url, sbuf *body)
{
    http_resp r; int rc;
    f->last_status=0;
    if(f->offline) {
        if(store_cache_get(f->st,url,NULL,0,NULL,0,body)) { fprintf(stderr,"  offline cache miss: %s\n",url); return -1; }
        f->last_status=200; f->n_cached++; return 0;
    }
    rc=request(f,url,0,1,f->interval_ms,&r);
    if(!rc && r.status==200) rc=sbuf_append(body,r.body.buf?r.body.buf:"",r.body.len);
    else rc=-1;
    f->last_status=(int)r.status; http_resp_free(&r); return rc;
}
int fetch_status(fetcher *f, const char *url)
{
    http_resp r; int rc,status;
    if(f->offline) return 0;
    rc=request(f,url,1,1,f->interval_ms,&r); status=rc ? 0 : (int)r.status;
    http_resp_free(&r); return status;
}
