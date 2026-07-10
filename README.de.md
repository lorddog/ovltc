# ovltc

Ein natives **Total Commander Packer-Plugin (WCX, 64-Bit)** für `.ovl`-Dateien im
**FRES-Format der Cobra-Engine** von Frontier Developments — u. a. verwendet in
*Elite Dangerous*, *Planet Coaster*, *Planet Zoo* und *Jurassic World Evolution 1/2*.

Mit dem Plugin lassen sich `.ovl`-Dateien in Total Commander wie ein gewöhnliches
Archiv öffnen (Enter/Doppelklick), der Inhalt durchsuchen und einzelne oder alle
enthaltenen Ressourcen entpacken — ganz ohne Python oder sonstige Laufzeitumgebung.

- [English version](README.en.md)

---

## Funktionsumfang

- **Vollständiges FRES-Header-Parsing** (Versionen 17–20+): Datei-, Mime- und
  Archiv-Tabellen, inkl. versionsabhängiger Struct-Layouts
- **Dekompression**:
  - ZLIB (raw deflate, spielspezifischer 2-Byte-Header wird übersprungen)
  - Oodle (`oo2core_*win64.dll`), wird zur Laufzeit automatisch im Verzeichnis
    der geöffneten `.ovl`-Datei und in bis zu 16 übergeordneten Ordnern gesucht
    (die DLL selbst ist proprietär und liegt **nicht** bei — sie muss aus der
    jeweiligen Spielinstallation stammen)
- **Externe `.ovs`-Archive**: werden automatisch neben der `.ovl`-Datei gesucht
  und eingebunden (Batch-/SHA1-Datenbank-Auflösung ist bewusst *nicht*
  implementiert, siehe Einschränkungen)
- **Pool- und Buffer-Extraktion** inkl. automatischer Dateiname-Erkennung anhand
  bekannter Signaturen (DDS, PNG, Lua, OGG, FLAC, XML, JPEG, ZIP, BMP, …) für
  Ressourcen ohne Namenszuordnung
- **Windows-Long-Path-Unterstützung** (`\\?\`-Präfix): auch sehr lange
  Ressourcennamen kombiniert mit tiefen Zielordnern (> 260 Zeichen) funktionieren
  zuverlässig
- Vollständig **schreibgeschützt** (kein Erstellen/Ändern von Archiven)

Das Plugin ist eine reine C-Portierung der Formatlogik aus dem mitgelieferten
Referenzskript `unpack_ovl.py` und wurde gegen über **2.400 echte `.ovl`-Dateien**
aus Jurassic World Evolution 2 und Elite Dangerous cross-validiert — byte-genau
identische Ergebnisse zur Referenzimplementierung.

## Installation

1. `build/ovl_wcx.zip` herunterladen (enthält `ovl_wcx.wcx64` + `pluginst.inf`)
2. Die ZIP-Datei in ein offenes Total-Commander-Fenster ziehen
3. Total Commander fragt nach dem Zielverzeichnis und registriert das Plugin
   automatisch für die Endung `.ovl`

**Manuell**, falls gewünscht:
- `ovl_wcx.wcx64` z. B. nach `%COMMANDER_PATH%\Plugins\wcx\OVL\` kopieren
- In `wincmd.ini` unter `[PackerPlugins]` eintragen:
  ```
  ovl=0,C:\Pfad\zu\Plugins\wcx\OVL\ovl_wcx.wcx64
  ```

## Build

Voraussetzung: Visual Studio (MSVC) mit x64-Toolchain.

```
build.bat
```

Das Ergebnis liegt anschließend unter `build\ovl_wcx.wcx64`. Ein Test-Werkzeug
(`build\test_harness.exe`), das die Plugin-Schnittstelle wie Total Commander
selbst über `LoadLibrary`/`GetProcAddress` aufruft, lässt sich mit
`build_test.bat` erzeugen.

## Aufbau

| Datei | Zweck |
|---|---|
| `src/ovl_wcx.c` | WCX-Schnittstelle (`OpenArchiveW`, `ReadHeaderExW`, `ProcessFileW`, …) |
| `src/ovl_format.c/h` | FRES-Header- und Pool-/Buffer-Parser |
| `src/ovl_oodle.c/h` | Dynamisches Laden der Oodle-DLL |
| `src/wcxhead.h` | Offizieller Total-Commander-Plugin-SDK-Header ([ghisler/WCX-SDK](https://github.com/ghisler/WCX-SDK)) |
| `src/zlib/` | Unveränderte zlib-Quellen (nur Inflate) |

## Einschränkungen

- Externe `.ovs`-Archive werden nur gefunden, wenn sie **neben** der `.ovl`-Datei
  liegen (Namenskonvention `<stem>.ovs.<archivname>`).
- Die Oodle-DLL ist proprietär und wird nicht mitgeliefert — ohne sie können
  Oodle-komprimierte Archive nicht gelistet/entpackt werden (das Plugin
  überspringt sie dann stillschweigend statt abzustürzen).
