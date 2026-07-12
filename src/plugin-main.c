/*
 * ODP Presenter — OBS plugin entry point
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("slides-for-obs", "en-US")

/* Defined in odp-source.c */
extern struct obs_source_info odp_presenter_source_info;

/* Defined in odp-export.c — locates LibreOffice/pdftoppm at load time. */
extern bool odp_tools_detect(void);

/* Defined in odp-dock.cpp (C++/Qt). Stage 1: a no-op that proves the Qt build
 * links. Later stages create the on-screen control dock here. */
extern void odp_dock_init(void);

/* Navigation hotkeys are registered PER SOURCE in odp-source.c via
 * obs_hotkey_register_source(). OBS persists source hotkeys automatically as
 * part of the scene collection, so bindings survive restarts with no custom
 * save/load. (An earlier global/frontend-hotkey approach did not persist —
 * OBS does not auto-save plugin frontend hotkeys — so it was removed.)
 *
 * Each ODP source exposes its own Next / Previous / First / Last / Reload
 * hotkeys in Settings → Hotkeys, grouped under that source's name. If you run
 * more than one deck, bind the same key to each deck's "Next" etc.; only the
 * deck visible in the current scene reacts, so there is no conflict. */

const char *obs_module_name(void)
{
	return "ODP Presenter";
}

const char *obs_module_description(void)
{
	return "Display and control LibreOffice .odp / .pptx presentations "
	       "as a native OBS source.";
}

bool obs_module_load(void)
{
	if (!odp_tools_detect())
		blog(LOG_WARNING,
		     "[odp-presenter] LibreOffice not found at load time; "
		     "install it or place it on a known path");
	obs_register_source(&odp_presenter_source_info);
	blog(LOG_INFO, "[odp-presenter] plugin loaded");
	return true;
}

/* Runs after the frontend is fully initialized — safe to call frontend API
 * query functions here. Step-1 smoke test: prove the frontend API links and
 * runs. Once confirmed, the real preview/program logic will build on this. */
void obs_module_post_load(void)
{
	blog(LOG_INFO,
	     "[odp-presenter] frontend API OK (studio mode active=%d)",
	     (int)obs_frontend_preview_program_mode_active());

	/* Create the on-screen control dock (Qt). Must run here in post_load,
	 * on the UI thread with the frontend ready. */
	odp_dock_init();
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[odp-presenter] plugin unloaded");
}
