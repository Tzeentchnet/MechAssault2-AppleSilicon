# Documentation

Developer documentation for the MechAssault 2 PC recompilation. The repository contains no game data; you need your own
copy of the retail disc to build or run anything.

| Document | What it is |
|---|---|
| [architecture.md](architecture.md) | How it works: XBE to generated C, runtime (kernel, NV2A to D3D11, APU/DSP audio, input), PC layer, hooks and patch set, FastCode, what the installer does on the user's PC. |
| [building.md](building.md) | Build the release installer from this repository, and build / iterate on the game from your own disc. |
| [patches.md](patches.md) | The patch set applied to generated code, and how it is stored and re-applied. |
| [bots.md](bots.md) | The offline bot system: brain, controller, navigation and NavCache, VTOL and BattleArmor, skills, recorder and environment switches. |
| [editor.md](editor.md) | The scenario editor and custom scenarios: file formats, how the game loads them, automation pipe. |
| [fact-index.md](fact-index.md) | How to search the fact wiki, address conventions, list of topics. |
| [apple-silicon/](apple-silicon/README.md) | Apple Silicon port scope, milestones, format and tool inventories, data-type rules, and the port issue register. |
| [wiki/](wiki/README.md) | The fact wiki itself: 3,305 facts in 37 topics with the scripts to validate, merge and query them. |

Suggested reading order for a new contributor: architecture.md, then fact-index.md (so you can look things up), then the
document for the area you work on. Before changing code that touches generated files, read patches.md.

The wiki is the long-form record: findings are added there with an address, a confidence level and a status. Corrections
and additions are welcome; the procedure is in `wiki/WIKI_BRIEF.md`. Never add game data (models, textures, sounds,
levels, movies, dialogue, string dumps or disc file listings; the installer's hash manifest,
`kit/installer/data/disc-manifest.json`, is the one exception: names, sizes and SHA-256 only).

This is an unofficial fan project and is not affiliated with or endorsed by the publisher or the original developers.
