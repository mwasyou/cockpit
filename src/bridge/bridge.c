/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#include "cockpitchannel.h"
#include "cockpitdbusinternal.h"
#include "cockpitdbusjson.h"
#include "cockpitechochannel.h"
#include "cockpitfslist.h"
#include "cockpitfsread.h"
#include "cockpitfswatch.h"
#include "cockpitfsreplace.h"
#include "cockpithttpstream.h"
#include "cockpitinteracttransport.h"
#include "cockpitnullchannel.h"
#include "cockpitpackages.h"
#include "cockpitinternalmetrics.h"
#include "cockpitpolkitagent.h"
#include "cockpitportal.h"
#include "cockpitstream.h"

#include "deprecated/cockpitdbusjson1.h"

#include "common/cockpitjson.h"
#include "common/cockpitlog.h"
#include "common/cockpitpipetransport.h"
#include "common/cockpitunixfd.h"

#include <sys/prctl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>

#include <glib-unix.h>


/* This program is run on each managed server, with the credentials
   of the user that is logged into the Server Console.
*/

static GHashTable *channels;
static gboolean init_received;
static CockpitPackages *packages;

static void
on_channel_closed (CockpitChannel *channel,
                   const gchar *problem,
                   gpointer user_data)
{
  g_hash_table_remove (channels, cockpit_channel_get_id (channel));
}

static void
process_init (CockpitTransport *transport,
              JsonObject *options)
{
  gint64 version = -1;

  if (!cockpit_json_get_int (options, "version", -1, &version))
    {
      g_warning ("invalid version field in init message");
      cockpit_transport_close (transport, "protocol-error");
    }

  if (version == 1)
    {
      g_debug ("received init message");
      init_received = TRUE;
    }
  else
    {
      g_message ("unsupported version of cockpit protocol: %" G_GINT64_FORMAT, version);
      cockpit_transport_close (transport, "not-supported");
    }
}

static struct {
  const gchar *name;
  GType (* function) (void);
} payload_types[] = {
  { "dbus-json1", cockpit_dbus_json1_get_type },
  { "dbus-json3", cockpit_dbus_json_get_type },
  { "http-stream1", cockpit_http_stream_get_type },
  { "stream", cockpit_stream_get_type },
  { "fsread1", cockpit_fsread_get_type },
  { "fsreplace1", cockpit_fsreplace_get_type },
  { "fswatch1", cockpit_fswatch_get_type },
  { "fslist1", cockpit_fslist_get_type },
  { "null", cockpit_null_channel_get_type },
  { "echo", cockpit_echo_channel_get_type },
  { "metrics1", cockpit_internal_metrics_get_type },
  { NULL },
};

static void
process_open (CockpitTransport *transport,
              const gchar *channel_id,
              JsonObject *options)
{
  CockpitChannel *channel;
  GType channel_type;
  const gchar *payload;
  gchar **capabilities = NULL;
  gint i;

  if (!channel_id)
    {
      g_warning ("Caller tried to open channel with invalid id");
      cockpit_transport_close (transport, "protocol-error");
    }
  else if (g_hash_table_lookup (channels, channel_id))
    {
      g_warning ("Caller tried to reuse a channel that's already in use");
      cockpit_transport_close (transport, "protocol-error");
    }

  /*
   * No capabilities are supported yet. When the first one arrives
   * we'll add code to process it.
   */

  else if (!cockpit_json_get_strv (options, "capabilities", NULL, &capabilities))
    {
      g_message ("got invalid capabilities field in init message");
      cockpit_transport_close (transport, "protocol-error");
    }
  else if (capabilities && capabilities[0])
    {
      g_message ("unsupported capability required: %s", capabilities[0]);
      cockpit_transport_close (transport, "not-supported");
    }

  else
    {
      if (!cockpit_json_get_string (options, "payload", NULL, &payload))
        payload = NULL;

      /* This will close with "not-supported" */
      channel_type = COCKPIT_TYPE_CHANNEL;

      for (i = 0; payload_types[i].name != NULL; i++)
        {
          if (g_strcmp0 (payload, payload_types[i].name) == 0)
            {
              channel_type = payload_types[i].function();
              break;
            }
        }

      channel = g_object_new (channel_type,
                              "transport", transport,
                              "id", channel_id,
                              "options", options,
                              NULL);

      g_hash_table_insert (channels, g_strdup (channel_id), channel);
      g_signal_connect (channel, "closed", G_CALLBACK (on_channel_closed), NULL);
    }

  g_free (capabilities);
}

