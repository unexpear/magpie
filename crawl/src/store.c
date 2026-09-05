#include "store.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct store {
    sqlite3      *db;
    char *path;
    int enrich_tags_only;
    sqlite3_stmt *ins;
    sqlite3_stmt *cache_get;
    sqlite3_stmt *cache_put;
    sqlite3_stmt *set_status;
    sqlite3_stmt *enrich;
};

static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=FULL;"
    "CREATE TABLE IF NOT EXISTS source_health(source TEXT PRIMARY KEY,last_attempt INTEGER DEFAULT 0,last_success INTEGER DEFAULT 0,state INTEGER DEFAULT 0,emitted INTEGER DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS assets("
    "  id TEXT PRIMARY KEY,"
    "  source TEXT NOT NULL,"
    "  title TEXT NOT NULL,"
    "  author TEXT,"
    "  source_url TEXT NOT NULL,"
    "  thumb_url TEXT,"
    "  asset_type TEXT,"
    "  licence TEXT,"
    "  licence_url TEXT,"
    "  commercial_ok INTEGER,"
    "  attribution INTEGER,"
    "  formats TEXT,"
    "  tags TEXT,"
    "  style TEXT,"
    "  rigged INTEGER,"
    "  tileable INTEGER,"
    "  polycount INTEGER,"
    "  price REAL,"
    "  updated_at INTEGER,"
    "  last_seen INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_source ON assets(source);"
    "CREATE INDEX IF NOT EXISTS idx_comm   ON assets(commercial_ok);"
    "CREATE INDEX IF NOT EXISTS idx_type   ON assets(asset_type);"
    /* _v2 because fts5 has no ALTER: the column set changed when derived tags
     * were split out, and a versioned name makes the upgrade a plain drop of
     * the old table rather than a schema sniff. */
    "CREATE VIRTUAL TABLE IF NOT EXISTS assets_fts_v2 USING fts5("
    "  id UNINDEXED, title, tags, author, tags_auto, tokenize='unicode61'"
    ");"
    "CREATE TABLE IF NOT EXISTS http_cache("
    "  url TEXT PRIMARY KEY,"
    "  etag TEXT,"
    "  last_modified TEXT,"
    "  body BLOB,"
    "  fetched_at INTEGER"
    ");";

/* Columns added after the first release. ALTER TABLE ADD COLUMN is the whole
 * migration story here - an existing index gains them without a re-crawl, and
 * "duplicate column" just means we already ran. */
static const char *MIGRATIONS[] = {
    "ALTER TABLE assets ADD COLUMN popularity INTEGER",
    "ALTER TABLE assets ADD COLUMN rank_hint INTEGER",
    "ALTER TABLE assets ADD COLUMN pop_pct INTEGER",
    "ALTER TABLE assets ADD COLUMN type_conf INTEGER",
    "ALTER TABLE assets ADD COLUMN type_ev TEXT",
    "ALTER TABLE assets ADD COLUMN style_conf INTEGER",
    "ALTER TABLE assets ADD COLUMN style_ev TEXT",
    "ALTER TABLE assets ADD COLUMN http_status INTEGER",
    "ALTER TABLE assets ADD COLUMN last_checked INTEGER",
    "ALTER TABLE assets ADD COLUMN enriched INTEGER DEFAULT 0",
    "ALTER TABLE assets ADD COLUMN tags_auto TEXT",
    "DROP TABLE IF EXISTS assets_fts",          /* superseded by _v2 */
    "CREATE TABLE IF NOT EXISTS request_budget("
    "  day TEXT PRIMARY KEY, used INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS idx_checked ON assets(last_checked)",
    "CREATE INDEX IF NOT EXISTS idx_enriched ON assets(enriched)",
    "ALTER TABLE assets ADD COLUMN enrich_retry_at INTEGER DEFAULT 0",
    "CREATE TABLE IF NOT EXISTS request_runs(id TEXT PRIMARY KEY, used INTEGER NOT NULL DEFAULT 0)",
    "CREATE TABLE IF NOT EXISTS request_run_hosts(id TEXT PRIMARY KEY, used INTEGER NOT NULL DEFAULT 0)",
    "CREATE TABLE IF NOT EXISTS request_hosts(host TEXT PRIMARY KEY, last_ms INTEGER NOT NULL DEFAULT 0, blocked_until INTEGER NOT NULL DEFAULT 0)",
    "UPDATE assets SET enriched=0 WHERE source='opengameart' AND enriched=1 AND attribution=1 AND COALESCE(author,'')=''",

};

static const char *SQL_INS =
    "INSERT INTO assets(id,source,title,author,source_url,thumb_url,asset_type,"
    "  licence,licence_url,commercial_ok,attribution,formats,tags,style,rigged,"
    "  tileable,polycount,price,updated_at,last_seen,popularity,rank_hint,"
    "  type_conf,type_ev,style_conf,style_ev,tags_auto)"
    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,"
    "        ?19,?20,?21,?22,?23,?24,?25,?26,?27)"
    " ON CONFLICT(id) DO UPDATE SET"
    "  source=excluded.source, title=excluded.title,"
    /* Enrichment fills author/tags that a listing could not provide; a later
     * plain crawl must not wipe them back out. */
    "  author=COALESCE(excluded.author, assets.author),"
    "  source_url=excluded.source_url,"
    /* Same reasoning as author: enrichment may have found a preview the
     * listing does not carry, and a later crawl must not blank it. */
    "  thumb_url=COALESCE(excluded.thumb_url, assets.thumb_url),"
    "  asset_type=excluded.asset_type, licence=excluded.licence,"
    "  licence_url=excluded.licence_url, commercial_ok=excluded.commercial_ok,"
    "  attribution=excluded.attribution, formats=excluded.formats,"
    "  tags=CASE WHEN assets.enriched=1 AND excluded.source IN ('kenney','opengameart')"
    "       AND COALESCE(excluded.tags,'')='' THEN assets.tags ELSE excluded.tags END,"
    "  style=excluded.style, rigged=excluded.rigged,"
    "  tileable=excluded.tileable, polycount=excluded.polycount,"
    "  price=excluded.price, updated_at=excluded.updated_at,"
    "  last_seen=excluded.last_seen,"
    "  popularity=COALESCE(excluded.popularity, assets.popularity),"
    "  rank_hint=COALESCE(excluded.rank_hint, assets.rank_hint),"
    "  type_conf=excluded.type_conf, type_ev=excluded.type_ev,"
    "  style_conf=excluded.style_conf, style_ev=excluded.style_ev,"
    "  tags_auto=excluded.tags_auto;";

