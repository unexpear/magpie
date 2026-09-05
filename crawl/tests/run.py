"""Run C and Python regression checks without touching the real index.

Build magpie first; then run python -X utf8 tests/run.py from any directory.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="magpie-tests-") as directory:
        work = Path(directory)
        binary = work / ("core.exe" if os.name == "nt" else "core")
        sources = ["tests/test_core.c", "src/robots.c", "src/util.c", "src/http.c",
                   "src/store.c", "src/asset.c", "src/classify.c", "vendor/sqlite3.c"]
        libraries = ["-lwinhttp"] if os.name == "nt" else ["-lcurl", "-lm", "-ldl", "-lpthread"]
        subprocess.run([os.environ.get("CC", "gcc"), "-std=c99", "-O0",
                        "-D_DEFAULT_SOURCE", "-DSQLITE_ENABLE_FTS5", "-Isrc", "-Ivendor",
                        *sources, *libraries, "-o", str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary), str(work / "core.sqlite")], check=True)
        return subprocess.run([sys.executable, "-X", "utf8", "-B", "-m", "unittest",
                               "discover", "-s", "tests", "-v"], cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main())
