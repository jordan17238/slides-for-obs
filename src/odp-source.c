/*
 * odp-source.c — the "ODP Presentation" OBS source type
 *
 * Design (matches the proven Python approach):
 *   • The source owns a cache directory of slide-XXXX.png files.
 *   • It internally hosts a child "image_source" that displays the
 *     current slide PNG. We don't reinvent image loading — we delegate
 *     to OBS's own image source and just swap its file path.
 *   • A background thread runs the LibreOffice -> PDF -> PNG export so
 *     the UI/graphics threads never block.
 *   • Hotkeys advance/retreat the current slide index.
 *
 * This file is intentionally written to be readable at a "some C++"
 * level: heavy logic is commented and kept linear.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <graphics/graphics.h>

#include "odp-export.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

/* ---- per-instance state ------------------------------------------------ */

struct odp_source {
	obs_source_t *self;        /* the source OBS gave us */

	/* --- Off-thread slide decode/display ---------------------------------
	 * The dropped frame was caused by OBS's image_source decoding the PNG
	 * synchronously on the graphics thread during navigation. We now do the
	 * expensive decode on our OWN background thread (decode_thread) and only
	 * do the cheap GPU upload on the graphics thread, so rendering never
	 * blocks on a decode.
	 *
	 * Flow:
	 *   navigate -> set want_index, signal decode_cond
	 *   decode_thread -> decodes want_index to a raw RGBA buffer (pending_*),
	 *                    sets pending_ready
	 *   odp_video_render (graphics thread) -> if pending_ready, upload buffer
	 *                    into `tex` (fast), then draw `tex`
	 *
	 * All the pending_* fields and want_index are guarded by decode_lock. */
	pthread_t       decode_thread;
	bool            decode_started;   /* decode_thread is joinable */
	bool            decode_exit;      /* ask decode_thread to exit (destroy) */
	pthread_mutex_t decode_lock;
	pthread_cond_t  decode_cond;
	int             want_index;       /* slide the decode thread should load */
	int             decoded_index;    /* slide currently in pending buffer */

	/* Handoff buffer: raw pixels produced by the decode thread. */
	uint8_t        *pending_data;     /* bmalloc'd RGBA pixels, or NULL */
	uint32_t        pending_cx;
	uint32_t        pending_cy;
	enum gs_color_format pending_format;
	bool            pending_ready;    /* a new buffer awaits GPU upload */

	/* Graphics-thread-owned texture (only touched in render). */
	gs_texture_t   *tex;
	int             tex_index;        /* slide currently uploaded to `tex` */
	uint32_t        tex_cx;
	uint32_t        tex_cy;

	int shown_index;          /* slide last presented (for tick comparison) */

	/* settings */
	struct dstr odp_path;      /* path to the .odp file */
	struct dstr cache_dir;     /* where slide PNGs are written */
	int dpi;
	int workers;

	/* slide state */
	int slide_count;
	int current_index;

	/* export worker */
	pthread_t worker;
	bool worker_running;   /* a worker thread is currently executing */
	bool worker_started;   /* s->worker holds a joinable thread handle */
	bool reload_requested; /* ask the running worker to restart */
	bool abort_requested;  /* ask the running worker to exit ASAP (destroy) */
	pthread_mutex_t lock;

	/* auto-refresh: poll the .odp mtime and re-export when it changes */
	time_t odp_mtime;        /* last-seen modification time of the .odp */
	float  poll_accum;       /* seconds accumulated toward next poll */

	/* hotkeys */
	obs_hotkey_id hk_next;
	obs_hotkey_id hk_prev;
	obs_hotkey_id hk_first;
	obs_hotkey_id hk_last;
	obs_hotkey_id hk_refresh;
};

/* ---- live-source registry --------------------------------------------------
 * The smart global hotkeys (registered in plugin-main.c) need to find the
 * ODP source that is currently visible in the program scene. We keep a simple
 * registry of all live instances, guarded by a mutex. */

#define ODP_MAX_SOURCES 64
static struct odp_source *g_sources[ODP_MAX_SOURCES];
static int g_source_count = 0;
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;

