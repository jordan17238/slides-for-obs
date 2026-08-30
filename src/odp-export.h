/*
 * odp-export.h — slide export pipeline (LibreOffice -> PDF -> PNG)
 *
 * This is the C port of the Python pipeline. It deliberately keeps the
 * same staged approach: convert the presentation to PDF with a headless
 * LibreOffice process, then rasterise each page to a PNG with pdftoppm.
 *
 * All functions here are blocking and meant to be called from a worker
 * thread, never from the OBS graphics or UI thread.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an export: how many slide PNGs were produced. */
typedef struct {
	int slide_count; /* number of slide-XXXX.png files on success */
	bool ok;         /* false if LibreOffice or the renderer failed */
	char error[512]; /* human-readable error if !ok */
} odp_export_result;

/*
 * Locate external tools (LibreOffice, pdftoppm). Called once at load.
 * Returns false if LibreOffice can't be found at all.
 * The discovered paths are cached in static storage inside the .c file.
 */
bool odp_tools_detect(void);

/* Accessor strings for logging/diagnostics; may return NULL if missing. */
const char *odp_tool_libreoffice(void);
const char *odp_tool_pdftoppm(void);

/*
 * Export `odp_path` into `cache_dir`, producing slide-0001.png ... in
 * `cache_dir`. `dpi` controls render resolution. `workers` controls how
 * many pages render in parallel.
 *
 * Incremental: pages whose source hasn't changed since last call are
 * skipped (compared via per-page hashing), exactly like the Python build.
 */
odp_export_result odp_export(const char *odp_path, const char *cache_dir, int dpi, int workers);

/*
 * Staged variant for fast first-paint. Renders only PDF pages
 * [first_page..last_page] (1-based, inclusive). Pass last_page = 0 to mean
 * "through the end of the document". The LibreOffice -> PDF step runs only
 * when the cached PDF is missing or older than the .odp; otherwise just the
 * requested pages are rasterised. This lets the caller render a small
 * priority batch (e.g. pages 1-5) to become displayable immediately, then
 * render the remainder in the background.
 *
 * On success, slide_count reports the TOTAL number of slide PNGs currently
 * present in cache_dir (not just the range rendered this call).
 */
/*
 * Cache modes for odp_export_range().
 *   ODP_CACHE_FULL — normal load: reuse cached slides/PDF when newer than the
 *                    source. Use when nothing is known to have changed.
 *   ODP_CACHE_PDF  — reuse only the PDF produced earlier in the SAME run
 *                    (the tail stage of a staged render); re-rasterise PNGs.
 *   ODP_CACHE_NONE — force a fresh LibreOffice conversion. Use when the deck
 *                    is known to have changed on disk (edit-triggered), where
 *                    a timestamp-newer PDF may still hold stale content.
 */
#define ODP_CACHE_FULL 0
#define ODP_CACHE_PDF 1
#define ODP_CACHE_NONE 2

odp_export_result odp_export_range(const char *odp_path, const char *cache_dir, int dpi, int workers, int first_page,
				   int last_page, int cache_mode);

/*
 * Hash the file's contents into *out_hash and return true on success.
 *
 * Returns false if the file cannot be opened — which, on Windows, includes the
 * case where an editor (PowerPoint) is holding it exclusively mid-save. So a
 * false return means "not readable right now, try later", and a true return
 * gives a content hash that changes only when the bytes actually change. The
 * watcher uses this to (a) wait until a locked file is released and (b) tell a
 * real edit apart from a cloud-sync touch that left the content identical.
 */
bool odp_hash_file(const char *path, uint64_t *out_hash);

/* Returns the page count of the most recently produced PDF in cache_dir,
 * or 0 if none/unknown. Cheap; reads the PDF only. */
int odp_pdf_page_count(const char *odp_path, const char *cache_dir);

/*
 * Number of usable cached slides for this deck, or 0 if a render is needed.
 *
 * Renders nothing — it only inspects what is already on disk, so it is cheap
 * enough to call before deciding HOW to export. A deck only counts as cached
 * if its slides are newer than the presentation AND there are as many of them
 * as the PDF has pages, so a half-finished render never passes as a whole one.
 */
int odp_cached_slide_count(const char *odp_path, const char *cache_dir);

/*
 * Compute the per-deck working subfolder for a presentation: cache_dir plus a
 * sanitised version of the presentation's filename stem. Both the exporter and
 * the source's render path MUST use this so they agree on where a deck's PNGs
 * live. Multiple decks can share one cache_dir without their slide-NN.png
 * files colliding, because each deck gets its own subfolder here.
 *
 * Writes the full path into `out` (a char buffer) up to out_size bytes.
 */
void odp_deck_subdir(const char *odp_path, const char *cache_dir, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
