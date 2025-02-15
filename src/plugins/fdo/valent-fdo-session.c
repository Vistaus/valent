// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 Andy Holmes <andrew.g.r.holmes@gmail.com>

#define G_LOG_DOMAIN "valent-fdo-session"

#include "config.h"

#include <libpeas/peas.h>
#include <libvalent-core.h>
#include <libvalent-session.h>

#include "valent-fdo-session.h"


struct _ValentFdoSession
{
  ValentSessionAdapter  parent_instance;

  GDBusProxy           *proxy;
  GCancellable         *cancellable;

  gboolean              active;
  gboolean              locked;
};

G_DEFINE_TYPE (ValentFdoSession, valent_fdo_session, VALENT_TYPE_SESSION_ADAPTER)


/*
 * DBus Callbacks
 */
static void
on_properties_changed (GDBusProxy       *proxy,
                       GVariant         *changed_properties,
                       GStrv             invalidated_properties,
                       ValentFdoSession *self)
{
  g_assert (VALENT_IS_FDO_SESSION (self));

  if (!g_variant_lookup (changed_properties, "Active", "b", &self->active))
    return;

  g_object_notify (G_OBJECT (self), "active");
  valent_session_adapter_emit_changed (VALENT_SESSION_ADAPTER (self));
}

static void
on_signal (GDBusProxy       *proxy,
           const char       *sender_name,
           const char       *signal_name,
           GVariant         *parameters,
           ValentFdoSession *self)
{
  g_assert (VALENT_IS_FDO_SESSION (self));

  if (g_strcmp0 (signal_name, "Lock") == 0)
    self->locked = TRUE;
  else if (g_strcmp0 (signal_name, "Unlock") == 0)
    self->locked = FALSE;

  g_object_notify (G_OBJECT (self), "locked");
  valent_session_adapter_emit_changed (VALENT_SESSION_ADAPTER (self));
}

/*
 * ValentSessionAdapter
 */
static gboolean
valent_fdo_session_get_active (ValentSessionAdapter *adapter)
{
  ValentFdoSession *self = VALENT_FDO_SESSION (adapter);

  g_assert (VALENT_IS_FDO_SESSION (self));

  return self->active;
}

static gboolean
valent_fdo_session_get_locked (ValentSessionAdapter *adapter)
{
  ValentFdoSession *self = VALENT_FDO_SESSION (adapter);

  g_assert (VALENT_IS_FDO_SESSION (self));

  return self->locked;
}

static void
valent_fdo_session_set_locked (ValentSessionAdapter *adapter,
                               gboolean              state)
{
  ValentFdoSession *self = VALENT_FDO_SESSION (adapter);

  g_assert (VALENT_IS_FDO_SESSION (self));

  if (self->proxy == NULL)
    return;

  g_dbus_proxy_call (self->proxy,
                     state ? "Lock" : "Unlock",
                     NULL,
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,
                     NULL,
                     NULL);
}

static void
new_session_cb (GObject          *object,
                GAsyncResult     *result,
                ValentFdoSession *self)
{
  g_autoptr (GVariant) active = NULL;
  g_autoptr (GVariant) locked = NULL;
  g_autoptr (GError) error = NULL;

  g_assert (VALENT_IS_FDO_SESSION (self));

  self->proxy = g_dbus_proxy_new_for_bus_finish (result, &error);

  if (self->proxy == NULL)
    {
      if (!valent_error_ignore (error))
        g_warning ("%s(): %s", G_STRFUNC, error->message);

      return;
    }

  /* Preload properties */
  active = g_dbus_proxy_get_cached_property (self->proxy, "Active");

  if (active != NULL)
    self->active = g_variant_get_boolean (active);

  locked = g_dbus_proxy_get_cached_property (self->proxy, "LockedHint");

  if (locked != NULL)
    self->locked = g_variant_get_boolean (locked);

  /* Watch for changes */
  g_signal_connect (self->proxy,
                    "g-properties-changed",
                    G_CALLBACK (on_properties_changed),
                    self);

  g_signal_connect (self->proxy,
                    "g-signal",
                    G_CALLBACK (on_signal),
                    self);
}