static gboolean
on_transport_control (CockpitTransport *transport,
                      const char *command,
                      const gchar *channel_id,
                      JsonObject *options,
                      GBytes *message,
                      gpointer user_data)
{
  if (g_str_equal (command, "init"))
    {
      process_init (transport, options);
      return TRUE;
    }

  if (!init_received)
    {
      g_warning ("caller did not send 'init' message first");
      cockpit_transport_close (transport, "protocol-error");
      return TRUE;
    }

  if (g_str_equal (command, "open"))
    {
      process_open (transport, channel_id, options);
      return TRUE;
    }
  else if (g_str_equal (command, "close"))
    {
      if (!channel_id)
        {
          g_warning ("Caller tried to close channel without an id");
          cockpit_transport_close (transport, "protocol-error");
        }
      else
        {
          /*
           * The channel may no longer exist due to a race of the bridge closing
           * a channel and the web closing it at the same time.
           */

          g_debug ("already closed channel %s", channel_id);
        }
    }

  return FALSE;
}

static void
on_closed_set_flag (CockpitTransport *transport,
                    const gchar *problem,
                    gpointer user_data)
{
  gboolean *flag = user_data;
  *flag = TRUE;
}

static void
send_init_command (CockpitTransport *transport)
{
  const gchar *checksum;
  const gchar *name;
  JsonObject *object;
  GBytes *bytes;

  object = json_object_new ();
  json_object_set_string_member (object, "command", "init");
  json_object_set_int_member (object, "version", 1);

  checksum = cockpit_packages_get_checksum (packages);
  if (checksum)
    json_object_set_string_member (object, "checksum", checksum);

  /* Happens when we're in --interact mode */
  name = cockpit_dbus_internal_name ();
  if (name)
    json_object_set_string_member (object, "bridge-dbus-name", name);

  bytes = cockpit_json_write_bytes (object);
  cockpit_transport_send (transport, NULL, bytes);
  g_bytes_unref (bytes);
}

static void
setup_dbus_daemon (gpointer addrfd)
{
  cockpit_unix_fd_close_all (3, GPOINTER_TO_INT (addrfd));
}

static GPid
start_dbus_daemon (void)
{
  GError *error = NULL;
  const gchar *env;
  GString *address = NULL;
  gchar *line;
  gsize len;
  gssize ret;
  GPid pid = 0;
  gchar *print_address = NULL;
  int addrfd[2] = { -1, -1 };
  GSpawnFlags flags;

  gchar *dbus_argv[] = {
      "dbus-daemon",
      "--print-address=X",
      "--session",
      NULL
  };

  /* Automatically start a DBus session if necessary */
  env = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  if (env != NULL && env[0] != '\0')
    {
      g_debug ("already have session bus: %s", env);
      goto out;
    }

  if (pipe (addrfd))
    {
      g_warning ("pipe failed to allocate fds: %m");
      goto out;
    }

  print_address = g_strdup_printf ("--print-address=%d", addrfd[1]);
  dbus_argv[1] = print_address;

  /* The DBus daemon produces useless messages on stderr mixed in */
  flags = G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_SEARCH_PATH |
          G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_STDOUT_TO_DEV_NULL;

  g_spawn_async_with_pipes (NULL, dbus_argv, NULL, flags,
                            setup_dbus_daemon, GINT_TO_POINTER (addrfd[1]),
                            &pid, NULL, NULL, NULL, &error);

  close (addrfd[1]);

  if (error != NULL)
    {
      g_warning ("couldn't start %s: %s", dbus_argv[0], error->message);
      g_error_free (error);
      pid = 0;
      goto out;
    }

  g_debug ("launched %s", dbus_argv[0]);

  address = g_string_new ("");
  for (;;)
    {
      len = address->len;
      g_string_set_size (address, len + 256);
      ret = read (addrfd[0], address->str + len, 256);
      if (ret < 0)
        {
          g_string_set_size (address, len);
          if (errno != EAGAIN && errno != EINTR)
            {
              g_warning ("couldn't read address from dbus-daemon: %s", g_strerror (errno));
              goto out;
            }
        }
      else if (ret == 0)
        {
          g_string_set_size (address, len);
          break;
        }
      else
        {
          g_string_set_size (address, len + ret);
          line = strchr (address->str, '\n');
          if (line != NULL)
            {
              *line = '\0';
              break;
            }
        }
    }

  if (address->str[0] == '\0')
    {
      g_warning ("dbus-daemon didn't send us a dbus address");
    }
  else
    {
      g_setenv ("DBUS_SESSION_BUS_ADDRESS", address->str, TRUE);
      g_debug ("session bus address: %s", address->str);
    }

out:
  if (addrfd[0] >= 0)
    close (addrfd[0]);
  if (address)
    g_string_free (address, TRUE);
  g_free (print_address);
  return pid;
}

