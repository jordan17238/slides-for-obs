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

static bool first_existing(const char *const *candidates, char *out, size_t out_sz)
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
		"/usr/bin/libreoffice",
		"/usr/bin/soffice",
		"/usr/local/bin/libreoffice",
		NULL,
	};
	static const char *pp[] = {
		"/usr/bin/pdftoppm",
		"/usr/local/bin/pdftoppm",
		NULL,
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

	blog(LOG_INFO, "[odp-presenter] LibreOffice: %s", have_lo ? s_libreoffice : "NOT FOUND");
	blog(LOG_INFO, "[odp-presenter] pdftoppm: %s", have_pp ? s_pdftoppm : "not found");

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
		char c[2] = {*p, '\0'};
		dstr_cat(out, c);
	}
	/* trailing backslashes must be doubled before the closing quote */
	for (int i = 0; i < backslashes * 2; i++)
		dstr_cat(out, "\\");
	dstr_cat(out, "\"");
}
#endif

static int run_argv(const char *const *argv, struct dstr *out, const char *log_path)
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
	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
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
		wr = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
		if (wr == INVALID_HANDLE_VALUE)
			wr = NULL;
	}

	STARTUPINFOA si = {0};
	si.cb = sizeof(si);
	if (wr) {
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = wr;
		si.hStdError = wr;
		si.hStdInput = NULL;
	}

	PROCESS_INFORMATION pi = {0};
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
	BOOL ok = CreateProcessA(NULL, cmdline.array, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	if (wr)
		CloseHandle(wr); /* our copy; child holds the other end */

	if (!ok) {
		blog(LOG_ERROR, "[odp-presenter] CreateProcess failed (%lu) for: %s", GetLastError(), cmdline.array);
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
		blog(LOG_ERROR, "[odp-presenter] tool timed out after 5 min; terminating. "
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
		if (ent->directory || strncmp(ent->d_name, "slide-", 6) != 0 || !strstr(ent->d_name, ".png"))
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
		blog(LOG_INFO, "[odp-presenter] removed %d partial slide image(s)", removed);
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
			if (!ent->directory && strncmp(ent->d_name, "slide-", 6) == 0 && strstr(ent->d_name, ".png"))
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
void odp_deck_subdir(const char *odp_path, const char *cache_dir, char *out, size_t out_size)
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
		if (*q == '/' || *q == '\\' || *q == ':' || *q == '*' || *q == '?' || *q == '"' || *q == '<' ||
		    *q == '>' || *q == '|')
			*q = '_';
	}

	snprintf(out, out_size, "%s/%s", cache_dir, stem.array);
	dstr_free(&stem);
}

/* Straight byte-for-byte file copy.
 *
 * Deliberately built on os_fopen/fread/fwrite rather than a platform copy
 * helper: this file already uses os_fopen, so there is no risk of reaching for
 * an API that isn't there. Opening for read succeeds even while another
 * program has the file open for editing, which is exactly the case this
 * exists to serve. */
static bool copy_file_bytes(const char *src, const char *dst)
{
#if defined(_WIN32)
	/* Open the source with CreateFile, not fopen.
	 *
	 * Office applications keep the document open while you edit it, and
	 * they hold it with delete rights — saving works by writing a temp file
	 * and replacing the original. Windows only lets a second process open
	 * the file if that process PERMITS everything the first one is doing,
	 * so a reader must pass FILE_SHARE_DELETE as well as READ and WRITE.
	 * The C runtime's fopen() grants only READ|WRITE, so it fails with a
	 * sharing violation on a deck that is open in PowerPoint — which is
	 * exactly the case this copy exists to handle. */
	HANDLE in = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (in == INVALID_HANDLE_VALUE) {
		blog(LOG_WARNING, "[odp-presenter] cannot read '%s' (Windows error %lu)", src, GetLastError());
		return false;
	}

	FILE *out = os_fopen(dst, "wb");
	if (!out) {
		blog(LOG_WARNING, "[odp-presenter] cannot create '%s'", dst);
		CloseHandle(in);
		return false;
	}

	char buf[64 * 1024];
	DWORD n = 0;
	bool ok = true;
	while (ReadFile(in, buf, (DWORD)sizeof(buf), &n, NULL) && n > 0) {
		if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
			ok = false;
			break;
		}
	}

	CloseHandle(in);
	fclose(out);
#else
	FILE *in = os_fopen(src, "rb");
	if (!in) {
		blog(LOG_WARNING, "[odp-presenter] cannot read '%s'", src);
		return false;
	}

	FILE *out = os_fopen(dst, "wb");
	if (!out) {
		blog(LOG_WARNING, "[odp-presenter] cannot create '%s'", dst);
		fclose(in);
		return false;
	}

	char buf[64 * 1024];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
	}
	if (ferror(in))
		ok = false;

	fclose(in);
	fclose(out);
