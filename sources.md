# Source access and publication policy

Reviewed September 5, 2026. Asset licences, API access, website access and preview rights are separate questions. Neither CC0 downloads nor robots.txt permission establish blanket permission to scrape or republish a website. These notes are an implementation policy, not legal clearance.

| Source | Collection | Public previews | Official basis |
|---|---|---|---|
| Poly Haven | Public API with an identifying Magpie User-Agent; website requests and website link checks blocked | API asset thumbnails under `/asset_img/thumbs/` only | [API terms](https://github.com/Poly-Haven/Public-API/blob/master/ToS.md), [website/asset terms](https://polyhaven.com/license). Give visible source credit; API attribution is separate from CC0. |
| ambientCG | Documented API, unfiltered paginated metadata | Material preview renders covered by CC0 | [API](https://docs.ambientcg.com/api/), [licence](https://docs.ambientcg.com/license/) |
| Game-icons.net | Official GitHub repository tree and licence record | Repository icons through jsDelivr; CC-BY previews require creator and licence link | [Licence record](https://github.com/game-icons/icons/blob/master/license.txt), [attribution guidance](https://game-icons.net/about.html). CC0 applies only to the named exceptions. |
| OpenGameArt | Existing metadata adapters obey robots, cooldowns and shared request budgets. No expansion of collection has been authorized by the source in this review | Withheld; link to the original page | [FAQ](https://opengameart.org/content/faq) distinguishes preview rights from download licences and identifies submission-specific attribution notices. Robots permission alone does not settle access or reuse rights. |
| Kenney | Existing curated pack list and metadata enrichment | Withheld pending specific preview permission | [Asset guidance](https://kenney.nl/support), [website terms](https://kenney.nl/terms-of-service). Asset-page downloads are CC0; do not assume website graphics or logos are. |

## Before adding or expanding a source

Record its official access terms, allowed endpoint, identification and rate requirements, metadata reuse basis, exact licence versions and preview rights. Prefer documented APIs or an explicit permission record. If access or reuse terms remain unclear, ask the source before expanding automated collection; do not circumvent restrictions or use an undocumented internal API as a substitute for permission. This review does not establish blanket permission for bulk HTML collection from OpenGameArt or Kenney.

Keep request budgets, robots enforcement, persistent cooldowns and a clear contact URL. Store metadata and source links, not downloadable asset files. Do not cache or mirror previews without a verified basis covering those images. No new source or broad live crawl was added in this review.

## Adapter behavior to preserve

- ambientCG: fetch unfiltered and classify each record's `dataType`. Previously tested unsupported `type` values were ignored by the server, causing redundant catalogue downloads. Verify actual filtering before adding parameters.
- Game-icons: derive creators from known repository contributors; leave unknown authors unresolved. Detect truncated tree responses. Read licence exceptions from the repository licence record rather than treating every icon as CC0.
- OpenGameArt: licence-filtered search listings do not contain complete attribution notices. Preserve the selected licence option without suggesting it is the only option. Missing creators and project-specific notices require source review.

## Publication and corrections

The deployed site is built with `npm run build:site` in `crawl/`. This applies the preview allowlist and `crawl/exclusions.json` before producing `crawl/build/site/`. Never publish the raw export directly as a replacement for this step.

For an upheld removal request, add its stable ID to `asset_ids` and its original source URL to `source_urls` in the versioned exclusions file, then build, test and deploy. Using both protects against a recrawl retaining either identity. Correct erroneous metadata at its origin and in future exports as appropriate. Site exclusions do not delete the source export or old Git commits; repository/history removal must be reviewed separately. Do not place private evidence in public issues or commits.

See the public [sources, rights and corrections page](https://unexpear.github.io/magpie/rights.html) and [hosting instructions](hosting.md).
