# Hosting Magpie

The implemented website is static: publish the contents of **`crawl/web/`**, including `index.html`, `model.js` and `data.json`. There is no search backend, database server or API key in the browser.

## Local preview

```powershell
python -m http.server 8231 --bind 127.0.0.1 --directory crawl/web
```

Open `http://127.0.0.1:8231`. Serving through HTTP allows the page to load its JSON catalogue.

## Publication

Use a static host such as GitHub Pages. On GitHub Free, Pages is available for public repositories, subject to [GitHub's documented limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits). The public repository is [unexpear/magpie](https://github.com/unexpear/magpie), and the site is [Magpie on GitHub Pages](https://unexpear.github.io/magpie/). The Pages workflow validates the catalogue and runs browser tests before publishing `crawl/web/` on pushes to `main`.

Publish all three site files together so the browser code and exported schema agree. Validate `data.json` before deployment, retain the last working deployment on failure, and inspect the deployed site's actual compression and caching headers.

The current export is about 8.83 MB uncompressed and 0.88 MB with local gzip. Actual transfer size depends on the host. Preview images still come from source sites and are loaded lazily; these figures exclude preview traffic. Thumbnail mirroring is not implemented.

## Keep crawling separate

The existing runner operates on the local PC. It requires persistent SQLite state for request caps, host cooldowns, robots caches and enrichment progress. Do not replace that state with a fresh database for each CI run.

A publishing workflow can validate and deploy the already exported static catalogue without contacting asset sites. Automatic metadata crawling in GitHub Actions is not part of the current setup.

Keep the SQLite database, WAL files, executable, local logs and runner state out of the public repository. The browser only needs the exported metadata.
