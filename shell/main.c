/*
 *  Copyright (C) 2004 Marco Pesenti Gritti
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include "ev-application.h"
#include "ev-metadata-manager.h"
#include "ev-debug.h"

#include <glib/gi18n.h>
#include <gdk/gdkx.h>
#include <gtk/gtkmain.h>
#include <stdlib.h>
#include <string.h>

#if WITH_GNOME
#include <libgnome/gnome-program.h>
#include <libgnomeui/gnome-ui-init.h>
#include <libgnomeui/gnome-app-helper.h>
#include <libgnomeui/gnome-authentication-manager.h>
#endif

#ifdef ENABLE_DBUS
#include <dbus/dbus-glib-bindings.h>
#endif

#include "ev-stock-icons.h"
#include "ev-job-queue.h"
#include "ev-file-helpers.h"
#include "ev-backends-manager.h"

static gchar   *ev_page_label;
static gchar   *ev_find_string;
static gboolean preview_mode = FALSE;
static gboolean fullscren_mode = FALSE;
static gboolean presentation_mode = FALSE;
static gboolean unlink_temp_file = FALSE;
static gchar   *print_settings;
static const char **file_arguments = NULL;

static const GOptionEntry goption_options[] =
{
	{ "page-label", 'p', 0, G_OPTION_ARG_STRING, &ev_page_label, N_("The page of the document to display."), N_("PAGE")},
	{ "fullscreen", 'f', 0, G_OPTION_ARG_NONE, &fullscren_mode, N_("Run evince in fullscreen mode"), NULL },
	{ "presentation", 's', 0, G_OPTION_ARG_NONE, &presentation_mode, N_("Run evince in presentation mode"), NULL },
	{ "preview", 'w', 0, G_OPTION_ARG_NONE, &preview_mode, N_("Run evince as a previewer"), NULL },
	{ "find", 'l', 0, G_OPTION_ARG_STRING, &ev_find_string, N_("The word or phrase to find in the document"), N_("STRING")},
	{ "unlink-tempfile", 'u', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &unlink_temp_file, NULL, NULL },
	{ "print-settings", 't', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_FILENAME, &print_settings, NULL, NULL },
	{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &file_arguments, NULL, N_("[FILE...]") },
	{ NULL }
};

static void
value_free (GValue *value)
{
	g_value_unset (value);
	g_free (value);
}

/**
 * arguments_parse:
 *
 * Parses the arguments and creates a #GHashTable with this data.
 *
 *  key                 ->  value
 *
 *  dislay              ->  display at the default screen.
 *  screen              ->  screen number.
 *  page-label          ->  only if the page label argument has been passed,
 *                          the page of the document to display.
 *  mode                ->  only if the view mode is one of the availables,
 *                          the view mode.
 *  unlink-temp-file    ->  only if the view mode is preview mode and
 *                          unlink-temp-file has been passed, unlink-temp-file.
 *
 * Returns: a pointer into #GHashTable with data from the arguments.
 */
