#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "src/xdp-utils.h"
#include "document-portal/permission-store-dbus.h"

char outdir[] = "/tmp/xdp-test-XXXXXX";

GTestDBus *dbus;
GDBusConnection *session_bus;
XdgPermissionStore *permissions;

static void
test_version (void)
{
  g_assert_cmpint (xdg_permission_store_get_version (permissions), ==, 1);
}

static int change_count;

static void
changed_cb (XdgPermissionStore *store,
            const char *table,
            const char *id,
            gboolean deleted,
            GVariant *data,
            GVariant *perms,
            gpointer user_data)
{
  char **strv;
  gboolean res;

  change_count++;

  g_assert_cmpstr (table, ==, "TEST");
  g_assert_cmpstr (id, ==, "test-resource");
  g_assert_false (deleted);
  g_assert_true (g_variant_is_of_type (perms, G_VARIANT_TYPE ("a{sas}")));
  res = g_variant_lookup (perms, "one.two.three", "^a&s", &strv);
  g_assert_true (res);
  g_assert_cmpint (g_strv_length (strv), ==, 2);
  g_assert (g_strv_contains ((const char *const *)strv, "one"));
  g_assert (g_strv_contains ((const char *const *)strv, "two"));
}

static void
changed_cb2 (XdgPermissionStore *store,
             const char *table,
             const char *id,
             gboolean deleted,
             GVariant *data,
             GVariant *perms,
             gpointer user_data)
{
  change_count++;

  g_assert_cmpstr (table, ==, "TEST");
  g_assert_cmpstr (id, ==, "test-resource");
  g_assert_true (deleted);
}

static gboolean
timeout_cb (gpointer data)
{
  gboolean *timeout_reached = data;

  *timeout_reached = TRUE;
  return G_SOURCE_CONTINUE;
}

static void
test_change (void)
{
  gulong changed_handler;
  gboolean res;
  g_autoptr(GError) error = NULL;
  const char * perms[] = { "one", "two", NULL };
  gboolean timeout_reached = FALSE;
  guint timeout_id;

  changed_handler = g_signal_connect (permissions, "changed", G_CALLBACK (changed_cb), NULL);

  change_count = 0;

  res = xdg_permission_store_call_set_permission_sync (permissions,
                                                       "TEST", TRUE,
                                                       "test-resource",
                                                       "one.two.three",
                                                       perms,
                                                       NULL,
                                                       &error);
  g_assert_no_error (error);
  g_assert_true (res);

  timeout_id = g_timeout_add (10000, timeout_cb, &timeout_reached);
  while (!timeout_reached && change_count == 0)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout_id);

  g_assert_cmpint (change_count, ==, 1);
                                      
  g_signal_handler_disconnect (permissions, changed_handler);

  changed_handler = g_signal_connect (permissions, "changed", G_CALLBACK (changed_cb2), NULL);

  change_count = 0;

  res = xdg_permission_store_call_delete_sync (permissions,
                                               "TEST",
                                               "test-resource",
                                               NULL,
                                               &error);
  g_assert_no_error (error);
  g_assert_true (res);

  timeout_id = g_timeout_add (10000, timeout_cb, &timeout_reached);
  while (!timeout_reached && change_count == 0)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout_id);

  g_assert_cmpint (change_count, ==, 1);
}

