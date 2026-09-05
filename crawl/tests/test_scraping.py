"""Regression tests: temporary indices and a loopback HTTP server only.

Build magpie first, then: python -X utf8 -m unittest discover -s tests -v
MAGPIE_TEST_EXE can select another build.
"""
import contextlib
import datetime
import gzip
import http.server
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "adapters"))
import opengameart as oga
import kenney
import gameicons
from network import FetchError

spec = importlib.util.spec_from_file_location("runner", ROOT / "runner" / "runner.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
EXE = Path(os.environ.get("MAGPIE_TEST_EXE", ROOT / ("magpie.exe" if os.name == "nt" else "magpie"))).resolve()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        server = self.server
        server.requests.append((self.command, self.path))
        status, headers, body = server.routes.get(self.path, (404, {}, b"missing"))
        self.send_response(status)
        for name, value in headers.items():
            self.send_header(name, value)
        if "Content-Length" not in headers:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            with contextlib.suppress(ConnectionError):
                self.wfile.write(body)
                self.wfile.flush()
        if headers.get("X-Truncate"):
            self.close_connection = True
            with contextlib.suppress(OSError):
                self.connection.shutdown(socket.SHUT_RDWR)


class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not EXE.exists():
            raise RuntimeError("Build magpie before running these tests")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        self.db = self.work / "index.sqlite"
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("MAGPIE_")}
        self.env.update(MAGPIE_DB=str(self.db), MAGPIE_RUN_ID=uuid.uuid4().hex,
                        MAGPIE_STOP_FILE=str(self.work / "STOP"), MAGPIE_EXE=str(EXE))
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.routes = {"/robots.txt": (200, {}, b"User-agent: *\nDisallow:\n"),
                              "/asset": (200, {}, b"asset"),
                              "/second": (200, {}, b"second")}
        self.server.requests = []
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"
        p = self.cmd("stats", "--db", str(self.db))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.cache(self.base + "/robots.txt", "User-agent: *\nDisallow:\n")

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.tmp.cleanup()

    def cmd(self, *args, env=None, cwd=None):
        return subprocess.run([str(EXE), *args], cwd=cwd or ROOT, env=env or self.env,
                              encoding="utf-8", capture_output=True, timeout=30)

    def sql(self, query, args=()):
        with contextlib.closing(sqlite3.connect(self.db)) as db, db:
            return db.execute(query, args).fetchall()

    def cache(self, url, text):
        self.sql("INSERT OR REPLACE INTO http_cache(url,body,fetched_at) VALUES(?,?,?)",
                 (url, text.encode(), int(time.time())))

    def fetch(self, path="/asset", method="GET", **env):
        return self.cmd("fetch", "--url", self.base + path, "--method", method,
                        env={**self.env, **env})

    def seed(self, source="opengameart", slug="fixture", path="/asset", status=None, checked=None):
        self.sql("""INSERT INTO assets(id,source,title,source_url,licence,attribution,
                     commercial_ok,http_status,last_checked) VALUES(?,?,?,?,?,?,?,?,?)""",
                 (source + ":" + slug, source, "Fixture", self.base + path, "cc_by", 1, 1, status, checked))

    def test_polyhaven_website_is_blocked_before_network_or_budget(self):
        for host in ('polyhaven.com', 'www.polyhaven.com'):
            result = self.cmd('fetch', '--url', 'https://' + host + '/a/fixture')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('source terms', result.stderr)
        self.assertEqual(self.sql('SELECT used FROM request_budget'), [])

    def test_redirect_to_polyhaven_website_is_blocked(self):
        self.server.routes['/asset'] = (302, {'Location': 'https://polyhaven.com/a/fixture'}, b'')
        result = self.fetch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('source terms', result.stderr)
        self.assertEqual(self.sql('SELECT used FROM request_budget'), [(1,)])

    def test_polyhaven_is_excluded_from_website_link_checks(self):
        self.seed(source='polyhaven')
        result = self.cmd('check', 'polyhaven', '--limit', '1', '--db', str(self.db))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.server.requests, [])

    def test_export_keeps_previous_file_on_database_iteration_error(self):
        self.seed()
        out=self.work / 'data.json'
        self.assertEqual(self.cmd('export','--db',str(self.db),'--out',str(out)).returncode,0)
        before=out.read_bytes()
        self.sql('ALTER TABLE assets RENAME COLUMN pop_pct TO missing_pop_pct')
        result=self.cmd('export','--db',str(self.db),'--out',str(out))
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(out.read_bytes(),before)
        self.assertEqual(list(self.work.glob('data.json.*.tmp')),[])

    @unittest.skipUnless(os.name=='nt','Windows replacement locking')
    def test_export_preserves_file_when_windows_denies_replacement(self):
        import ctypes
        from ctypes import wintypes
        self.seed()
        out=self.work/'data.json'; out.write_text('{"previous":true}')
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE]
        kernel.CreateFileW.restype=wintypes.HANDLE
        kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        # Read sharing permits inspection but denies deleting/replacing this file.
        handle=kernel.CreateFileW(str(out),0x80000000,1,None,3,0,None)
        self.assertNotEqual(handle,ctypes.c_void_p(-1).value)
        try:
            result=self.cmd('export','--db',str(self.db),'--out',str(out))
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(out.read_text(),'{"previous":true}')
        finally:
            kernel.CloseHandle(handle)
        self.assertEqual(list(self.work.glob('data.json.*.tmp')),[])

    def test_export_stable_identity_and_source_freshness(self):
        self.seed()
        self.sql("UPDATE assets SET last_seen=12345,last_checked=23456")
        self.sql("INSERT INTO source_health(source,last_attempt,last_success,state) VALUES('opengameart',34567,30000,3)")
        out=self.work/'data.json'
        self.assertEqual(self.cmd('export','--db',str(self.db),'--out',str(out)).returncode,0)
        data=json.loads(out.read_text())
        self.assertEqual(data['assets'][0]['id'],'opengameart:fixture')
        self.assertEqual(data['assets'][0]['seen'],12345)
        self.assertEqual(data['sources'][0]['last_success'],30000)
        self.assertEqual(data['sources'][0]['crawl_state'],3)
        self.assertGreater(data['generated'],data['sources'][0]['last_seen'])

    def test_export_rejects_invalid_asset_without_replacing_file(self):
        self.seed()
        out=self.work/'data.json'; out.write_text('{"previous":true}')
        self.sql("UPDATE assets SET source_url='javascript:alert(1)'")
        self.assertNotEqual(self.cmd('export','--db',str(self.db),'--out',str(out)).returncode,0)
        self.assertEqual(out.read_text(),'{"previous":true}')

    def test_missing_tags_queue_skips_already_tagged_rows(self):
        self.seed(slug='tagged'); self.seed(slug='blank',path='/blank')
        self.sql("UPDATE assets SET author='Ada',tags=CASE WHEN id LIKE '%tagged' THEN 'wood' ELSE '' END")
        self.server.routes['/blank']=(200,{},b'<div class="field-name-author-submitter"><a href="/users/ada">Ada</a></div><div class="field-name-field-art-tags"><a href="/art-search?keys=stone">stone</a></div>')
        result=self.cmd('enrich','opengameart','--missing-tags','--limit','1','--max-requests','1','--db',str(self.db))
        self.assertEqual([path for method,path in self.server.requests],['/blank'])
        self.assertEqual(self.sql("SELECT enriched FROM assets WHERE id='opengameart:tagged'"),[(0,)])

    def test_runner_rotates_all_work_and_skips_paused_source(self):
        queues={'enrich_pending':{'opengameart':2},'tag_pending':{'kenney':3},'refresh_due':['polyhaven'],'unchecked':5}
        jobs=[runner.pick_job({**queues,'work_turn':i},{},20)[0] for i in range(4)]
        self.assertEqual([j[:2] for j in jobs],[['enrich','opengameart'],['enrich','kenney'],['check','--limit'],['crawl','polyhaven']])
        job,_=runner.pick_job(queues,{'opengameart':True},20)
        self.assertEqual(job[:2],['enrich','kenney'])

    def test_offline_cache_and_miss_make_no_requests(self):
        self.cache(self.base + "/asset", "cached")
        self.assertEqual(json.loads(self.fetch(MAGPIE_OFFLINE="1").stdout)["body"], "cached")
        self.assertNotEqual(self.fetch("/missing", MAGPIE_OFFLINE="1").returncode, 0)
        self.assertEqual(self.server.requests, [])

    def test_offline_external_crawl_uses_cached_tree(self):
        self.cache(gameicons.TREE, json.dumps({"tree": [{"path": "lorc/test.svg", "type": "blob"}]}))
        p = self.cmd("crawl", "gameicons", "--offline", "--db", str(self.db))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.sql("SELECT count(*) FROM assets"), [(1,)])
        self.assertEqual(self.server.requests, [])

    def test_retry_cap_charged_before_attempt(self):
        self.server.routes["/asset"] = (500, {}, b"failed")
        p = self.fetch(MAGPIE_MAX_REQUESTS="1", MAGPIE_DAILY_CAP="1")
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(self.server.requests, [("GET", "/asset")])
        self.assertEqual(self.sql("SELECT used FROM request_budget"), [(1,)])

    def test_head_retry_cap(self):
        self.server.routes["/asset"] = (500, {}, b"failed")
        self.assertNotEqual(self.fetch(method="HEAD", MAGPIE_MAX_REQUESTS="1").returncode, 0)
        self.assertEqual(self.server.requests, [("HEAD", "/asset")])

    def test_robots_disallow_prevents_target(self):
        self.cache(self.base + "/robots.txt", "User-agent: *\nDisallow: /asset\n")
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertEqual(self.server.requests, [])

    def test_robots_unreachable_prevents_target(self):
        self.sql("DELETE FROM http_cache")
        self.server.routes["/robots.txt"] = (503, {}, b"offline")
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertEqual(self.server.requests, [("GET", "/robots.txt")])

    def test_robots_404_allows_target_and_is_cached(self):
        self.sql("DELETE FROM http_cache")
        self.server.routes["/robots.txt"] = (404, {}, b"not found")
        p = self.fetch()
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.server.requests, [("GET", "/robots.txt"), ("GET", "/asset")])

    def test_cached_304_keeps_body_and_validators(self):
        self.cache(self.base + "/asset", "cached body")
        self.sql("UPDATE http_cache SET etag=? WHERE url=?", ('"v1"', self.base + "/asset"))
        self.server.routes["/asset"] = (304, {}, b"")
        p = self.fetch()
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(p.stdout)["body"], "cached body")
        self.assertEqual(self.sql("SELECT etag FROM http_cache WHERE url=?", (self.base + "/asset",)), [('"v1"',)])

    def test_new_response_drops_obsolete_validator(self):
        self.cache(self.base + "/asset", "old")
        self.sql("UPDATE http_cache SET etag=? WHERE url=?", ('"v1"', self.base + "/asset"))
        self.assertEqual(self.fetch().returncode, 0)
        self.assertEqual(self.sql("SELECT etag FROM http_cache WHERE url=?", (self.base + "/asset",)), [(None,)])

    def test_gzip_response_is_validated_after_decompression(self):
        text = "café " * 100
        self.server.routes["/asset"] = (200, {"Content-Encoding": "gzip"}, gzip.compress(text.encode()))
        p = self.fetch()
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(p.stdout)["body"], text)

    def test_query_string_reaches_server_unchanged(self):
        self.server.routes["/asset?license=4&type=9"] = (200, {}, b"filtered")
        p = self.fetch("/asset?license=4&type=9")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.server.requests, [("GET", "/asset?license=4&type=9")])

    def test_robots_request_counts_against_cap(self):
        self.sql("DELETE FROM http_cache")
        self.assertNotEqual(self.fetch(MAGPIE_MAX_REQUESTS="1").returncode, 0)
        self.assertEqual(self.server.requests, [("GET", "/robots.txt")])
        self.assertEqual(self.sql("SELECT used FROM request_budget"), [(1,)])

    def test_throttle_defers_host_across_processes(self):
        self.server.routes["/asset"] = (429, {"Retry-After": "3600"}, b"slow")
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertNotEqual(self.fetch("/second", MAGPIE_RUN_ID=uuid.uuid4().hex).returncode, 0)
        self.assertEqual(len(self.server.requests), 1)
        self.assertGreater(self.sql("SELECT blocked_until FROM request_hosts")[0][0], time.time()*1000 + 3500000)

    def test_stop_during_wait_prevents_next_request(self):
        self.sql("INSERT INTO request_hosts(host,last_ms) VALUES('127.0.0.1',?)", (int(time.time()*1000)+3000,))
        timer = threading.Timer(0.3, lambda: (self.work / "STOP").write_text("stop"))
        timer.start()
        try:
            start = time.monotonic()
            self.assertNotEqual(self.fetch().returncode, 0)
            self.assertLess(time.monotonic()-start, 3)
        finally:
            timer.join()
        self.assertEqual(self.server.requests, [])

    def test_redirect_hop_obeys_budget_and_robots(self):
        self.server.routes["/asset"] = (302, {"Location": "/second"}, b"")
        self.assertNotEqual(self.fetch(MAGPIE_MAX_REQUESTS="1").returncode, 0)
        self.assertEqual(self.server.requests, [("GET", "/asset")])

    def test_redirect_destination_is_checked_against_robots(self):
        self.cache(self.base + "/robots.txt", "User-agent: *\nDisallow: /second\n")
        self.server.routes["/asset"] = (302, {"Location": "/second"}, b"")
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertEqual(self.server.requests, [("GET", "/asset")])

    def test_weekly_cap_includes_earlier_days(self):
        today = datetime.date.today()
        monday = today - datetime.timedelta(days=today.weekday())
        self.sql("INSERT INTO request_budget(day,used) VALUES(?,1)", (monday.isoformat(),))
        self.assertNotEqual(self.fetch(MAGPIE_WEEKLY_CAP="1").returncode, 0)
        self.assertEqual(self.server.requests, [])

    def test_old_day_does_not_consume_todays_allowance(self):
        self.sql("INSERT INTO request_budget(day,used) VALUES('2000-01-01',500)")
        self.assertEqual(self.fetch(MAGPIE_DAILY_CAP="1").returncode, 0)
        today = datetime.date.today().isoformat()
        self.assertEqual(self.sql("SELECT used FROM request_budget WHERE day=?", (today,)), [(1,)])

    def test_ledger_failure_prevents_network(self):
        self.sql("DROP TABLE request_runs")
        self.assertNotEqual(self.fetch().returncode, 0)
        self.assertEqual(self.server.requests, [])

    def test_partial_native_crawl_returns_failure_but_keeps_rows(self):
        url = "https://ambientcg.com/api/v2/full_json?limit=250&offset={}&include=imageData&sort=Alphabet"
        assets = [{"assetId": f"Test{i}", "displayName": "Test", "dataType": "Material"} for i in range(250)]
        self.cache(url.format(0), json.dumps({"numberOfResults": 251, "foundAssets": assets}))
        self.cache(url.format(250), "{}")
        p = self.cmd("crawl", "ambientcg", "--offline", "--db", str(self.db))
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(self.sql("SELECT count(*) FROM assets"), [(250,)])
        self.assertEqual(self.server.requests, [])

    def test_new_native_tags_replace_longer_old_tags(self):
        url = "https://api.polyhaven.com/assets"
        self.seed("polyhaven", "fixture")
        self.sql("UPDATE assets SET tags='old,long,incorrect,tags'")
        self.cache(url, json.dumps({"fixture": {"name": "Fixture", "type": 2, "tags": ["new"]}}))
        p = self.cmd("crawl", "polyhaven", "--offline", "--db", str(self.db))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.sql("SELECT tags FROM assets"), [("new",)])

    def test_truncated_body_is_not_cached(self):
        self.server.routes["/asset"] = (200, {"Content-Length": "1000", "ETag": '"x"', "X-Truncate": "yes"}, b"partial")
        self.assertNotEqual(self.fetch(MAGPIE_MAX_REQUESTS="1").returncode, 0)
        self.assertEqual(self.sql("SELECT count(*) FROM http_cache WHERE url=?", (self.base+"/asset",)), [(0,)])

    def test_concurrent_processes_share_atomic_daily_cap(self):
        env = {**self.env, "MAGPIE_DAILY_CAP": "1"}
        args = [str(EXE), "fetch", "--url", self.base + "/asset"]
        children = [subprocess.Popen(args, env={**env, "MAGPIE_RUN_ID": uuid.uuid4().hex},
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
        for child in children:
            child.communicate(timeout=15)
        self.assertEqual(sorted(p.returncode for p in children), [0, 1])
        self.assertEqual(len(self.server.requests), 1)
        self.assertEqual(self.sql("SELECT used FROM request_budget"), [(1,)])

    def test_enrichment_sources_share_budget(self):
        self.server.routes["/asset"] = (200, {}, b'<meta property="og:image" content="https://example.test/image">')
        self.server.routes["/second"] = (200, {}, b'<div class="field-name-author-submitter"><span class="username"><a href="/users/a">Alice</a></span></div>')
        self.seed("kenney")
        self.seed(path="/second")
        p = self.cmd("enrich", "--limit", "2", "--max-requests", "1", "--db", str(self.db))
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(len(self.server.requests), 1)
        self.assertEqual(self.sql("SELECT sum(enriched) FROM assets"), [(1,)])

    def test_empty_enrichment_stays_retryable(self):
        self.seed()
        p = self.cmd("enrich", "opengameart", "--limit", "1", "--db", str(self.db))
        self.assertNotEqual(p.returncode, 0)
        enriched, retry = self.sql("SELECT enriched,enrich_retry_at FROM assets")[0]
        self.assertEqual(enriched, 0)
        self.assertGreater(retry, time.time())

    def test_temporary_head_status_does_not_hide_asset(self):
        self.seed()
        self.server.routes["/asset"] = (405, {}, b"GET only")
        p = self.cmd("check", "opengameart", "--limit", "1", "--db", str(self.db))
        self.assertNotEqual(p.returncode, 0)
        p = self.cmd("search", "--db", str(self.db), "--urls")
        self.assertIn(self.base + "/asset", p.stdout)

    def test_dead_link_recovers_on_due_recheck(self):
        self.seed(status=404, checked=int(time.time())-8*86400)
        p = self.cmd("check", "opengameart", "--limit", "1", "--db", str(self.db))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.sql("SELECT http_status FROM assets"), [(200,)])

    def test_runner_uses_actionable_source_backlog(self):
        self.seed("gameicons")
        with patch.object(runner, "CRAWL", str(self.work)):
            stats, err = runner.stats()
        self.assertIsNone(err)
        job, _ = runner.pick_job(stats, {}, 120)
        self.assertEqual(job[0], "check")


class AdapterTests(unittest.TestCase):
    def test_oga_licence_versions_emit_distinct_official_links(self):
        html = 'Displaying 1 - 1 of 1 <span class="art-preview-title"><a href="/content/test">Test</a></span>'
        with patch.object(oga, 'TYPES', oga.TYPES[:1]), patch.object(oga, 'get', return_value=html):
            rc, rows = self.capture(oga.crawl, 1, 'oga_by', None)
        self.assertEqual(rc, 0)
        self.assertEqual([r['licence_url'] for r in rows], [
            'https://opengameart.org/content/oga-by-30-faq',
            'https://opengameart.org/content/oga-by-40-faq'])

    def capture(self, fn, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = fn(*args)
        return rc, [json.loads(line) for line in out.getvalue().splitlines()]

    def test_dual_licence_emits_preferred_option_last(self):
        html = '<span class="art-preview-title"><a href="/content/dual">Dual</a></span>'
        with patch.object(oga, "LICENCES", [oga.LICENCES[0], oga.LICENCES[-1]]), patch.object(oga, "TYPES", oga.TYPES[:1]), patch.object(oga, "get", return_value=html):
            rc, rows = self.capture(oga.crawl, 1, None, None)
        self.assertEqual(rc, 0)
        self.assertEqual([r["licence"] for r in rows], ["gpl", "cc0"])

    def test_oga_aborts_source_on_throttle(self):
        with patch.object(oga, "get", side_effect=FetchError("fixture", 429)) as get:
            rc, rows = self.capture(oga.crawl, 1, None, None)
        self.assertNotEqual(rc, 0)
        self.assertEqual(get.call_count, 1)
        self.assertEqual(rows, [])

    def test_oga_rejects_maintenance_listing(self):
        with patch.object(oga, "get", return_value="<html>Maintenance</html>"):
            rc, _ = self.capture(oga.crawl, 1, None, None)
        self.assertNotEqual(rc, 0)

    def test_partial_kenney_refresh_leaves_file_unchanged(self):
        with tempfile.TemporaryDirectory() as td:
            data = Path(td) / "packs.json"
            original = json.dumps({"packs": [{"slug": "keep", "name": "Custom", "type": "ui", "thumb": "image"}, {"slug": "later"}]})
            data.write_text(original)
            response = kenney.Response({"status": 200, "body": '<a href="/assets/keep">Keep</a>'})
            with patch.object(kenney, "DATA", str(data)), patch.object(kenney, "polite_get", side_effect=[response, FetchError("fixture", 503)]):
                rc, _ = self.capture(kenney.refresh)
            self.assertNotEqual(rc, 0)
            self.assertEqual(data.read_text(), original)

    def test_complete_kenney_refresh_preserves_metadata(self):
        with tempfile.TemporaryDirectory() as td:
            data = Path(td) / "packs.json"
            original = {"slug": "keep", "name": "Custom", "type": "ui", "thumb": "image"}
            data.write_text(json.dumps({"packs": [original]}))
            response = kenney.Response({"status": 200, "body": '<a href="/assets/keep">Keep</a>'})
            with patch.object(kenney, "DATA", str(data)), patch.object(kenney, "polite_get", side_effect=[response, FetchError("fixture", 404)]):
                rc, _ = self.capture(kenney.refresh)
            self.assertEqual(rc, 0)
            self.assertEqual(json.loads(data.read_text())["packs"], [original])

    def test_truncated_github_tree_reports_failure(self):
        with patch.object(gameicons, "get_text", return_value=json.dumps({"tree": [{"path": "lorc/test.svg", "type": "blob"}], "truncated": True})):
            rc, rows = self.capture(gameicons.main)
        self.assertNotEqual(rc, 0)
        self.assertEqual(len(rows), 1)


if __name__ == "__main__":
    unittest.main()