static gboolean
on_signal_done (gpointer data)
{
  gboolean *closed = data;
  *closed = TRUE;
  return TRUE;
}

static struct passwd *
getpwuid_a (uid_t uid)
{
  int err;
  long bufsize = sysconf (_SC_GETPW_R_SIZE_MAX);
  struct passwd *ret = NULL;
  struct passwd *buf;

  if (bufsize <= 0)
    bufsize = 8192;

  buf = g_malloc (sizeof(struct passwd) + bufsize);
  err = getpwuid_r (uid, buf, (char *)(buf + 1), bufsize, &ret);

  if (ret == NULL)
    {
      free (buf);
      if (err == 0)
        err = ENOENT;
      errno = err;
    }

  return ret;
}

static int
run_bridge (const gchar *interactive)
{
  CockpitTransport *transport;
  gboolean terminated = FALSE;
  gboolean interupted = FALSE;
  gboolean closed = FALSE;
  CockpitPortal *super = NULL;
  CockpitPortal *pcp = NULL;
  gpointer polkit_agent = NULL;
  struct passwd *pwd;
  GPid daemon_pid = 0;
  guint sig_term;
  guint sig_int;
  int outfd;
  uid_t uid;

  cockpit_set_journal_logging (G_LOG_DOMAIN, !isatty (2));

  /* Always set environment variables early */
  uid = geteuid();
  pwd = getpwuid_a (uid);
  if (pwd == NULL)
    {
      g_message ("couldn't get user info: %s", g_strerror (errno));
    }
  else
    {
      g_setenv ("USER", pwd->pw_name, TRUE);
      g_setenv ("HOME", pwd->pw_dir, TRUE);
      g_setenv ("SHELL", pwd->pw_shell, TRUE);
    }

  /*
   * This process talks on stdin/stdout. However lots of stuff wants to write
   * to stdout, such as g_debug, and uses fd 1 to do that. Reroute fd 1 so that
   * it goes to stderr, and use another fd for stdout.
   */

  outfd = dup (1);
  if (outfd < 0 || dup2 (2, 1) < 1)
    {
      g_warning ("bridge couldn't redirect stdout to stderr");
      outfd = 1;
    }

  sig_term = g_unix_signal_add (SIGTERM, on_signal_done, &terminated);
  sig_int = g_unix_signal_add (SIGINT, on_signal_done, &interupted);

  g_type_init ();

  /* Start a session daemon if necessary */
  if (!interactive)
    daemon_pid = start_dbus_daemon ();

  packages = cockpit_packages_new ();
  cockpit_dbus_internal_startup (interactive != NULL);

  if (interactive)
    {
      /* Allow skipping the init message when interactive */
      init_received = TRUE;

      transport = cockpit_interact_transport_new (0, outfd, interactive);
    }
  else
    {
      transport = cockpit_pipe_transport_new_fds ("stdio", 0, outfd);
    }

  if (uid != 0)
    {
      if (!interactive)
        polkit_agent = cockpit_polkit_agent_register (transport, NULL);
      super = cockpit_portal_new_superuser (transport);
    }

  pcp = cockpit_portal_new_pcp (transport);

  cockpit_dbus_time_startup ();
  cockpit_dbus_user_startup (pwd);
  cockpit_dbus_setup_startup ();

  g_free (pwd);
  pwd = NULL;

  g_signal_connect (transport, "control", G_CALLBACK (on_transport_control), NULL);
  g_signal_connect (transport, "closed", G_CALLBACK (on_closed_set_flag), &closed);
  send_init_command (transport);

  /* Owns the channels */
  channels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  while (!terminated && !closed && !interupted)
    g_main_context_iteration (NULL, TRUE);

  if (polkit_agent)
    cockpit_polkit_agent_unregister (polkit_agent);
  if (super)
    g_object_unref (super);
  g_object_unref (pcp);
  g_object_unref (transport);
  g_hash_table_destroy (channels);

  cockpit_dbus_internal_cleanup ();
  cockpit_packages_free (packages);
  packages = NULL;

  if (daemon_pid)
    kill (daemon_pid, SIGTERM);

  g_source_remove (sig_term);
  g_source_remove (sig_int);

  /* So the caller gets the right signal */
  if (terminated)
    raise (SIGTERM);

  return 0;
}