#endif

	if (!ok)
		os_unlink(dst); /* never leave a truncated copy behind */
	return ok;
}

/* ---- parallel PDF -> PNG rendering ------------------------------------- */

/* Upper bound on concurrent pdftoppm processes, whatever the user asks for. */
#define ODP_MAX_RENDER_JOBS 32

/* One slice of the page range, rendered by its own pdftoppm process. */
struct render_job {
	const char *pdf;
	const char *dir;
	int dpi;
	int first;
	int last; /* 0 = render through to the end of the document */
	int index;
	int rc;
	pthread_t thread;
	bool started;
};

/* Render one slice. Each job writes with its OWN filename prefix (w0-, w1-,
 * ...) so concurrent processes can never race on the same output file; the
 * files are renamed to their canonical names afterwards. */
static void *render_job_fn(void *param)
{
	struct render_job *j = (struct render_job *)param;

	char dpi_s[16], first_s[16], last_s[16];
	snprintf(dpi_s, sizeof(dpi_s), "%d", j->dpi);
	snprintf(first_s, sizeof(first_s), "%d", j->first);
	snprintf(last_s, sizeof(last_s), "%d", j->last);

	struct dstr prefix;
	dstr_init(&prefix);
	dstr_printf(&prefix, "%s/w%d", j->dir, j->index);

	const char *argv[16];
	int n = 0;
	argv[n++] = odp_tool_pdftoppm();
	argv[n++] = "-r";
	argv[n++] = dpi_s;
	argv[n++] = "-png";
	if (j->first >= 1) {
		argv[n++] = "-f";
		argv[n++] = first_s;
	}
	if (j->last >= 1) {
		argv[n++] = "-l";
		argv[n++] = last_s;
	}
	argv[n++] = j->pdf;
	argv[n++] = prefix.array;
	argv[n] = NULL;

	j->rc = run_argv(argv, NULL, NULL);
	dstr_free(&prefix);
	return NULL;
}

/* If `name` looks like "w<job>-<page>.png", return <page>; otherwise 0. */
static int worker_png_page(const char *name)
{
	if (!name || name[0] != 'w')
		return 0;
	const char *p = name + 1;
	if (*p < '0' || *p > '9')
		return 0;
	while (*p >= '0' && *p <= '9')
		p++;
	if (*p != '-')
		return 0;
	p++;
	if (*p < '0' || *p > '9')
		return 0;
	int page = atoi(p);
	while (*p >= '0' && *p <= '9')
		p++;
	if (strcmp(p, ".png") != 0)
		return 0;
	return page;
}

/* Rename every wN-<page>.png produced by the render jobs to the canonical
 * slide-<page>.png, zero-padded to one consistent width.
 *
 * This exists because pdftoppm pads page numbers to the width of the highest
 * page in ITS OWN invocation — so parallel slices would otherwise disagree
 * (slide-7.png from one chunk, slide-007.png from another). Normalising here
 * means the rest of the plugin only ever sees one naming scheme, no matter how
 * the work was divided or what pdftoppm decided to do.
 *
 * Returns the number of files renamed. */