static void registry_add(struct odp_source *s)
{
	pthread_mutex_lock(&g_registry_lock);
	if (g_source_count < ODP_MAX_SOURCES)
		g_sources[g_source_count++] = s;
	pthread_mutex_unlock(&g_registry_lock);
}

static void registry_remove(struct odp_source *s)
{
	pthread_mutex_lock(&g_registry_lock);
	for (int i = 0; i < g_source_count; i++) {
		if (g_sources[i] == s) {
			g_sources[i] = g_sources[--g_source_count];
			break;
		}
	}
	pthread_mutex_unlock(&g_registry_lock);
}

/* ---- helpers ----------------------------------------------------------- */

/* Build the absolute path of a given slide index (1-based).
 *
 * pdftoppm zero-pads the page number to the width of the total page count,
 * so a 156-slide deck yields slide-001.png while a 9-slide deck yields
 * slide-1.png. Rather than assume a fixed width, probe the common widths
 * (1..6 digits) and return the first file that actually exists. Falls back
 * to a 4-digit name if none are found (so callers still get a usable path
 * for logging). */
static void slide_png_path(struct odp_source *s, int index1, struct dstr *out)
{
	dstr_init(out);
	/* Probe widest-first. pdftoppm pads page numbers to the width of the
	 * highest page in a given call, so a full-deck render (e.g. 156 pages)
	 * writes 3-digit names while a small focused render writes fewer. By
	 * checking wide names first we always prefer the full deck's canonical
	 * file over any narrower leftover, avoiding stale-image mismatches. */
	for (int width = 6; width >= 1; width--) {
		dstr_printf(out, "%s/slide-%0*d.png",
			    s->cache_dir.array, width, index1);
		if (os_file_exists(out->array))
			return;
	}
	/* Nothing matched — leave a sane default path. */
	dstr_printf(out, "%s/slide-%04d.png", s->cache_dir.array, index1);
}

/* Request that slide `index` be shown. Cheap: just records the wanted index
 * and wakes the decode thread. Safe to call from any thread (tick, hotkey).
 * The actual decode happens on decode_thread; the upload happens in render. */
static void request_slide(struct odp_source *s, int index)
{
	if (s->slide_count <= 0)
		return;
	if (index < 1)
		index = 1;
	if (index > s->slide_count)
		index = s->slide_count;

	pthread_mutex_lock(&s->decode_lock);
	s->want_index = index;
	pthread_cond_signal(&s->decode_cond);
	pthread_mutex_unlock(&s->decode_lock);

	s->shown_index = index; /* tick uses this to avoid re-requesting */
}

/* Background decode thread: waits for want_index to change, decodes that
 * slide's PNG to a raw RGBA buffer with gs_create_texture_file_data2 (pure CPU
 * work — this is the expensive step we moved OFF the graphics thread), and
 * publishes it via the pending_* fields for the graphics thread to upload.
 * Re-decodes when asked to repeat the same index (force == reload after edit
 * is handled by always re-decoding whatever want_index holds when signalled). */
static void *decode_thread_fn(void *param)
{
	struct odp_source *s = param;

	pthread_mutex_lock(&s->decode_lock);
	while (!s->decode_exit) {
		int want = s->want_index;

		/* Nothing new to do: wait. (decoded_index tracks what we last
		 * produced; if it already matches want, sleep until signalled.) */
		if (want == s->decoded_index || want < 1) {
			pthread_cond_wait(&s->decode_cond, &s->decode_lock);
			continue;
		}

		/* Snapshot what we need, then decode WITHOUT holding the lock
		 * (decode is slow; we must not block navigation/render). */
		int idx = want;
		struct dstr path;
		dstr_init(&path);
		pthread_mutex_unlock(&s->decode_lock);

		slide_png_path(s, idx, &path);

		enum gs_color_format fmt = GS_RGBA;
		uint32_t cx = 0, cy = 0;
		/* Decode with STRAIGHT alpha: obs_source_draw() uses the default
		 * (non-premultiplied) effect, so the pixel data must be straight
		 * alpha or the image draws wrong (e.g. black). */
		uint8_t *data = gs_create_texture_file_data2(
			path.array, GS_IMAGE_ALPHA_STRAIGHT, &fmt, &cx, &cy);
		dstr_free(&path);

		pthread_mutex_lock(&s->decode_lock);
		if (s->decode_exit) {
			if (data)
				bfree(data);
			break;
		}
		if (data && cx > 0 && cy > 0) {
			/* Replace any unconsumed previous buffer. */
			if (s->pending_data)
				bfree(s->pending_data);
			s->pending_data = data;
			s->pending_cx = cx;
			s->pending_cy = cy;
			s->pending_format = fmt;
			s->pending_ready = true;
			s->decoded_index = idx;
			blog(LOG_DEBUG,
			     "[odp-presenter] decoded slide %d (%ux%u)",
			     idx, cx, cy);
		} else {
			if (data)
				bfree(data);
			/* Mark as "done" so we don't spin retrying a bad file. */
			s->decoded_index = idx;
			blog(LOG_WARNING,
			     "[odp-presenter] decode failed for slide %d", idx);
		}
		/* Loop: if want_index changed again while we decoded, we'll
		 * pick it up on the next iteration. */
	}
	pthread_mutex_unlock(&s->decode_lock);
	return NULL;
}

