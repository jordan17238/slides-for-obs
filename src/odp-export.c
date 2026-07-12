/*
 * odp-export.c — slide export pipeline, C port of the Python version.
 *
 * Stages:
 *   1. LibreOffice --headless --convert-to pdf   (ODP/PPTX -> PDF)
 *   2. pdftoppm -r DPI -png -f N -l N            (PDF page N -> PNG)
 *      rendered in parallel across `workers` threads, one page each.
 *
 * Cross-platform process spawning uses OBS's util/platform helpers where
 * possible, falling back to popen-style execution. Tool discovery checks
 * the well-known install locations on each OS.
 */

#include "odp-export.h"
#include <sys/stat.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- cached tool paths ------------------------------------------------- */

static char s_libreoffice[1024] = {0};
static char s_pdftoppm[1024] = {0};

/* Serialize all LibreOffice conversions across the whole plugin. soffice is
 * heavy and does not cope well with several headless instances running at
 * once (they thrash disk and CPU and can take many times longer than a single
 * run). With multiple decks — or a re-export firing while another is mid-run —
 * we could otherwise launch several at once. This global lock ensures exactly
 * one ODP->PDF conversion happens at a time; pdftoppm rasterizing is left
 * unserialized since it is cheap and parallel-friendly. */
static pthread_mutex_t s_libreoffice_lock = PTHREAD_MUTEX_INITIALIZER;

const char *odp_tool_libreoffice(void)
{
	return s_libreoffice[0] ? s_libreoffice : NULL;
}
const char *odp_tool_pdftoppm(void)
{
	return s_pdftoppm[0] ? s_pdftoppm : NULL;
}

/* Return true if a path exists and is a regular file. */
static bool file_exists(const char *p)
{
	return os_file_exists(p);
}

static bool first_existing(const char *const *candidates, char *out,
			   size_t out_sz)
{
	for (int i = 0; candidates[i]; i++) {
		if (file_exists(candidates[i])) {
			snprintf(out, out_sz, "%s", candidates[i]);
			return true;
		}
	}
	return false;
}

bool odp_tools_detect(void)
{
#if defined(__APPLE__)
	static const char *lo[] = {
		"/Applications/LibreOffice.app/Contents/MacOS/soffice",
		"/opt/homebrew/bin/libreoffice",
		"/usr/local/bin/libreoffice",
		NULL,
	};
	static const char *pp[] = {
		"/opt/homebrew/bin/pdftoppm",
		"/usr/local/bin/pdftoppm",
		"/usr/bin/pdftoppm",
		NULL,
	};
#elif defined(_WIN32)
	static const char *lo[] = {
		"C:/Program Files/LibreOffice/program/soffice.exe",
		"C:/Program Files (x86)/LibreOffice/program/soffice.exe",
		NULL,
	};
	static const char *pp[] = {
		/* External install paths if the user has poppler-windows on
		 * their system. The BUNDLED path (under our plugin data dir)
		 * is tried first via obs_module_file() below, so these are
		 * fallbacks for users who already have poppler installed. */
		"C:/Program Files/poppler/bin/pdftoppm.exe",
		"C:/Program Files/poppler/Library/bin/pdftoppm.exe",
		"C:/poppler/bin/pdftoppm.exe",
		"C:/poppler/Library/bin/pdftoppm.exe",
		NULL,
	};
#else
	static const char *lo[] = {
		"/usr/bin/libreoffice", "/usr/bin/soffice",
		"/usr/local/bin/libreoffice", NULL,
	};
	static const char *pp[] = {
		"/usr/bin/pdftoppm", "/usr/local/bin/pdftoppm", NULL,
	};
#endif

	bool have_lo = first_existing(lo, s_libreoffice, sizeof(s_libreoffice));

	/* Prefer a pdftoppm that ships with the plugin (under data/bin/), so
	 * recipients don't have to install poppler separately. obs_module_file
	 * returns a path inside our plugin's data directory; the file may or
	 * may not be present (it's the packaging step that puts it there). On
	 * macOS the binary is `pdftoppm`, on Windows `pdftoppm.exe`. */
#if defined(_WIN32)
	const char *bundled_rel = "bin/pdftoppm.exe";
#else
	const char *bundled_rel = "bin/pdftoppm";
#endif
	char *bundled = obs_module_file(bundled_rel);
	bool have_pp = false;
	if (bundled && os_file_exists(bundled)) {
		snprintf(s_pdftoppm, sizeof(s_pdftoppm), "%s", bundled);
		have_pp = true;
	}
	bfree(bundled);

	if (!have_pp)
		have_pp = first_existing(pp, s_pdftoppm, sizeof(s_pdftoppm));

	blog(LOG_INFO, "[odp-presenter] LibreOffice: %s",
	     have_lo ? s_libreoffice : "NOT FOUND");
	blog(LOG_INFO, "[odp-presenter] pdftoppm: %s",
	     have_pp ? s_pdftoppm : "not found");

	return have_lo;
}