static int normalise_render_output(const char *dir, int total_pages)
{
	os_dir_t *d = os_opendir(dir);
	if (!d)
		return 0;

	/* Collect first, rename after: mutating a directory while iterating it
	 * is not portable. */
	char **names = NULL;
	size_t count = 0, cap = 0;
	int max_page = total_pages > 0 ? total_pages : 0;

	struct os_dirent *ent;
	while ((ent = os_readdir(d)) != NULL) {
		if (ent->directory)
			continue;
		int page = worker_png_page(ent->d_name);
		if (page < 1)
			continue;
		if (count == cap) {
			cap = cap ? cap * 2 : 64;
			names = brealloc(names, cap * sizeof(char *));
		}
		names[count++] = bstrdup(ent->d_name);
		if (page > max_page)
			max_page = page;
	}
	os_closedir(d);

	if (!count) {
		bfree(names);
		return 0;
	}

	int width = 1;
	for (int n = max_page; n >= 10; n /= 10)
		width++;

	int moved = 0;
	for (size_t i = 0; i < count; i++) {
		int page = worker_png_page(names[i]);
		struct dstr src, dst;
		dstr_init(&src);
		dstr_init(&dst);
		dstr_printf(&src, "%s/%s", dir, names[i]);
		dstr_printf(&dst, "%s/slide-%0*d.png", dir, width, page);
		/* rename() will not replace an existing file on Windows. */
		os_unlink(dst.array);
		if (os_rename(src.array, dst.array) == 0)
			moved++;
		else
			os_unlink(src.array); /* don't leave a stray wN- file */
		dstr_free(&src);
		dstr_free(&dst);
		bfree(names[i]);
	}
	bfree(names);
	return moved;
}

/* Page count of an existing PDF via `pdfinfo` (ships with poppler alongside
 * pdftoppm). Returns 0 if it can't be determined. */
static int pdf_page_count_at(const char *pdf_path)
{
	if (!pdf_path || !*pdf_path || !file_exists(pdf_path))
		return 0;

	const char *ppm = odp_tool_pdftoppm();
	if (!ppm)
		return 0;

	/* pdfinfo lives next to pdftoppm. Keep the directory part and swap the
	 * executable name (on Windows the ".exe" suffix matters — without it
	 * file_exists() misses the bundled pdfinfo.exe and counting silently
	 * fails). */
	struct dstr info;
	dstr_init_copy(&info, ppm);
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

	int pages = 0;
	if (file_exists(info.array)) {
		const char *iargv[] = {info.array, pdf_path, NULL};
		struct dstr out;
		dstr_init(&out);
		run_argv(iargv, &out, NULL);
		if (out.array) {
			const char *p = strstr(out.array, "Pages:");
			if (p)
				pages = atoi(p + 6);
		}
		dstr_free(&out);
	}
	dstr_free(&info);
	return pages;
}

/* Number of usable cached slides for this deck, or 0 if a render is needed.
 * Performs no rendering — it is cheap enough to call before deciding how to
 * export. */
static int cache_fresh_count(const char *work_dir, const char *odp_path, const char *pdf_path)
{
	int existing = count_pngs(work_dir);
	if (existing <= 0)
		return 0;

	/* Slide 1 must be at least as new as the presentation, otherwise the
	 * deck was edited after we rendered. Probe widest-padded name first. */
	struct stat odp_st, png_st;
	struct dstr first_png;
	dstr_init(&first_png);
	bool got_png = false;
	for (int w = 6; w >= 1 && !got_png; w--) {
		dstr_printf(&first_png, "%s/slide-%0*d.png", work_dir, w, 1);
		if (stat(first_png.array, &png_st) == 0)
			got_png = true;
	}
	dstr_free(&first_png);
	if (!got_png)
		return 0;
	if (stat(odp_path, &odp_st) != 0 || png_st.st_mtime < odp_st.st_mtime)
		return 0;

	/* The cache must also be COMPLETE. Comparing the PNG count against the
	 * PDF's real page count catches a half-finished render — a pdftoppm
	 * that was killed, or the single page written by stage 1 of a staged
	 * render — which would otherwise be mistaken for a whole deck (the
	 * "reusing 9 cached slides" report for a 60-slide deck). */
	int pages = pdf_page_count_at(pdf_path);
	if (pages > 0 && existing != pages)
		return 0;

	return existing;
}