/* ---- export worker thread --------------------------------------------- */

static void *export_thread(void *param)
{
	struct odp_source *s = param;

	pthread_mutex_lock(&s->lock);
	struct dstr odp = {0};
	struct dstr cache = {0};
	dstr_copy(&odp, s->odp_path.array ? s->odp_path.array : "");
	dstr_copy(&cache, s->cache_dir.array ? s->cache_dir.array : "");
	int dpi = s->dpi;
	int workers = s->workers;
	pthread_mutex_unlock(&s->lock);

	if (!odp.array || !*odp.array) {
		blog(LOG_WARNING, "[odp-presenter] no .odp path set");
		s->worker_running = false;
		dstr_free(&odp);
		dstr_free(&cache);
		return NULL;
	}

	/* If no cache dir was set, derive a safe default in a PERSISTENT
	 * location so we never call os_mkdirs() with an empty/NULL path (which
	 * crashes) AND so the cached PNGs survive reboots / idle time.
	 *
	 * Earlier this used $TMPDIR, but macOS treats /var/folders/.../T as a
	 * temporary area and purges it on reboot and after a few idle days.
	 * That silently wiped the slide cache, forcing a slow full LibreOffice
	 * re-export on the next load. Application Support is not purged, so a
	 * converted deck stays converted until the .odp itself changes. */
	if (!cache.array || !*cache.array) {
		/* Per-source subfolder from the .odp filename stem so multiple
		 * sources never share a cache dir and clobber each other. */
		const char *base = strrchr(odp.array, '/');
		base = base ? base + 1 : odp.array;
		struct dstr stem;
		dstr_init_copy(&stem, base);
		char *dot = strrchr(stem.array, '.');
		if (dot)
			*dot = '\0';
		/* sanitise spaces/odd chars to underscores for a clean path */
		for (char *q = stem.array; q && *q; q++) {
			if (*q == ' ' || *q == '/' || *q == '\\')
				*q = '_';
		}

		/* os_get_config_path resolves to the per-user config area
		 * (~/Library/Application Support on macOS), which persists.
		 * Group our caches under an obs-studio plugin_config subtree. */
		char *cfg = obs_module_config_path(stem.array);
		if (cfg && *cfg) {
			dstr_copy(&cache, cfg);
		} else {
			/* Last-resort fallback: still better than nothing. */
			const char *tmp = getenv("TMPDIR");
			if (!tmp || !*tmp)
				tmp = "/tmp";
			dstr_printf(&cache, "%s/obs_odp_slides/%s", tmp,
				    stem.array);
		}
		bfree(cfg);
		dstr_free(&stem);

		blog(LOG_INFO, "[odp-presenter] no cache dir set, using '%s'",
		     cache.array);
		/* Persist the resolved default back to the source so the render
		 * path (slide_png_path) looks in the SAME folder we export to. */
		pthread_mutex_lock(&s->lock);
		dstr_copy(&s->cache_dir, cache.array);
		pthread_mutex_unlock(&s->lock);
	}

	if (os_mkdirs(cache.array) < 0) {
		blog(LOG_ERROR, "[odp-presenter] could not create cache dir '%s'",
		     cache.array);
		s->worker_running = false;
		dstr_free(&odp);
		dstr_free(&cache);
		return NULL;
	}

	blog(LOG_INFO, "[odp-presenter] exporting '%s' -> '%s'",
	     odp.array, cache.array);

	/* Render strategy: a single full-deck render. pdftoppm rasterises the
	 * entire deck in ONE process (far faster than one process per page),
	 * and odp_export_range handles the slow ODP->PDF LibreOffice step only
	 * when the .odp is newer than the cached PDF. When LibreOffice does
	 * re-run, it first clears stale slide PNGs so a shortened deck (e.g.
	 * 156 -> 60 slides) can't leave orphaned pages behind. */

	/* Single full-deck render. pdftoppm rasterises the whole deck in one
	 * process, which on a ~60-slide deck is only a few seconds — not worth
	 * the complexity (and cache-coherence bugs) of a separate focus batch.
	 * odp_export_range clears stale PNGs and re-runs LibreOffice whenever
	 * the .odp is newer than the cache, so edits always re-render fully. */
	odp_export_result r = odp_export_range(odp.array, cache.array, dpi,
					       workers, 1, 0);

	if (r.ok) {
		struct stat st;
		time_t mt = (stat(odp.array, &st) == 0) ? st.st_mtime : 0;
		pthread_mutex_lock(&s->lock);
		s->slide_count = r.slide_count;
		if (s->current_index < 1)
			s->current_index = 1;
		s->shown_index = 0; /* force video_tick to (re)request the slide */
		pthread_mutex_unlock(&s->lock);
		/* The PNGs were (re)generated, so the decode thread's cached
		 * decoded_index is now stale. Reset it under decode_lock so the
		 * next request re-decodes the fresh bytes even if the index is
		 * unchanged (the refresh-after-edit case). */
		pthread_mutex_lock(&s->decode_lock);
		s->decoded_index = 0;
		pthread_mutex_unlock(&s->decode_lock);
		pthread_mutex_lock(&s->lock);
		s->odp_mtime = mt;
		pthread_mutex_unlock(&s->lock);
		blog(LOG_INFO,
		     "[odp-presenter] full deck ready: %d slides",
		     r.slide_count);
	} else {
		blog(LOG_ERROR, "[odp-presenter] export failed: %s", r.error);
	}

	dstr_free(&odp);
	dstr_free(&cache);
	s->worker_running = false;
	return NULL;
}

