# Improvements without paid services — 2026-09-05

Reviewed the browser search and credits flow, C export/storage/licence logic, runner, existing tests, project documentation and current index. This pass changes no application code or production data. Recommendations below require development time; none require a paid API or backend.

Implementation status: the seven requested improvements are recorded in [PRODUCT_IMPROVEMENTS.md](crawl/PRODUCT_IMPROVEMENTS.md). The findings below describe the pre-change state.

## Prioritized findings and plan

1. **Make credits durable and show metadata changes.** `crawl/web/index.html:1025` saves whole asset objects and restores those snapshots without comparing them with the newly loaded index. A creator added by enrichment therefore remains missing in an existing basket. Export stable asset IDs, keep the originally saved record, compare against current metadata and flag differences. Offer project JSON import/export alongside CREDITS.md so browser storage is not the only editable copy. Do not silently replace the originally recorded licence: users may need its history.

2. **Make exports atomic and reject failed exports.** `crawl/src/main.c:849` truncates the public data file before generating its replacement. Write and validate a unique temporary file in the destination directory, check database iteration and file-write/close errors, then replace the destination. Read source statistics and assets from one database snapshot. An interrupted export should leave the last usable website intact. Add interrupted/write-failure and malformed-data regression checks.

3. **Complete existing metadata before expanding coverage.** Read-only database counts: 19,359 assets; 2,423 attribution-required records missing authors, including 2,421 OGA records; 8,948 OGA records without source tags. The runner currently prioritizes missing required authors, then link checks; it has no general tag-enrichment or scheduled recrawl/export job. Add a lower-priority tag-enrichment queue, bounded periodic source refreshes and export after validated updates. Keep current shared limits and cancellation. Failures should defer the affected source while allowing healthy sources to progress. This is more immediately useful than another source adapter.

4. **Separate commercial permission from licence obligations.** `crawl/src/asset.c:208` puts BY-SA in the commercial bucket while conservatively excluding GPL; the browser reduces the positive state to “commercial, credit required.” This is a product-policy shortcut, not a complete licence model. Preserve licence version, source evidence and separate flags for attribution, ShareAlike/copyleft and unknown terms. Show obligations explicitly and describe conservative filters accurately. BY-SA allows commercial use but has ShareAlike requirements for shared adaptations; GNU's FAQ also permits selling GPL software. Avoid inferring how a particular asset licence applies to an entire game. See [Creative Commons BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) and [GNU FAQ](https://www.gnu.org/licenses/gpl-faq.html#DoesTheGPLAllowMoney).

5. **Optimize browser search locally.** `crawl/web/index.html:574` scans the full catalogue for results and again for each facet. Its sort comparator recalculates relevance repeatedly. Compute scores once per matching record, reuse query matches across facets, and measure real browser typing latency. Move search into a Web Worker if that latency warrants it; workers are a built-in browser capability, with no service fee. Current substring matching also makes very short queries noisy: `a` matches 19,353 of 19,359 assets. Introduce a small fixed set of relevance examples before changing token matching. See [MDN Web Workers](https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Using_web_workers).

6. **Expose real freshness and source health.** The current browser export was generated on August 13; each source's newest `last_seen` is August 10. Exporting today would change the “indexed” date without actually refreshing any source. Export per-source last successful crawl, completeness, last seen and check times; display these separately from export generation time. Track failures and unexpected count/author/tag coverage changes. Do not remove unseen assets after a partial crawl. This requires local records and a static status display, not hosted monitoring.

7. **Add browser and publication regression coverage.** The scraping suite now covers 34 scenarios plus C assertions, but the browser's basket, search, facets, dialogs and credits generation need their own checks. Start with basket reload after enrichment, backup round-trip, licence obligations, malformed exports and keyboard operation with a dialog open. Add small sanitized HTML fixtures for each adapter so parser changes are reviewable. Local test execution is free. If the project becomes a public GitHub repository, standard hosted Actions runners can run CI without usage charges under current terms; [GitHub Actions billing](https://docs.github.com/en/actions/concepts/billing-and-usage).

8. **Update the project documentation.** `architecture.md` still describes a 5,252-item index, no web UI and an HTTP-vfs plan; README says the background runner is design-only. These disagree with the implementation. Keep a single current setup/run guide, distinguish implemented behavior from proposals, and replace old counts with a dated generated health summary. No runtime risk and little maintenance cost.

## Free hosting and size

`crawl/web/data.json` is 7,532,686 bytes; a local gzip measurement produces 772,278 bytes (about 90% smaller). This is not a measurement of deployed transfer size: verify the actual host's Content-Encoding and caching headers. At this scale the existing static architecture is appropriate; first improve compression, caching and local search rather than introduce a search service or database server.

[GitHub Pages](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits) is available for public repositories on GitHub Free, subject to usage limits. Keep the slow crawler on the existing PC initially and publish validated static output separately. Do not move the request ledger into disposable CI workspaces: daily limits, robots caches and host cooldowns must survive between runs. No repository, hosting account or deployment was created in this review.

## Verification and limits

- Read the actual implementation and queried `index.sqlite` in read-only mode; did not run a crawl or migrate the database.
- Measured file size and gzip size locally.
- Exercised the existing filtering/scoring functions in a Node VM against the current export. Median times for filtering, sorting and five catalogue scans were approximately 73 ms (empty), 191 ms (`a`), 104 ms (`dungeon`) and 108 ms (`stone wall`). These include VM overhead and exclude DOM/facet rendering, so they are diagnostic evidence, not browser performance benchmarks.
- Confirmed current free hosting/CI terms and browser capabilities in official documentation. No paid service is needed for the proposed changes.
- This was a targeted whole-project improvement review, not an exhaustive security audit or visual/accessibility browser test.

Recommended order: atomic exports and credit preservation; clearer licence obligations and metadata completion; freshness reporting; measured search optimization; browser tests and current documentation alongside those changes.