static void
print_version (void)
{
  gint i, offset, len;

  g_print ("Version: %s\n", PACKAGE_VERSION);
  g_print ("Protocol: 1\n");

  g_print ("Payloads: ");
  offset = 10;
  for (i = 0; payload_types[i].name != NULL; i++)
    {
      len = strlen (payload_types[i].name);
      if (offset + len > 70)
        {
          g_print ("\n");
          offset = 0;
        }

      if (offset == 0)
        {
          g_print ("    ");
          offset = 4;
        };

      g_print ("%s ", payload_types[i].name);
      offset += len + 1;
    }
  g_print ("\n");

  g_print ("Authorization: crypt1\n");
}

int
main (int argc,
      char **argv)
{
  GOptionContext *context;
  GError *error = NULL;
  int ret;

  static gboolean opt_packages = FALSE;
  static gboolean opt_version = FALSE;
  static gchar *opt_interactive = NULL;

  static GOptionEntry entries[] = {
    { "interact", 0, 0, G_OPTION_ARG_STRING, &opt_interactive, "Interact with the raw protocol", "boundary" },
    { "packages", 0, 0, G_OPTION_ARG_NONE, &opt_packages, "Show Cockpit package information", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Show Cockpit version information", NULL },
    { NULL }
  };

  signal (SIGPIPE, SIG_IGN);

  /*
   * We have to tell GLib about an alternate default location for XDG_DATA_DIRS
   * if we've been compiled with a different prefix. GLib caches that, so need
   * to do this very early.
   */
  if (!g_getenv ("XDG_DATA_DIRS") && !g_str_equal (DATADIR, "/usr/share"))
    g_setenv ("XDG_DATA_DIRS", DATADIR, TRUE);

  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_setenv ("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_set_description (context,
                                    "cockpit-bridge is run automatically inside of a Cockpit session. When\n"
                                    "run from the command line one of the options above must be specified.\n");

  g_option_context_parse (context, &argc, &argv, &error);
  g_option_context_free (context);

  if (error)
    {
      g_printerr ("cockpit-bridge: %s\n", error->message);
      g_error_free (error);
      return 1;
    }

  if (opt_packages)
    {
      cockpit_packages_dump ();
      return 0;
    }
  else if (opt_version)
    {
      print_version ();
      return 0;
    }

  if (!opt_interactive && isatty (1))
    {
      g_printerr ("cockpit-bridge: no option specified\n");
      return 2;
    }

  ret = run_bridge (opt_interactive);

  g_free (opt_interactive);
  return ret;
}
