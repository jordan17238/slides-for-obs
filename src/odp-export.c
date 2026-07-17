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

#if defined(_WIN32)
#include <windows.h>
#endif

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
/* ---- process execution -------------------------------------------------
 *
 * WINDOWS: we do NOT go through cmd.exe. Building a command string and letting
 * cmd parse it produced three separate, hard-to-find bugs (quote stripping at
 * the start of the line, an unquoted argument containing spaces, and a `>`
 * redirect that corrupted the argument list badly enough to crash soffice with
 * STACK_BUFFER_OVERRUN). CreateProcess takes the arguments we actually mean,
 * with one well-defined quoting rule, and stdout is captured through a pipe
 * rather than a shell redirect — so the whole class of bug disappears.
 *
 * POSIX: popen with a shell-quoted line, which has always worked fine.
 *
 * Both platforms go through run_argv(): pass argv as a NULL-terminated array.
 * If `out` is non-NULL, the child's stdout+stderr are appended to it.
 * If `log_path` is non-NULL, they're also written to that file.
 */

#if !defined(_WIN32)
/* Single-quote an argument for /bin/sh. No leading space — the caller spaces. */
static void append_quoted_posix(struct dstr *cmd, const char *arg)
{
	dstr_cat(cmd, "'");
	for (const char *c = arg; *c; c++) {
		if (*c == '\'')
			dstr_cat(cmd, "'\\''");
		else
			dstr_ncat(cmd, c, 1);
	}
	dstr_cat(cmd, "'");
}
#endif

#if defined(_WIN32)
/* Quote one argument per the MSVCRT/CommandLineToArgvW rules that
 * CreateProcess consumers use: wrap in quotes if it contains a space or quote,
 * escape embedded quotes, and double any backslashes that immediately precede
 * a quote. */
static void win_quote_arg(struct dstr *out, const char *arg)
{
	bool needs = (*arg == '\0') || strpbrk(arg, " \t\"") != NULL;
	if (!needs) {
		dstr_cat(out, arg);
		return;
	}
	dstr_cat(out, "\"");
	int backslashes = 0;
	for (const char *p = arg; *p; p++) {
		if (*p == '\\') {
			backslashes++;
			continue;
		}
		if (*p == '"') {
			/* escape the run of backslashes, then the quote */
			for (int i = 0; i < backslashes * 2 + 1; i++)
				dstr_cat(out, "\\");
			backslashes = 0;
			dstr_cat(out, "\"");
			continue;
		}
		for (int i = 0; i < backslashes; i++)
			dstr_cat(out, "\\");
		backslashes = 0;
		char c[2] = { *p, '\0' };
		dstr_cat(out, c);
	}
	/* trailing backslashes must be doubled before the closing quote */
	for (int i = 0; i < backslashes * 2; i++)
		dstr_cat(out, "\\");
	dstr_cat(out, "\"");
}
#endif

