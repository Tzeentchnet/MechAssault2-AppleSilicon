# Game and project formats

This is an engineering inventory, not a specification derived from distributed
game data. It records only information already present in the repository or
established by local tests against a legally owned copy. Never add extracted
assets, complete disc file listings, game strings, or generated game code.

## Known formats

| Format | Purpose and verified properties | Existing implementation or reference | Port status |
|---|---|---|---|
| ISO/XISO | Input disc container. The development pipeline accepts a supported retail disc as an ISO/XISO or extracted directory. Container contents remain local. | [building.md](../building.md), [xiso tools](../../kit/toolkit/tools/xiso/) | Host-independent tooling must be exercised on arm64 macOS. |
| XBE | Original Xbox executable. The supported title uses 32-bit x86 code and linked guest virtual addresses. The parser recovers headers, sections, entry point, and kernel imports before recompilation. | [architecture.md](../architecture.md), [xbe_parser](../../kit/toolkit/tools/xbe_parser/) | Parsing is expected to be portable; generated-code and host-ABI validation are open. |
| MGF v4 | Resource archive. It has a fixed-width header, 32-byte file records, 24-byte directory rows, a string table, and optional independent zlib-compressed blocks. Paths are case-insensitive at lookup. | [mgf.h](../../editor/src/core/mgf.h), [archive.cpp](../../editor/models/src/archive.cpp), [inspect_mgf.py](../../kit/installer/inspect_mgf.py) | Reader/writer code is portable C++ in principle; add arm64 round-trip tests before relying on that assumption. |
| WDF | World data as a flat sequence of property-bag objects linked by 32-bit UID, parent, child, and sibling properties. Headers, class keys, property keys, types, and payload sizes are 32-bit fields. Unknown values are retained for lossless round trips. | [wdf.h](../../editor/src/core/wdf.h), [editor.md](../editor.md) | Existing lossless tests are editor-oriented and currently built through Windows scripts. Make the format tests host-buildable later. |
| MGIc picture (`.tif`) | Game texture container, distinct from a conventional TIFF workflow. The decoder covers DXT1/3/5, several packed RGB/alpha formats, swizzling, cube/volume textures, frames, and mip levels. Decoded images are RGBA8. | [ma2models.h](../../editor/models/ma2models.h), [mgic.h](../../editor/src/core/mgic.h) | CPU decoding should be separated from renderer upload and tested on arm64. |
| Model family | The model library reads `.MGMODEL` XML and legacy `.NODE`, `.mesh`, `.geomVert`, `.geomFace`, and `.mat` sets from MGF archives. Paths use case-insensitive archive semantics; game space is Y-up in metres. | [ma2models.h](../../editor/models/ma2models.h), [model sources](../../editor/models/src/) | CPU parsing is useful later for tools; the game renderer does not depend on porting the editor first. |
| MTB/template data | Model/template metadata selects nodes, damage states, effects, and related resources. Existing editor/model code consumes it, but this port log does not yet contain a complete binary or text layout. | [model_cache.cpp](../../editor/src/app/model_cache.cpp), [templates.cpp](../../editor/models/src/templates.cpp) | Characterize only when a gameplay or renderer issue requires it. |
| `.ma2map` | User-authored custom scenario. It is a plain MGF v4 archive containing `scenario.ini`, `world.wdf`, and authored scenario resources. It must not embed retail game files. | [editor.md](../editor.md), [scenario.h](../../editor/src/core/scenario.h) | Editor support is deferred; the game-side loader must continue to compile and preserve format compatibility. |
| XMV/Bink and enhanced video | Retail and converted movie paths are handled by game/runtime video code. The shared backend uses Media Foundation on Windows, and the game has an FFmpeg-based native Bink path. The Apple guest currently stubs that native contract, so lifted retail decode supplies a linear 720x480 X8R8G8B8 movie surface. A traced corrupted frame is only a fullscreen two-triangle composite, proving the observed blocks precede NV2A rasterization. | [video source](../../kit/source/runtime/src/video/), [bink_native.c](../../kit/source/game/src/bink_native.c), [optional_contracts.c](../../kit/source/game/src/apple/guest/optional_contracts.c) | Safe-start skips required movies for the gameplay milestone. Native decoder choice, CPU-buffer transfer/upload, audio sync, and licensing still need a macOS design and synthetic test. |
| Xbox audio resources | The tree recognizes Xbox audio-bank/resource extensions and emulates the APU/DSP at runtime, but this document does not yet claim complete XWB/XSB/XGS layouts. | [audio runtime](../../kit/source/runtime/src/audio/), [APU runtime](../../kit/source/runtime/src/apu/) | Record layouts only as needed for a failing runtime path; output-backend work is tracked separately. |

The upstream installer currently targets the US/NTSC retail executable. A
locally named "World", PAL, Japanese, bonus, or compilation image must not be
assumed compatible from its archive name. Compatibility is established only by
the repository's existing local validation pipeline. Do not add new disc hashes
or manifests without first confirming that they fit upstream policy.

## Serialized data rules

- Treat all documented on-disc integer widths as fixed. Never deserialize into
  host `long`, pointer-sized integers, or native C++ structs whose padding is
  compiler-dependent.
- The original Xbox title is little-endian. Decode multi-byte values
  explicitly when a parser is shared with a potentially different host.
- Preserve unknown fields and payloads in round-trip tools. Naming an unknown
  value is not evidence that it can be rewritten safely.
- Keep archive paths separate from host filesystem paths. Archive lookup accepts
  Xbox-style separators and case-insensitive spelling; the macOS filesystem
  must not silently define game semantics.
- Keep decompression, parsing, and GPU upload as separate stages so CPU-only
  format tests do not require a window or graphics device.

## Format record template

Add a section when new work depends on a format:

```text
### Name and extension

- Role:
- Evidence status: Confirmed | Observed | Hypothesis
- Producer and consumer:
- Byte order and field widths:
- Header or framing:
- Compression:
- Path and string encoding:
- Existing parser/writer:
- Round-trip or fixture test:
- Unknown fields:
- Port issue IDs:
```

Fixtures must be synthetic, freely redistributable, or generated during a local
test and removed afterward. A test must not commit derived game bytes merely
because they are small.
