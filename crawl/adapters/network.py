"""Adapter transport. The C fetcher owns network policy, cache and budgets.

Standalone maintenance commands share one logical run too. No urllib fallback:
if the core is missing or refuses a request, stop rather than bypass its limits.
"""
import json
import os
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parent.parent


class FetchError(RuntimeError):
    def __init__(self, url, status=0, detail="request refused"):
        self.code = status
        super().__init__(f"{url}: HTTP {status or 'unknown'} ({detail})")


def fetch(url, method="GET"):
    os.environ.setdefault("MAGPIE_RUN_ID", uuid.uuid4().hex)
    env = os.environ.copy()
    env.setdefault("MAGPIE_DB", str(ROOT / "index.sqlite"))
    env.setdefault("MAGPIE_STOP_FILE", str(ROOT / "runner" / "STOP"))
    exe = env.get("MAGPIE_EXE", str(ROOT / ("magpie.exe" if os.name == "nt" else "magpie")))
    try:
        result = subprocess.run(
            [exe, "fetch", "--url", url, "--method", method],
            stdout=subprocess.PIPE, encoding="utf-8", env=env,
        )
        doc = json.loads(result.stdout)
    except (OSError, ValueError) as exc:
        raise FetchError(url, detail=str(exc)) from exc
    if result.returncode or not doc.get("ok"):
        raise FetchError(url, doc.get("status", 0))
    return doc


def get_text(url):
    return fetch(url)["body"]
