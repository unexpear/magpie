#!/usr/bin/env python3
"""Magpie background runner.

Runs the slow, resumable magpie jobs a little at a time, unattended.

Design in ../../background-runner.md. The short version: the scarce resource
is not CPU or bandwidth, it is other people's goodwill, so this is built to be
boring. It makes **no network requests of its own** — it only decides which
bounded magpie command to run next, and stops.

Every failure path ends in "do nothing", never "retry harder". Nobody is
watching an unattended job, so the safe direction is always to stop.

    python runner.py            one tick; what Task Scheduler calls
    python runner.py --status   print state and exit, touching nothing
    python runner.py --dry-run  decide and report, but run no command
"""

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import sqlite3
from contextlib import closing
import time
from pathlib import Path

HERE   = os.path.dirname(os.path.abspath(__file__))
CRAWL  = os.path.dirname(HERE)                     # ...\magpie\crawl
MAGPIE = os.path.join(CRAWL, "magpie.exe")
STATE  = os.path.join(HERE, "state.json")
STATUS = os.path.join(HERE, "status.json")
LOG    = os.path.join(HERE, "runner.log")
STOP   = os.path.join(HERE, "STOP")

# --- hard limits (background-runner.md) -----------------------------------
# Deliberately below what any source enforces. If our accounting is ever
# wrong, we want to be wrong on the quiet side.
DAILY_BUDGET   = 500
WEEKLY_BUDGET  = 3000
BATCH          = 120       # requests per tick; a tick should be minutes, not hours
QUIET_HOURS    = (9, 18)   # local time to stay off; their peak, roughly
MAX_RUN_SECS   = 60 * 90


def now():
    return dt.datetime.now()


def log(msg):
    line = f"{now():%Y-%m-%d %H:%M:%S}  {msg}"
    print(line, flush=True)
    try:
        with open(LOG, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError:
        pass


def load(path, default):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return default


def save(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2)
    os.replace(tmp, path)          # atomic; a half-written state file is worse
                                   # than a stale one


def default_state():
    return {"day": "", "day_used": 0, "week": "", "week_used": 0,
            "paused": {}, "last_error": None, "last_run": None}


def roll_periods(st):
    today = f"{now():%Y-%m-%d}"
    week  = f"{now():%G-W%V}"
    if st.get("day") != today:
        st["day"], st["day_used"] = today, 0
    if st.get("week") != week:
        st["week"], st["week_used"] = week, 0
    return st


def magpie(args, timeout=MAX_RUN_SECS):
    """Run one bounded magpie command. Never raises."""
    cmd = [MAGPIE] + args
    log("run: " + " ".join(args))
    try:
        env = os.environ.copy()
        env["MAGPIE_STOP_FILE"] = STOP
        env["MAGPIE_DEADLINE"] = str(int(time.time() + timeout))
        env["MAGPIE_WEEKLY_CAP"] = str(WEEKLY_BUDGET)
        p = subprocess.run(cmd, cwd=CRAWL, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env, timeout=timeout + 45)
    except subprocess.TimeoutExpired:
        return None, "timed out"
    except OSError as e:
        return None, f"could not start magpie: {e}"
    out = (p.stdout or "") + (p.stderr or "")
    if p.returncode != 0:
        return out, f"exit {p.returncode}"
    return out, None


def stats():
    """Read actionable queues and actual request charges; never create an index."""
    try:
        path = Path(CRAWL, "index.sqlite").resolve()
        with closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=5)) as db:
            cols = {row[1] for row in db.execute("PRAGMA table_info(assets)")}
            retry = "COALESCE(enrich_retry_at,0)" if "enrich_retry_at" in cols else "0"
            pending = dict(db.execute(f"""
                SELECT source, COUNT(*) FROM assets
                WHERE COALESCE(enriched,0)=0 AND {retry}<=?
                  AND attribution=1 AND COALESCE(author,'')=''
                  AND source IN ('opengameart','kenney') GROUP BY source
                """, (int(time.time()),)))
            tags = dict(db.execute(f"""SELECT source,COUNT(*) FROM assets
                WHERE COALESCE(enriched,0)=0 AND {retry}<=? AND COALESCE(tags,'')=''
                AND NOT (attribution=1 AND COALESCE(author,'')='')
                AND source IN ('opengameart','kenney') GROUP BY source""", (int(time.time()),)))
            tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            health = {r[0]: r[1:] for r in db.execute("SELECT source,last_attempt,last_success,state FROM source_health")} if 'source_health' in tables else {}
            refresh = [source for source in ('polyhaven','ambientcg','gameicons')
                       if time.time()-health.get(source,(0,0,0))[1]>=7*86400
                       and time.time()-health.get(source,(0,0,0))[0]>=86400]
            due = db.execute("""SELECT COUNT(*) FROM assets
                WHERE source <> 'polyhaven' AND (last_checked IS NULL OR last_checked < ? - CASE
                WHEN http_status IN (404,410) THEN 604800
                WHEN http_status BETWEEN 200 AND 299 THEN 2592000 ELSE 86400 END)
                """, (int(time.time()),)).fetchone()[0]
            today = now().date()
            monday = today - dt.timedelta(days=today.weekday())
            day_used = db.execute("SELECT COALESCE(SUM(used),0) FROM request_budget WHERE day=?",
                                  (today.isoformat(),)).fetchone()[0]
            week_used = db.execute("SELECT COALESCE(SUM(used),0) FROM request_budget WHERE day>=?",
                                   (monday.isoformat(),)).fetchone()[0]
            return {
                "assets": db.execute("SELECT COUNT(*) FROM assets").fetchone()[0],
                "uncreditable": db.execute("SELECT COUNT(*) FROM assets WHERE attribution=1 AND COALESCE(author,'')=''").fetchone()[0],
                "unchecked": due, "enrich_pending": pending, "tag_pending": tags, "refresh_due": refresh,
                "day_used": day_used, "week_used": week_used,
            }, None
    except (OSError, sqlite3.Error) as exc:
        return None, str(exc)