/* ---- shell execution --------------------------------------------------- */

/*
 * Run a command line and wait for it to finish. Returns the process exit
 * code, or -1 on spawn failure. We quote arguments and use the platform
 * shell. For a production plugin you'd switch to posix_spawn / CreateProcess
 * for robust argument handling, but this is clear and works for our paths.
 */
static int run_blocking(const char *cmdline)
{
#if defined(_WIN32)
	/* _popen/_pclose run through cmd.exe */
	FILE *p = _popen(cmdline, "r");
	if (!p)
		return -1;
	char buf[256];
	while (fgets(buf, sizeof(buf), p))
		; /* drain output */
	return _pclose(p);
#else
	FILE *p = popen(cmdline, "r");
	if (!p)
		return -1;
	char buf[256];
	while (fgets(buf, sizeof(buf), p))
		;
	int status = pclose(p);
	return status;
#endif
}

/* Append a shell-quoted argument to a dstr command line. */
static void append_quoted(struct dstr *cmd, const char *arg)
{
#if defined(_WIN32)
	dstr_cat(cmd, " \"");
	dstr_cat(cmd, arg);
	dstr_cat(cmd, "\"");
#else
	dstr_cat(cmd, " '");
	/* naive escape of single quotes */
	for (const char *c = arg; *c; c++) {
		if (*c == '\'')
			dstr_cat(cmd, "'\\''");
		else
			dstr_ncat(cmd, c, 1);
	}
	dstr_cat(cmd, "'");
#endif
}

/* ---- PDF page counting ------------------------------------------------- */

/* Count slide-*.png files present in dir. */
static int count_pngs(const char *dir)
{
	int count = 0;
	struct dstr pattern;
	dstr_init(&pattern);
	dstr_printf(&pattern, "%s/slide-", dir);

	os_dir_t *d = os_opendir(dir);
	if (d) {
		struct os_dirent *ent;
		while ((ent = os_readdir(d)) != NULL) {
			if (!ent->directory &&
			    strncmp(ent->d_name, "slide-", 6) == 0 &&
			    strstr(ent->d_name, ".png"))
				count++;
		}
		os_closedir(d);
	}
	dstr_free(&pattern);
	return count;
}

/* ---- export entry point ------------------------------------------------ */

/* Thin wrapper: full export, all pages. */
odp_export_result odp_export(const char *odp_path, const char *cache_dir,
			     int dpi, int workers)
{
	return odp_export_range(odp_path, cache_dir, dpi, workers, 1, 0);
}