static GHashTable *
arguments_parse (void)
{
	GHashTable      *args;
	GValue          *value;
	EvWindowRunMode  mode;
	GdkScreen       *screen;
	GdkDisplay      *display;
	const gchar     *display_name;
	gint             screen_number;

	args = g_hash_table_new_full (g_str_hash,
				      g_str_equal,
				      (GDestroyNotify)g_free,
				      (GDestroyNotify)value_free);
	
	screen = gdk_screen_get_default ();
	display = gdk_screen_get_display (screen);

	display_name = gdk_display_get_name (display);
	screen_number = gdk_screen_get_number (screen);

	value = g_new0 (GValue, 1);
	g_value_init (value, G_TYPE_STRING);
	g_value_set_string (value, display_name);
	g_hash_table_insert (args, g_strdup ("display"), value);

	value = g_new0 (GValue, 1);
	g_value_init (value, G_TYPE_INT);
	g_value_set_int (value, screen_number);
	g_hash_table_insert (args, g_strdup ("screen"), value);

	if (ev_page_label) {
		value = g_new0 (GValue, 1);
		g_value_init (value, G_TYPE_STRING);
		g_value_set_string (value, ev_page_label);

		g_hash_table_insert (args, g_strdup ("page-label"), value);

		g_free (ev_page_label);
		ev_page_label = NULL;
	}

	if (ev_find_string) {
		value = g_new0 (GValue, 1);
		g_value_init (value, G_TYPE_STRING);
		g_value_set_string (value, ev_find_string);

		g_hash_table_insert (args, g_strdup ("find-string"), value);

		g_free (ev_find_string);
		ev_page_label = NULL;
	}

	if (fullscren_mode)
		mode = EV_WINDOW_MODE_FULLSCREEN;
	else if (presentation_mode)
		mode = EV_WINDOW_MODE_PRESENTATION;
	else if (preview_mode)
		mode = EV_WINDOW_MODE_PREVIEW;
	else
		return args;

	value = g_new0 (GValue, 1);
	g_value_init (value, G_TYPE_UINT);
	g_value_set_uint (value, mode);

	g_hash_table_insert (args, g_strdup ("mode"), value);

	if (mode == EV_WINDOW_MODE_PREVIEW && unlink_temp_file) {
		value = g_new0 (GValue, 1);
		g_value_init (value, G_TYPE_BOOLEAN);
		g_value_set_boolean (value, unlink_temp_file);

		g_hash_table_insert (args,
				     g_strdup ("unlink-temp-file"),
				     value);
	}

	if (mode == EV_WINDOW_MODE_PREVIEW && print_settings) {
		value = g_new0 (GValue, 1);
		g_value_init (value, G_TYPE_STRING);
		g_value_set_string (value, print_settings);

		g_hash_table_insert (args,
				     g_strdup ("print-settings"),
				     value);
		g_free (print_settings);
		print_settings = NULL;
	}

	return args;
}

static void
load_files (const char **files,
	    GHashTable  *args)
{
	int i;

	if (!files) {
		ev_application_open_window (EV_APP, args, GDK_CURRENT_TIME, NULL);
		return;
	}

	for (i = 0; files[i]; i++) {
		char   *uri;
		char   *label;
		GValue *old = NULL;
		GFile  *file;

		file = g_file_new_for_commandline_arg (files[i]);
		uri = g_file_get_uri (file);
		g_object_unref (file);
		
		label = strchr (uri, '#');

		if (label) {
			GValue *new;

			*label = 0; label++;
			
			old = g_hash_table_lookup (args, "page-label");
			
			new = g_new0 (GValue, 1);
			g_value_init (new, G_TYPE_STRING);
			g_value_set_string (new, label);

			g_hash_table_insert (args, g_strdup ("page-label"), new);

		}

		ev_application_open_uri (EV_APP, uri, args,
					 GDK_CURRENT_TIME, NULL);

		if (old)
			g_hash_table_insert (args, g_strdup ("page-label"), old);
		
		g_free (uri);
        }
}

