# ovltc

Total Commander packer plugin for `.ovl` (FRES / Cobra Engine) archive files.

- [Deutsch](README.de.md)
- [English](README.en.md)

## Overview

**ovltc** is a native Total Commander packer plugin (WCX, 64-bit) for `.ovl`
files — the FRES archive format used by Frontier Developments' Cobra Engine
in *Planet Coaster*, *Planet Zoo*, and *Jurassic World Evolution 1/2*. It lets you open `.ovl` files like any other archive
(Enter/double-click), browse their contents, and extract resources.

- Parses the FRES header (versions 17–20+) and decompresses ZLIB- or
  Oodle-compressed archives, including external `.ovs` files next to the
  `.ovl`
- Automatically locates the Oodle DLL in the game's install tree
- Handles Windows long paths (> 260 characters) reliably
- Fully read-only — no archive creation or modification
- Validated against 2,400+ real `.ovl` files

See the full [Deutsch](README.de.md) / [English](README.en.md) docs for
installation, build instructions, and known limitations.
