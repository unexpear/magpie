#include "robots.h"
#include "util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int c)
{
    if(c>='0' && c<='9') return c-'0';
    c=tolower(c); return c>='a' && c<='f' ? c-'a'+10 : -1;
}
static int unreserved(int c)
{ return (c<128 && isalnum(c)) || c=='-' || c=='.' || c=='_' || c=='~'; }
/* RFC 9309: decode escaped unreserved ASCII, preserve reserved escapes and
 * percent-encode non-ASCII octets before matching. */
static char *normalize(const char *p)
{
    sbuf b; sbuf_init(&b);
    for(;*p;p++) {
        unsigned char c=(unsigned char)*p;
        if(c=='%' && p[1] && p[2] && hexval(p[1])>=0 && hexval(p[2])>=0) {
            c=(unsigned char)(hexval(p[1])*16+hexval(p[2])); p+=2;
            if(unreserved(c)) sbuf_append(&b,(char *)&c,1);
            else sbuf_printf(&b,"%%%02X",c);
        } else if(c>=128) sbuf_printf(&b,"%%%02X",c);
        else sbuf_append(&b,(char *)&c,1);
    }
    return b.buf ? b.buf : xstrdup("");
}
/* Greedy wildcard matching, with prefix match unless the rule ends in $. */
static int matches(const char *rule, const char *path)
{
    const char *star=NULL, *retry=NULL;
    for(;;) {
        if(!*rule) return 1;
        if(*rule=='$' && !rule[1]) { if(!*path) return 1; }
        else if(*rule=='*') { star=++rule; retry=path; continue; }
        else if(*path && *rule==*path) { rule++; path++; continue; }
        if(star && *retry) { rule=star; path=++retry; continue; }
        return 0;
    }
}
int robots_allowed(const char *text, const char *url, int *delay_ms)
{
    char origin[4096], *target, *copy, *line, *next;
    int pass, specific=0, best=-1, allow=1;
    const char *path;
    if(url_origin(url,origin,sizeof origin)) return -1;
    path=url+strlen(origin);
    if(!*path) path="/";
    target=normalize(path); if(!target) return -1;
    for(pass=0;pass<2;pass++) {
        int group_match=0, saw_rule=0;
        copy=xstrdup(text ? text : ""); if(!copy) { free(target); return -1; }
        line=copy;
        if(!strncmp(line,"\xef\xbb\xbf",3)) line+=3;
        for(;line;line=next) {
            char *key,*value,*colon,*comment;
            next=strchr(line,'\n'); if(next) *next++=0;
            comment=strchr(line,'#'); if(comment) *comment=0;
            key=str_trim(line); colon=strchr(key,':'); if(!colon) continue;
            *colon=0; key=str_trim(key); value=str_trim(colon+1);
            if(str_ieq(key,"user-agent")) {
                if(saw_rule) { group_match=0; saw_rule=0; }
                if(str_ieq(value,"Magpie")) { specific=1; group_match=1; }
                if(pass && !specific && !strcmp(value,"*")) group_match=1;
                continue;
            }
            if(!str_ieq(key,"allow") && !str_ieq(key,"disallow") && !str_ieq(key,"crawl-delay")) continue;
            saw_rule=1;
            if(!pass || !group_match) continue;
            if(str_ieq(key,"crawl-delay")) {
                char *end; double d=strtod(value,&end);
                if(end!=value && !*end && d>=0 && d<=2147483 && d*1000>*delay_ms)
                    *delay_ms=(int)(d*1000+0.999);
            } else if(*value=='/') {
                char *rule=normalize(value); int length=0; const char *p;
                if(!rule) { free(copy); free(target); return -1; }
                for(p=rule;*p;p++) {
                    if(*p=='*' || (*p=='$' && !p[1])) continue;
                    if(*p=='%' && p[1] && p[2]) p+=2;
                    length++;
                }
                if(matches(rule,target) && (length>best || (length==best && str_ieq(key,"allow")))) {
                    best=length; allow=str_ieq(key,"allow");
                }
                free(rule);
            }
        }
        free(copy);
    }
    free(target); return allow;
}