/* Kick off an export on a background thread if one isn't already running. */
static void trigger_export(struct odp_source *s)
{
	if (s->worker_running) {
		s->reload_requested = true;
		return;
	}
	/* If a previous worker finished but was never joined, join it now so
	 * its thread resources are reclaimed before we start another. */
	if (s->worker_started) {
		pthread_join(s->worker, NULL);
		s->worker_started = false;
	}
	s->reload_requested = false;
	s->worker_running = true;
	if (pthread_create(&s->worker, NULL, export_thread, s) == 0)
		s->worker_started = true;
	else
		s->worker_running = false;
}

/* ---- hotkey callbacks -------------------------------------------------- */

/* Apply a navigation action to one source. action: 0=next 1=prev 2=first
 * 3=last 4=reload. Safe to call from any thread. */
static void odp_navigate(struct odp_source *s, int action)
{
	if (!s)
		return;
	if (action == 4) {
		trigger_export(s);
		return;
	}
	if (s->slide_count <= 0)
		return;
	int idx = s->current_index;
	switch (action) {
	case 0: idx++; break;
	case 1: idx--; break;
	case 2: idx = 1; break;
	case 3: idx = s->slide_count; break;
	default: return;
	}
	/* Clamp HERE, before storing, so pressing Forward on the last slide
	 * (or Back on the first) leaves current_index unchanged. Otherwise the
	 * index would briefly exceed the range, video_tick would see
	 * current_index != shown_index and call show_current_slide, which
	 * re-loads the SAME slide's PNG every click — needless decode work that
	 * spikes render time even though nothing visibly changes. */
	if (idx < 1)
		idx = 1;
	if (idx > s->slide_count)
		idx = s->slide_count;
	s->current_index = idx;
	/* Don't load the slide here on the input thread — just update the
	 * index. odp_video_tick() runs on OBS's graphics thread and will pick
	 * up the index change on the next frame and do the actual load there.
	 * Loading on the graphics thread (rather than mid-keypress/click)
	 * avoids stalling the render pipeline and dropping a frame. The one-
	 * frame latency before the new slide appears is imperceptible. */
}

