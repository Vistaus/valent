// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 Andy Holmes <andrew.g.r.holmes@gmail.com>

#include <gio/gio.h>
#include <libvalent-core.h>
#include <libvalent-session.h>
#include <libvalent-test.h>


typedef struct
{
  ValentSession        *session;
  ValentSessionAdapter *adapter;
  gpointer              data;
} SessionComponentFixture;

static void
session_component_fixture_set_up (SessionComponentFixture *fixture,
                                  gconstpointer            user_data)
{
  fixture->session = valent_session_get_default ();
  fixture->adapter = valent_test_await_adapter (fixture->session);

  g_object_ref (fixture->adapter);
}

static void
session_component_fixture_tear_down (SessionComponentFixture *fixture,
                                     gconstpointer            user_data)
{
  v_assert_finalize_object (fixture->session);
  v_await_finalize_object (fixture->adapter);
}

static void
on_changed (ValentSessionAdapter    *adapter,
            SessionComponentFixture *fixture)
{
  fixture->data = adapter;
}

static void
test_session_component_adapter (SessionComponentFixture *fixture,
                                 gconstpointer            user_data)
{
  gboolean active, locked;
  PeasPluginInfo *plugin_info;

  /* Compare Device & Aggregator */
  g_object_get (fixture->adapter,
                "active",      &active,
                "locked",      &locked,
                "plugin-info", &plugin_info,
                NULL);

  g_assert_false (active);
  g_assert_false (locked);
  g_assert_nonnull (plugin_info);

  g_boxed_free (PEAS_TYPE_PLUGIN_INFO, plugin_info);

  /* Change adapter */
  g_signal_connect (fixture->adapter,
                    "changed",
                    G_CALLBACK (on_changed),
                    fixture);

  g_object_set (fixture->adapter,
                "locked", !locked,
                NULL);

  g_assert_true (valent_session_adapter_get_locked (fixture->adapter));
  g_assert_true (fixture->data == fixture->adapter);
  fixture->data = NULL;
}

static void
test_session_component_self (SessionComponentFixture *fixture,
                             gconstpointer            user_data)
{
  gboolean session_active, session_locked;
  gboolean adapter_active, adapter_locked;

  /* Compare session & adapter */
  session_active = valent_session_get_active (fixture->session);
  session_locked = valent_session_get_locked (fixture->session);

  g_object_get (fixture->adapter,
                "active",      &adapter_active,
                "locked",      &adapter_locked,
                NULL);

  g_assert_true (session_active == adapter_active);
  g_assert_true (session_locked == adapter_locked);

  /* Change session */
  g_signal_connect (fixture->adapter,
                    "changed",
                    G_CALLBACK (on_changed),
                    fixture);

  valent_session_set_locked (fixture->session, !session_locked);

  g_assert_true (valent_session_get_locked (fixture->session));
  g_assert_true (fixture->data == fixture->adapter);
  fixture->data = NULL;
}

int
main (int   argc,
      char *argv[])
{
  valent_test_init (&argc, &argv, NULL);

  g_test_add ("/components/session/adapter",
              SessionComponentFixture, NULL,
              session_component_fixture_set_up,
              test_session_component_adapter,
              session_component_fixture_tear_down);

  g_test_add ("/components/session/self",
              SessionComponentFixture, NULL,
              session_component_fixture_set_up,
              test_session_component_self,
              session_component_fixture_tear_down);

  return g_test_run ();
}
