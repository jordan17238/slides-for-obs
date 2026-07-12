# ODP Presenter — Native OBS Plugin (work in progress)

This is the native C plugin port of the Python `obs_odp_presenter.py` script.
It registers a real **"ODP Presentation"** source type in OBS, so end users
just Add Source → ODP Presentation, pick a file, and go — no Python, no
interpreter path, no manual script loading.

## Current status

**Milestone 1 (this code):** loads as a plugin, registers the source type,
runs the LibreOffice → PDF → PNG pipeline on a background thread, displays
slides via an internal image source, and supports next/prev/first/last/reload
hotkeys.

**Not yet ported from the Python version:**
- Incremental rendering (per-page hashing to skip unchanged slides)
- Parallel page rendering (currently one pdftoppm call for the whole PDF)
- Fixed 4-digit filename normalisation
- Scene-aware "active deck" arrow keys (native hotkeys are per-source, which
  actually solves this more cleanly — each source has its own hotkeys)

These come in milestone 2 once milestone 1 compiles and loads for you.

## Why it still needs LibreOffice

There is no good native library to render `.odp`. Like the script, the plugin
shells out to a headless LibreOffice to make a PDF, then rasterises with
pdftoppm. Your users install LibreOffice (and on Windows, poppler) once —
everything else is bundled in the plugin.

## Building

The easiest path is to build inside the official OBS plugin template, which
provides libobs discovery, packaging, signing hooks, and GitHub Actions CI for
both macOS and Windows:

1. Clone the template:
   ```
   git clone https://github.com/obsproject/obs-plugintemplate.git
   ```
2. Copy these files in, replacing the template's `src/` and `CMakeLists.txt`:
   ```
   src/plugin-main.c
   src/odp-source.c
   src/odp-export.c
   src/odp-export.h
   CMakeLists.txt   (merge — keep the template's helper includes)
   ```
3. Follow the template's build steps:
   - **macOS:** `cmake --preset macos` then build in Xcode, or
     `cmake --build --preset macos`
   - **Windows:** `cmake --preset windows-x64` then build in Visual Studio

The template's CI can produce signed `.pkg` (macOS) and `.exe`/`.zip`
(Windows) installers automatically on every git tag — that's how you'd
distribute to your users.

## File overview

| File | Purpose |
|---|---|
| `src/plugin-main.c` | Module entry point, registers the source type |
| `src/odp-source.c` | The "ODP Presentation" source: settings, hotkeys, render delegation, worker thread |
| `src/odp-export.c` | LibreOffice → PDF → PNG pipeline (the C port of the Python logic) |
| `src/odp-export.h` | Export pipeline interface |
| `CMakeLists.txt` | Build configuration |

## Architecture notes

The source doesn't draw pixels itself. It creates a private child
`image_source` and, on each slide change, updates that child's `file` setting
to the current `slide-XXXX.png`, then delegates `video_render` to it. This
reuses OBS's battle-tested image loading/decoding rather than managing GPU
textures by hand — the same pragmatic choice the Python script made by
driving an Image source.

Hotkeys are registered with `obs_hotkey_register_source`, meaning each ODP
source instance gets its own Next/Prev/etc. This is cleaner than the script's
"active deck" detection: because hotkeys are per-source, OBS routes them to
whichever source is relevant, and you bind keys per source in Settings →
Hotkeys.