int odp_cached_slide_count(const char *odp_path, const char *cache_dir)
{
	if (!odp_path || !*odp_path || !cache_dir || !*cache_dir)
		return 0;

	char work[1024];
	odp_deck_subdir(odp_path, cache_dir, work, sizeof(work));

	/* work_dir ends in the deck's stem, and the PDF sits inside it under
	 * that same stem — so the PDF path can be derived without recomputing
	 * the stem. */
	const char *stem = strrchr(work, '/');
	if (!stem)
		return 0;
	struct dstr pdf;
	dstr_init(&pdf);
	dstr_printf(&pdf, "%s/%s.pdf", work, stem + 1);
	int n = cache_fresh_count(work, odp_path, pdf.array);
	dstr_free(&pdf);
	return n;
}

odp_export_result odp_export(const char *odp_path, const char *cache_dir, int dpi, int workers)
{
	return odp_export_range(odp_path, cache_dir, dpi, workers, 1, 0, true);
}

odp_export_result odp_export_range(const char *odp_path, const char *cache_dir, int dpi, int workers, int first_page,
				   int last_page, bool use_cache)
{
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
		snprintf(res.error, sizeof(res.error), "LibreOffice not found");
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
		if (*q == '/' || *q == '\\' || *q == ':' || *q == '*' || *q == '?' || *q == '"' || *q == '<' ||
		    *q == '>' || *q == '|')
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

	struct dstr pdf_path;
	dstr_init(&pdf_path);
	dstr_printf(&pdf_path, "%s/%s.pdf", work_dir, stem.array);

	/* ── Fast path: reuse existing slides ────────────────────────────
	 * If the cached PNGs are newer than the source presentation AND there
	 * are as many of them as the PDF has pages, the cache is current and
	 * complete — skip LibreOffice and pdftoppm entirely and just report
	 * the count. */
	if (use_cache && first_page <= 1 && last_page == 0) {
		int fresh = cache_fresh_count(work_dir, odp_path, pdf_path.array);
		if (fresh > 0) {
			res.ok = true;
			res.slide_count = fresh;
			blog(LOG_INFO,
			     "[odp-presenter] reusing %d cached slides "
			     "for '%s' (no re-export needed)",
			     fresh, stem.array);
			dstr_free(&stem);
			dstr_free(&work);
			dstr_free(&pdf_path);
			return res;
		}
	}

	/* Skip the (slow) LibreOffice conversion if a PDF already exists and is
	 * at least as new as the .odp. This makes the tail render of the staged
	 * flow nearly free, and avoids re-launching LibreOffice for the second
	 * page-range call. */
	bool pdf_fresh = false;
	{
		struct stat pst, ost;
		if (stat(pdf_path.array, &pst) == 0 && stat(odp_path, &ost) == 0 && pst.st_mtime >= ost.st_mtime)
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

		/* ── Convert a private COPY, not the original ────────────────
		 *
		 * When the presentation is open in PowerPoint (or Impress), the
		 * editor leaves a hidden lock file beside it named "~$<filename>".
		 * LibreOffice honours that lock: it decides the document is in
		 * use, tries to ask the user what to do, and — with --headless
		 * there is nobody to ask — gives up immediately with exit code 1.
		 * That is why a deck that converts perfectly when closed fails in
		 * half a second while you have it open for editing.
		 *
		 * Copying the deck into our own working folder sidesteps this
		 * entirely: the copy has no "~$" lock file next to it, so
		 * LibreOffice just opens it. The copy also forces a cloud-synced
		 * file (OneDrive and similar) to be fully materialised on disk
		 * before we hand it over.
		 *
		 * The copy keeps the SAME filename stem, so --convert-to still
		 * writes <stem>.pdf into work_dir exactly as before. If the copy
		 * fails for any reason we fall back to converting the original,
		 * which is no worse than the previous behaviour. */
		struct dstr src_copy;
		dstr_init(&src_copy);
		const char *ext = strrchr(base, '.');
		dstr_printf(&src_copy, "%s/%s%s", work_dir, stem.array, ext ? ext : "");

		const char *convert_input = odp_path;
		if (strcmp(src_copy.array, odp_path) != 0) {
			os_unlink(src_copy.array); /* replace any previous copy */
			if (copy_file_bytes(odp_path, src_copy.array)) {
				convert_input = src_copy.array;
			} else {
				blog(LOG_WARNING,
				     "[odp-presenter] could not copy the presentation "
				     "aside (%s) — converting the original; if it is "
				     "open in another program this may fail",
				     src_copy.array);
			}
		}

		const char *argv[] = {
			odp_tool_libreoffice(), envarg.array, "--headless", "--norestore",
			"--convert-to",         "pdf",        "--outdir",   work_dir,
			convert_input,          NULL,
		};

		blog(LOG_INFO,
		     "[odp-presenter] running: %s %s --headless --norestore "
		     "--convert-to pdf --outdir '%s' '%s'",
		     odp_tool_libreoffice(), envarg.array, work_dir, convert_input);

		/* Only one LibreOffice conversion at a time across the plugin
		 * (see s_libreoffice_lock). Without this, two decks or a
		 * save-triggered re-export can launch concurrent soffice
		 * instances that thrash and take many times longer. */
		pthread_mutex_lock(&s_libreoffice_lock);
		uint64_t t0 = os_gettime_ns();
		int rc = run_argv(argv, NULL, NULL);
		blog(LOG_INFO, "[odp-presenter] LibreOffice took %.1f s", (os_gettime_ns() - t0) / 1.0e9);
		pthread_mutex_unlock(&s_libreoffice_lock);

		/* Drop the working copy immediately. The cache folder often sits
		 * in a cloud-synced location, and leaving a full duplicate of
		 * every deck there would sync for no reason. */
		if (convert_input == src_copy.array)
			os_unlink(src_copy.array);
		dstr_free(&src_copy);
		dstr_free(&envarg);
		dstr_free(&profdir);
		dstr_free(&profile);
		dstr_free(&logf);
		if (rc != 0) {
			snprintf(res.error, sizeof(res.error), "LibreOffice exit code %d", rc);
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
				if (strncmp(ent->d_name, "slide-", 6) == 0 && strstr(ent->d_name, ".png")) {
					struct dstr p;
					dstr_init(&p);
					dstr_printf(&p, "%s/%s", work_dir, ent->d_name);
					os_unlink(p.array);
					dstr_free(&p);
				}
			}
			os_closedir(d);
		}
	} else {
		blog(LOG_INFO, "[odp-presenter] PDF already fresh, skipping LibreOffice");
	}

	if (!file_exists(pdf_path.array)) {
		snprintf(res.error, sizeof(res.error), "PDF not produced (%s)", pdf_path.array);
		dstr_free(&stem);
		dstr_free(&work);
		dstr_free(&pdf_path);
		return res;
	}

	if (!odp_tool_pdftoppm()) {
		snprintf(res.error, sizeof(res.error), "pdftoppm not found");
		dstr_free(&stem);
		dstr_free(&work);
		dstr_free(&pdf_path);
		return res;
	}

	/* ── Stage 2: PDF -> PNGs, rendered in PARALLEL ──────────────────
	 *
	 * pdftoppm is single-threaded: one process rasterises pages one after
	 * another, so a long deck spends nearly all of this stage using a
	 * single core. Splitting the page range across `workers` processes puts
	 * the whole CPU to work and is close to a linear speed-up here.
	 *
	 * (This is what the "Parallel render workers" setting always claimed to
	 * do; until now the value was accepted and ignored.) */
	int total_pages = pdf_page_count_at(pdf_path.array);

	int lo = (first_page >= 1) ? first_page : 1;
	int hi = (last_page >= 1) ? last_page : total_pages;

	/* Without a page count and without an explicit last page there is
	 * nothing to divide, so fall back to a single open-ended job that
	 * renders through to the end of the document — exactly the old
	 * behaviour, and still correct. */
	bool open_ended = (hi < lo);

	int njobs = workers;
	if (njobs < 1)
		njobs = 1;
	if (njobs > ODP_MAX_RENDER_JOBS)
		njobs = ODP_MAX_RENDER_JOBS;
	if (open_ended)
		njobs = 1;
	else if (njobs > (hi - lo + 1))
		njobs = hi - lo + 1;

	struct render_job jobs[ODP_MAX_RENDER_JOBS];
	memset(jobs, 0, sizeof(jobs));

	int span = open_ended ? 0 : (hi - lo + 1);
	int per = (span + njobs - 1) / njobs; /* ceiling division */

	if (open_ended) {
		jobs[0].pdf = pdf_path.array;
		jobs[0].dir = work_dir;
		jobs[0].dpi = dpi;
		jobs[0].index = 0;
		jobs[0].first = lo;
		jobs[0].last = 0; /* through to the end of the document */
		njobs = 1;
	} else {
		/* Ceiling division can exhaust the pages before the last worker
		 * gets a slice (5 pages over 4 workers is 2 each, so the fourth
		 * would start at page 7 of a 5-page deck). Stop as soon as a
		 * slice would start past the end rather than handing pdftoppm a
		 * backwards range, which it rejects — failing the whole render
		 * and discarding a deck that was otherwise fine. */
		int assigned = 0;
		for (int i = 0; i < njobs; i++) {
			int f = lo + i * per;
			if (f > hi)
				break;
			int l = f + per - 1;
			if (l > hi)
				l = hi;
			jobs[assigned].pdf = pdf_path.array;
			jobs[assigned].dir = work_dir;
			jobs[assigned].dpi = dpi;
			jobs[assigned].index = assigned;
			jobs[assigned].first = f;
			jobs[assigned].last = l;
			assigned++;
		}
		njobs = assigned;
	}

	uint64_t pt0 = os_gettime_ns();

	/* Run job 0 on this thread, so a single worker costs no thread at all. */
	for (int i = 1; i < njobs; i++) {
		if (pthread_create(&jobs[i].thread, NULL, render_job_fn, &jobs[i]) == 0)
			jobs[i].started = true;
		else
			jobs[i].rc = -1;
	}
	render_job_fn(&jobs[0]);
	for (int i = 1; i < njobs; i++) {
		if (jobs[i].started)
			pthread_join(jobs[i].thread, NULL);
	}

	int rc = 0;
	for (int i = 0; i < njobs; i++) {
		if (jobs[i].rc != 0) {
			rc = jobs[i].rc;
			break;
		}
	}

	/* Give every rendered page its canonical slide-NNN.png name. */
	int produced = normalise_render_output(work_dir, total_pages);

	blog(LOG_INFO, "[odp-presenter] rendered %d page(s) across %d worker(s) in %.1f s", produced, njobs,
	     (os_gettime_ns() - pt0) / 1.0e9);

	/* If any slice failed or was killed part-way, what survives is an
	 * INCOMPLETE deck. Leaving it behind poisons the cache, so wipe it and
	 * let the next attempt render cleanly. */
	if (rc != 0) {
		blog(LOG_WARNING,
		     "[odp-presenter] render failed (code %d) — discarding "
		     "partial slides so the cache isn't left incomplete",
		     rc);
		delete_slide_pngs(work_dir);
		snprintf(res.error, sizeof(res.error), "pdftoppm exit code %d", rc);
		dstr_free(&stem);
		dstr_free(&work);
		dstr_free(&pdf_path);
		return res;
	}

	res.slide_count = count_pngs(work_dir);
	res.ok = res.slide_count > 0;
	if (!res.ok)
		snprintf(res.error, sizeof(res.error), "no PNGs produced");

	dstr_free(&stem);
	dstr_free(&work);
	dstr_free(&pdf_path);
	return res;
}

/* Page count of the deck's cached PDF. Thin wrapper over pdf_page_count_at()
 * that resolves the per-deck working folder first, so it agrees with where the
 * exporter actually writes. */
int odp_pdf_page_count(const char *odp_path, const char *cache_dir)
{
	if (!odp_path || !*odp_path || !cache_dir || !*cache_dir)
		return 0;

	char work[1024];
	odp_deck_subdir(odp_path, cache_dir, work, sizeof(work));

	const char *stem = strrchr(work, '/');
	if (!stem)
		return 0;

	struct dstr pdf;
	dstr_init(&pdf);
	dstr_printf(&pdf, "%s/%s.pdf", work, stem + 1);
	int pages = pdf_page_count_at(pdf.array);
	dstr_free(&pdf);
	return pages;
}