/* ---- smart global navigation ------------------------------------------------
 * Each source tracks its own visibility via the .show/.hide callbacks. The
 * global hotkeys then act on the most-recently-shown visible deck. This needs
 * only libobs (no obs-frontend-api), so it builds against the plugin template's
 * prebuilt dependencies. */

/* Defined below — true if this source is in the scene the operator is driving
 * (preview scene in Studio Mode, otherwise the live/program scene). */
static bool source_is_in_active_scene(struct odp_source *s);
/* Same, but explicitly the PROGRAM (live) scene or explicitly the PREVIEW
 * scene, regardless of Studio Mode. Used by the split Live/Preview dock
 * buttons so the operator can drive either side without disturbing the other. */
static bool source_is_in_program_scene(struct odp_source *s);
static bool source_is_in_preview_scene(struct odp_source *s);

/* Internal: drive whichever deck is in the scene returned by `picker`.
 * The dock and hotkey entry points are thin wrappers around this. */
typedef bool (*scene_picker_fn)(struct odp_source *);

static void odp_navigate_in_scene(int action, scene_picker_fn picker,
				   const char *which)
{
	/* Snapshot the current sources under the lock, then release it BEFORE
	 * calling the picker, which calls into the OBS frontend API. Holding
	 * our registry lock across an OBS call could deadlock if OBS calls
	 * back into our source callbacks. */
	struct odp_source *snapshot[ODP_MAX_SOURCES];
	int n = 0;
	pthread_mutex_lock(&g_registry_lock);
	for (int i = 0; i < g_source_count; i++)
		snapshot[n++] = g_sources[i];
	int total = g_source_count;
	pthread_mutex_unlock(&g_registry_lock);

	struct odp_source *target = NULL;
	for (int i = 0; i < n; i++) {
		if (picker(snapshot[i])) {
			target = snapshot[i];
			break;
		}
	}
	/* Fallback: if no deck matched but there's only one deck, act on it.
	 * This keeps the simple single-deck case working even outside Studio
	 * Mode where preview vs program isn't meaningful. */
	if (!target && total == 1)
		target = snapshot[0];

	if (target) {
		odp_navigate(target, action);
	} else {
		blog(LOG_WARNING,
		     "[odp-presenter] dock %s: no deck in %s scene "
		     "(sources=%d)",
		     which, which, total);
	}
}

/* Public entry points used by the on-screen dock buttons (odp-dock.cpp).
 * action: 0=next 1=prev 2=first 3=last 4=reload */

/* Drives the deck in the LIVE (program) scene — the one currently going to
 * recording/output. Available even when Studio Mode is on, so the operator can
 * advance the live deck without first swapping it back to preview. */
void odp_navigate_program(int action)
{
	odp_navigate_in_scene(action, source_is_in_program_scene, "live");
}

/* Drives the deck in the PREVIEW scene (Studio Mode only). */
void odp_navigate_preview(int action)
{
	odp_navigate_in_scene(action, source_is_in_preview_scene, "preview");
}

/* Drives the "active" deck using the original rule (preview if Studio Mode,
 * else program). Kept for the per-source hotkeys, which continue to behave
 * as before. The dock now uses the explicit program/preview variants above. */
void odp_navigate_active(int action)
{
	odp_navigate_in_scene(action, source_is_in_active_scene, "active");
}

/* Helper: is `s` in the scene currently held by `scene_src`? scene_src may be
 * NULL (no such scene), in which case we return false. Does not take
 * ownership; caller releases scene_src. */
