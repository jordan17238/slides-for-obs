# Third-Party Licenses

This plugin distributes binaries from third-party projects. Their licenses and
sources are listed here.

## Poppler (bundled)

The plugin bundles the following Windows binaries from the **Poppler** PDF
rendering library, used to rasterise PDF pages into slide images:

- `pdftoppm.exe`, `pdfinfo.exe`
- their supporting DLLs (cairo, freetype, fontconfig, jpeg, lcms2, libpng,
  openjp2, poppler, poppler-cpp, poppler-glib, tiff, zlib, and related
  dependencies)

**License:** GNU General Public License (GPL), versions 2 and 3.
**Project:** https://poppler.freedesktop.org/
**Windows build source:** https://github.com/oschwartz10612/poppler-windows

The exact release bundled, its version, and the SHA-256 of each file are
recorded in `data/POPPLER-BUNDLE-MANIFEST.txt` within the installed plugin.

Poppler's source code is available from the project and the Windows-build
repository linked above, in satisfaction of the GPL's source-availability
requirement.

## LibreOffice (NOT bundled — user-installed)

Presentation-to-PDF conversion is performed by **LibreOffice**, which the user
installs separately. It is **not** included in, modified by, or redistributed
with this plugin — the plugin only invokes it if present on the system.

**License:** Mozilla Public License v2.0.
**Project:** https://www.libreoffice.org/

## OBS Studio plugin template

This project is derived from the OBS Studio plugin template.

**License:** GNU General Public License v2.
**Project:** https://github.com/obsproject/obs-plugintemplate