static int run_argv(const char *const *argv, struct dstr *out,
		    const char *log_path)
{
#if defined(_WIN32)
	/* argv[0] is the executable; build the command line CreateProcess wants.
	 * Backslashes are the native separator, so normalise the exe path. */
	struct dstr exe;
	dstr_init_copy(&exe, argv[0]);
	dstr_replace(&exe, "/", "\\");

	struct dstr cmdline;
	dstr_init(&cmdline);
	win_quote_arg(&cmdline, exe.array);
	for (int i = 1; argv[i]; i++) {
		dstr_cat(&cmdline, " ");
		win_quote_arg(&cmdline, argv[i]);
	}

	/* Give the child NUL for stdout/stderr rather than a pipe.
	 *
	 * Why: launching soffice with a pipe and/or creation flags reproducibly
	 * killed it with STACK_BUFFER_OVERRUN (0xC0000409) after several seconds,
	 * while the *identical command line* run by hand in a console converts
	 * fine. Rather than keep guessing which detail soffice dislikes, this
	 * makes our spawn as close as possible to the hand-run that works:
	 * inherited-style handles pointing at NUL, and no creation flags at all.
	 *
	 * Cost: we can't capture the tool's output, so soffice.log is no longer
	 * written. That's an acceptable trade — the log has never once been
	 * readable on Windows anyway, and a working conversion beats a log.
	 * pdfinfo still needs its stdout, so it opts back into a pipe below. */
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
	HANDLE rd = NULL, wr = NULL;
	bool want_output = (out != NULL);

	if (want_output) {
		if (!CreatePipe(&rd, &wr, &sa, 0)) {
			dstr_free(&exe);
			dstr_free(&cmdline);
			return -1;
		}
		SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	} else {
		wr = CreateFileA("NUL", GENERIC_WRITE,
				 FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
				 OPEN_EXISTING, 0, NULL);
		if (wr == INVALID_HANDLE_VALUE)
			wr = NULL;
	}

	STARTUPINFOA si = { 0 };
	si.cb = sizeof(si);
	if (wr) {
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = wr;
		si.hStdError = wr;
		si.hStdInput = NULL;
	}

	PROCESS_INFORMATION pi = { 0 };
	/* CREATE_NO_WINDOW: run the tool with no console window at all.
	 *
	 * Without it, every spawned tool pops a console the user has to close —
	 * and closing it KILLS the tool (pdftoppm died with 0xC000013A,
	 * "terminated by CTRL+C / console close", mid-render).
	 *
	 * This flag was briefly removed while hunting a soffice crash, but that
	 * crash turned out to be a LibreOffice *profile collision* (headless
	 * soffice handing off to an already-open LibreOffice window), fixed by
	 * the isolated -env:UserInstallation profile. The two are unrelated, so
	 * we keep both: a private profile AND no console windows.
	 *
	 * lpApplicationName = NULL: the quoted command line names the exe. */
	BOOL ok = CreateProcessA(NULL, cmdline.array, NULL, NULL, TRUE,
				 CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	if (wr)
		CloseHandle(wr); /* our copy; child holds the other end */

	if (!ok) {
		blog(LOG_ERROR,
		     "[odp-presenter] CreateProcess failed (%lu) for: %s",
		     GetLastError(), cmdline.array);
		if (rd)
			CloseHandle(rd);
		dstr_free(&exe);
		dstr_free(&cmdline);
		return -1;
	}

	/* Drain the pipe (only when we asked for output) until the child closes
	 * it, so it can never block on a full buffer. */
	struct dstr captured;
	dstr_init(&captured);
	if (rd) {
		char buf[512];
		DWORD n = 0;
		while (ReadFile(rd, buf, sizeof(buf) - 1, &n, NULL) && n > 0) {
			buf[n] = '\0';
			dstr_cat(&captured, buf);
		}
		CloseHandle(rd);
	}

	/* Wait for the child, but never forever. A soffice that collides with a
	 * running LibreOffice instance (or stops on a hidden dialog) will sit
	 * there indefinitely, and without a bound this thread would hang for the
	 * life of OBS. Five minutes is far beyond any legitimate conversion. */
	DWORD wait = WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
	DWORD code = 1;
	if (wait == WAIT_TIMEOUT) {
		blog(LOG_ERROR,
		     "[odp-presenter] tool timed out after 5 min; terminating. "
		     "Is a LibreOffice window open and blocking headless mode?");
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 5000);
		code = 1;
	} else {
		GetExitCodeProcess(pi.hProcess, &code);
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	if (log_path && captured.array) {
		FILE *f = os_fopen(log_path, "wb");
		if (f) {
			fwrite(captured.array, 1, strlen(captured.array), f);
			fclose(f);
		}
	}
	if (out && captured.array)
		dstr_cat(out, captured.array);

	dstr_free(&captured);
	dstr_free(&exe);
	dstr_free(&cmdline);
	return (int)code;
#else
	/* POSIX: shell-quote into a line and popen it (unchanged behaviour). */
	struct dstr cmd;
	dstr_init(&cmd);
	for (int i = 0; argv[i]; i++) {
		if (i)
			dstr_cat(&cmd, " ");
		append_quoted_posix(&cmd, argv[i]);
	}
	if (log_path) {
		dstr_cat(&cmd, " > ");
		append_quoted_posix(&cmd, log_path);
		dstr_cat(&cmd, " 2>&1");
	}

	FILE *p = popen(cmd.array, "r");
	dstr_free(&cmd);
	if (!p)
		return -1;
	char buf[512];
	while (fgets(buf, sizeof(buf), p)) {
		if (out)
			dstr_cat(out, buf);
	}
	return pclose(p);
#endif
}

/* ---- PDF page counting ------------------------------------------------- */

/* Delete every slide-*.png in dir. Used when a render fails part-way, so the
 * incomplete output can't be mistaken for a valid cached deck next load. */
static void delete_slide_pngs(const char *dir)
{
	os_dir_t *d = os_opendir(dir);
	if (!d)
		return;

	struct os_dirent *ent;
	int removed = 0;
	while ((ent = os_readdir(d)) != NULL) {
		if (ent->directory || strncmp(ent->d_name, "slide-", 6) != 0 ||
		    !strstr(ent->d_name, ".png"))
			continue;
		struct dstr p;
		dstr_init(&p);
		dstr_printf(&p, "%s/%s", dir, ent->d_name);
		if (os_unlink(p.array) == 0)
			removed++;
		dstr_free(&p);
	}
	os_closedir(d);
	if (removed)
		blog(LOG_INFO,
		     "[odp-presenter] removed %d partial slide image(s)",
		     removed);
}

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
/* Shared: cache_dir + sanitised filename stem. See header. */
void odp_deck_subdir(const char *odp_path, const char *cache_dir,
		     char *out, size_t out_size)
{
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
	for (char *q = stem.array; q && *q; q++) {
		if (*q == '/' || *q == '\\' || *q == ':' || *q == '*' ||
		    *q == '?' || *q == '"' || *q == '<' || *q == '>' ||
		    *q == '|')
			*q = '_';
	}

	snprintf(out, out_size, "%s/%s", cache_dir, stem.array);
	dstr_free(&stem);
}

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

	/* ── Per-deck working subfolder ──────────────────────────────────
	 * Multiple ODP sources can (and typically will) share one cache
	 * folder. pdftoppm names its output slide-01.png, slide-02.png … with
	 * no deck identifier, so if two decks rendered into the same directory
	 * they'd overwrite each other's PNGs and every source would show the
	 * SAME slides (whichever deck rendered last). To keep them separate,
	 * each deck gets its own subfolder under the cache dir, named from the
	 * presentation's filename stem. Everything below — the reuse check, the
	 * PDF, the PNGs, counting and cleanup — operates inside this per-deck
	 * `work_dir`, so decks never collide even in a shared cache folder. */
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
	for (char *q = stem.array; q && *q; q++) {
		if (*q == '/' || *q == '\\' || *q == ':' || *q == '*' ||
		    *q == '?' || *q == '"' || *q == '<' || *q == '>' ||
		    *q == '|')
			*q = '_';
	}

	/* Per-deck subfolder via the SHARED helper, so the source's render path
	 * (which calls the same helper) looks in exactly this directory. */
	char work_buf[1024];
	odp_deck_subdir(odp_path, cache_dir, work_buf, sizeof(work_buf));
	struct dstr work;
	dstr_init_copy(&work, work_buf);
	os_mkdirs(work.array);
	const char *work_dir = work.array;

	/* ── Fast path: reuse existing slides ────────────────────────────
	 * If we already have slide PNGs that are newer than the source .odp,
	 * the cache is current — skip the slow LibreOffice render and just
	 * report the count. Widest-padded slide-1 name first, matching the
	 * canonical full-deck render's padding. */
	if (first_page <= 1 && last_page == 0) {
		int existing = count_pngs(work_dir);
		if (existing > 0) {
			struct stat odp_st, png_st;
			struct dstr first_png;
			dstr_init(&first_png);
			bool got_png = false;
			for (int w = 6; w >= 1 && !got_png; w--) {
				dstr_printf(&first_png, "%s/slide-%0*d.png",
					    work_dir, w, 1);
				if (stat(first_png.array, &png_st) == 0)
					got_png = true;
			}
			if (got_png && stat(odp_path, &odp_st) == 0 &&
			    png_st.st_mtime >= odp_st.st_mtime) {
				res.ok = true;
				res.slide_count = existing;
				blog(LOG_INFO,
				     "[odp-presenter] reusing %d cached slides "
				     "for '%s' (no re-export needed)",
				     existing, stem.array);
				dstr_free(&first_png);
				dstr_free(&stem);
				dstr_free(&work);
				return res;
			}
			dstr_free(&first_png);
		}
	}

	struct dstr pdf_path;
	dstr_init(&pdf_path);
	dstr_printf(&pdf_path, "%s/%s.pdf", work_dir, stem.array);

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

		/* LibreOffice profile isolation.
		 *
		 * This flag is NOT optional: if the user has a LibreOffice window
		 * open, a headless soffice with the default profile tries to hand
		 * off to that running instance and then hangs (or dies). It must
		 * have a profile of its own.
		 *
		 * But the URL is fragile. Earlier we pointed it at the slide cache
		 * dir, whose path routinely contains spaces ("OBS Inputs/obs
		 * cache") — and a file:// URL with raw spaces is malformed. So:
		 *   - put the profile under the plugin's own config directory,
		 *     which we control and can keep space-free;
		 *   - percent-encode anything left over (spaces in the Windows
		 *     user name, etc.) so the URL is always well-formed;
		 *   - and on Windows prepend the extra '/' for file:///C:/...
		 */
		char *cfg = obs_module_config_path("lo_profile");
		struct dstr profdir;
		dstr_init(&profdir);
		if (cfg && *cfg) {
			dstr_copy(&profdir, cfg);
		} else {
			/* Fall back to the cache dir if config path is absent. */
			dstr_printf(&profdir, "%s/lo_profile", cache_dir);
		}
		bfree(cfg);
		dstr_replace(&profdir, "\\", "/");
		os_mkdirs(profdir.array);

		struct dstr envarg;
		dstr_init(&envarg);
		dstr_copy(&envarg, "-env:UserInstallation=file://");
		if (profdir.array[0] != '/')
			dstr_cat(&envarg, "/"); /* file:///C:/... on Windows */
		for (const char *c = profdir.array; *c; c++) {
			if (*c == ' ')
				dstr_cat(&envarg, "%20");
			else
				dstr_ncat(&envarg, c, 1);
		}

		const char *argv[] = {
			odp_tool_libreoffice(),
			envarg.array,
			"--headless",
			"--norestore",
			"--convert-to",
			"pdf",
			"--outdir",
			work_dir,
			odp_path,
			NULL,
		};

		blog(LOG_INFO,
		     "[odp-presenter] running: %s %s --headless --norestore "
		     "--convert-to pdf --outdir '%s' '%s'",
		     odp_tool_libreoffice(), envarg.array, work_dir, odp_path);

		/* Only one LibreOffice conversion at a time across the plugin
		 * (see s_libreoffice_lock). Without this, two decks or a
		 * save-triggered re-export can launch concurrent soffice
		 * instances that thrash and take many times longer. */
		pthread_mutex_lock(&s_libreoffice_lock);
		uint64_t t0 = os_gettime_ns();
		int rc = run_argv(argv, NULL, NULL);
		blog(LOG_INFO, "[odp-presenter] LibreOffice took %.1f s",
		     (os_gettime_ns() - t0) / 1.0e9);
		pthread_mutex_unlock(&s_libreoffice_lock);
		dstr_free(&envarg);
		dstr_free(&profdir);
		dstr_free(&profile);
		dstr_free(&logf);
		if (rc != 0) {
			snprintf(res.error, sizeof(res.error),
				 "LibreOffice exit code %d", rc);
			dstr_free(&stem);
			dstr_free(&pdf_path);
			return res;
		}

		/* The PDF was just regenerated from an edited .odp. Delete ALL
		 * existing slide PNGs now so stale pages from a previous version
		 * (especially if the deck got shorter, e.g. 156 -> 60 slides)
		 * cannot survive and inflate the count or shadow new content.
		 * The render below recreates the current set cleanly. */
		os_dir_t *d = os_opendir(work_dir);
		if (d) {
			struct os_dirent *ent;
			while ((ent = os_readdir(d)) != NULL) {
				if (ent->directory)
					continue;
				if (strncmp(ent->d_name, "slide-", 6) == 0 &&
				    strstr(ent->d_name, ".png")) {
					struct dstr p;
					dstr_init(&p);
					dstr_printf(&p, "%s/%s", work_dir,
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
		dstr_free(&work);
		dstr_free(&pdf_path);
		return res;
	}

	if (!odp_tool_pdftoppm()) {
		snprintf(res.error, sizeof(res.error),
			 "pdftoppm not found");
		dstr_free(&stem);
		dstr_free(&work);
		dstr_free(&pdf_path);
		return res;
	}

	struct dstr prefix;
	dstr_init(&prefix);
	dstr_printf(&prefix, "%s/slide", work_dir);

	/* Build pdftoppm's arguments as a real vector (no shell). Numeric
	 * arguments are rendered into small local buffers. */
	char dpi_s[16], first_s[16], last_s[16];
	snprintf(dpi_s, sizeof(dpi_s), "%d", dpi);
	snprintf(first_s, sizeof(first_s), "%d", first_page);
	snprintf(last_s, sizeof(last_s), "%d", last_page);

	const char *rargv[16];
	int n = 0;
	rargv[n++] = odp_tool_pdftoppm();
	rargv[n++] = "-r";
	rargv[n++] = dpi_s;
	rargv[n++] = "-png";
	if (first_page >= 1) {
		rargv[n++] = "-f";
		rargv[n++] = first_s;
	}
	if (last_page >= 1) {
		rargv[n++] = "-l";
		rargv[n++] = last_s;
	}
	rargv[n++] = pdf_path.array;
	rargv[n++] = prefix.array;
	rargv[n] = NULL;

	uint64_t pt0 = os_gettime_ns();
	int rc = run_argv(rargv, NULL, NULL);
	blog(LOG_INFO,
	     "[odp-presenter] pdftoppm (pages %d-%s) took %.1f s",
	     first_page >= 1 ? first_page : 1,
	     last_page >= 1 ? "range" : "end",
	     (os_gettime_ns() - pt0) / 1.0e9);
	dstr_free(&prefix);

	/* If the render failed or was killed part-way (e.g. 0xC000013A when a
	 * console window is closed), any PNGs it managed to write are an
	 * INCOMPLETE deck. Leaving them behind poisons the cache: the next load
	 * counts them and happily reports "reusing 9 cached slides" for a
	 * 60-slide deck. Wipe them so the next attempt re-renders cleanly. */
	if (rc != 0) {
		blog(LOG_WARNING,
		     "[odp-presenter] render failed (code %d) — discarding "
		     "partial slides so the cache isn't left incomplete",
		     rc);
		delete_slide_pngs(work_dir);
	}

	if (rc != 0) {
		snprintf(res.error, sizeof(res.error),
			 "pdftoppm exit code %d", rc);
		dstr_free(&stem);
		dstr_free(&work);
		dstr_free(&pdf_path);
		return res;
	}

	res.slide_count = count_pngs(work_dir);
	res.ok = res.slide_count > 0;
	if (!res.ok)
		snprintf(res.error, sizeof(res.error),
			 "no PNGs produced");

	/* After a FULL render, prune any slide PNG whose page-number width
	 * doesn't match the canonical width, so a leftover narrow name can't
	 * shadow the canonical one. Done AFTER the render so the live slide is
	 * never momentarily missing. Operates within this deck's work_dir. */
	if (res.ok && first_page <= 1 && last_page == 0) {
		int canon_w = 1;
		for (int n = res.slide_count; n >= 10; n /= 10)
			canon_w++;
		os_dir_t *d = os_opendir(work_dir);
		if (d) {
			struct os_dirent *ent;
			while ((ent = os_readdir(d)) != NULL) {
				if (ent->directory)
					continue;
				if (strncmp(ent->d_name, "slide-", 6) != 0 ||
				    !strstr(ent->d_name, ".png"))
					continue;
				const char *num = ent->d_name + 6;
				int w = 0;
				while (num[w] >= '0' && num[w] <= '9')
					w++;
				if (w != canon_w) {
					struct dstr p;
					dstr_init(&p);
					dstr_printf(&p, "%s/%s", work_dir,
						    ent->d_name);
					os_unlink(p.array);
					dstr_free(&p);
				}
			}
			os_closedir(d);
		}
	}

	dstr_free(&stem);
	dstr_free(&work);
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
				/* Run pdfinfo through the same shell-free path
				 * and capture its stdout, then find the Pages:
				 * line. */
				const char *iargv[] = {
					info.array,
					pdf_path.array,
					NULL,
				};
				struct dstr out;
				dstr_init(&out);
				run_argv(iargv, &out, NULL);
				if (out.array) {
					const char *p = strstr(out.array,
							       "Pages:");
					if (p)
						pages = atoi(p + 6);
				}
				dstr_free(&out);
			}
			dstr_free(&info);
		}
	}

	dstr_free(&stem);
	dstr_free(&pdf_path);
	return pages;
}