static bool source_in_scene_src(struct odp_source *s, obs_source_t *scene_src)
{
	if (!scene_src)
		return false;
	bool found = false;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene) {
		const char *name = obs_source_get_name(s->self);
		if (name && obs_scene_find_source(scene, name))
			found = true;
	}
	return found;
}

static bool source_is_in_program_scene(struct odp_source *s)
{
	obs_source_t *prog = obs_frontend_get_current_scene();
	bool found = source_in_scene_src(s, prog);
	if (prog)
		obs_source_release(prog);
	return found;
}

static bool source_is_in_preview_scene(struct odp_source *s)
{
	obs_source_t *prev = obs_frontend_get_current_preview_scene();
	bool found = source_in_scene_src(s, prev);
	if (prev)
		obs_source_release(prev);
	return found;
}

/* Returns true if this source is part of the scene the user is currently
 * "driving": the PREVIEW scene when Studio Mode is on, otherwise the live
 * PROGRAM scene. This lets the same key be bound to several decks while only
 * the one the operator is working with responds. Uses the frontend API:
 *   - obs_frontend_get_current_preview_scene() is non-NULL only in Studio Mode
 *   - obs_frontend_get_current_scene() is the live/program scene
 * Both return new references that must be released. */
static bool source_is_in_active_scene(struct odp_source *s)
{
	/* Prefer the preview scene (Studio Mode); fall back to program. */
	obs_source_t *scene_src = obs_frontend_get_current_preview_scene();
	if (!scene_src)
		scene_src = obs_frontend_get_current_scene();
	bool found = source_in_scene_src(s, scene_src);
	if (scene_src)
		obs_source_release(scene_src);
	return found;
}

static void hk_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
		  bool pressed)
{
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct odp_source *s = data;
	if (s->slide_count <= 0)
		return;

	/* Only the deck in the scene the operator is currently driving should
	 * react (preview scene in Studio Mode, otherwise the live scene). This
	 * lets the same key be bound to every deck with only one responding. */
	if (!source_is_in_active_scene(s))
		return;

	if (id == s->hk_next)
		odp_navigate(s, 0);
	else if (id == s->hk_prev)
		odp_navigate(s, 1);
	else if (id == s->hk_first)
		odp_navigate(s, 2);
	else if (id == s->hk_last)
		odp_navigate(s, 3);
	else if (id == s->hk_refresh)
		odp_navigate(s, 4);
}

/* ---- OBS source callbacks ---------------------------------------------- */

static const char *odp_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "ODP Presentation";
}

static void odp_update(void *data, obs_data_t *settings)
{
	struct odp_source *s = data;

	pthread_mutex_lock(&s->lock);
	dstr_copy(&s->odp_path, obs_data_get_string(settings, "odp_path"));
	dstr_copy(&s->cache_dir, obs_data_get_string(settings, "cache_dir"));
	s->dpi = (int)obs_data_get_int(settings, "dpi");
	s->workers = (int)obs_data_get_int(settings, "workers");
	if (s->dpi <= 0)
		s->dpi = 150;
	if (s->workers <= 0)
		s->workers = 4;
	pthread_mutex_unlock(&s->lock);

	trigger_export(s);
}

static void *odp_create(obs_data_t *settings, obs_source_t *source)
{
	struct odp_source *s = bzalloc(sizeof(struct odp_source));
	s->self = source;
	s->current_index = 1;
	pthread_mutex_init(&s->lock, NULL);
	registry_add(s);
	dstr_init(&s->odp_path);
	dstr_init(&s->cache_dir);

	/* Off-thread decode: init the handoff primitives and start the decode
	 * thread. It sleeps until request_slide() signals it. */
	pthread_mutex_init(&s->decode_lock, NULL);
	pthread_cond_init(&s->decode_cond, NULL);
	s->want_index = 0;
	s->decoded_index = 0;
	if (pthread_create(&s->decode_thread, NULL, decode_thread_fn, s) == 0)
		s->decode_started = true;
	else
		blog(LOG_ERROR,
		     "[odp-presenter] failed to start decode thread");

	/* Register hotkeys bound to this source instance. */
	s->hk_next = obs_hotkey_register_source(
		source, "odp.next", "Next Slide", hk_cb, s);
	s->hk_prev = obs_hotkey_register_source(
		source, "odp.prev", "Previous Slide", hk_cb, s);
	s->hk_first = obs_hotkey_register_source(
		source, "odp.first", "First Slide", hk_cb, s);
	s->hk_last = obs_hotkey_register_source(
		source, "odp.last", "Last Slide", hk_cb, s);
	s->hk_refresh = obs_hotkey_register_source(
		source, "odp.refresh", "Reload from disk", hk_cb, s);

	odp_update(s, settings);
	return s;
}

