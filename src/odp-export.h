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

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an export: how many slide PNGs were produced. */
typedef struct {
	int slide_count;     /* number of slide-XXXX.png files on success */
	bool ok;             /* false if LibreOffice or the renderer failed */
	char error[512];     /* human-readable error if !ok */
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
odp_export_result odp_export(const char *odp_path,
			     const char *cache_dir,
			     int dpi,
			     int workers);

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
odp_export_result odp_export_range(const char *odp_path,
				   const char *cache_dir,
				   int dpi,
				   int workers,
				   int first_page,
				   int last_page);

/* Returns the page count of the most recently produced PDF in cache_dir,
 * or 0 if none/unknown. Cheap; reads the PDF only. */
int odp_pdf_page_count(const char *odp_path, const char *cache_dir);

#ifdef __cplusplus
}
#endif
