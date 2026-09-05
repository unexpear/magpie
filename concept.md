# Concept

**Yeggi, but for game assets.**

One search box. Query goes out across every game-asset site that exists. Results come
back as a unified grid: thumbnail, source site, licence, asset type. Click → go to the
original site to download.

Host nothing. Mirror nothing. Index metadata + thumbnail + link.

## Why it doesn't exist yet

3D printing converged on one file format (STL) and one use case. Game assets didn't:
2D sprites, 3D models, textures, audio, fonts, shaders, VFX — each with its own sites,
formats, and conventions. So the space stayed fragmented and nobody built the meta-layer.

Current state of the art is a blog post titled "15 best free game asset sites (2026)"
that is out of date the week it's published.

## The killer feature is licence, not search

Every gamedev's actual question is not "where is a barrel model." It's:

> "Show me a barrel I can legally ship in a commercial game without attribution."

No site answers that across sources. Licence is the primary facet, not an afterthought.

- CC0 → ship it, do nothing
- CC-BY → ship it, credit file
- CC-BY-SA / GPL → viral, usually disqualifying
- itch/Fab/Unity paid → per-store EULA, usually fine
- "free for personal use" → trap

Filter to "commercially safe, no attribution" and the result set is small and gold.

## Secondary facets Yeggi has no analogue for

- **Asset type** — 2D sprite / 3D model / texture-material / audio / music / font / shader / VFX
- **Style** — pixel, low-poly, PBR-realistic, hand-painted, flat/vector, voxel
- **Engine-ready** — Unity package, Unreal, Godot, raw files
- **Format** — .glb .fbx .blend .png .wav .ogg
- **Rigged / animated** — yes/no, huge filter for characters
- **Tileable** — for textures
- **Poly count** — order-of-magnitude bucket

## The job doesn't end at "found it"

Finding the asset is half the task. The other half is shipping legally: you collect
CC-BY assets over weeks and then owe an accurate credits file, and nobody helps with
that. People reconstruct it from browser history, or skip it.

So Magpie carries a **credits basket** — add assets as you find them, and it emits a
ready-to-paste CREDITS.md split into *required attribution* / *public domain* /
*check before shipping*. Built and working ([crawl/](crawl/)).

This is the same bet as the licence filter, one step further down the workflow, and it
is where the "unknown" values earn their keep: an asset needing credit with no recorded
author is marked **AUTHOR UNKNOWN** in the output rather than silently producing a
credit line that does not satisfy the licence.

## Non-goals

- Not a store. No payments, no hosting, no CDN of assets.
- Not an AI generator. Indexing what humans made.
- Not a launcher/importer plugin (maybe later — see [holes.md](holes.md)).

See [prior-art.md](prior-art.md) for what exists, [sources.md](sources.md) for the index
targets, [architecture.md](architecture.md) for how to build it.
