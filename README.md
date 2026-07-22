# Slides for OBS

Display LibreOffice Impress (`.odp`) and PowerPoint (`.pptx`) presentations
directly inside OBS Studio as a native source — no window capture, no
second screen, no PowerPoint running in the background.

The plugin converts each deck to images once (via LibreOffice) and shows them
as a normal OBS source you can position, resize, and switch between. Slide
navigation is frame-accurate and doesn't drop frames on your stream.

## Features

- **Native source** — slides render straight into the scene, not captured
  from another window.
- **`.odp` and `.pptx`** — anything LibreOffice can open.
- **On-screen navigation dock** — separate *Live* and *Preview* controls, so
  you can queue the next slide without disturbing what's on air.
- **Automatic refresh** — edit the deck, save, and the slides update.
- **Multiple decks at once** — several presentation sources side by side, each
  independent.
- **Parallel rendering** — uses multiple CPU cores to convert long decks
  quickly. Conversion happens once and is cached, so later loads are instant.

## Requirements

- **OBS Studio 30 or newer** (64-bit).
- **[LibreOffice](https://www.libreoffice.org/download/download/)** — free,
  and required to convert presentations. The plugin cannot render slides
  without it. If it's missing, the installer will point you to the download.

LibreOffice does not need to be running; the plugin calls it in the background
only when converting a deck.

## Installation (Windows)

1. Download `SlidesForOBS-Setup.exe` from the
   [latest release](../../releases/latest).
2. Close OBS Studio if it's open.
3. Run the installer. It finds your OBS folder automatically and installs the
   plugin. Running it again later updates an existing install.
4. Start OBS. Add a source: **Slides for OBS → ODP Presentation**, and pick
   your `.odp` or `.pptx` file.

> **"Windows protected your PC" / unknown publisher:** the installer isn't
> code-signed, so Windows SmartScreen shows this warning. Click **More info →
> Run anyway**. (Code signing costs a yearly fee this free project doesn't
> carry.)

## Usage

1. Add an **ODP Presentation** source and select your presentation file.
2. Optionally set a **cache folder** (where converted slide images live) — one
   folder can be shared by every deck; they don't collide.
3. Use the **Slides for OBS** dock to navigate:
   - **Live** — controls the slide currently on air.
   - **Preview** — cues the next slide independently (Studio Mode).

The first time you add a deck it converts (this can take from a few seconds to
a minute depending on deck size and CPU); after that it's cached and loads
instantly until you edit the file.

### Settings

- **Render DPI** — image resolution. Match it to your canvas: **144** for a
  1080p canvas is a pixel-perfect, efficient choice. Higher values are
  downscaled by OBS and just cost time and memory.
- **Parallel render workers** — how many CPU cores to use when converting.
  Set it to your core count (e.g. 8).

## Building from source

This is a standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
project. See the template's documentation for platform build steps. CI builds
for Windows, macOS, and Linux on every push.

## Licenses and credits

This plugin is licensed under the **GNU General Public License v2** — see
[LICENSE](LICENSE).

It bundles binaries from the **Poppler** PDF rendering library (used to convert
PDF pages to images). Poppler is licensed under the GPL. See
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for details and source
locations.

Presentation conversion is performed by
**[LibreOffice](https://www.libreoffice.org/)** (MPL 2.0), which the user
installs separately — it is not bundled or modified by this plugin.

## Disclaimer

Provided as-is under the terms of the GPL, without warranty. It's a community
tool built for a specific need; test it in your own setup before relying on it
for a live production.
