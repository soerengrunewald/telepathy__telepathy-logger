/* Simple utility code used by the regression tests.
 *
 * Copyright © 2008-2010 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright © 2008 Nokia Corporation
 *
 * Copying and distribution of this file, with or without modification,
 * are permitted in any medium without royalty provided the copyright
 * notice and this notice are preserved.
 */

#include "config.h"

#include "util.h"

#include <stdlib.h>
#ifdef G_OS_UNIX
# include <unistd.h> /* for alarm() */
#endif

void
tp_tests_proxy_run_until_prepared (gpointer proxy,
    const GQuark *features)
{
  GError *error = NULL;

  tp_tests_proxy_run_until_prepared_or_failed (proxy, features, &error);
  g_assert_no_error (error);
}

/* A GAsyncReadyCallback whose user_data is a GAsyncResult **. It writes a
 * reference to the result into that pointer. */
void
tp_tests_result_ready_cb (GObject *object,
    GAsyncResult *res,
    gpointer user_data)
{
  GAsyncResult **result = user_data;

  *result = g_object_ref (res);
}

/* Run until *result contains a result. Intended to be used with a pending
 * async call that uses tp_tests_result_ready_cb. */
void
tp_tests_run_until_result (GAsyncResult **result)
{
  /* not synchronous */
  g_assert (*result == NULL);

  while (*result == NULL)
    g_main_context_iteration (NULL, TRUE);
}

gboolean
tp_tests_proxy_run_until_prepared_or_failed (gpointer proxy,
    const GQuark *features,
    GError **error)
{
  GAsyncResult *result = NULL;
  gboolean r;

  tp_proxy_prepare_async (proxy, features, tp_tests_result_ready_cb, &result);

  tp_tests_run_until_result (&result);

  r =  tp_proxy_prepare_finish (proxy, result, error);
  g_object_unref (result);
  return r;
}

TpDBusDaemon *
tp_tests_dbus_daemon_dup_or_die (void)
{
  TpDBusDaemon *d = tp_dbus_daemon_dup (NULL);

  /* In a shared library, this would be very bad (see fd.o #18832), but in a
   * regression test that's going to be run under a temporary session bus,
   * it's just what we want. */
  if (d == NULL)
    {
      g_error ("Unable to connect to session bus");
    }

  return d;
}

static void
introspect_cb (TpProxy *proxy G_GNUC_UNUSED,
    const gchar *xml G_GNUC_UNUSED,
    const GError *error G_GNUC_UNUSED,
    gpointer user_data,
    GObject *weak_object G_GNUC_UNUSED)
{
  g_main_loop_quit (user_data);
}

void
tp_tests_proxy_run_until_dbus_queue_processed (gpointer proxy)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  tp_cli_dbus_introspectable_call_introspect (proxy, -1, introspect_cb,
      loop, NULL, NULL);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

typedef struct {
    GMainLoop *loop;
    TpHandle handle;
} HandleRequestResult;

static void
handles_requested_cb (TpConnection *connection G_GNUC_UNUSED,
    TpHandleType handle_type G_GNUC_UNUSED,
    guint n_handles,
    const TpHandle *handles,
    const gchar * const *ids G_GNUC_UNUSED,
    const GError *error,
    gpointer user_data,
    GObject *weak_object G_GNUC_UNUSED)
{
  HandleRequestResult *result = user_data;

  g_assert_no_error ((GError *) error);
  g_assert_cmpuint (n_handles, ==, 1);
  result->handle = handles[0];
}

static void
handle_request_result_finish (gpointer r)
{
  HandleRequestResult *result = r;

  g_main_loop_quit (result->loop);
}

TpHandle
tp_tests_connection_run_request_contact_handle (TpConnection *connection,
    const gchar *id)
{
  HandleRequestResult result = { g_main_loop_new (NULL, FALSE), 0 };
  const gchar * const ids[] = { id, NULL };

  tp_connection_request_handles (connection, -1, TP_HANDLE_TYPE_CONTACT, ids,
      handles_requested_cb, &result, handle_request_result_finish, NULL);
  g_main_loop_run (result.loop);
  g_main_loop_unref (result.loop);
  return result.handle;
}

void
_test_assert_empty_strv (const char *file,
    int line,
    gconstpointer strv)
{
  const gchar * const *strings = strv;

  if (strv != NULL && strings[0] != NULL)
    {
      guint i;

      g_message ("%s:%d: expected empty strv, but got:", file, line);

      for (i = 0; strings[i] != NULL; i++)
        {
          g_message ("* \"%s\"", strings[i]);
        }

      g_error ("%s:%d: strv wasn't empty (see above for contents",
          file, line);
    }
}