store *store_open(const char *path)
{
    store *s = calloc(1, sizeof *s);
    char  *err = NULL;
    size_t i;

    if (!s) return NULL;
    if (sqlite3_open(path, &s->db) != SQLITE_OK) {
        fprintf(stderr, "sqlite: %s\n", sqlite3_errmsg(s->db));
        sqlite3_close(s->db); free(s);
        return NULL;
    }
    s->path = xstrdup(path);
    if(!s->path) { store_close(s); return NULL; }
    sqlite3_busy_timeout(s->db, 5000);
    if (sqlite3_exec(s->db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "schema: %s\n", err ? err : "?");
        sqlite3_free(err);
        store_close(s);
        return NULL;
    }
    {
        sqlite3_stmt *version=NULL;
        int current=0;
        if (sqlite3_prepare_v2(s->db,"PRAGMA user_version",-1,&version,NULL)!=SQLITE_OK) { store_close(s); return NULL; }
        if (sqlite3_step(version)==SQLITE_ROW) current=sqlite3_column_int(version,0);
        sqlite3_finalize(version);
        if (current < 1) {
            if (sqlite3_exec(s->db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK) { store_close(s); return NULL; }
            for (i=0;i<sizeof MIGRATIONS/sizeof MIGRATIONS[0];i++) {
                err=NULL;
                if (sqlite3_exec(s->db,MIGRATIONS[i],NULL,NULL,&err)!=SQLITE_OK &&
                    (!err || !strstr(err,"duplicate column name"))) {
                    fprintf(stderr,"migration: %s\n",err ? err : "failed");
                    sqlite3_free(err); sqlite3_exec(s->db,"ROLLBACK",NULL,NULL,NULL); store_close(s); return NULL;
                }
                sqlite3_free(err);
            }
            if (sqlite3_exec(s->db,"PRAGMA user_version=1; COMMIT",NULL,NULL,NULL)!=SQLITE_OK) { store_close(s); return NULL; }
        }
    }

    if (sqlite3_prepare_v2(s->db, SQL_INS, -1, &s->ins, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare insert: %s\n", sqlite3_errmsg(s->db));
        store_close(s);
        return NULL;
    }
    sqlite3_prepare_v2(s->db,
        "SELECT etag,last_modified,body FROM http_cache WHERE url=?1",
        -1, &s->cache_get, NULL);
    sqlite3_prepare_v2(s->db,
        "INSERT INTO http_cache(url,etag,last_modified,body,fetched_at)"
        " VALUES(?1,?2,?3,?4,?5)"
        " ON CONFLICT(url) DO UPDATE SET etag=excluded.etag,"
        " last_modified=excluded.last_modified, body=excluded.body,"
        " fetched_at=excluded.fetched_at",
        -1, &s->cache_put, NULL);
    sqlite3_prepare_v2(s->db,
        "UPDATE assets SET http_status=?2, last_checked=?3 WHERE id=?1",
        -1, &s->set_status, NULL);
    /* Enrichment can correct fields. Missing patch fields preserve prior data. */
    sqlite3_prepare_v2(s->db,
        "UPDATE assets SET"
        "  author    = COALESCE(NULLIF(?2,''), author),"
        "  tags      = COALESCE(NULLIF(?3,''), tags),"
        "  thumb_url = COALESCE(NULLIF(?4,''), thumb_url),"
        "  enriched  = 1, enrich_retry_at=0"
        " WHERE id=?1",
        -1, &s->enrich, NULL);
    if(!s->cache_get || !s->cache_put || !s->set_status || !s->enrich) { store_close(s); return NULL; }
    return s;
}

const char *store_path(store *s) { return s->path; }

void store_close(store *s)
{
    if (!s) return;
    sqlite3_finalize(s->ins);
    sqlite3_finalize(s->cache_get);
    sqlite3_finalize(s->cache_put);
    sqlite3_finalize(s->set_status);
    sqlite3_finalize(s->enrich);
    sqlite3_close(s->db);
    free(s->path);
    free(s);
}

static int exec1(store *s, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(s->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql: %s\n", err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int store_begin(store *s)  { return exec1(s, "BEGIN"); }
int store_commit(store *s) { return exec1(s, "COMMIT"); }

static void bind_txt(sqlite3_stmt *st, int i, const char *v)
{
    if (v) sqlite3_bind_text(st, i, v, -1, SQLITE_STATIC);
    else   sqlite3_bind_null(st, i);
}

int store_upsert(store *s, const asset *a, const classification *c)
{
    sqlite3_stmt *st = s->ins;
    int           rc;

    sqlite3_reset(st);
    sqlite3_clear_bindings(st);

    bind_txt(st, 1,  a->id);
    bind_txt(st, 2,  a->source);
    bind_txt(st, 3,  a->title);
    bind_txt(st, 4,  a->author);
    bind_txt(st, 5,  a->source_url);
    bind_txt(st, 6,  a->thumb_url);
    bind_txt(st, 7,  asset_type_str(a->type));
    bind_txt(st, 8,  licence_str(a->licence));
    bind_txt(st, 9,  a->licence_url);
    /* Derived here, never taken from the adapter. */
    sqlite3_bind_int(st, 10, licence_commercial_ok(a->licence));
    sqlite3_bind_int(st, 11, licence_needs_attribution(a->licence));
    bind_txt(st, 12, a->formats);
    bind_txt(st, 13, a->tags);
    bind_txt(st, 14, style_str(a->style));

    if (a->rigged   == TRI_UNKNOWN) sqlite3_bind_null(st, 15);
    else sqlite3_bind_int(st, 15, a->rigged);
    if (a->tileable == TRI_UNKNOWN) sqlite3_bind_null(st, 16);
    else sqlite3_bind_int(st, 16, a->tileable);
    if (a->polycount < 0) sqlite3_bind_null(st, 17);
    else sqlite3_bind_int64(st, 17, a->polycount);
    if (a->price < 0) sqlite3_bind_null(st, 18);
    else sqlite3_bind_double(st, 18, a->price);

    sqlite3_bind_int64(st, 19, a->updated_at);
    sqlite3_bind_int64(st, 20, now_unix());

    if (a->popularity < 0) sqlite3_bind_null(st, 21);
    else sqlite3_bind_int64(st, 21, a->popularity);
    if (a->rank_hint < 0) sqlite3_bind_null(st, 22);
    else sqlite3_bind_int64(st, 22, a->rank_hint);

    sqlite3_bind_int(st, 23, c ? c->type_conf : 0);
    bind_txt(st, 24, c ? evidence_str(c->type_from) : NULL);
    sqlite3_bind_int(st, 25, c ? c->style_conf : 0);
    bind_txt(st, 26, c ? evidence_str(c->style_from) : NULL);
    bind_txt(st, 27, a->tags_auto);

    rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "upsert %s: %s\n", a->id, sqlite3_errmsg(s->db));
        return -1;
    }
    return 0;
}

int store_rebuild_fts(store *s)
{
    if (exec1(s,"BEGIN IMMEDIATE")) return -1;
    if (exec1(s, "DELETE FROM assets_fts_v2") != 0) { exec1(s,"ROLLBACK"); return -1; }
    if (exec1(s,
        "INSERT INTO assets_fts_v2(id,title,tags,author,tags_auto)"
        " SELECT id,title,COALESCE(tags,''),COALESCE(author,''),"
        "        COALESCE(tags_auto,'') FROM assets")) { exec1(s,"ROLLBACK"); return -1; }
    return exec1(s,"COMMIT");
}

int store_recompute_popularity(store *s)
{
    /* Raw counts are not comparable across sources - Poly Haven reports
     * downloads in the tens of thousands, OpenGameArt gives us nothing but a
     * position in a favourites-sorted list. NTILE per source turns both into
     * the same 1-100 scale. */
    return exec1(s,
        "WITH ranked AS ("
        "  SELECT id, NTILE(100) OVER ("
        "     PARTITION BY source"
        "     ORDER BY COALESCE(popularity, -rank_hint)"
        "  ) AS pct"
        "  FROM assets"
        "  WHERE popularity IS NOT NULL OR rank_hint IS NOT NULL"
        ")"
        " UPDATE assets SET pop_pct = (SELECT pct FROM ranked WHERE ranked.id = assets.id)"
        " WHERE id IN (SELECT id FROM ranked)");
}

int store_reclassify(store *s)
{
    sqlite3_stmt *sel = NULL, *upd = NULL;
    int           n = 0;

    /* Read every row into memory first. 10k rows is a couple of MB, and
     * writing through a statement we are still stepping is asking for
     * trouble. */
    struct row {
        char      *id, *title, *tags, *formats, *source;
        long long  poly;
        asset_type dtype;   /* AT_UNKNOWN unless the source declared it  */
        style_id   dstyle;
    };
    struct row *rows = NULL;
    size_t      cap = 0, len = 0, i;

    if (sqlite3_prepare_v2(s->db,
            "SELECT id,title,COALESCE(tags,''),COALESCE(formats,''),source,"
            " COALESCE(polycount,-1),asset_type,style,type_ev,style_ev"
            " FROM assets", -1, &sel, NULL) != SQLITE_OK)
        return -1;

    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *tev, *sev;

        if (len == cap) {
            size_t nc = cap ? cap * 2 : 1024;
            struct row *nr = realloc(rows, nc * sizeof *nr);
            if (!nr) break;
            rows = nr; cap = nc;
        }
        rows[len].id      = xstrdup((const char *)sqlite3_column_text(sel, 0));
        rows[len].title   = xstrdup((const char *)sqlite3_column_text(sel, 1));
        rows[len].tags    = xstrdup((const char *)sqlite3_column_text(sel, 2));
        rows[len].formats = xstrdup((const char *)sqlite3_column_text(sel, 3));
        rows[len].source  = xstrdup((const char *)sqlite3_column_text(sel, 4));
        rows[len].poly    = sqlite3_column_int64(sel, 5);

        /* Provenance decides what survives a reclassify. A value the source
         * stated is data and must be preserved; a value we inferred is ours
         * to re-derive. A NULL evidence column means the row predates this
         * bookkeeping, and the conservative reading is that the adapter set
         * it - re-deriving would silently destroy good data. */
        tev = (const char *)sqlite3_column_text(sel, 8);
        sev = (const char *)sqlite3_column_text(sel, 9);

        rows[len].dtype = (!tev || str_ieq(tev, "declared"))
            ? asset_type_parse((const char *)sqlite3_column_text(sel, 6))
            : AT_UNKNOWN;
        rows[len].dstyle = (!sev || str_ieq(sev, "declared"))
            ? style_parse((const char *)sqlite3_column_text(sel, 7))
            : ST_UNKNOWN;

        /* An earlier "unclassifiable" verdict is not a declaration. */
        if (rows[len].dtype  == AT_UNCLASSIFIABLE) rows[len].dtype  = AT_UNKNOWN;
        if (rows[len].dstyle == ST_UNCLASSIFIABLE) rows[len].dstyle = ST_UNKNOWN;
        len++;
    }
    sqlite3_finalize(sel);

    /* Note ?4 writes tags_auto, not tags: reclassify must never touch what the
     * source published. */
    if (sqlite3_prepare_v2(s->db,
            "UPDATE assets SET asset_type=?2, style=?3, tags_auto=?4,"
            " type_conf=?5, type_ev=?6, style_conf=?7, style_ev=?8 WHERE id=?1",
            -1, &upd, NULL) != SQLITE_OK) {
        for (i = 0; i < len; i++) {
            free(rows[i].id); free(rows[i].title); free(rows[i].tags);
            free(rows[i].formats); free(rows[i].source);
        }
        free(rows);
        return -1;
    }

    exec1(s, "BEGIN");
    for (i = 0; i < len; i++) {
        asset          a;
        classification c;

        asset_init(&a);
        a.title     = rows[i].title;
        a.tags      = xstrdup(rows[i].tags);
        a.formats   = rows[i].formats;
        a.source    = rows[i].source;
        a.polycount = rows[i].poly;
        a.type      = rows[i].dtype;    /* declared values survive */
        a.style     = rows[i].dstyle;

        classify(&a, &c);
        classify_apply(&a, &c);   /* writes a.tags_auto, leaves a.tags alone */

        sqlite3_reset(upd);
        sqlite3_clear_bindings(upd);
        sqlite3_bind_text(upd, 1, rows[i].id, -1, SQLITE_STATIC);
        sqlite3_bind_text(upd, 2, asset_type_str(c.type), -1, SQLITE_STATIC);
        sqlite3_bind_text(upd, 3, style_str(c.style), -1, SQLITE_STATIC);
        sqlite3_bind_text(upd, 4, a.tags_auto ? a.tags_auto : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int (upd, 5, c.type_conf);
        sqlite3_bind_text(upd, 6, evidence_str(c.type_from), -1, SQLITE_STATIC);
        sqlite3_bind_int (upd, 7, c.style_conf);
        sqlite3_bind_text(upd, 8, evidence_str(c.style_from), -1, SQLITE_STATIC);
        if (sqlite3_step(upd) == SQLITE_DONE) n++;

        free(a.tags);
        free(a.tags_auto);
        free(rows[i].id); free(rows[i].title); free(rows[i].tags);
        free(rows[i].formats); free(rows[i].source);
    }
    exec1(s, "COMMIT");

    sqlite3_finalize(upd);
    free(rows);
    return n;
}

/* ---- search ------------------------------------------------------------- */

/* Free text -> a safe FTS5 MATCH expression: every term quoted, trailing * for
 * prefix matching. Quoting is what stops a stray " or - becoming an operator. */
static char *build_match(const char *text)
{
    sbuf        b;
    const char *p = text;

    sbuf_init(&b);
    while (*p) {
        const char *start;
        while (*p && (unsigned char)*p <= ' ') p++;
        if (!*p) break;
        start = p;
        while (*p && (unsigned char)*p > ' ') p++;

        if (b.len) sbuf_appendz(&b, " ");
        sbuf_appendz(&b, "\"");
        for (; start < p; start++) {
            if (*start == '"') sbuf_appendz(&b, "\"\"");
            else sbuf_append(&b, start, 1);
        }
        sbuf_appendz(&b, "\"*");
    }
    return b.buf;
}

/* The ranking formula. fts5's bm25() is negative-is-better, so every boost
 * subtracts and every penalty adds.
 *
 *  - popularity is the big one: a texture nobody downloads and a texture
 *    everyone downloads are not equally useful answers.
 *  - permissive licences rank above restrictive ones, because the whole point
 *    of the tool is "what can I actually ship".
 *  - a missing thumbnail is a worse result in any UI.
 *  - unclassifiable rows sink. They are still findable, just not first.
 */
#define SCORE_EXPR \
    "( bm25(assets_fts_v2)" \
    "  - (COALESCE(a.pop_pct,0)/100.0) * 1.2" \
    "  - CASE a.licence WHEN 'cc0' THEN 0.50 WHEN 'cc_by' THEN 0.25" \
    "                   WHEN 'oga_by' THEN 0.25 ELSE 0 END" \
    "  - CASE WHEN COALESCE(a.thumb_url,'') <> '' THEN 0.20 ELSE 0 END" \
    "  + CASE WHEN a.asset_type = 'unclassifiable' THEN 0.60 ELSE 0 END )"

static void add_filters(sbuf *sql, const search_query *q, int *arg)
{
    if (q->commercial_only)     sbuf_appendz(sql, "AND a.commercial_ok=1 ");
    if (q->no_attribution)      sbuf_appendz(sql, "AND a.attribution=0 ");
    if (!q->include_dead)
        sbuf_appendz(sql, "AND (a.http_status IS NULL OR a.http_status NOT IN (404,410)) ");
    if (q->type_filter    >= 0) sbuf_printf(sql, "AND a.asset_type=?%d ", (*arg)++);
    if (q->licence_filter >= 0) sbuf_printf(sql, "AND a.licence=?%d ",    (*arg)++);
    if (q->style_filter   >= 0) sbuf_printf(sql, "AND a.style=?%d ",      (*arg)++);
    if (q->source_filter)       sbuf_printf(sql, "AND a.source=?%d ",     (*arg)++);
    /* A tag filter searches both provenances - a user looking for "tileset"
     * does not care whether the source said it or we derived it. The
     * distinction matters for trust and for auditing, not for filtering. */
    if (q->tag_filter) {
        sbuf_printf(sql,
            "AND (','||COALESCE(a.tags,'')||',' LIKE '%%,'||?%d||',%%' "
            " OR  ','||COALESCE(a.tags_auto,'')||',' LIKE '%%,'||?%d||',%%') ",
            *arg, *arg + 1);
        *arg += 2;
    }
}

static int bind_filters(sqlite3_stmt *st, const search_query *q, int *arg)
{
    if (q->type_filter >= 0)
        sqlite3_bind_text(st, (*arg)++, asset_type_str((asset_type)q->type_filter),
                          -1, SQLITE_STATIC);
    if (q->licence_filter >= 0)
        sqlite3_bind_text(st, (*arg)++, licence_str((licence_id)q->licence_filter),
                          -1, SQLITE_STATIC);
    if (q->style_filter >= 0)
        sqlite3_bind_text(st, (*arg)++, style_str((style_id)q->style_filter),
                          -1, SQLITE_STATIC);
    if (q->source_filter)
        sqlite3_bind_text(st, (*arg)++, q->source_filter, -1, SQLITE_STATIC);
    if (q->tag_filter) {
        sqlite3_bind_text(st, (*arg)++, q->tag_filter, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, (*arg)++, q->tag_filter, -1, SQLITE_STATIC);
    }
    return 0;
}

static const char *col_txt(sqlite3_stmt *st, int i)
{
    return (const char *)sqlite3_column_text(st, i);
}

int store_search(store *s, const search_query *q, search_cb cb, void *ud)
{
    sbuf          sql;
    sqlite3_stmt *st    = NULL;
    char         *match = NULL;
    int           n = 0, arg, rc;

    sbuf_init(&sql);
    sbuf_appendz(&sql,
        "SELECT a.id,a.source,a.title,a.author,a.source_url,a.thumb_url,"
        " a.asset_type,a.licence,a.licence_url,a.formats,a.tags,a.style,"
        " a.polycount,a.updated_at,a.type_conf,a.type_ev,a.style_conf,"
        " a.style_ev,a.pop_pct,a.http_status,a.price,a.tileable,"
        " COALESCE(a.tags_auto,''),a.last_seen,a.last_checked FROM ");

    if (q->text && *q->text) {
        match = build_match(q->text);
        if (match && !*match) { free(match); match = NULL; }
    }

    if (match)
        sbuf_appendz(&sql,
            "assets_fts_v2 f JOIN assets a ON a.id=f.id "
            "WHERE assets_fts_v2 MATCH ?1 ");
    else
        sbuf_appendz(&sql, "assets a WHERE 1=1 ");

    arg = match ? 2 : 1;
    add_filters(&sql, q, &arg);

    if (match) sbuf_appendz(&sql, "ORDER BY " SCORE_EXPR " ASC ");
    else       sbuf_appendz(&sql, "ORDER BY COALESCE(a.pop_pct,0) DESC, "
                                  "a.updated_at DESC ");
    sbuf_printf(&sql, "LIMIT ?%d OFFSET ?%d", arg, arg + 1);

    if (sqlite3_prepare_v2(s->db, sql.buf, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "search: %s\n", sqlite3_errmsg(s->db));
        sbuf_free(&sql);
        free(match);
        return -1;
    }

    arg = 1;
    if (match) sqlite3_bind_text(st, arg++, match, -1, SQLITE_STATIC);
    bind_filters(st, q, &arg);
    sqlite3_bind_int(st, arg++, q->limit > 0 ? q->limit : 25);
    sqlite3_bind_int(st, arg,   q->offset > 0 ? q->offset : 0);

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        search_hit h;
        memset(&h, 0, sizeof h);
        asset_init(&h.a);

        /* Borrowed pointers, valid until the next step(). */
        h.a.id          = (char *)col_txt(st, 0);
        h.a.source      = (char *)col_txt(st, 1);
        h.a.title       = (char *)col_txt(st, 2);
        h.a.author      = (char *)col_txt(st, 3);
        h.a.source_url  = (char *)col_txt(st, 4);
        h.a.thumb_url   = (char *)col_txt(st, 5);
        h.a.type        = asset_type_parse(col_txt(st, 6));
        h.a.licence     = licence_parse(col_txt(st, 7));
        h.a.licence_url = (char *)col_txt(st, 8);
        h.a.formats     = (char *)col_txt(st, 9);
        h.a.tags        = (char *)col_txt(st, 10);
        h.a.style       = style_parse(col_txt(st, 11));
        h.a.polycount   = sqlite3_column_type(st, 12) == SQLITE_NULL
                              ? -1 : sqlite3_column_int64(st, 12);
        h.a.updated_at  = sqlite3_column_int64(st, 13);
        h.type_conf     = sqlite3_column_int(st, 14);
        h.type_ev       = col_txt(st, 15);
        h.style_conf    = sqlite3_column_int(st, 16);
        h.style_ev      = col_txt(st, 17);
        h.pop_pct       = sqlite3_column_type(st, 18) == SQLITE_NULL
                              ? -1 : sqlite3_column_int(st, 18);
        h.http_status   = sqlite3_column_int(st, 19);
        h.a.price       = sqlite3_column_type(st, 20) == SQLITE_NULL
                              ? -1 : sqlite3_column_double(st, 20);
        h.a.tileable    = sqlite3_column_type(st, 21) == SQLITE_NULL
                              ? TRI_UNKNOWN : (tribool)sqlite3_column_int(st, 21);
        h.a.tags_auto   = (char *)col_txt(st, 22);
        h.last_seen = sqlite3_column_int64(st, 23);
        h.last_checked = sqlite3_column_int64(st, 24);
        cb(&h, ud);
        n++;
    }
    if (rc != SQLITE_DONE)
        fprintf(stderr, "search step: %s\n", sqlite3_errmsg(s->db));

    sqlite3_finalize(st);
    sbuf_free(&sql);
    free(match);
    return rc == SQLITE_DONE ? n : -1;
}

int store_count(store *s, const search_query *q)
{
    sbuf          sql;
    sqlite3_stmt *st    = NULL;
    char         *match = NULL;
    int           arg, total = 0;

    sbuf_init(&sql);
    sbuf_appendz(&sql, "SELECT COUNT(*) FROM ");

    if (q->text && *q->text) {
        match = build_match(q->text);
        if (match && !*match) { free(match); match = NULL; }
    }
    if (match)
        sbuf_appendz(&sql,
            "assets_fts_v2 f JOIN assets a ON a.id=f.id "
            "WHERE assets_fts_v2 MATCH ?1 ");
    else
        sbuf_appendz(&sql, "assets a WHERE 1=1 ");

    arg = match ? 2 : 1;
    add_filters(&sql, q, &arg);

    if (sqlite3_prepare_v2(s->db, sql.buf, -1, &st, NULL) == SQLITE_OK) {
        arg = 1;
        if (match) sqlite3_bind_text(st, arg++, match, -1, SQLITE_STATIC);
        bind_filters(st, q, &arg);
        if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sbuf_free(&sql);
    free(match);
    return total;
}

/* ---- link health -------------------------------------------------------- */

static int list_urls(store *s, const char *sql, const char *source, int limit,
                     url_cb cb, void *ud)
{
    sqlite3_stmt *st=NULL; int n=0, rc, i;
    struct job { char *id, *url; } *jobs;
    if (limit<=0) return 0;
    jobs=calloc((size_t)limit,sizeof *jobs); if(!jobs) return -1;
    if (sqlite3_prepare_v2(s->db,sql,-1,&st,NULL)!=SQLITE_OK) { free(jobs); return -1; }
    sqlite3_bind_text(st,1,source?source:"",-1,SQLITE_STATIC); sqlite3_bind_int(st,2,limit);
    while ((rc=sqlite3_step(st))==SQLITE_ROW && n<limit) {
        jobs[n].id=xstrdup((const char *)sqlite3_column_text(st,0));
        jobs[n].url=xstrdup((const char *)sqlite3_column_text(st,1));
        if (!jobs[n].id || !jobs[n].url) { free(jobs[n].id); free(jobs[n].url); rc=SQLITE_NOMEM; break; }
        n++;
    }
    sqlite3_finalize(st); /* No live read transaction while a callback makes requests. */
    if (rc==SQLITE_DONE) for(i=0;i<n;i++) cb(jobs[i].id,jobs[i].url,ud);
    for(i=0;i<n;i++) { free(jobs[i].id); free(jobs[i].url); }
    free(jobs); return rc==SQLITE_DONE ? n : -1;
}

int store_list_unchecked(store *s, const char *source, int limit,
                         url_cb cb, void *ud)
{
    return list_urls(s,
        "SELECT id, source_url FROM assets"
        " WHERE (last_checked IS NULL OR ("
        " last_checked < CAST(strftime('%s','now') AS INTEGER) - CASE"
        " WHEN http_status IN (404,410) THEN 604800"
        " WHEN http_status BETWEEN 200 AND 299 THEN 2592000 ELSE 86400 END))"
        "   AND source <> 'polyhaven'"
        "   AND (?1 = '' OR source = ?1)"
        " ORDER BY COALESCE(last_checked,0), source, id LIMIT ?2",
        source, limit, cb, ud);
}

int store_set_status(store *s, const char *id, int http_status)
{
    sqlite3_reset(s->set_status);
    sqlite3_clear_bindings(s->set_status);
    sqlite3_bind_text(s->set_status, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_int(s->set_status, 2, http_status);
    sqlite3_bind_int64(s->set_status, 3, now_unix());
    if (sqlite3_step(s->set_status) != SQLITE_DONE) {
        fprintf(stderr, "set_status %s: %s\n", id, sqlite3_errmsg(s->db));
        sqlite3_reset(s->set_status);
        return -1;
    }
    sqlite3_reset(s->set_status);
    return 0;
}

/* ---- enrichment --------------------------------------------------------- */

void store_enrich_tags_only(store *s, int enabled) { s->enrich_tags_only=enabled; }

int store_list_unenriched(store *s, const char *source, int limit,
                          url_cb cb, void *ud)
{
    /* The enriched flag is the only condition. It was previously ANDed with
     * "author or tags is empty", which quietly broke the redo path: clearing
     * the flag on a row that already had an author selected a different row
     * instead, so a parsing fix could never be applied to the rows it broke.
     * Flag clear == redo this row, with no second opinion.
     *
     * Order by legal necessity before popularity. An asset whose licence
     * demands credit but whose creator we never recorded cannot lawfully be
     * shipped from what we hold - that is a broken promise, not a thin
     * listing. A CC0 row missing tags is merely harder to find. Sorting this
     * way cut the critical path from 27 hours of enrichment to 9.
     *
     * Then one level finer: prefer rows where the missing credit actually
     * unlocks something. Of the backlog, 797 are GPL - already flagged as not
     * cleared for commercial use - so fetching their authors would spend two
     * hours of someone else's server time on assets that stay unusable
     * either way. They are deprioritised, not skipped: the data is still
     * worth having, just last. */
    sbuf sql; sbuf_init(&sql);
    sbuf_appendz(&sql,
        "SELECT id, source_url FROM assets"
        " WHERE COALESCE(enriched,0) = 0 AND COALESCE(enrich_retry_at,0) <= CAST(strftime('%s','now') AS INTEGER)"
        "   AND (?1 = '' OR source = ?1)"
        );
    if(s->enrich_tags_only) sbuf_appendz(&sql," AND COALESCE(tags,'')='' AND NOT (attribution=1 AND COALESCE(author,'')='')");
    sbuf_appendz(&sql,
        " ORDER BY (attribution = 1 AND COALESCE(author,'') = ''"
        "           AND commercial_ok = 1) DESC,"
        "          (attribution = 1 AND COALESCE(author,'') = '') DESC,"
        "          COALESCE(pop_pct,0) DESC, id"
        " LIMIT ?2");
    int rc=list_urls(s,sql.buf,source,limit,cb,ud);
    sbuf_free(&sql); return rc;
}

int store_redo_blank_authors(store *s, const char *source)
{
    sqlite3_stmt *st;
    int           n = 0;

    if (sqlite3_prepare_v2(s->db,
            "UPDATE assets SET enriched = 0, enrich_retry_at=0"
            " WHERE COALESCE(enriched,0) = 1"
            "   AND COALESCE(author,'') = ''"
            "   AND (?1 = '' OR source = ?1)",
            -1, &st, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(st, 1, source ? source : "", -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_DONE) n = sqlite3_changes(s->db);
    sqlite3_finalize(st);
    return n;
}

int store_enrich(store *s, const char *id, const char *author, const char *tags,
                 const char *thumb_url)
{
    sqlite3_reset(s->enrich);
    sqlite3_clear_bindings(s->enrich);
    sqlite3_bind_text(s->enrich, 1, id, -1, SQLITE_STATIC);
    bind_txt(s->enrich, 2, author && *author ? author : NULL);
    bind_txt(s->enrich, 3, tags && *tags ? tags : NULL);
    bind_txt(s->enrich, 4, thumb_url && *thumb_url ? thumb_url : NULL);
    if (sqlite3_step(s->enrich) != SQLITE_DONE) {
        fprintf(stderr, "enrich %s: %s\n", id, sqlite3_errmsg(s->db));
        sqlite3_reset(s->enrich);
        return -1;
    }
    sqlite3_reset(s->enrich);
    return 0;
}

/* ---- stats -------------------------------------------------------------- */

static long long scalar(store *s, const char *sql)
{
    sqlite3_stmt *st;
    long long     v = 0;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    else v=-1;
    sqlite3_finalize(st);
    return v;
}

int store_get_stats(store *s, store_stats *out)
{
    out->total           = scalar(s, "SELECT COUNT(*) FROM assets");
    out->commercial_ok   = scalar(s, "SELECT COUNT(*) FROM assets WHERE commercial_ok=1");
    out->no_attribution  = scalar(s, "SELECT COUNT(*) FROM assets WHERE attribution=0");
    out->unknown_licence = scalar(s, "SELECT COUNT(*) FROM assets WHERE licence='unknown'");
    out->unclassifiable  = scalar(s, "SELECT COUNT(*) FROM assets WHERE asset_type='unclassifiable'");
    out->dead            = scalar(s, "SELECT COUNT(*) FROM assets WHERE http_status IN (404,410)");
    out->unchecked       = scalar(s, "SELECT COUNT(*) FROM assets WHERE last_checked IS NULL");
    out->no_tags         = scalar(s, "SELECT COUNT(*) FROM assets"
                                     " WHERE COALESCE(tags,'')=''"
                                     "   AND COALESCE(tags_auto,'')=''");
    out->source_tagged   = scalar(s, "SELECT COUNT(*) FROM assets"
                                     " WHERE COALESCE(tags,'')<>''");
    out->auto_only       = scalar(s, "SELECT COUNT(*) FROM assets"
                                     " WHERE COALESCE(tags,'')=''"
                                     "   AND COALESCE(tags_auto,'')<>''");
    out->uncreditable    = scalar(s, "SELECT COUNT(*) FROM assets"
                                     " WHERE attribution=1"
                                     "   AND COALESCE(author,'')=''");
    return out->total<0 || out->commercial_ok<0 || out->no_attribution<0 ||
        out->unknown_licence<0 || out->unclassifiable<0 || out->dead<0 ||
        out->unchecked<0 || out->no_tags<0 || out->source_tagged<0 ||
        out->auto_only<0 || out->uncreditable<0 ? -1 : 0;
}

int store_print_breakdown(store *s)
{
    sqlite3_stmt *st;
    const char   *qs[4];
    const char   *hd[4] = { "by source", "by licence", "by type", "by style" };
    int           i;

    qs[0] = "SELECT source, COUNT(*) FROM assets GROUP BY source ORDER BY 2 DESC";
    qs[1] = "SELECT licence, COUNT(*) FROM assets GROUP BY licence ORDER BY 2 DESC";
    qs[2] = "SELECT asset_type, COUNT(*) FROM assets GROUP BY asset_type ORDER BY 2 DESC";
    qs[3] = "SELECT style, COUNT(*) FROM assets GROUP BY style ORDER BY 2 DESC";

    for (i = 0; i < 4; i++) {
        printf("\n  %s\n", hd[i]);
        if (sqlite3_prepare_v2(s->db, qs[i], -1, &st, NULL) != SQLITE_OK) continue;
        while (sqlite3_step(st) == SQLITE_ROW)
            printf("    %-16s %8lld\n",
                   (const char *)sqlite3_column_text(st, 0),
                   (long long)sqlite3_column_int64(st, 1));
        sqlite3_finalize(st);
    }
    return 0;
}

int store_list_sources(store *s, source_cb cb, void *ud)
{
    sqlite3_stmt *st;
    int           n = 0, rc;
    const char   *sql =
        "SELECT source, COUNT(*),"
        "  SUM(CASE WHEN http_status IN (404,410) THEN 1 ELSE 0 END),"
        "  SUM(CASE WHEN last_checked IS NULL THEN 1 ELSE 0 END),"
        "  SUM(CASE WHEN asset_type='unclassifiable' THEN 1 ELSE 0 END),"
        "  SUM(CASE WHEN commercial_ok=1 THEN 1 ELSE 0 END),"
        "  MAX(COALESCE(last_seen,0)), MAX(COALESCE(last_checked,0)),"
        "  COALESCE((SELECT last_attempt FROM source_health h WHERE h.source=assets.source),0),"
        "  COALESCE((SELECT last_success FROM source_health h WHERE h.source=assets.source),0),"
        "  COALESCE((SELECT state FROM source_health h WHERE h.source=assets.source),0),"
        "  SUM(COALESCE(tags,'')<>''), SUM(attribution=1 AND COALESCE(author,'')='')"
        " FROM assets GROUP BY source ORDER BY 2 DESC";

    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    while ((rc=sqlite3_step(st)) == SQLITE_ROW) {
        source_row r;
        memset(&r, 0, sizeof r);
        snprintf(r.source, sizeof r.source, "%s",
                 (const char *)sqlite3_column_text(st, 0));
        r.assets        = sqlite3_column_int64(st, 1);
        r.dead          = sqlite3_column_int64(st, 2);
        r.unchecked     = sqlite3_column_int64(st, 3);
        r.unclassified  = sqlite3_column_int64(st, 4);
        r.commercial_ok = sqlite3_column_int64(st, 5);
        r.last_seen     = sqlite3_column_int64(st, 6);
        r.last_checked=sqlite3_column_int64(st,7);
        r.last_attempt=sqlite3_column_int64(st,8);
        r.last_success=sqlite3_column_int64(st,9);
        r.crawl_state=sqlite3_column_int(st,10);
        r.source_tagged=sqlite3_column_int64(st,11);
        r.missing_authors=sqlite3_column_int64(st,12);
        cb(&r, ud);
        n++;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? n : -1;
}

/* ---- persistent request budget ------------------------------------------ */

long long store_budget_used(store *s, const char *day)
{
    sqlite3_stmt *st;
    long long     v = -1;

    if (sqlite3_prepare_v2(s->db,
            "SELECT used FROM request_budget WHERE day=?1", -1, &st, NULL)
        != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, day, -1, SQLITE_STATIC);
    { int rc=sqlite3_step(st); if(rc==SQLITE_ROW) v=sqlite3_column_int64(st,0); else if(rc==SQLITE_DONE) v=0; }
    sqlite3_finalize(st);
    return v;
}

int store_budget_add(store *s, const char *day, int n)
{
    sqlite3_stmt *st;
    int           rc;

    if (n <= 0) return 0;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO request_budget(day,used) VALUES(?1,?2)"
            " ON CONFLICT(day) DO UPDATE SET used = used + ?2",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, day, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, n);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

void store_budget_today_key(char out[11])
{
    time_t     t  = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(out, 11, "%Y-%m-%d", tm);
}

long long store_budget_used_today(store *s)
{
    char day[11];
    store_budget_today_key(day);
    return store_budget_used(s, day);
}

int store_budget_add_today(store *s, int n)
{
    char day[11];
    store_budget_today_key(day);
    return store_budget_add(s, day, n);
}

int store_budget_report(store *s, int days)
{
    sqlite3_stmt *st;

    if (sqlite3_prepare_v2(s->db,
            "SELECT day, used FROM request_budget ORDER BY day DESC LIMIT ?1",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(st, 1, days > 0 ? days : 14);
    while (sqlite3_step(st) == SQLITE_ROW)
        printf("  %-12s %6lld\n", (const char *)sqlite3_column_text(st, 0),
               (long long)sqlite3_column_int64(st, 1));
    sqlite3_finalize(st);
    return 0;
}

/* ---- http cache --------------------------------------------------------- */

int store_cache_get(store *s, const char *url,
                    char *etag, size_t etag_n,
                    char *lastmod, size_t lastmod_n,
                    sbuf *body)
{
    int rc = -1;

    if (etag)    etag[0] = '\0';
    if (lastmod) lastmod[0] = '\0';

    sqlite3_reset(s->cache_get);
    sqlite3_clear_bindings(s->cache_get);
    sqlite3_bind_text(s->cache_get, 1, url, -1, SQLITE_STATIC);

    if (sqlite3_step(s->cache_get) == SQLITE_ROW) {
        const char *e = (const char *)sqlite3_column_text(s->cache_get, 0);
        const char *m = (const char *)sqlite3_column_text(s->cache_get, 1);
        if (e && etag)    snprintf(etag, etag_n, "%s", e);
        if (m && lastmod) snprintf(lastmod, lastmod_n, "%s", m);
        if (body) {
            const void *blob = sqlite3_column_blob(s->cache_get, 2);
            int         n    = sqlite3_column_bytes(s->cache_get, 2);
            if (blob && n > 0) sbuf_append(body, blob, (size_t)n);
        }
        rc = 0;
    }
    sqlite3_reset(s->cache_get);
    return rc;
}

int store_cache_put(store *s, const char *url, const char *etag,
                    const char *lastmod, const char *body, size_t len)
{
    sqlite3_reset(s->cache_put);
    sqlite3_clear_bindings(s->cache_put);
    sqlite3_bind_text(s->cache_put, 1, url, -1, SQLITE_STATIC);
    bind_txt(s->cache_put, 2, etag && *etag ? etag : NULL);
    bind_txt(s->cache_put, 3, lastmod && *lastmod ? lastmod : NULL);
    sqlite3_bind_blob(s->cache_put, 4, body, (int)len, SQLITE_STATIC);
    sqlite3_bind_int64(s->cache_put, 5, now_unix());

    if (sqlite3_step(s->cache_put) != SQLITE_DONE) {
        fprintf(stderr, "cache put: %s\n", sqlite3_errmsg(s->db));
        sqlite3_reset(s->cache_put);
        return -1;
    }
    sqlite3_reset(s->cache_put);
    return 0;
}


static long long keyed_number(store *s, const char *sql, const char *key)
{
    sqlite3_stmt *q=NULL; long long value=-1; int rc;
    if(sqlite3_prepare_v2(s->db,sql,-1,&q,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(q,1,key,-1,SQLITE_STATIC); rc=sqlite3_step(q);
    if(rc==SQLITE_ROW) value=sqlite3_column_int64(q,0); else if(rc==SQLITE_DONE) value=0;
    sqlite3_finalize(q); return value;
}
long long store_cache_time(store *s, const char *url)
{ return keyed_number(s,"SELECT fetched_at FROM http_cache WHERE url=?1",url); }
long long store_run_used(store *s, const char *run)
{ return keyed_number(s,"SELECT used FROM request_runs WHERE id=?1",run); }
int store_host_pause(store *s, const char *host, int seconds)
{
    sqlite3_stmt *q=NULL; int rc;
    if(sqlite3_prepare_v2(s->db,"INSERT INTO request_hosts(host,blocked_until) VALUES(?1,?2) ON CONFLICT(host) DO UPDATE SET blocked_until=MAX(blocked_until,excluded.blocked_until)",-1,&q,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(q,1,host,-1,SQLITE_STATIC); sqlite3_bind_int64(q,2,wall_ms()+(long long)seconds*1000);
    rc=sqlite3_step(q); sqlite3_finalize(q); return rc==SQLITE_DONE ? 0 : -1;
}
int store_request_reserve(store *s, const char *run, const char *host,
                          int interval_ms, int daily_cap, int run_cap,
                          int host_cap, int weekly_cap, int *wait_ms)
{
    char day[11], monday[11], host_key[400]; long long daily, used, host_used, week, next, blocked, now=wall_ms();
    sqlite3_stmt *q=NULL; int rc=-1;
    time_t t=time(NULL); struct tm tm=*localtime(&t);
    *wait_ms=0;
    if(!sqlite3_get_autocommit(s->db)) { fprintf(stderr,"request inside asset transaction refused\n"); return -1; }
    if(exec1(s,"BEGIN IMMEDIATE")) return -1;
    store_budget_today_key(day);
    tm.tm_mday-=(tm.tm_wday+6)%7; tm.tm_hour=12; mktime(&tm); strftime(monday,sizeof monday,"%Y-%m-%d",&tm);
    snprintf(host_key,sizeof host_key,"%s\n%s",run,host);
    daily=store_budget_used(s,day); used=store_run_used(s,run);
    host_used=keyed_number(s,"SELECT used FROM request_run_hosts WHERE id=?1",host_key);
    week=keyed_number(s,"SELECT COALESCE(SUM(used),0) FROM request_budget WHERE day>=?1",monday);
    next=keyed_number(s,"SELECT last_ms FROM request_hosts WHERE host=?1",host);
    blocked=keyed_number(s,"SELECT blocked_until FROM request_hosts WHERE host=?1",host);
    if(daily<0 || used<0 || host_used<0 || week<0 || next<0 || blocked<0) goto done;
    if((daily_cap>0 && daily>=daily_cap) || (run_cap>0 && used>=run_cap) ||
       (host_cap>0 && host_used>=host_cap) || (weekly_cap>0 && week>=weekly_cap) || blocked>now) { rc=1; goto done; }
    next += interval_ms;
    if(next>now) { *wait_ms=(int)(next-now>1000 ? 1000 : next-now); rc=2; goto done; }
    if(stop_requested()) { rc=1; goto done; }
    if(store_budget_add(s,day,1)) goto done;
    if(sqlite3_prepare_v2(s->db,"INSERT INTO request_runs(id,used) VALUES(?1,1) ON CONFLICT(id) DO UPDATE SET used=used+1",-1,&q,NULL)!=SQLITE_OK) goto done;
    sqlite3_bind_text(q,1,run,-1,SQLITE_STATIC);
    if(sqlite3_step(q)!=SQLITE_DONE) goto done;
    sqlite3_finalize(q); q=NULL;
    if(sqlite3_prepare_v2(s->db,"INSERT INTO request_run_hosts(id,used) VALUES(?1,1) ON CONFLICT(id) DO UPDATE SET used=used+1",-1,&q,NULL)!=SQLITE_OK) goto done;
    sqlite3_bind_text(q,1,host_key,-1,SQLITE_STATIC);
    if(sqlite3_step(q)!=SQLITE_DONE) goto done;
    sqlite3_finalize(q); q=NULL;
    if(sqlite3_prepare_v2(s->db,"INSERT INTO request_hosts(host,last_ms) VALUES(?1,?2) ON CONFLICT(host) DO UPDATE SET last_ms=excluded.last_ms",-1,&q,NULL)!=SQLITE_OK) goto done;
    sqlite3_bind_text(q,1,host,-1,SQLITE_STATIC); sqlite3_bind_int64(q,2,now);
    if(sqlite3_step(q)!=SQLITE_DONE) goto done;
    sqlite3_finalize(q); q=NULL;
    if(exec1(s,"COMMIT")) goto done;
    return 0;
done:
    sqlite3_finalize(q); exec1(s,"ROLLBACK"); return rc;
}
int store_enrich_retry(store *s, const char *id)
{
    sqlite3_stmt *q=NULL; int rc;
    if(sqlite3_prepare_v2(s->db,"UPDATE assets SET enrich_retry_at=?2 WHERE id=?1 AND COALESCE(enriched,0)=0",-1,&q,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(q,1,id,-1,SQLITE_STATIC); sqlite3_bind_int64(q,2,now_unix()+86400);
    rc=sqlite3_step(q); sqlite3_finalize(q); return rc==SQLITE_DONE?0:-1;
}

int store_crawl_state(store *s, const char *source, int state, int emitted)
{
    sqlite3_stmt *st=NULL;
    const char *sql="INSERT INTO source_health(source,last_attempt,last_success,state,emitted) VALUES(?1,?2,CASE WHEN ?3=2 THEN ?2 ELSE 0 END,?3,?4) ON CONFLICT(source) DO UPDATE SET last_attempt=CASE WHEN ?3=1 THEN ?2 ELSE source_health.last_attempt END,last_success=CASE WHEN ?3=2 THEN ?2 ELSE source_health.last_success END,state=?3,emitted=?4";
    if(sqlite3_prepare_v2(s->db,sql,-1,&st,NULL)!=SQLITE_OK) return -1;
    sqlite3_bind_text(st,1,source,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(st,2,now_unix()); sqlite3_bind_int(st,3,state); sqlite3_bind_int(st,4,emitted);
    int rc=sqlite3_step(st); sqlite3_finalize(st); return rc==SQLITE_DONE ? 0 : -1;
}
