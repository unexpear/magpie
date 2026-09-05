# Prior art

Checked Aug 2026. **No true cross-site game asset meta-search exists.**

## The model to copy

- **Yeggi** — meta-search for 3D printables since 2013. Indexes Thingiverse, Printables,
  Cults3D, MyMiniFactory, etc. Hosts nothing, links out. Proves the model is legally and
  economically survivable.
- **STLFinder**, **Thangs** — same idea, Thangs adds geometric/shape similarity search.

## Big silos (each is a source, not a competitor to the idea)

| Site | Covers | Notes |
|---|---|---|
| itch.io | 2D, 3D, audio, tools | Huge, tag-driven, wildly variable licences |
| OpenGameArt | everything, all free | Old Drupal, great licence metadata, poor search |
| Sketchfab | 3D | Best 3D viewer, real API, store moved to Fab |
| Fab (Epic) | 3D, VFX, blueprints | Merged Unreal Marketplace + Sketchfab store + Quixel |
| Unity Asset Store | Unity-first | Walled, restrictive ToS |
| Kenney.nl | 2D + 3D, all CC0 | ~40k assets, one person, gold standard |
| Poly Haven | HDRI, texture, model | CC0, clean public API |
| ambientCG | PBR materials | CC0, clean public API |
| Freesound | audio | CC, real API |
| Poly Pizza | low-poly 3D | Google Poly refugee, has an API |
| CGTrader / TurboSquid / Free3D | 3D | Commercial, hostile ToS |
| Quaternius, KayKit | low-poly packs | Small, CC0, easy |

## Adjacent but not it

- **BlenderKit** — in-app asset browser, but only its own library.
- **Meshy / AI generators** — making assets, not finding them.
- **awesome-gamedev GitHub lists** — static, stale, no search.
- **assethoard.com** and similar — blog listicles, SEO farms.

## Read

Nobody has built the aggregator. The pieces (public APIs on the CC0 sites) are sitting
there unused. That's the opening.