#ifdef ENABLE_DBUS
static gboolean
load_files_remote (const char **files,
		   GHashTable  *args)
{
	int i;
	GError *error = NULL;
	DBusGConnection *connection;
	gboolean result = FALSE;
	DBusGProxy *remote_object;
	GdkDisplay *display;
	guint32 timestamp;

	display = gdk_display_get_default ();
	timestamp = gdk_x11_display_get_user_time (display);
	connection = dbus_g_bus_get (DBUS_BUS_STARTER, &error);

	if (connection == NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);	

		return FALSE;
	}

	remote_object = dbus_g_proxy_new_for_name (connection,
						   "org.gnome.evince.ApplicationService",
                                                   "/org/gnome/evince/Evince",
                                                   "org.gnome.evince.Application");
	if (!files) {
		if (!dbus_g_proxy_call (remote_object, "OpenWindow", &error,
					dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE), args,
					G_TYPE_UINT, timestamp,
					G_TYPE_INVALID,
					G_TYPE_INVALID)) {
			g_warning ("%s", error->message);
			g_clear_error (&error);
			g_object_unref (remote_object);
			dbus_g_connection_unref (connection);
			return FALSE;
		}

		g_object_unref (remote_object);
		dbus_g_connection_unref (connection);
		
		return TRUE;
	}

	for (i = 0; files[i]; i++) {
		const char *page_label;
		GFile *file;
		char *uri;

		file = g_file_new_for_commandline_arg (files[i]);
		uri = g_file_get_uri (file);
		g_object_unref (file);

		page_label = ev_page_label ? ev_page_label : "";

		if (!dbus_g_proxy_call (remote_object, "OpenURI", &error,
					G_TYPE_STRING, uri,
					dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE), args,
					G_TYPE_UINT, timestamp,
					G_TYPE_INVALID,
					G_TYPE_INVALID)) {
			g_warning ("%s", error->message);
			g_clear_error (&error);
			g_free (uri);
			continue;
		}

		g_free (uri);
		result = TRUE;
        }

	g_object_unref (remote_object);
	dbus_g_connection_unref (connection);

	gdk_notify_startup_complete ();

	return result;
}
#endif /* ENABLE_DBUS */

int
main (int argc, char *argv[])
{
	gboolean enable_metadata = FALSE;
	GOptionContext *context;
	GHashTable *args;
#if WITH_GNOME
	GnomeProgram *program;
#else
	char *accel_filename;
	GError *error = NULL;
#endif

	context = g_option_context_new (_("GNOME Document Viewer"));

#ifdef ENABLE_NLS
	/* Initialize the i18n stuff */
	bindtextdomain(GETTEXT_PACKAGE, GNOMELOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
	textdomain(GETTEXT_PACKAGE);
	g_option_context_add_main_entries (context, goption_options, GETTEXT_PACKAGE);
	g_option_context_set_translation_domain(context, GETTEXT_PACKAGE);
#else
	g_option_context_add_main_entries (context, goption_options, NULL);
#endif

#if WITH_GNOME
	program = gnome_program_init (PACKAGE, VERSION,
				      LIBGNOMEUI_MODULE, argc, argv,
				      GNOME_PARAM_GOPTION_CONTEXT, context,
				      GNOME_PARAM_HUMAN_READABLE_NAME, _("Evince"),
				      GNOME_PARAM_APP_DATADIR, GNOMEDATADIR,
				      NULL);
#else
	g_option_context_add_group (context, gtk_get_option_group (TRUE));
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_warning ("Cannot parse arguments: %s", error->message);
		g_error_free (error);
		return 1;
	}
	g_option_context_free (context);

	accel_filename = g_build_filename (ev_dot_dir (), "accels", NULL);
	gtk_accel_map_load (accel_filename);
#endif

	args = arguments_parse ();

#ifdef ENABLE_DBUS
	if (!ev_application_register_service (EV_APP)) {
		if (load_files_remote (file_arguments, args)) {
			g_hash_table_destroy (args);
#if WITH_GNOME
			g_object_unref (program);
#endif
			return 0;
		}
	} else {
		enable_metadata = TRUE;
	}
#endif

	ev_debug_init ();
	
#if WITH_GNOME	
	gnome_authentication_manager_init ();
#endif

	ev_backends_manager_init ();
	
	if (enable_metadata) {
		ev_metadata_manager_init ();
	}

	ev_job_queue_init ();
	g_set_application_name (_("Evince Document Viewer"));

	ev_file_helpers_init ();
	ev_stock_icons_init ();
	gtk_window_set_default_icon_name ("evince");

	load_files (file_arguments, args);
	g_hash_table_destroy (args);

	gtk_main ();

#if WITH_GNOME
	gnome_accelerators_sync ();
#else
	gtk_accel_map_save (accel_filename);
	g_free (accel_filename);
#endif

	ev_file_helpers_shutdown ();

	if (enable_metadata) {
		ev_metadata_manager_shutdown ();
	}

	ev_backends_manager_shutdown ();

#if WITH_GNOME
 	g_object_unref (program);
#endif
	
	return 0;
}