static void odp_destroy(void *data)
{
	struct odp_source *s = data;

	/* Remove from the registry first so the global hotkeys can no longer
	 * pick this source while we tear it down. */
	registry_remove(s);

	/* Tell any running export worker to stop, then WAIT for it to finish.
	 * The worker writes into this struct, so it must be fully stopped
	 * before we free anything — otherwise it reads freed memory and the
	 * process crashes (e.g. when a source is removed mid-export). */
	s->abort_requested = true;
	s->reload_requested = true;
	if (s->worker_started) {
		pthread_join(s->worker, NULL);
		s->worker_started = false;
	}

	/* Stop the decode thread the same way: signal exit, wake it, join.
	 * It must be fully stopped before we free its buffers/texture. */
	if (s->decode_started) {
		pthread_mutex_lock(&s->decode_lock);
		s->decode_exit = true;
		pthread_cond_signal(&s->decode_cond);
		pthread_mutex_unlock(&s->decode_lock);
		pthread_join(s->decode_thread, NULL);
		s->decode_started = false;
	}

	/* The texture is owned by the graphics thread; destroy it inside the
	 * graphics context. */
	if (s->tex) {
		obs_enter_graphics();
		gs_texture_destroy(s->tex);
		obs_leave_graphics();
		s->tex = NULL;
	}
	if (s->pending_data)
		bfree(s->pending_data);

	pthread_cond_destroy(&s->decode_cond);
	pthread_mutex_destroy(&s->decode_lock);
	dstr_free(&s->odp_path);
	dstr_free(&s->cache_dir);
	pthread_mutex_destroy(&s->lock);
	bfree(s);
}

/* Delegate rendering to the internal image source. */
/* Runs every frame on the graphics thread. Keeps the image child pointed at
 * the current slide once the background export has populated slide_count, and
 * whenever the index changes via hotkeys. */
static void odp_video_tick(void *data, float seconds)
{
	struct odp_source *s = data;

	/* Keep the displayed slide in sync with the current index. */
	if (s->slide_count > 0 && s->current_index != s->shown_index)
		request_slide(s, s->current_index);

	/* Auto-refresh: poll the .odp mtime roughly every 2 seconds and
	 * re-export if the file changed on disk (e.g. the user saved edits
	 * in LibreOffice). The export's own incremental check makes this
	 * cheap when nothing actually changed. */
	s->poll_accum += seconds;
	if (s->poll_accum < 2.0f)
		return;
	s->poll_accum = 0.0f;

	if (s->worker_running)
		return; /* an export is already in flight */

	pthread_mutex_lock(&s->lock);
	bool have_path = s->odp_path.array && *s->odp_path.array;
	char path_copy[1024];
	if (have_path)
		snprintf(path_copy, sizeof(path_copy), "%s", s->odp_path.array);
	time_t baseline = s->odp_mtime;
	pthread_mutex_unlock(&s->lock);

	if (!have_path)
		return;

	struct stat st;
	if (stat(path_copy, &st) == 0 && st.st_mtime > baseline) {
		blog(LOG_INFO,
		     "[odp-presenter] '%s' changed on disk, re-exporting",
		     path_copy);
		/* Bump baseline immediately so we don't fire repeatedly while the
		 * re-export runs. The worker will set the authoritative value. */
		pthread_mutex_lock(&s->lock);
		s->odp_mtime = st.st_mtime;
		pthread_mutex_unlock(&s->lock);
		trigger_export(s);
	}
}