def pick_job(s, paused, remaining):
    """What to do next, in priority order.

    Retiring the uncreditable backlog comes first: those assets carry a licence
    the index cannot currently honour, which is the one gap that makes a result
    wrong rather than merely thin. It is also finite — it ends.
    """
    batch = min(BATCH, remaining)
    if batch < 1:
        return None, "budget spent"

    candidates = []
    for key, label in (("enrich_pending", "need a creator"), ("tag_pending", "need source tags")):
        for source, count in sorted(s.get(key, {}).items()):
            if count > 0 and not paused.get(source):
                candidates.append((["enrich", source, "--limit", str(min(batch,count)),
                                    "--max-requests", str(batch)]+(["--missing-tags"] if key=="tag_pending" else []), f"{count} {source} assets {label}"))
                break
    if s.get("unchecked", 0)>0 and not paused.get("check"):
        candidates.append((["check", "--limit", str(batch), "--max-requests", str(batch)],
                           f"{s['unchecked']} urls due for link checking"))
    for source in s.get("refresh_due", []):
        if not paused.get(source):
            candidates.append((["crawl", source, "--max-requests", str(batch)], f"weekly {source} catalogue refresh"))
            break
    if candidates:
        return candidates[s.get("work_turn",0) % len(candidates)]

    return None, "nothing to do"


def write_status(st, s, job, reason, note=None):
    save(STATUS, {
        "generated": int(dt.datetime.now().timestamp()),
        "stopped": os.path.exists(STOP),
        "day_used": st["day_used"], "day_budget": DAILY_BUDGET,
        "week_used": st["week_used"], "week_budget": WEEKLY_BUDGET,
        "uncreditable": s.get("uncreditable") if s else None,
        "unchecked": s.get("unchecked") if s else None,
        "assets": s.get("assets") if s else None,
        "next": " ".join(job) if job else None,
        "reason": reason,
        "note": note,
        "last_error": st.get("last_error"),
        "last_run": st.get("last_run"),
        "quiet_hours": list(QUIET_HOURS),
        "paused": st.get("paused", {}),
    })


def blockers(st):
    """Reasons not to run, cheapest to establish first."""
    out = []
    if os.path.exists(STOP):
        out.append("stopped by STOP file")
    if not os.path.exists(MAGPIE):
        out.append("magpie.exe not found")
    hour = now().hour
    if QUIET_HOURS[0] <= hour < QUIET_HOURS[1]:
        out.append(f"quiet hours ({QUIET_HOURS[0]}:00-{QUIET_HOURS[1]}:00)")
    if min(DAILY_BUDGET - st["day_used"], WEEKLY_BUDGET - st["week_used"]) < 1:
        out.append("budget spent for this period")
    return out


