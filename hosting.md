# Hosting Magpie

Magpie is static: there is no search backend, database server or browser API key. Publish only the staged output, `crawl/build/site/`.

## Build and preview

```powershell
cd crawl
npm ci
npm run build:site
$env:MAGPIE_TEST_SITE = 'build/site'
npm test
python -m http.server 8231 --bind 127.0.0.1 --directory build/site
```

The builder reads `web/data.json`, applies the preview policy and `exclusions.json`, and copies the public HTML and model files. Invalid exclusions fail the build. On Windows the browser tests use installed Edge by default; Linux CI installs Chromium. Open `http://127.0.0.1:8231` for a local preview.

## Publication

The public repository is [unexpear/magpie](https://github.com/unexpear/magpie), and the site is [Magpie on GitHub Pages](https://unexpear.github.io/magpie/). The Pages workflow builds the staged site and runs browser tests against it before deploying on pushes to `main`. GitHub Pages is subject to [documented limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits).

Publish the staged files together. Do not upload `web/` directly: that would bypass persistent publication exclusions and preview stripping. Build or test failure prevents deployment. Images permitted by the source policy are loaded lazily from external hosts; thumbnail mirroring is not implemented.

For rights-holder requests, see [source policy and exclusion instructions](sources.md). Exclusions affect the deployed catalogue, not the raw source export or repository history.

## Keep crawling separate

The local runner requires persistent SQLite state for request caps, host cooldowns, robots caches and enrichment progress. Do not replace that state with a fresh database for each CI run. The publishing workflow uses the existing export and does not crawl asset sites.

Keep the SQLite database, WAL files, executable, local logs and runner state out of the public repository. Project notes remain in browser storage and user-downloaded backups.