static void odp_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct odp_source *s = data;

	/* If the decode thread produced a new slide, upload it to a GPU
	 * texture now (this is the only graphics-thread cost — a fast upload,
	 * NOT a decode). We consume the buffer under decode_lock but do the
	 * actual gs_* work after copying out the pointer, keeping the lock
	 * hold short. We're already inside the graphics context here. */
	pthread_mutex_lock(&s->decode_lock);
	if (s->pending_ready && s->pending_data) {
		uint8_t *data_buf = s->pending_data;
		uint32_t cx = s->pending_cx;
		uint32_t cy = s->pending_cy;
		enum gs_color_format fmt = s->pending_format;
		int idx = s->decoded_index;
		s->pending_data = NULL; /* take ownership */
		s->pending_ready = false;
		pthread_mutex_unlock(&s->decode_lock);

		/* Recreate the texture from the decoded pixels. Creating a fresh
		 * texture WITH the data (rather than a GS_DYNAMIC texture updated
		 * via set_image) is the most reliable path across backends and
		 * avoids alpha/linesize subtleties. We're on the graphics thread
		 * inside the render context, so this is valid. */
		if (s->tex) {
			gs_texture_destroy(s->tex);
			s->tex = NULL;
		}
		const uint8_t *datas[1] = { data_buf };
		s->tex = gs_texture_create(cx, cy, fmt, 1, datas, 0);
		s->tex_cx = cx;
		s->tex_cy = cy;
		s->tex_index = idx;

		bfree(data_buf);
		blog(LOG_DEBUG,
		     "[odp-presenter] uploaded slide %d (%ux%u)",
		     idx, cx, cy);
	} else {
		pthread_mutex_unlock(&s->decode_lock);
	}

	/* Draw the current texture explicitly with the base effect. We do this
	 * manually (rather than obs_source_draw) so we control exactly which
	 * effect/technique renders the texture — obs_source_draw was producing
	 * a black frame despite a valid texture. Bind the texture to the
	 * effect's "image" param and run the Draw technique over a sprite. */
	if (s->tex) {
		gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image =
			gs_effect_get_param_by_name(eff, "image");
		gs_effect_set_texture(image, s->tex);
		while (gs_effect_loop(eff, "Draw"))
			gs_draw_sprite(s->tex, 0, s->tex_cx, s->tex_cy);
	}
}

static uint32_t odp_get_width(void *data)
{
	struct odp_source *s = data;
	return s->tex_cx;
}

static uint32_t odp_get_height(void *data)
{
	struct odp_source *s = data;
	return s->tex_cy;
}

static obs_properties_t *odp_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	/* If LibreOffice can't be found, the export pipeline can't run, so
	 * surface a clear, actionable message right at the top of the source's
	 * settings rather than letting the source silently stay blank. */
	if (!odp_tool_libreoffice()) {
		obs_property_t *warn = obs_properties_add_text(
			props, "lo_missing_warning",
			"LibreOffice was not found. This plugin needs "
			"LibreOffice to convert slides. Install it (free) "
			"from libreoffice.org, then reopen this source.",
			OBS_TEXT_INFO);
		obs_property_text_set_info_type(warn, OBS_TEXT_INFO_WARNING);
	}

	obs_properties_add_path(props, "odp_path", "Presentation file",
				OBS_PATH_FILE,
				"Presentations (*.odp *.pptx *.ppt)", NULL);
	obs_properties_add_path(props, "cache_dir", "Slide cache folder",
				OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_int(props, "dpi", "Render DPI", 72, 400, 1);
	obs_properties_add_int(props, "workers", "Parallel render workers",
			       1, 16, 1);

	return props;
}

static void odp_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "dpi", 150);
	obs_data_set_default_int(settings, "workers", 4);
}

/* ---- registration struct ---------------------------------------------- */

struct obs_source_info odp_presenter_source_info = {
	.id = "odp_presenter_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = odp_get_name,
	.create = odp_create,
	.destroy = odp_destroy,
	.update = odp_update,
	.video_tick = odp_video_tick,
	.video_render = odp_video_render,
	.get_width = odp_get_width,
	.get_height = odp_get_height,
	.get_properties = odp_get_properties,
	.get_defaults = odp_get_defaults,
	.icon_type = OBS_ICON_TYPE_SLIDESHOW,
};