def tick(dry_run=False):
    st = roll_periods(load(STATE, default_state()))
    s, err = stats()
    if err:
        st["last_error"] = f"index read failed: {err}"
        log(st["last_error"])
        write_status(st, None, None, "could not read the index")
        return 1
    st["day_used"], st["week_used"] = s["day_used"], s["week_used"]
    stop = blockers(st)

    # A dry run makes no requests, so the gates should *inform* it rather than
    # end it. Short-circuiting here meant the runner could not be inspected
    # during working hours - precisely when someone is sitting at the keyboard
    # wanting to know what it would do tonight.
    if stop and not dry_run:
        log(" / ".join(stop) + " - doing nothing")
        write_status(st, None, None, " / ".join(stop))
        return 1 if "magpie.exe not found" in stop else 0

    if "magpie.exe not found" in stop:
        log("magpie.exe not found - cannot even report")
        write_status(st, None, None, "magpie.exe missing")
        return 1

    remaining = max(0, min(DAILY_BUDGET - st["day_used"],
                           WEEKLY_BUDGET - st["week_used"]))
    if dry_run and remaining < 1:
        remaining = BATCH          # report what a fresh period would pick up

    s["work_turn"] = st.get("work_turn",0)
    paused = {key: value for key,value in st.get("paused",{}).items()
              if value is True or (isinstance(value,(int,float)) and value>time.time())}
    st["paused"] = paused
    job, reason = pick_job(s, paused, remaining)
    if not job:
        log(reason)
        write_status(st, s, None, reason)
        return 0

    if dry_run:
        log(f"[dry run] would run: {' '.join(job)}")
        log(f"[dry run] because:  {reason}")
        if stop:
            log("[dry run] but right now it would NOT run: " + " / ".join(stop))
        write_status(st, s, job,
                     reason + (" (blocked: " + " / ".join(stop) + ")" if stop else ""))
        return 0

    write_status(st, s, job, reason, note="running")

    out, err = magpie(job)
    st["work_turn"] = st.get("work_turn",0)+1
    job_key = job[1] if job[0] in ("crawl","enrich") else "check"
    if err:
        st["paused"][job_key] = int(time.time())+86400
    # Publish valid changes, including partial progress; native export validates
    # a consistent snapshot and preserves the old file on failure.
    _, export_error = magpie(["export"])
    if export_error:
        err = (err+"; " if err else "")+"export "+export_error
    latest, read_error = stats()
    if latest:
        st["day_used"], st["week_used"] = latest["day_used"], latest["week_used"]
    elif not err:
        err = f"could not verify request charges: {read_error}"
    st["last_run"]   = f"{now():%Y-%m-%d %H:%M}"
    st["last_error"] = err

    if err:
        # One failure is not a reason to try again harder. Task Scheduler will
        # call us again on the normal cadence; if it is broken it stays broken
        # and the status page says so.
        log(f"job failed ({err}) - not retrying this tick")
        for line in (out or "").splitlines()[-6:]:
            log("  "+line)
    else:
        tail = [l for l in (out or "").splitlines() if l.strip()][-2:]
        log("ok: " + " | ".join(t.strip() for t in tail))

    save(STATE, st)

    s2, _ = stats()
    write_status(st, s2 or s, None, reason, note="idle")
    return 0 if not err else 1


def show_status():
    s = load(STATUS, None)
    if not s:
        print("no status yet - run: python runner.py --dry-run")
        return 0
    print(f"magpie background runner")
    print(f"  stopped        {s['stopped']}")
    print(f"  today          {s['day_used']} / {s['day_budget']} requests")
    print(f"  this week      {s['week_used']} / {s['week_budget']}")
    print(f"  uncreditable   {s.get('uncreditable')}")
    print(f"  unchecked urls {s.get('unchecked')}")
    print(f"  next           {s.get('next') or '-'}   ({s.get('reason')})")
    print(f"  last run       {s.get('last_run')}")
    print(f"  last error     {s.get('last_error')}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--status", action="store_true", help="print state, change nothing")
    ap.add_argument("--dry-run", action="store_true", help="decide but run nothing")
    a = ap.parse_args()

    if a.status:
        return show_status()
    return tick(dry_run=a.dry_run)


if __name__ == "__main__":
    sys.exit(main())
