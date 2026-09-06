#include "robots.h"
#include "util.h"
#include "http.h"
#include "store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void policy(const char *text, const char *path, int expected, int delay)
{
    char url[512]; int ms=0;
    snprintf(url,sizeof url,"https://example.test%s",path);
    assert(robots_allowed(text,url,&ms)==expected);
    assert(ms==delay);
}
int main(int argc, char **argv)
{
    const char *base="http://a/b/c/d;p?q";
    const char *refs[][2]={
        {"g","http://a/b/c/g"},{"./g","http://a/b/c/g"},
        {"../g","http://a/b/g"},{"../../g","http://a/g"},
        {"../../../g","http://a/g"},{"/g","http://a/g"},
        {"//g/x","http://g/x"},{"?y","http://a/b/c/d;p?y"},
        {"#s","http://a/b/c/d;p?q"},{"","http://a/b/c/d;p?q"},
        {"g/./h","http://a/b/c/g/h"},{"g/../h","http://a/b/c/h"},
        {"g?y/../x","http://a/b/c/g?y/../x"},
        {"/a//b","http://a/a//b"}, {".","http://a/b/c/"}, {"..","http://a/b/"}
    };
    size_t i; store *s; int wait;
    for(i=0;i<sizeof refs/sizeof refs[0];i++) {
        char *got=url_resolve(base,refs[i][0]);
        if(!got || strcmp(got,refs[i][1])) fprintf(stderr,"resolve %s: %s\n",refs[i][0],got?got:"NULL");
        assert(got && !strcmp(got,refs[i][1])); free(got);
    }
    policy("User-agent: *\nDisallow: /private\nAllow: /private/open\n", "/private",0,0);
    policy("User-agent: *\nDisallow: /private\nAllow: /private/open\n", "/private/open",1,0);
    policy("User-agent: *\nDisallow: /\nUser-agent: Magpie\nAllow: /\nCrawl-delay: 12.5\n", "/x",1,12500);
    policy("User-agent: Other\nCrawl-delay: 99\nUser-agent: *\nCrawl-delay: 2\n", "/x",1,2000);
    policy("User-agent: Magpie\nDisallow: /a\nUser-agent: magpie\nDisallow: /b\n", "/b",0,0);
    policy("User-agent: *\nDisallow: /*.zip$\n", "/x.zip?download=1",1,0);
    policy("User-agent: *\nDisallow: /*.zip$\n", "/x.zip",0,0);
    policy("User-agent: *\nDisallow: /x\nAllow: /x\n", "/x",1,0);
    policy("User-agent: *\nDisallow: /foo/bar\n", "/foo%2Fbar",1,0);
    policy("User-agent: *\nDisallow: /foo/bar\n", "/foo/%62ar",0,0);
    policy("User-agent: *\nDisallow: /caf%C3%A9\n", "/caf\xc3\xa9",0,0);
    policy("User-agent: *\nDisallow:\n", "/",1,0);
    assert(http_retry_after("3600")==3600);
    assert(http_retry_after("Wed, 01 Jan 2098 00:00:00 GMT")>3600);
    assert(http_retry_after("invalid")==0);
    assert(licence_parse("CC-BY-NC 4.0")==LIC_UNKNOWN);
    assert(licence_parse("CC-BY-SA 4.0")==LIC_CC_BY_SA);
    assert(url_resolve(base,"file:///tmp/file")==NULL);
    assert(parse_datetime("2026-08-08T20:00:00")-parse_datetime("2026-08-08 00:00:00")==72000);
    assert(disk_free_bytes("missing-dir-does-not-exist/db.sqlite")==-1);
    assert(disk_free_bytes("db.sqlite")>0);
    assert(argc==2); s=store_open(argv[1]); assert(s);
    assert(!store_request_reserve(s,"run","a",0,1,1,500,3000,&wait));
    assert(store_request_reserve(s,"run","b",0,1,1,500,3000,&wait)==1);
    assert(!store_begin(s));
    assert(store_request_reserve(s,"other","c",0,500,500,500,3000,&wait)==-1);
    store_close(s); s=store_open(argv[1]); assert(s);
    assert(store_budget_used_today(s)==1); assert(store_run_used(s,"run")==1);
    {
        size_t size=20*1024*1024;
        char *body=malloc(size); sbuf cached; assert(body); memset(body,'x',size);
        assert(!store_cache_put(s,"https://a/first",NULL,NULL,body,size));
        assert(!store_cache_put(s,"https://a/second",NULL,NULL,body,size));
        sbuf_init(&cached);
        assert(store_cache_get(s,"https://a/first",NULL,0,NULL,0,&cached)!=0);
        assert(!store_cache_get(s,"https://a/second",NULL,0,NULL,0,&cached));
        assert(cached.len==size);sbuf_free(&cached);
        assert(store_cache_put(s,"https://a/large",NULL,NULL,body,33554433)!=0);
        free(body);
    }
    store_close(s); puts("core policy, URL, date and durable accounting checks passed"); return 0;
}