void
_tp_tests_assert_strv_equals (const char *file,
    int line,
    const char *expected_desc,
    gconstpointer expected_strv,
    const char *actual_desc,
    gconstpointer actual_strv)
{
  const gchar * const *expected = expected_strv;
  const gchar * const *actual = actual_strv;
  guint i;

  g_assert (expected != NULL);
  g_assert (actual != NULL);

  for (i = 0; expected[i] != NULL || actual[i] != NULL; i++)
    {
      if (expected[i] == NULL)
        {
          g_error ("%s:%d: assertion failed: (%s)[%u] == (%s)[%u]: "
              "NULL == %s", file, line, expected_desc, i,
              actual_desc, i, actual[i]);
        }
      else if (actual[i] == NULL)
        {
          g_error ("%s:%d: assertion failed: (%s)[%u] == (%s)[%u]: "
              "%s == NULL", file, line, expected_desc, i,
              actual_desc, i, expected[i]);
        }
      else if (tp_strdiff (expected[i], actual[i]))
        {
          g_error ("%s:%d: assertion failed: (%s)[%u] == (%s)[%u]: "
              "%s == %s", file, line, expected_desc, i,
              actual_desc, i, expected[i], actual[i]);
        }
    }
}

void
tp_tests_copy_dir (const gchar *from_dir, const gchar *to_dir)
{
  gchar *command;

  // If destination directory exist erase it
  command = g_strdup_printf ("rm -rf %s", to_dir);
  g_assert (system (command) == 0);
  g_free (command);

  command = g_strdup_printf ("cp -r %s %s", from_dir, to_dir);
  g_assert (system (command) == 0);
  g_free (command);

  // In distcheck mode the files and directory are read-only, fix that
  command = g_strdup_printf ("chmod -R +w %s", to_dir);
  g_assert (system (command) == 0);
  g_free (command);
}

void
tp_tests_create_and_connect_conn (GType conn_type,
    const gchar *account,
    TpBaseConnection **service_conn,
    TpConnection **client_conn)
{
  TpDBusDaemon *dbus;
  gchar *name;
  gchar *conn_path;
  GError *error = NULL;
  GQuark conn_features[] = { TP_CONNECTION_FEATURE_CONNECTED, 0 };

  g_assert (service_conn != NULL);
  g_assert (client_conn != NULL);

  dbus = tp_tests_dbus_daemon_dup_or_die ();

  *service_conn = tp_tests_object_new_static_class (
        conn_type,
        "account", account,
        "protocol", "simple",
        NULL);
  g_assert (*service_conn != NULL);

  g_assert (tp_base_connection_register (*service_conn, "simple",
        &name, &conn_path, &error));
  g_assert_no_error (error);

  *client_conn = tp_connection_new (dbus, name, conn_path,
      &error);
  g_assert (*client_conn != NULL);
  g_assert_no_error (error);

  tp_cli_connection_call_connect (*client_conn, -1, NULL, NULL, NULL, NULL);
  tp_tests_proxy_run_until_prepared (*client_conn, conn_features);

  g_free (name);
  g_free (conn_path);

  g_object_unref (dbus);
}

/* This object exists solely so that tests/tests.supp can ignore "leaked"
 * classes. */
gpointer
tp_tests_object_new_static_class (GType type,
    ...)
{
  va_list ap;
  GObject *object;
  const gchar *first_property;

  va_start (ap, type);
  first_property = va_arg (ap, const gchar *);
  object = g_object_new_valist (type, first_property, ap);
  va_end (ap);
  return object;
}

static gboolean
time_out (gpointer nil G_GNUC_UNUSED)
{
  g_error ("Timed out");
  g_assert_not_reached ();
  return FALSE;
}

void
tp_tests_abort_after (guint sec)
{
  if (g_getenv ("TP_TESTS_NO_TIMEOUT") != NULL)
    return;

  g_timeout_add_seconds (sec, time_out, NULL);

#ifdef G_OS_UNIX
  /* On Unix, we can kill the process more reliably; this is a safety-catch
   * in case it deadlocks or something, in which case the main loop won't be
   * processed. The default handler for SIGALRM is process termination. */
  alarm (sec + 2);
#endif
}