static void
test_lookup (void)
{
  gboolean res;
  g_autoptr(GError) error = NULL;
  const char * perms[] = { "one", "two", NULL };
  g_autoptr(GVariant) p = NULL;
  g_autoptr(GVariant) d = NULL;
  char **strv;
  GVariantBuilder pb;

  res = xdg_permission_store_call_lookup_sync (permissions,
                                               "TEST",
                                               "test-resource",
                                               &p,
                                               &d,
                                               NULL,
                                               &error);
  g_assert_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND);
  g_assert_false (res);
  g_clear_error (&error);

  g_variant_builder_init (&pb, G_VARIANT_TYPE ("a{sas}"));
  g_variant_builder_add (&pb, "{s@as}", "one.two.three", g_variant_new_strv (perms, -1));
  res = xdg_permission_store_call_set_sync (permissions,
                                            "TEST", TRUE,
                                            "test-resource",
                                            g_variant_builder_end (&pb),
                                            g_variant_new_variant (g_variant_new_boolean (TRUE)),
                                            NULL,
                                            &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = xdg_permission_store_call_lookup_sync (permissions,
                                               "TEST",
                                               "test-resource",
                                               &p,
                                               &d,
                                               NULL,
                                               &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_assert_true (g_variant_is_of_type (p, G_VARIANT_TYPE ("a{sas}")));
  res = g_variant_lookup (p, "one.two.three", "^a&s", &strv);
  g_assert_true (res);
  g_assert_cmpint (g_strv_length (strv), ==, 2);
  g_assert (g_strv_contains ((const char *const *)strv, "one"));
  g_assert (g_strv_contains ((const char *const *)strv, "two"));
  g_assert_true (g_variant_is_of_type (d, G_VARIANT_TYPE_VARIANT));
  g_assert_true (g_variant_is_of_type (g_variant_get_variant (d), G_VARIANT_TYPE_BOOLEAN));
  g_assert_true (g_variant_get_boolean (g_variant_get_variant (d)));

  res = xdg_permission_store_call_delete_sync (permissions,
                                               "TEST",
                                               "test-resource",
                                               NULL,
                                               &error);
  g_assert_no_error (error);
}

static void
test_create1 (void)
{
  gboolean res;
  g_autoptr(GError) error = NULL;
  const char * perms[] = { "one", "two", NULL };

  res = xdg_permission_store_call_set_permission_sync (permissions,
                                                       "DOESNOTEXIST", FALSE,
                                                       "test-resource",
                                                       "one.two.three",
                                                       perms,
                                                       NULL,
                                                       &error);
  g_assert_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND);
  g_assert_false (res);
}

static void
test_create2 (void)
{
  gboolean res;
  g_autoptr(GError) error = NULL;
  const char * perms[] = { "one", "two", NULL };

  res = xdg_permission_store_call_set_permission_sync (permissions,
                                                       "DOESNOTEXIST", TRUE,
                                                       "test-resource",
                                                       "one.two.three",
                                                       perms,
                                                       NULL,
                                                       &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

static void
global_setup (void)
{
  GError *error = NULL;
  g_autofree gchar *services = NULL;
  GQuark portal_errors G_GNUC_UNUSED;

  /* make sure errors are registered */
  portal_errors = XDG_DESKTOP_PORTAL_ERROR;

  g_mkdtemp (outdir);
  g_print ("outdir: %s\n", outdir);

  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);
  g_setenv ("XDG_DATA_HOME", outdir, TRUE);

  dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  services = g_test_build_filename (G_TEST_BUILT, "services", NULL);
  g_test_dbus_add_service_dir (dbus, services);
  g_test_dbus_up (dbus);

  /* g_test_dbus_up unsets this, so re-set */
  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  permissions = xdg_permission_store_proxy_new_sync (session_bus, 0,
                                                     "org.freedesktop.impl.portal.PermissionStore",
                                                     "/org/freedesktop/impl/portal/PermissionStore",
                                                     NULL, &error);
  g_assert_no_error (error);
  g_assert (permissions != NULL);
}

static gboolean
rm_rf_dir (GFile         *dir,
           GError       **error)
{
  GFileEnumerator *enumerator = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  enumerator = g_file_enumerate_children (dir, "standard::type,standard::name",
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, error);
  if (!enumerator)
    return FALSE;

  while ((child_info = g_file_enumerator_next_file (enumerator, NULL, &temp_error)))
    {
      const char *name = g_file_info_get_name (child_info);
      g_autoptr(GFile) child = g_file_get_child (dir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!rm_rf_dir (child, error))
            return FALSE;
        }
      else
        {
          if (!g_file_delete (child, NULL, error))
            return FALSE;
        }

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      return FALSE;
    }

  if (!g_file_delete (dir, NULL, error))
    return FALSE;

  return TRUE;
}

static void
global_teardown (void)
{
  GError *error = NULL;
  g_autoptr(GFile) outdir_file = g_file_new_for_path (outdir);
  int res;

  g_object_unref (permissions);

  g_dbus_connection_close_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_object_unref (session_bus);

  g_test_dbus_down (dbus);

  g_object_unref (dbus);

  res = rm_rf_dir (outdir_file, &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

int
main (int argc, char **argv)
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/permissions/version", test_version);
  g_test_add_func ("/permissions/change", test_change);
  g_test_add_func ("/permissions/lookup", test_lookup);
  g_test_add_func ("/permissions/create1", test_create1);
  g_test_add_func ("/permissions/create2", test_create2);

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}