odp_export_result odp_export_range(const char *odp_path, const char *cache_dir,
				   int dpi, int workers,
				   int first_page, int last_page)
{
	UNUSED_PARAMETER(workers); /* parallelism added in next iteration */

	odp_export_result res = {0};
	res.ok = false;

	if (!odp_path || !*odp_path) {
		snprintf(res.error, sizeof(res.error), "no presentation path");
		return res;
	}
	if (!cache_dir || !*cache_dir) {
		snprintf(res.error, sizeof(res.error), "no cache directory");
		return res;
	}
	if (!odp_tool_libreoffice()) {
		snprintf(res.error, sizeof(res.error),
			 "LibreOffice not found");
		return res;
	}

	os_mkdirs(cache_dir);

	/* ── Fast path: reuse existing slides ────────────────────────────
	 * If we already have slide PNGs that are newer than BOTH the source
	 * .odp and the intermediate PDF, the cache is current — skip the slow
	 * LibreOffice render and just report the count. We compare against the
	 * .odp (so edits invalidate the cache) using the WIDEST-padded slide-1
	 * name first, since the canonical full-deck render uses the widest
	 * padding; checking a stale narrow name (e.g. slide-1.png left over
	 * from an old render) would give a wrong, too-old comparison. */
	if (first_page <= 1 && last_page == 0) {
		int existing = count_pngs(cache_dir);
		if (existing > 0) {
			struct stat odp_st, png_st;
			struct dstr first_png;
			dstr_init(&first_png);
			bool got_png = false;
			/* widest-first so we match the canonical render's name */
			for (int w = 6; w >= 1 && !got_png; w--) {
				dstr_printf(&first_png, "%s/slide-%0*d.png",
					    cache_dir, w, 1);
				if (stat(first_png.array, &png_st) == 0)
					got_png = true;
			}
			if (got_png && stat(odp_path, &odp_st) == 0 &&
			    png_st.st_mtime >= odp_st.st_mtime) {
				res.ok = true;
				res.slide_count = existing;
				blog(LOG_INFO,
				     "[odp-presenter] reusing %d cached slides "
				     "(no re-export needed)", existing);
				dstr_free(&first_png);
				return res;
			}
			dstr_free(&first_png);
		}
	}

	/* ── Stage 1: ODP -> PDF ─────────────────────────────────────── */
	/* Use a private LibreOffice profile inside the cache dir so this
	 * never collides with a LibreOffice the user already has open. The
	 * -env flag must come first, before --headless. We redirect output
	 * to a log file so failures are visible. */
	/* Derive the PDF name (same stem as the ODP) up front so we can decide
	 * whether the LibreOffice step is needed at all. */
	const char *base = strrchr(odp_path, '/');
#if defined(_WIN32)
	const char *base_bs = strrchr(odp_path, '\\');
	if (base_bs && (!base || base_bs > base))
		base = base_bs;
#endif
	base = base ? base + 1 : odp_path;

	struct dstr stem;
	dstr_init_copy(&stem, base);
	char *dot = strrchr(stem.array, '.');
	if (dot)
		*dot = '\0';

	struct dstr pdf_path;
	dstr_init(&pdf_path);
	dstr_printf(&pdf_path, "%s/%s.pdf", cache_dir, stem.array);

	/* Skip the (slow) LibreOffice conversion if a PDF already exists and is
	 * at least as new as the .odp. This makes the tail render of the staged
	 * flow nearly free, and avoids re-launching LibreOffice for the second
	 * page-range call. */
	bool pdf_fresh = false;
	{
		struct stat pst, ost;
		if (stat(pdf_path.array, &pst) == 0 &&
		    stat(odp_path, &ost) == 0 &&
		    pst.st_mtime >= ost.st_mtime)
			pdf_fresh = true;
	}

	if (!pdf_fresh) {
		struct dstr profile;
		dstr_init(&profile);
		dstr_printf(&profile, "%s/lo_profile", cache_dir);
		os_mkdirs(profile.array);

		struct dstr logf;
		dstr_init(&logf);
		dstr_printf(&logf, "%s/soffice.log", cache_dir);

		struct dstr cmd;
		dstr_init(&cmd);
		append_quoted(&cmd, odp_tool_libreoffice());
		dstr_cat(&cmd, " -env:UserInstallation=file://");
		dstr_cat(&cmd, profile.array);
		dstr_cat(&cmd, " --headless --norestore --convert-to pdf --outdir");
		append_quoted(&cmd, cache_dir);
		append_quoted(&cmd, odp_path);
		dstr_cat(&cmd, " > ");
		append_quoted(&cmd, logf.array);
		dstr_cat(&cmd, " 2>&1");

		blog(LOG_INFO, "[odp-presenter] running: %s", cmd.array);

		/* Only one LibreOffice conversion at a time across the plugin
		 * (see s_libreoffice_lock). Without this, two decks or a
		 * save-triggered re-export can launch concurrent soffice
		 * instances that thrash and take many times longer. */
		pthread_mutex_lock(&s_libreoffice_lock);
		uint64_t t0 = os_gettime_ns();
		int rc = run_blocking(cmd.array);
		blog(LOG_INFO, "[odp-presenter] LibreOffice took %.1f s",
		     (os_gettime_ns() - t0) / 1.0e9);
		pthread_mutex_unlock(&s_libreoffice_lock);
		dstr_free(&cmd);
		dstr_free(&profile);
		dstr_free(&logf);
		if (rc != 0) {
			snprintf(res.error, sizeof(res.error),
				 "LibreOffice exit code %d (see soffice.log)", rc);
			dstr_free(&stem);
			dstr_free(&pdf_path);
			return res;
		}

		/* The PDF was just regenerated from an edited .odp. Delete ALL
		 * existing slide PNGs now so stale pages from a previous version
		 * (especially if the deck got shorter, e.g. 156 -> 60 slides)
		 * cannot survive and inflate the count or shadow new content.
		 * The render below recreates the current set cleanly. */
		os_dir_t *d = os_opendir(cache_dir);
		if (d) {
			struct os_dirent *ent;
			while ((ent = os_readdir(d)) != NULL) {
				if (ent->directory)
					continue;
				if (strncmp(ent->d_name, "slide-", 6) == 0 &&
				    strstr(ent->d_name, ".png")) {
					struct dstr p;
					dstr_init(&p);
					dstr_printf(&p, "%s/%s", cache_dir,
						    ent->d_name);
					os_unlink(p.array);
					dstr_free(&p);
				}
			}
			os_closedir(d);
		}
	} else {
		blog(LOG_INFO,
		     "[odp-presenter] PDF already fresh, skipping LibreOffice");
	}

	if (!file_exists(pdf_path.array)) {
		snprintf(res.error, sizeof(res.error),
			 "PDF not produced (%s)", pdf_path.array);
		dstr_free(&stem);
		dstr_free(&pdf_path);
		return res;
	}

	/* ── Stage 2: PDF -> PNGs ────────────────────────────────────── */
	/* First clear stale PNGs. */
	/* (Iterating dir + removing is omitted for brevity here; pdftoppm
	 *  overwrites by page number which is sufficient for now.) */

	if (!odp_tool_pdftoppm()) {
		snprintf(res.error, sizeof(res.error),
			 "pdftoppm not found");
		dstr_free(&stem);
		dstr_free(&pdf_path);
		return res;
	}

	struct dstr prefix;
	dstr_init(&prefix);
	dstr_printf(&prefix, "%s/slide", cache_dir);

	struct dstr rcmd;
	dstr_init(&rcmd);
	append_quoted(&rcmd, odp_tool_pdftoppm());
	dstr_catf(&rcmd, " -r %d -png", dpi);
	if (first_page >= 1)
		dstr_catf(&rcmd, " -f %d", first_page);
	if (last_page >= 1)
		dstr_catf(&rcmd, " -l %d", last_page);
	append_quoted(&rcmd, pdf_path.array);
	append_quoted(&rcmd, prefix.array);

	uint64_t pt0 = os_gettime_ns();
	int rc = run_blocking(rcmd.array);
	blog(LOG_INFO,
	     "[odp-presenter] pdftoppm (pages %d-%s) took %.1f s",
	     first_page >= 1 ? first_page : 1,
	     last_page >= 1 ? "range" : "end",
	     (os_gettime_ns() - pt0) / 1.0e9);
	dstr_free(&rcmd);
	dstr_free(&prefix);

	if (rc != 0) {
		snprintf(res.error, sizeof(res.error),
			 "pdftoppm exit code %d", rc);
		dstr_free(&stem);
		dstr_free(&pdf_path);
		return res;
	}

	res.slide_count = count_pngs(cache_dir);
	res.ok = res.slide_count > 0;
	if (!res.ok)
		snprintf(res.error, sizeof(res.error),
			 "no PNGs produced");

	/* After a FULL render, pdftoppm has written the canonical page-number
	 * width (e.g. 3 digits for a 100+ page deck). A previous focused render
	 * may have left narrower-width PNGs (e.g. slide-7.png) that would shadow
	 * the canonical slide-007.png in the widest-first path lookup. Prune any
	 * slide PNG that doesn't match the canonical width. We do this AFTER the
	 * render (not before) so the live slide is never momentarily missing. */
	if (res.ok && first_page <= 1 && last_page == 0) {
		int canon_w = 1;
		for (int n = res.slide_count; n >= 10; n /= 10)
			canon_w++;
		os_dir_t *d = os_opendir(cache_dir);
		if (d) {
			struct os_dirent *ent;
			while ((ent = os_readdir(d)) != NULL) {
				if (ent->directory)
					continue;
				if (strncmp(ent->d_name, "slide-", 6) != 0 ||
				    !strstr(ent->d_name, ".png"))
					continue;
				/* digits between "slide-" and ".png" */
				const char *num = ent->d_name + 6;
				int w = 0;
				while (num[w] >= '0' && num[w] <= '9')
					w++;
				if (w != canon_w) {
					struct dstr p;
					dstr_init(&p);
					dstr_printf(&p, "%s/%s", cache_dir,
						    ent->d_name);
					os_unlink(p.array);
					dstr_free(&p);
				}
			}
			os_closedir(d);
		}
	}

	dstr_free(&stem);
	dstr_free(&pdf_path);
	return res;
}