static void
get_display_cb (GDBusConnection  *connection,
                GAsyncResult     *result,
                ValentFdoSession *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) value = NULL;
  const char *session_id, *object_path;

  g_assert (VALENT_IS_FDO_SESSION (self));

  reply = g_dbus_connection_call_finish (connection, result, &error);

  if (reply == NULL)
    {
      if (!valent_error_ignore (error))
        g_warning ("%s(): %s", G_STRFUNC, error->message);

      return;
    }

  g_variant_get (reply, "(v)", &value);
  g_variant_get (value, "(&s&o)", &session_id, &object_path);
  g_dbus_proxy_new (connection,
                    G_DBUS_PROXY_FLAGS_NONE,
                    NULL,
                    "org.freedesktop.login1",
                    object_path,
                    "org.freedesktop.login1.Session",
                    self->cancellable,
                    (GAsyncReadyCallback)new_session_cb,
                    self);
}

static void
get_user_cb (GDBusConnection  *connection,
             GAsyncResult     *result,
             ValentFdoSession *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  const char *object_path;

  g_assert (VALENT_IS_FDO_SESSION (self));

  reply = g_dbus_connection_call_finish (connection, result, &error);

  if (reply == NULL)
    {
      if (!valent_error_ignore (error))
        g_warning ("%s(): %s", G_STRFUNC, error->message);

      return;
    }

  g_variant_get (reply, "(&o)", &object_path);
  g_dbus_connection_call (connection,
                          "org.freedesktop.login1",
                          object_path,
                          "org.freedesktop.DBus.Properties",
                          "Get",
                          g_variant_new ("(ss)",
                                         "org.freedesktop.login1.User",
                                         "Display"),
                          G_VARIANT_TYPE ("(v)"),
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          self->cancellable,
                          (GAsyncReadyCallback)get_display_cb,
                          self);
}

/*
 * GObject
 */
static void
valent_fdo_session_constructed (GObject *object)
{
  ValentFdoSession *self = VALENT_FDO_SESSION (object);
  g_autoptr (GDBusConnection) connection = NULL;

  /* Check for phosh session */
  if ((connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL)) != NULL)
    {
      g_dbus_connection_call (connection,
                              "org.freedesktop.login1",
                              "/org/freedesktop/login1",
                              "org.freedesktop.login1.Manager",
                              "GetUser",
                              g_variant_new ("(u)", geteuid ()),
                              G_VARIANT_TYPE ("(o)"),
                              G_DBUS_CALL_FLAGS_NONE,
                              -1,
                              self->cancellable,
                              (GAsyncReadyCallback)get_user_cb,
                              self);
    }

  G_OBJECT_CLASS (valent_fdo_session_parent_class)->constructed (object);
}

static void
valent_fdo_session_dispose (GObject *object)
{
  ValentFdoSession *self = VALENT_FDO_SESSION (object);

  if (!g_cancellable_is_cancelled (self->cancellable))
    g_cancellable_cancel (self->cancellable);

  if (self->proxy != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->proxy, self);
      g_clear_object (&self->proxy);
    }

  G_OBJECT_CLASS (valent_fdo_session_parent_class)->dispose (object);
}

static void
valent_fdo_session_finalize (GObject *object)
{
  ValentFdoSession *self = VALENT_FDO_SESSION (object);

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (valent_fdo_session_parent_class)->finalize (object);
}

static void
valent_fdo_session_class_init (ValentFdoSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ValentSessionAdapterClass *session_class = VALENT_SESSION_ADAPTER_CLASS (klass);

  object_class->constructed = valent_fdo_session_constructed;
  object_class->dispose = valent_fdo_session_dispose;
  object_class->finalize = valent_fdo_session_finalize;

  session_class->get_active = valent_fdo_session_get_active;
  session_class->get_locked = valent_fdo_session_get_locked;
  session_class->set_locked = valent_fdo_session_set_locked;
}

static void
valent_fdo_session_init (ValentFdoSession *self)
{
  self->cancellable = g_cancellable_new ();
}

