# Sources

Ranked by how easy they are to index legally. Build outward from the top.

## Tier 1 — public API, CC0, no ToS fight

Start here. All-CC0 means the licence facet is trivially correct, and CC0 sites *want*
traffic.

| Source | Content | Endpoint | Auth | Verified notes |
|---|---|---|---|---|
| Poly Haven | HDRI, PBR texture, models | `api.polyhaven.com/assets` | none | OpenAPI 3.0 documented. **Requires a unique `User-Agent`.** Free for commercial use, but must credit "Powered by Poly Haven" in the UI. **Measured: 2,296 assets in 1 request.** Sends ETag, so re-crawls 304 |
| ambientCG | PBR materials, HDRI, decals | `/api/v2/full_json` | none | Also exposes `/rss` and `/downloads_csv`. **Measured: 2,877 assets in 3 requests** at 1000/page. Sends no ETag. **Never pass `?type=`** — see the gotcha below |
| Kenney.nl | 2D + 3D packs | none | — | Static pages. ~40k assets in ~80 packs, all CC0. Hand-list beats scraping here (decisions.md D3) |
| Quaternius | low-poly packs | none | — | Small, scrape, CC0. Not yet indexed |
| Freesound | SFX | `freesound.org/apiv2` | free key | **Only source with published limits: 60/min, 2000/day per key.** 429 on breach. Higher limits available on request to the maintainers. Not yet indexed |

### Gotcha: ambientCG's `type=` parameter

Passing `?type=3DModel` or `?type=PlainTexture` does **not** filter — the API ignores
unrecognised values and returns the entire catalogue. Enumerating seven types fetched
everything three times over (13 requests, 8,370 rows for 2,877 assets).

Fetch unfiltered and read `dataType` off each asset. Observed distribution:

```
Material 2007   HDRI 418   Substance 209   Decal 126   Atlas 60
3DModel    34   PlainTexture 9   Terrain 5   Brush 5   HDRIElement 4
```

Generalizes: **verify that a filter parameter actually filters.** A silently-ignored
filter looks like a working crawl while costing the source many times what it should.

A v1 that only indexes Tier 1 is already useful, is 100% commercially-safe results, and
costs the sources involved **about 10 HTTP requests per week** total. See
[crawler.md](crawler.md).

## Tier 2 — real API, mixed licences

| Source | Content | Access |
|---|---|---|
| Sketchfab | 3D models | Data API v3, documented + Swagger. Filter `downloadable=true` + licence. No published rate limit but 429s exist. 24/page → a full sweep is ~5k requests, spread it over days |
| Poly Pizza | low-poly 3D | API exists — *verify current key policy* |
| **OpenGameArt** | everything, mixed licences | **Indexed.** Drupal 7, no API, scraped. See the recipe below |
| **game-icons.net** | 4,239 game/UI icons | **Indexed.** The best-shaped source we have — see below |
| BlenderKit | Blender assets | API exists — *verify terms for third-party indexing* |
| GitHub | repos tagged `cc0` / `game-assets` | Search API, 5000/hr authenticated |

### game-icons.net recipe (verified Aug 2026)

Index it from its GitHub repo (`game-icons/icons`), not the website — which, incidentally,
did not resolve from this machine at all.

**One request** returns the entire catalogue:
`GET api.github.com/repos/game-icons/icons/git/trees/master?recursive=1` → 4,239 SVG
paths, not truncated. Compare with OpenGameArt's ten seconds per 144 assets.

Two properties make the data unusually clean:

- **Author is in the path.** Files sit at `<contributor>/<icon>.svg`, so every asset has a
  creator for free — exactly what OpenGameArt rows lack and what a CC-BY credits line
  needs. One folder, `various-artists`, has no single creator; those are emitted with **no
  author** so they get flagged rather than credited to a fiction.
- **Licence is knowable per asset.** `license.txt` puts everything under CC-BY 3.0 *except*
  two contributors marked CC0 (Viscious Speed, Zeromancer) — 123 of the 4,180 indexed.
  Transcribed into the adapter, not inferred.

Thumbnails come from **jsDelivr**, not `raw.githubusercontent`. A single raw URL loads
fine, but a grid requests ~50 at once and GitHub throttles that — every icon rendered
blank with no error event to notice it by. GitHub also asks people not to use raw as a
CDN.

### OpenGameArt recipe (verified Aug 2026)

`robots.txt` says **`Crawl-delay: 10`** — ten seconds between requests, honoured by the
adapter at runtime rather than hardcoded. `/art-search-advanced` and `/content/` are not
disallowed.

Endpoint: `GET /art-search-advanced` with query params

| Param | Notes |
|---|---|
| `field_art_licenses_tid[]` | note the plural *licenses*. `4`=CC0, `2`=CC-BY 3.0, `17981`=CC-BY 4.0, `3`=CC-BY-SA 3.0, `17982`=CC-BY-SA 4.0, `5`=GPL 2.0, `6`=GPL 3.0, `10310`=OGA-BY 3.0, `31772`=OGA-BY 4.0 |
| `field_art_type_tid[]` | `9`=2D Art, `10`=3D Art, `14`=Texture, `12`=Music, `13`=Sound Effect, `7273`=Concept Art, `11`=Document |
| `items_per_page` | **use 144**, the maximum. Default is 24 — a 6× difference in page count, which at 10s/request is the difference between 19 minutes and 1.9 hours |
| `page` | zero-based |

Result rows: `<span class="art-preview-title"><a href="/content/SLUG">TITLE</a></span>`
plus a sibling `<a href="/content/SLUG"><img src='THUMB'>`. **Licence and author are not
in the listing** — licence comes from the filter (decisions.md D6), author is left null
rather than guessed, since getting it means a node fetch per asset.

**Note:** apart from Freesound, none of these publish a hard rate limit. That is not
permission — it means the ceiling is your own judgement.

## Tier 3 — scrape, ToS grey, do last

| Source | Content | Risk |
|---|---|---|
| itch.io | huge, all types | No public search API. Tag pages scrapable. High value, so worth asking them directly. |
| Fab (Epic) | 3D, VFX | Internal API only, ToS restrictive |
| Unity Asset Store | Unity packages | Walled, restrictive |
| CGTrader / TurboSquid / Free3D | 3D | Hostile ToS, mostly paid, low value for the CC0 pitch |

## Tier 4 — long tail

- GitHub repos tagged `game-assets` / `cc0` — searchable via GitHub API, surprisingly rich
- Individual artist sites (KayKit, Penzilla, etc.) — hand-add, low volume high quality
- Public domain museum scans (Smithsonian 3D, Met) — CC0 3D, unusual angle

## Rules for every crawler

- Respect `robots.txt`. Rate limit hard. Identify with a real User-Agent + contact URL.
- Store **metadata only**: title, author, source URL, licence, tags, format, thumbnail URL.
- Never mirror the asset file. Always deep-link to the source page, never the direct download.
- Prefer official APIs; email the site before scraping anything big.
- Cache thumbnails (don't hotlink and burn their bandwidth) but keep them small and
  attribute-linked. *Grey area — see [holes.md](holes.md).*
