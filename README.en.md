# ovltc

A native **Total Commander packer plugin (WCX, 64-bit)** for `.ovl` files in the
**FRES format used by Frontier Developments' Cobra Engine** — including
*Planet Coaster*, *Planet Zoo*, and *Jurassic World Evolution 1/2*.

It lets you open `.ovl` files in Total Commander like any other archive
(Enter/double-click), browse their contents, and extract individual or all
contained resources — no Python or other runtime required.

- [Deutsche Version](README.de.md)

---

## Features

- **Full FRES header parsing** (versions 17–20+): file, mime, and archive
  tables, including version-dependent struct layouts
- **Decompression**:
  - ZLIB (raw deflate; the game-specific 2-byte header is skipped)
  - Oodle (`oo2core_*win64.dll`), automatically located at runtime in the
    directory of the opened `.ovl` file and up to 16 parent directories above
    it, as well as a fallback right next to the plugin DLL itself (handy for
    standalone `.ovl` files outside a full game installation) (the DLL itself
    is proprietary and **not** bundled — it must come from the actual game
    installation)
- **External `.ovs` archives**: automatically discovered next to the `.ovl`
  file and merged in (SHA1/batch-database resolution is intentionally *not*
  implemented — see Limitations)
- **Pool and buffer extraction**, including automatic filename detection via
  known signatures (DDS, PNG, Lua, OGG, FLAC, XML, JPEG, ZIP, BMP, …) for
  resources without a name mapping
- **Same-named files of different types** (e.g. `x.ms2`, `x.mdl2` and `x.lua`)
  share the name hash; RootEntries and DataEntries are therefore also matched by
  their `ext_hash`, so none of them gets hidden or misnamed
- **References to other OVLs** (include entries in the header) are listed first
  as a text file `Ref - <name>.ovl.txt` with the target, its resolved path and
  whether it exists – otherwise an OVL that only includes other OVLs looks empty
- **Buffers merged per owning file** (assigned via the DataEntries: in table
  order for v19, via BufferGroups for v20). Each file shows up once under its
  own name, with all its buffers concatenated in slot order (`model.ms2` =
  buffer 0 + 1 + 2, `script.lua`); only buffers without an owner are still
  called `STATIC_bufNNN.bin`
- **Windows long-path support** (`\\?\` prefix): reliably handles very long
  resource names combined with deep destination folders (> 260 characters)
- Fully **read-only** (no archive creation/modification)

The plugin is a straight C port of the format logic found in the accompanying
reference script `unpack_ovl.py`, and has been cross-validated against more
than **2,400 real `.ovl` files** — byte-identical output to the reference
implementation.

## Installation

1. Download `build/ovl_wcx.zip` (contains `ovl_wcx.wcx64` + `pluginst.inf`)
2. Drag the zip file into an open Total Commander window
3. Total Commander will ask for the target directory and automatically
   register the plugin for the `.ovl` extension

**Manual install**, if preferred:
- Copy `ovl_wcx.wcx64` to e.g. `%COMMANDER_PATH%\Plugins\wcx\OVL\`
- Add to `wincmd.ini` under `[PackerPlugins]`:
  ```
  ovl=0,C:\path\to\Plugins\wcx\OVL\ovl_wcx.wcx64
  ```

## Building

Requires Visual Studio (MSVC) with the x64 toolchain.

```
build.bat
```

The output is written to `build\ovl_wcx.wcx64`. A test tool
(`build\test_harness.exe`) that exercises the plugin interface the same way
Total Commander does (via `LoadLibrary`/`GetProcAddress`) can be built with
`build_test.bat`.

## Layout

| File | Purpose |
|---|---|
| `src/ovl_wcx.c` | WCX interface (`OpenArchiveW`, `ReadHeaderExW`, `ProcessFileW`, …) |
| `src/ovl_format.c/h` | FRES header and pool/buffer parser |
| `src/ovl_oodle.c/h` | Dynamic loading of the Oodle DLL |
| `src/wcxhead.h` | Official Total Commander plugin SDK header ([ghisler/WCX-SDK](https://github.com/ghisler/WCX-SDK)) |
| `src/zlib/` | Unmodified zlib sources (inflate only) |

## Limitations

- External `.ovs` archives are only found when located **next to** the `.ovl`
  file (naming convention `<stem>.ovs.<archive-name>`).
- The Oodle DLL is proprietary and not bundled — without it, Oodle-compressed
  archives cannot be listed/extracted (the plugin silently skips them instead
  of crashing).
- **Known bug (unresolved format detail):** Some pools bundle several
  independent resources back-to-back in a single memory region (observed in
  `Config.ovl`-style XMLConfig files containing many small settings XMLs).
  Since our parser (like the Python reference) only extracts *one* file per
  pool, only one of the bundled resources comes out correctly named in these
  cases; the rest are missing. So far this only affects
  `Casino:XMLConfig:xmlconfig` files — a full scan of JWE2's entire asset tree
  (4,750 `.ovl` files) found exactly **one** affected file. Regular assets
  (textures, models, audio, paint jobs) are unaffected (2,400+ files tested
  byte-identical). The exact structure that would resolve this bundling
  (likely via the `num_fragments`/`num_root_entries` header fields) hasn't
  been reverse-engineered yet.