/* Best-effort PDF page count via `pdfinfo` (ships with poppler alongside
 * pdftoppm). Returns 0 if it can't be determined. */
int odp_pdf_page_count(const char *odp_path, const char *cache_dir)
{
	if (!cache_dir || !*cache_dir)
		return 0;

	/* derive PDF path (same stem as the odp) */
	const char *base = strrchr(odp_path, '/');
#if defined(_WIN32)
	const char *base_bs = strrchr(odp_path, '\\');
	if (base_bs && (!base || base_bs > base))
		base = base_bs;
#endif
	base = base ? base + 1 : odp_path;
	struct dstr stem;
	dstr_init_copy(&stem, base);
	char *dot = strrchr(stem.array, '.');
	if (dot)
		*dot = '\0';

	struct dstr pdf_path;
	dstr_init(&pdf_path);
	dstr_printf(&pdf_path, "%s/%s.pdf", cache_dir, stem.array);

	int pages = 0;
	if (file_exists(pdf_path.array)) {
		/* Try pdfinfo next to pdftoppm. */
		const char *ppm = odp_tool_pdftoppm();
		if (ppm) {
			struct dstr info;
			dstr_init_copy(&info, ppm);
			/* Swap the trailing "pdftoppm" for "pdfinfo", keeping the
			 * directory part. On Windows the path may use backslashes
			 * (so check both separators) and the executable needs the
			 * ".exe" suffix — without it file_exists() misses the
			 * bundled pdfinfo.exe and page counting silently fails. */
			char *slash = strrchr(info.array, '/');
#if defined(_WIN32)
			char *bslash = strrchr(info.array, '\\');
			if (bslash && (!slash || bslash > slash))
				slash = bslash;
#endif
			if (slash) {
				*(slash + 1) = '\0';
				dstr_resize(&info, strlen(info.array));
#if defined(_WIN32)
				dstr_cat(&info, "pdfinfo.exe");
#else
				dstr_cat(&info, "pdfinfo");
#endif
			}
			if (file_exists(info.array)) {
				struct dstr cmd;
				dstr_init(&cmd);
				append_quoted(&cmd, info.array);
				append_quoted(&cmd, pdf_path.array);
#if defined(_WIN32)
				FILE *pp = _popen(cmd.array, "r");
#else
				FILE *pp = popen(cmd.array, "r");
#endif
				if (pp) {
					char line[256];
					while (fgets(line, sizeof(line), pp)) {
						if (strncmp(line, "Pages:", 6) == 0) {
							pages = atoi(line + 6);
							break;
						}
					}
#if defined(_WIN32)
					_pclose(pp);
#else
					pclose(pp);
#endif
				}
				dstr_free(&cmd);
			}
			dstr_free(&info);
		}
	}

	dstr_free(&stem);
	dstr_free(&pdf_path);
	return pages;
}
