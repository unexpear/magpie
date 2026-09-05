#ifndef ROBOTS_H
#define ROBOTS_H
/* Evaluate a fetched robots file for Magpie. Returns 1 allow, 0 disallow,
 * -1 allocation/parse error. delay_ms is increased by matching Crawl-delay. */
int robots_allowed(const char *text, const char *url, int *delay_ms);
#endif
