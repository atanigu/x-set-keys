/***************************************************************************
 *
 * Copyright (C) 2017 Tomoyuki KAWAO
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ***************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <glib-unix.h>
#include <X11/Xlib.h>

#define MAIN
#include "common.h"
#include "x-set-keys.h"
#include "config.h"

typedef struct _Option_ {
  gchar *config_filepath;
  gchar *device_filepath;
  gchar **excluded_classes;
} _Option;

static volatile gboolean _caught_sigint = FALSE;
static volatile gboolean _caught_sigterm = FALSE;
static volatile gboolean _caught_sighup = FALSE;
static volatile gboolean _error_occurred = FALSE;

static void _set_debug_flag();
static gboolean _handle_signal(gpointer flag_pointer);
static gint _handle_x_error(Display *display, XErrorEvent *event);
static gint _handle_xio_error(Display *display);
static gboolean _run(const _Option *option);

gint main(gint argc, const gchar *argv[])
{
  _Option option = { 0 };
  gint error_retry_count = 0;

  g_set_prgname(g_path_get_basename(argv[0]));
  _set_debug_flag();

  g_unix_signal_add(SIGINT, _handle_signal, (gpointer)&_caught_sigint);
  g_unix_signal_add(SIGTERM, _handle_signal, (gpointer)&_caught_sigterm);
  g_unix_signal_add(SIGHUP, _handle_signal, (gpointer)&_caught_sighup);

  XSetErrorHandler(_handle_x_error);
  XSetIOErrorHandler(_handle_xio_error);

  {
    static gchar *excluded_classes[] = {
      "emacs", "Gnome-terminal", NULL
    };
    option.excluded_classes = excluded_classes;
  }

  while (_run(&option)) {
    if (_error_occurred) {
      if (++error_retry_count > 10) {
        g_critical("Maximum error retry count exceeded");
        break;
      }
      _error_occurred = FALSE;
    } else {
      error_retry_count = 0;
    }
    g_message("Restarting");
  }
  g_message("Exiting");

  return _error_occurred ? EXIT_FAILURE : EXIT_SUCCESS;
}

void notify_error()
{
  _error_occurred = TRUE;
}

static void _set_debug_flag()
{
  const gchar *domains = g_getenv("G_MESSAGES_DEBUG");

  is_debug = domains != NULL && strcmp(domains, "all") == 0;
}

gboolean _handle_signal(gpointer flag_pointer)
{
  *((volatile gboolean *)flag_pointer) = TRUE;
  g_main_context_wakeup(NULL);
  return G_SOURCE_CONTINUE;
}

static gint _handle_x_error(Display *display, XErrorEvent *event)
{
  gchar message[256];

  XGetErrorText(display, event->error_code, message, sizeof (message));
  g_critical("X protocol error : %s on protocol request %d",
             message,
             event->request_code);
  _error_occurred = TRUE;
  return 0;
}

static gint _handle_xio_error(Display *display)
{
  g_critical("Connection lost to X server `%s'", DisplayString(display));
  _error_occurred = TRUE;
  return 0;
}

static gboolean _run(const _Option *option)
{
  gboolean result = TRUE;
  XSetKeys xsk = { 0 };

  if (!xsk_initialize(&xsk, option->excluded_classes)) {
    _error_occurred = TRUE;
  } else if (!config_load(&xsk, option->config_filepath)) {
    _error_occurred = TRUE;
  } else if (!xsk_start(&xsk, option->device_filepath)) {
    _error_occurred = TRUE;
  }

  if (_error_occurred) {
    result = FALSE;
  } else {
    debug_print("Starting main loop");
    _caught_sighup = FALSE;
    while (!_caught_sigint &&
           !_caught_sigterm &&
           !_caught_sighup &&
           !_error_occurred) {
      g_main_context_iteration(NULL, TRUE);
    }

    if (_caught_sigint) {
      result = FALSE;
      g_message("Caught SIGINT");
    }
    if (_caught_sigterm) {
      result = FALSE;
      g_message("Caught SIGTERM");
    }
    if (_caught_sighup) {
      g_message("Caught SIGHUP");
    }
  }
  g_message(result ? "Initiating restart" : "Initiating shutdown");

  xsk_finalize(&xsk);
  return result;
}
