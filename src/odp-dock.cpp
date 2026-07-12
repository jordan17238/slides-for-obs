/* odp-dock.cpp — Qt control dock for the ODP presenter plugin.
 *
 * Adds a small dockable panel to the OBS main window with TWO halves:
 *
 *   ┌──────────────────────────┐
 *   │  LIVE                    │   ⏮ First   ◀ Back   ▶ Forward
 *   │  ──────────────────────  │  (separator)
 *   │  PREVIEW                 │   ⏮ First   ◀ Back   ▶ Forward
 *   └──────────────────────────┘
 *
 * The LIVE half drives whichever deck is currently in the program (live)
 * scene; the PREVIEW half drives whichever deck is in the preview scene
 * (Studio Mode). This means during a service the operator can advance the
 * live deck WITHOUT swapping it back to preview first — which was the fiddly
 * bit from the original single-target dock.
 *
 * All routing logic lives in odp-source.c (odp_navigate_program /
 * odp_navigate_preview); this file is only the UI, so behaviour stays
 * consistent and portable (incl. to a future Windows build).
 *
 * This is C++ because Qt is a C++ framework, but it exposes only a single
 * C-linkage entry point (odp_dock_init) for the C plugin to call at load.
 *
 * Button actions: 0 = next/forward, 1 = previous/back, 2 = first.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QFont>

/* Implemented in odp-source.c (C linkage). */
extern "C" void odp_navigate_program(int action);
extern "C" void odp_navigate_preview(int action);

/* Build a single navigation row (First / Back / Forward) that calls `cb`. */
static void add_nav_row(QVBoxLayout *layout, QWidget *parent,
			void (*cb)(int))
{
	QPushButton *first = new QPushButton(
		QString::fromUtf8("\xE2\x8F\xAE First"), parent);
	QObject::connect(first, &QPushButton::clicked,
			 [cb]() { cb(2); });

	QPushButton *back = new QPushButton(
		QString::fromUtf8("\xE2\x97\x80 Back"), parent);
	QObject::connect(back, &QPushButton::clicked,
			 [cb]() { cb(1); });

	QPushButton *fwd = new QPushButton(
		QString::fromUtf8("\xE2\x96\xB6 Forward"), parent);
	QObject::connect(fwd, &QPushButton::clicked,
			 [cb]() { cb(0); });

	layout->addWidget(first);
	layout->addWidget(back);
	layout->addWidget(fwd);
}

/* Section header label — bold, slightly larger, so the two halves are clearly
 * different "blocks" rather than six identical buttons in a row. */
static QLabel *make_section_label(const QString &text, QWidget *parent)
{
	QLabel *lbl = new QLabel(text, parent);
	QFont f = lbl->font();
	f.setBold(true);
	f.setPointSize(f.pointSize() + 1);
	lbl->setFont(f);
	lbl->setAlignment(Qt::AlignCenter);
	return lbl;
}

/* Visible horizontal divider between the two halves. */
static QFrame *make_divider(QWidget *parent)
{
	QFrame *line = new QFrame(parent);
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	return line;
}

/* Build the dock's content widget: two clearly separated halves. */
static QWidget *odp_build_dock_contents()
{
	QWidget *root = new QWidget();
	QVBoxLayout *layout = new QVBoxLayout(root);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(4);

	/* LIVE half — drives the deck currently going to recording/output. */
	layout->addWidget(make_section_label(QObject::tr("LIVE"), root));
	add_nav_row(layout, root, odp_navigate_program);

	layout->addSpacing(6);
	layout->addWidget(make_divider(root));
	layout->addSpacing(6);

	/* PREVIEW half — drives the deck in the preview scene (Studio Mode). */
	layout->addWidget(make_section_label(QObject::tr("PREVIEW"), root));
	add_nav_row(layout, root, odp_navigate_preview);

	layout->addStretch();
	return root;
}

/* Called once from obs_module_post_load() (C side), on the UI thread after the
 * frontend is ready. Creates the dock and registers it with OBS. */
extern "C" void odp_dock_init(void)
{
	/* Clear any prior registration so repeated loads (or leftovers from
	 * earlier builds that used a different API/id) can't stack docks. */
	obs_frontend_remove_dock("slides-for-obs-dock");
	obs_frontend_remove_dock("slides-for-obs-dock-v2");

	/* obs_frontend_add_dock_by_id wants a PLAIN QWidget — OBS wraps it in
	 * the dock frame and adds the title bar + Docks-menu toggle itself.
	 * Passing a QDockWidget here would produce TWO title bars. */
	QWidget *contents = odp_build_dock_contents();
	contents->setObjectName("SlidesForObsDockV2");
	obs_frontend_add_dock_by_id("slides-for-obs-dock-v2",
				    "Slides for OBS", contents);

	blog(LOG_INFO, "[odp-presenter] control dock added");
}
