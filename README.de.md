# ovltc

Ein natives **Total Commander Packer-Plugin (WCX, 64-Bit)** für `.ovl`-Dateien im
**FRES-Format der Cobra-Engine** von Frontier Developments — u. a. verwendet in
*Planet Coaster*, *Planet Zoo* und *Jurassic World Evolution 1/2*.

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
    der geöffneten `.ovl`-Datei und in bis zu 16 übergeordneten Ordnern gesucht,
    sowie als Fallback direkt neben der Plugin-DLL selbst (nützlich für
    einzelne `.ovl`-Dateien außerhalb einer vollständigen Spielinstallation)
    (die DLL selbst ist proprietär und liegt **nicht** bei — sie muss aus der
    jeweiligen Spielinstallation stammen)
- **Externe `.ovs`-Archive**: werden automatisch neben der `.ovl`-Datei gesucht
  und eingebunden (Batch-/SHA1-Datenbank-Auflösung ist bewusst *nicht*
  implementiert, siehe Einschränkungen)
- **Pool- und Buffer-Extraktion** inkl. automatischer Dateiname-Erkennung anhand
  bekannter Signaturen (DDS, PNG, Lua, OGG, FLAC, XML, JPEG, ZIP, BMP, …) für
  Ressourcen ohne Namenszuordnung
- **Gleichnamige Dateien verschiedenen Typs** (z. B. `x.ms2`, `x.mdl2` und
  `x.lua`) teilen sich den Namens-Hash; RootEntries und DataEntries werden
  deshalb zusätzlich über ihren `ext_hash` zugeordnet, damit keine davon
  verdeckt oder falsch benannt wird
- **Verweise auf andere OVLs** (Include-Einträge im Header) erscheinen oben als
  Textdatei `Ref - <name>.ovl.txt` mit Ziel, aufgelöstem Pfad und ob die Datei
  vorhanden ist – sonst wirkt eine OVL, die nur andere OVLs einbindet, leer
- **Buffer je Besitzer-Datei zusammengefügt** (Zuordnung über die DataEntries:
  v19 der Reihe nach, v20 über BufferGroups). Jede Datei erscheint einmal unter
  ihrem Namen, mit allen ihren Buffern in Slot-Reihenfolge hintereinander
  (`model.ms2` = Buffer 0 + 1 + 2, `script.lua`); nur Buffer ohne Besitzer
  heißen noch `STATIC_bufNNN.bin`
- **Windows-Long-Path-Unterstützung** (`\\?\`-Präfix): auch sehr lange
  Ressourcennamen kombiniert mit tiefen Zielordnern (> 260 Zeichen) funktionieren
  zuverlässig
- Vollständig **schreibgeschützt** (kein Erstellen/Ändern von Archiven)

Das Plugin ist eine reine C-Portierung der Formatlogik aus dem mitgelieferten
Referenzskript `unpack_ovl.py` und wurde gegen über **2.400 echte `.ovl`-Dateien**
cross-validiert — byte-genau identische Ergebnisse zur Referenzimplementierung.

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
- **Bekannter Bug (unbestätigtes Format-Detail):** Manche Pools bündeln mehrere
  eigenständige Ressourcen hintereinander in einem einzigen Speicherbereich
  (beobachtet bei `Config.ovl`-artigen XMLConfig-Dateien mit vielen kleinen
  Einstellungs-XMLs). Da unser Parser (ebenso wie die Python-Referenz) pro Pool
  nur *eine* Datei extrahiert, wird in diesen Fällen nur eine der gebündelten
  Ressourcen korrekt benannt ausgegeben; die anderen fehlen. Betroffen sind
  bislang ausschließlich `Casino:XMLConfig:xmlconfig`-Dateien — ein vollständiger
  Scan des kompletten JWE2-Asset-Baums (4.750 `.ovl`-Dateien) fand genau
  **eine** betroffene Datei. Reguläre Assets (Texturen, Modelle, Audio,
  Paintjobs) sind nachweislich nicht betroffen (2.400+ getestete Dateien ohne
  Abweichung). Die genaue Struktur, die diese Bündelung auflösen würde
  (vermutlich über die Header-Felder `num_fragments`/`num_root_entries`), ist
  noch nicht rekonstruiert.
