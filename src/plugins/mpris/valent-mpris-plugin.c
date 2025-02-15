// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 Andy Holmes <andrew.g.r.holmes@gmail.com>

#define G_LOG_DOMAIN "valent-mpris-plugin"

#include "config.h"

#include <math.h>

#include <glib/gi18n.h>
#include <libpeas/peas.h>
#include <libvalent-core.h>
#include <libvalent-media.h>

#include "valent-mpris-plugin.h"
#include "valent-mpris-remote.h"


struct _ValentMprisPlugin
{
  ValentDevicePlugin  parent_instance;

  ValentMedia        *media;
  gboolean            media_watch : 1;

  GHashTable         *remotes;
  GHashTable         *artwork_transfers;
};

G_DEFINE_TYPE (ValentMprisPlugin, valent_mpris_plugin, VALENT_TYPE_DEVICE_PLUGIN)

static void valent_mpris_plugin_handle_action       (ValentMprisPlugin *self,
                                                     ValentMediaPlayer *player,
                                                     const char        *action);
static void valent_mpris_plugin_request_player_list (ValentMprisPlugin *self);
static void valent_mpris_plugin_request_update      (ValentMprisPlugin *self,
                                                     const char        *name);
static void valent_mpris_plugin_send_album_art      (ValentMprisPlugin *self,
                                                     ValentMediaPlayer *player,
                                                     const char        *requested_uri);
static void valent_mpris_plugin_send_player_info    (ValentMprisPlugin *self,
                                                     ValentMediaPlayer *player,
                                                     gboolean           now_playing,
                                                     gboolean           volume);
static void valent_mpris_plugin_send_player_list    (ValentMprisPlugin *self);


/*
 * Local Players
 */
static void
send_album_art_cb (ValentTransfer    *transfer,
                   GAsyncResult      *result,
                   ValentMprisPlugin *self)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *id = NULL;

  g_assert (VALENT_IS_TRANSFER (transfer));

  if (!valent_transfer_execute_finish (transfer, result, &error))

    g_debug ("Failed to upload album art: %s", error->message);

  id = valent_transfer_dup_id (transfer);
  g_hash_table_remove (self->artwork_transfers, id);
}

static void
valent_mpris_plugin_send_album_art (ValentMprisPlugin *self,
                                    ValentMediaPlayer *player,
                                    const char        *requested_uri)
{
  g_autoptr (GVariant) metadata = NULL;
  const char *real_uri;
  g_autoptr (GFile) real_file = NULL;
  g_autoptr (GFile) requested_file = NULL;
  JsonBuilder *builder;
  g_autoptr (JsonNode) packet = NULL;
  g_autoptr (ValentTransfer) transfer = NULL;
  ValentDevice *device;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));

  /* Ignore concurrent requests */
  if (g_hash_table_contains (self->artwork_transfers, requested_uri))
    return;

  /* Check player and URL are safe */
  if ((metadata = valent_media_player_get_metadata (player)) == NULL ||
      !g_variant_lookup (metadata, "mpris:artUrl", "&s", &real_uri))
    {
      g_warning ("Album art request \"%s\" for track without album art",
                 requested_uri);
      return;
    }

  /* Compare normalized URLs */
  requested_file = g_file_new_for_uri (requested_uri);
  real_file = g_file_new_for_uri (real_uri);

  if (!g_file_equal (requested_file, real_file))
    {
      g_warning ("Album art request \"%s\" doesn't match current track \"%s\"",
                 requested_uri, real_uri);
      return;
    }

  /* Build the payload packet */
  builder = valent_packet_start ("kdeconnect.mpris");
  json_builder_set_member_name (builder, "player");
  json_builder_add_string_value (builder, valent_media_player_get_name (player));
  json_builder_set_member_name (builder, "albumArtUrl");
  json_builder_add_string_value (builder, requested_uri);
  json_builder_set_member_name (builder, "transferringAlbumArt");
  json_builder_add_boolean_value (builder, TRUE);
  packet = valent_packet_finish (builder);

  /* Start the transfer */
  device = valent_device_plugin_get_device (VALENT_DEVICE_PLUGIN (self));
  transfer = valent_device_transfer_new_for_file (device, packet, real_file);

  g_hash_table_insert (self->artwork_transfers,
                       g_strdup (requested_uri),
                       g_object_ref (transfer));

  valent_transfer_execute (transfer,
                           NULL,
                           (GAsyncReadyCallback)send_album_art_cb,
                           self);
}

static void
on_player_changed (ValentMedia       *media,
                   ValentMediaPlayer *player,
                   ValentMprisPlugin *self)
{
  g_assert (VALENT_IS_MPRIS_PLUGIN (self));

  valent_mpris_plugin_send_player_info (self, player, TRUE, TRUE);
}

static void
on_player_seeked (ValentMedia       *media,
                  ValentMediaPlayer *player,
                  gint64             position,
                  ValentMprisPlugin *self)
{
  const char *name;
  JsonBuilder *builder;
  g_autoptr (JsonNode) packet = NULL;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));

  name = valent_media_player_get_name (player);

  builder = valent_packet_start ("kdeconnect.mpris");
  json_builder_set_member_name (builder, "player");
  json_builder_add_string_value (builder, name);
  json_builder_set_member_name (builder, "pos");
  json_builder_add_int_value (builder, floor (position / 1000));
  packet = valent_packet_finish (builder);

  valent_device_plugin_queue_packet (VALENT_DEVICE_PLUGIN (self), packet);
}

static void
on_players_changed (ValentMedia       *media,
                    ValentMediaPlayer *player,
                    ValentMprisPlugin *self)
{
  valent_mpris_plugin_send_player_list (self);
}

static void
valent_mpris_plugin_handle_action (ValentMprisPlugin *self,
                                   ValentMediaPlayer *player,
                                   const char        *action)
{
  g_assert (VALENT_IS_MPRIS_PLUGIN (self));
  g_assert (VALENT_IS_MEDIA_PLAYER (player));
  g_assert (action && *action);

  if (g_str_equal (action, "Next"))
    valent_media_player_next (player);

  else if (g_str_equal (action, "Pause"))
    valent_media_player_pause (player);

  else if (g_str_equal (action, "Play"))
    valent_media_player_play (player);

  else if (g_str_equal (action, "PlayPause"))
    valent_media_player_play_pause (player);

  else if (g_str_equal (action, "Previous"))
    valent_media_player_previous (player);

  else if (g_str_equal (action, "Stop"))
    valent_media_player_stop (player);

  else
    g_warning ("%s(): Unknown action: %s", G_STRFUNC, action);
}

static void
valent_mpris_plugin_handle_mpris_request (ValentMprisPlugin *self,
                                          JsonNode          *packet)
{
  ValentMediaPlayer *player = NULL;
  const char *name;
  const char *action;
  const char *url;
  gint64 offset;
  gint64 position;
  gboolean request_now_playing;
  gboolean request_volume;
  gint64 volume;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));

  /* Start by checking for a player */
  if (valent_packet_get_string (packet, "player", &name))
    player = valent_media_get_player_by_name (self->media, name);

  if (player == NULL || valent_packet_check_field (packet, "requestPlayerList"))
    {
      valent_mpris_plugin_send_player_list (self);
      return;
    }

  /* A request for a player's status */
  request_now_playing = valent_packet_check_field (packet, "requestNowPlaying");
  request_volume = valent_packet_check_field (packet, "requestVolume");

  if (request_now_playing || request_volume)
    valent_mpris_plugin_send_player_info (self,
                                          player,
                                          request_now_playing,
                                          request_volume);

  /* A player command */
  if (valent_packet_get_string (packet, "action", &action))
    valent_mpris_plugin_handle_action (self, player, action);

  /* A request to change the relative position */
  if (valent_packet_get_int (packet, "Seek", &offset))
    valent_media_player_seek (player, offset * 1000);

  /* A request to change the absolute position */
  if (valent_packet_get_int (packet, "SetPosition", &position))
    {
      offset = (position * 1000) - valent_media_player_get_position (player);
      valent_media_player_seek (player, offset);
    }

  /* A request to change the player volume */
  if (valent_packet_get_int (packet, "setVolume", &volume))
    valent_media_player_set_volume (player, volume / 100.0);

  /* An album art request */
  if (valent_packet_get_string (packet, "albumArtUrl", &url))
    valent_mpris_plugin_send_album_art (self, player, url);
}

static void
valent_mpris_plugin_send_player_info (ValentMprisPlugin *self,
                                      ValentMediaPlayer *player,
                                      gboolean           request_now_playing,
                                      gboolean           request_volume)
{
  const char *name;
  JsonBuilder *builder;
  g_autoptr (JsonNode) response = NULL;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));
  g_assert (VALENT_IS_MEDIA_PLAYER (player));

  /* Start the packet */
  builder = valent_packet_start ("kdeconnect.mpris");

  name = valent_media_player_get_name (player);
  json_builder_set_member_name (builder, "player");
  json_builder_add_string_value (builder, name);

  /* Player State & Metadata */
  if (request_now_playing)
    {
      ValentMediaActions flags;
      gboolean is_playing;
      gint64 position;

      g_autoptr (GVariant) metadata = NULL;
      g_autofree char *artist = NULL;
      const char *title = NULL;
      g_autofree char *now_playing = NULL;

      /* Player State */
      flags = valent_media_player_get_flags (player);
      json_builder_set_member_name (builder, "canPause");
      json_builder_add_boolean_value (builder, (flags & VALENT_MEDIA_ACTION_PAUSE) != 0);
      json_builder_set_member_name (builder, "canPlay");
      json_builder_add_boolean_value (builder, (flags & VALENT_MEDIA_ACTION_PLAY) != 0);
      json_builder_set_member_name (builder, "canGoNext");
      json_builder_add_boolean_value (builder, (flags & VALENT_MEDIA_ACTION_NEXT) != 0);
      json_builder_set_member_name (builder, "canGoPrevious");
      json_builder_add_boolean_value (builder,(flags & VALENT_MEDIA_ACTION_PREVIOUS) != 0);
      json_builder_set_member_name (builder, "canSeek");
      json_builder_add_boolean_value (builder, (flags & VALENT_MEDIA_ACTION_SEEK) != 0);

      is_playing = valent_media_player_is_playing (player);
      json_builder_set_member_name (builder, "isPlaying");
      json_builder_add_boolean_value (builder, is_playing);

      position = valent_media_player_get_position (player);
      json_builder_set_member_name (builder, "pos");
      json_builder_add_int_value (builder, position / 1000);

      /* Track Metadata
       *
       * See: https://www.freedesktop.org/wiki/Specifications/mpris-spec/metadata/
       */
      if ((metadata = valent_media_player_get_metadata (player)) != NULL)
        {
          g_autofree const char **artists = NULL;
          gint64 length;
          const char *art_url;
          const char *album;

          if (g_variant_lookup (metadata, "xesam:artist", "^a&s", &artists) &&
              g_strv_length ((char **)artists) > 0 &&
              g_utf8_strlen (artists[0], -1) > 0)
            {
              artist = g_strjoinv (", ", (char **)artists);
              json_builder_set_member_name (builder, "artist");
              json_builder_add_string_value (builder, artist);
            }

          if (g_variant_lookup (metadata, "xesam:title", "&s", &title) &&
              g_utf8_strlen (title, -1) > 0)
            {
              json_builder_set_member_name (builder, "title");
              json_builder_add_string_value (builder, title);
            }

          if (g_variant_lookup (metadata, "xesam:album", "&s", &album) &&
              g_utf8_strlen (album, -1) > 0)
            {
              json_builder_set_member_name (builder, "album");
              json_builder_add_string_value (builder, album);
            }

          if (g_variant_lookup (metadata, "mpris:length", "x", &length))
            {
              json_builder_set_member_name (builder, "length");
              json_builder_add_int_value (builder, length / 1000);
            }

          if (g_variant_lookup (metadata, "mpris:artUrl", "&s", &art_url))
            {
              json_builder_set_member_name (builder, "albumArtUrl");
              json_builder_add_string_value (builder, art_url);
            }
        }

      /*
       * A composite string only used by kdeconnect-android
       */
      if (artist != NULL && title != NULL)
        now_playing = g_strdup_printf ("%s - %s", artist, title);
      else if (artist != NULL)
        now_playing = g_strdup (artist);
      else if (title != NULL)
        now_playing = g_strdup (title);
      else
        now_playing = g_strdup (_("Unknown"));

      json_builder_set_member_name (builder, "nowPlaying");
      json_builder_add_string_value (builder, now_playing);
    }

  /* Volume Level */
  if (request_volume)
    {
      double level;

      level = floor (valent_media_player_get_volume (player) * 100);
      json_builder_set_member_name (builder, "volume");
      json_builder_add_double_value (builder, level);
    }

  /* Send Response */
  response = valent_packet_finish (builder);
  valent_device_plugin_queue_packet (VALENT_DEVICE_PLUGIN (self), response);
}

static void
valent_mpris_plugin_send_player_list (ValentMprisPlugin *self)
{
  JsonBuilder *builder;
  g_autoptr (JsonNode) packet = NULL;
  g_autoptr (GPtrArray) players = NULL;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));

  builder = valent_packet_start ("kdeconnect.mpris");

  /* Player List */
  json_builder_set_member_name (builder, "playerList");
  json_builder_begin_array (builder);

  players = valent_media_get_players (self->media);

  for (unsigned int i = 0; i < players->len; i++)
    {
      ValentMediaPlayer *player;
      const char *name;

      player = g_ptr_array_index (players, i);

      if ((name = valent_media_player_get_name (player)) != NULL)
        json_builder_add_string_value (builder, name);
    }

  json_builder_end_array (builder);

  /* Album Art */
  json_builder_set_member_name (builder, "supportAlbumArtPayload");
  json_builder_add_boolean_value (builder, TRUE);

  packet = valent_packet_finish (builder);
  valent_device_plugin_queue_packet (VALENT_DEVICE_PLUGIN (self), packet);
}

static void
valent_mpris_plugin_watch_media (ValentMprisPlugin *self,
                                 gboolean           connect)
{
  if (connect == self->media_watch)
    return;

  self->media = valent_media_get_default ();

  if (connect)
    {
      g_signal_connect (self->media,
                        "player-added",
                        G_CALLBACK (on_players_changed),
                        self);
      g_signal_connect (self->media,
                        "player-removed",
                        G_CALLBACK (on_players_changed),
                        self);
      g_signal_connect (self->media,
                        "player-changed",
                        G_CALLBACK (on_player_changed),
                        self);
      g_signal_connect (self->media,
                        "player-seeked",
                        G_CALLBACK (on_player_seeked),
                        self);
      self->media_watch = TRUE;
    }
  else
    {
      g_signal_handlers_disconnect_by_data (self->media, self);
      self->media_watch = FALSE;
    }
}

/*
 * Remote Players
 */
static void
on_remote_method (ValentMprisRemote *remote,
                  const char        *method_name,
                  GVariant          *args,
                  ValentMprisPlugin *self)
{
  ValentMediaPlayer *player = VALENT_MEDIA_PLAYER (remote);
  JsonBuilder *builder;
  g_autoptr (JsonNode) packet = NULL;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));

  builder = valent_packet_start ("kdeconnect.mpris.request");

  json_builder_set_member_name (builder, "player");
  json_builder_add_string_value (builder, valent_media_player_get_name (player));

  if (g_strcmp0 (method_name, "Next") == 0 ||
      g_strcmp0 (method_name, "Pause") == 0 ||
      g_strcmp0 (method_name, "Play") == 0 ||
      g_strcmp0 (method_name, "PlayPause") == 0 ||
      g_strcmp0 (method_name, "Previous") == 0 ||
      g_strcmp0 (method_name, "Stop") == 0)
    {
      json_builder_set_member_name (builder, "action");
      json_builder_add_string_value (builder, method_name);
    }

  // TODO: kdeconnect-android doesn't support `Seek` and instead expects a
  //       `SetPosition` command...
  else if (g_strcmp0 (method_name, "Seek") == 0)
    {
      gint64 offset;

      g_variant_get (args, "(x)", &offset);
      json_builder_set_member_name (builder, "Seek");
      json_builder_add_int_value (builder, floor (offset / 1000));
    }

  // TODO: test kdeconnect-android response to this
  else if (g_strcmp0 (method_name, "SetPosition") == 0)
    {
      const char *track_id;
      gint64 position;

      g_variant_get (args, "(&ox)", &track_id, &position);
      json_builder_set_member_name (builder, "SetPosition");
      json_builder_add_int_value (builder, floor (position / 1000));
    }
  else
    {
      g_object_unref (builder);
      return;
    }

  packet = valent_packet_finish (builder);
  valent_device_plugin_queue_packet (VALENT_DEVICE_PLUGIN (self), packet);
}

static void
on_remote_set_property (ValentMprisRemote *remote,
                        const char        *property_name,
                        GVariant          *value,
                        ValentMprisPlugin *self)
{
  ValentMediaPlayer *player = VALENT_MEDIA_PLAYER (remote);
  JsonBuilder *builder;
  g_autoptr (JsonNode) packet = NULL;

  builder = valent_packet_start ("kdeconnect.mpris.request");

  json_builder_set_member_name (builder, "player");
  json_builder_add_string_value (builder, valent_media_player_get_name (player));

  if (g_strcmp0 (property_name, "Volume") == 0)
    {
      double volume;

      volume = g_variant_get_double (value);

      json_builder_set_member_name (builder, "setVolume");
      json_builder_add_int_value (builder, floor (volume * 100));
    }
  else
    {
      g_object_unref (builder);
      return;
    }

  packet = valent_packet_finish (builder);
  valent_device_plugin_queue_packet (VALENT_DEVICE_PLUGIN (self), packet);
}

static void
valent_mpris_plugin_request_player_list (ValentMprisPlugin *self)
{
  JsonBuilder *builder;
  g_autoptr (JsonNode) packet = NULL;

  builder = valent_packet_start ("kdeconnect.mpris.request");
  json_builder_set_member_name (builder, "requestPlayerList");
  json_builder_add_boolean_value (builder, TRUE);
  packet = valent_packet_finish (builder);

  valent_device_plugin_queue_packet (VALENT_DEVICE_PLUGIN (self), packet);
}

static void
receive_art_cb (ValentTransfer    *transfer,
                GAsyncResult      *result,
                ValentMprisPlugin *self)
{
  g_autoptr (JsonNode) packet = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GError) error = NULL;
  const char *player;
  ValentMprisRemote *remote;

  if (!valent_transfer_execute_finish (transfer, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("%s(): %s", G_STRFUNC, error->message);

      return;
    }

  g_object_get (transfer,
                "file",   &file,
                "packet", &packet,
                NULL);

  if (valent_packet_get_string (packet, "player", &player) &&
      (remote = g_hash_table_lookup (self->remotes, player)) != NULL)
    valent_mpris_remote_update_art (remote, file);
}

static void
valent_mpris_plugin_receive_album_art (ValentMprisPlugin *self,
                                       JsonNode          *packet)
{
  ValentDevice *device;
  g_autoptr (ValentData) data = NULL;
  const char *url;
  g_autofree char *filename = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (ValentTransfer) transfer = NULL;

  if (!valent_packet_get_string (packet, "albumArtUrl", &url))
    {
      g_warning ("%s(): expected \"albumArtUrl\" field holding a string",
                 G_STRFUNC);
      return;
    }

  device = valent_device_plugin_get_device (VALENT_DEVICE_PLUGIN (self));
  data = valent_device_ref_data (device);
  filename = g_compute_checksum_for_string (G_CHECKSUM_MD5, url, -1);
  file = valent_data_new_cache_file (data, filename);

  transfer = valent_device_transfer_new_for_file (device, packet, file);
  valent_transfer_execute (transfer,
                           NULL,
                           (GAsyncReadyCallback)receive_art_cb,
                           self);
}

static void
valent_mpris_plugin_request_album_art (ValentMprisPlugin *self,
                                       const char        *player,
                                       const char        *url,
                                       GVariantDict      *metadata)
{
  ValentDevice *device;
  JsonBuilder *builder;
  g_autoptr (JsonNode) packet = NULL;
  g_autoptr (ValentData) data = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree char *filename = NULL;

  device = valent_device_plugin_get_device (VALENT_DEVICE_PLUGIN (self));
  data = valent_device_ref_data (device);
  filename = g_compute_checksum_for_string (G_CHECKSUM_MD5, url, -1);
  file = valent_data_new_cache_file (data, filename);

  /* If the album art has been cached, update the metadata dictionary */
  if (g_file_query_exists (file, NULL))
    {
      g_autofree char *art_url = NULL;

      art_url = g_file_get_uri (file);
      g_variant_dict_insert (metadata, "mpris:artUrl", "s", art_url);

      return;
    }

  /* Request the album art payload */
  builder = valent_packet_start ("kdeconnect.mpris.request");
  json_builder_set_member_name (builder, "player");
  json_builder_add_string_value (builder, player);
  json_builder_set_member_name (builder, "albumArtUrl");
  json_builder_add_string_value (builder, url);
  packet = valent_packet_finish (builder);

  valent_device_plugin_queue_packet (VALENT_DEVICE_PLUGIN (self), packet);
}

static void
valent_mpris_plugin_request_update (ValentMprisPlugin *self,
                                    const char        *player)
{
  JsonBuilder *builder;
  g_autoptr (JsonNode) packet = NULL;

  builder = valent_packet_start ("kdeconnect.mpris.request");
  json_builder_set_member_name (builder, "player");
  json_builder_add_string_value (builder, player);
  json_builder_set_member_name (builder, "requestNowPlaying");
  json_builder_add_boolean_value (builder, TRUE);
  json_builder_set_member_name (builder, "requestVolume");
  json_builder_add_boolean_value (builder, TRUE);
  packet = valent_packet_finish (builder);

  valent_device_plugin_queue_packet (VALENT_DEVICE_PLUGIN (self), packet);
}

static void
valent_mpris_plugin_handle_player_list (ValentMprisPlugin *self,
                                        JsonArray         *player_list)
{
  GHashTableIter iter;
  unsigned int n_players;
  gpointer key;
  g_autofree const char **names = NULL;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));
  g_assert (player_list != NULL);

#ifndef __clang_analyzer__
  /* Collect the remote player names */
  n_players = json_array_get_length (player_list);
  names = g_new (const char *, n_players + 1);

  for (unsigned int i = 0; i < n_players; i++)
    names[i] = json_array_get_string_element (player_list, i);
  names[n_players] = NULL;

  /* Remove old players */
  g_hash_table_iter_init (&iter, self->remotes);

  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      if (!g_strv_contains (names, key))
        g_hash_table_iter_remove (&iter);
    }

  /* Add new players */
  for (unsigned int i = 0; names[i]; i++)
    {
      ValentMprisRemote *remote;
      const char *player = names[i];

      if (g_hash_table_contains (self->remotes, player))
        continue;

      remote = valent_mpris_remote_new ();
      valent_mpris_remote_set_name (remote, player);
      g_hash_table_insert (self->remotes, g_strdup (player), remote);

      g_signal_connect (remote,
                        "method-call",
                        G_CALLBACK (on_remote_method),
                        self);
      g_signal_connect (remote,
                        "set-property",
                        G_CALLBACK (on_remote_set_property),
                        self);

      valent_mpris_remote_export (remote);
      valent_mpris_plugin_request_update (self, player);
    }
#endif /* __clang_analyzer__ */
}

static void
valent_mpris_plugin_handle_player_update (ValentMprisPlugin *self,
                                          JsonNode          *packet)
{
  ValentMprisRemote *remote;
  const char *player;
  const char *url;
  ValentMediaActions flags = VALENT_MEDIA_ACTION_NONE;
  GVariantDict metadata;
  const char *artist, *title, *album;
  gint64 length, position;
  gint64 volume;
  double volume_level = 100.0;
  gboolean is_playing;

  /* Get the remote */
  if (!valent_packet_get_string (packet, "player", &player) ||
      (remote = g_hash_table_lookup (self->remotes, player)) == NULL)
    {
      valent_mpris_plugin_request_player_list (self);
      return;
    }

  if (valent_packet_check_field (packet, "transferringAlbumArt"))
    {
      valent_mpris_plugin_receive_album_art (self, packet);
      return;
    }

  /* Available actions */
  if (valent_packet_check_field (packet, "canGoNext"))
    flags |= VALENT_MEDIA_ACTION_NEXT;

  if (valent_packet_check_field (packet, "canGoPrevious"))
    flags |= VALENT_MEDIA_ACTION_PREVIOUS;

  if (valent_packet_check_field (packet, "canPause"))
    flags |= VALENT_MEDIA_ACTION_PAUSE;

  if (valent_packet_check_field (packet, "canPlay"))
    flags |= VALENT_MEDIA_ACTION_PLAY;

  if (valent_packet_check_field (packet, "canSeek"))
    flags |= VALENT_MEDIA_ACTION_SEEK;

  /* Metadata */
  g_variant_dict_init (&metadata, NULL);

  if (valent_packet_get_string (packet, "artist", &artist))
    {
      g_auto (GStrv) artists = NULL;
      GVariant *value;

      artists = g_strsplit (artist, ",", -1);
      value = g_variant_new_strv ((const char * const *)artists, -1);
      g_variant_dict_insert_value (&metadata, "xesam:artist", value);
    }

  if (valent_packet_get_string (packet, "title", &title))
    g_variant_dict_insert (&metadata, "xesam:title", "s", title);

  if (valent_packet_get_string (packet, "album", &album))
    g_variant_dict_insert (&metadata, "xesam:album", "s", album);

  if (valent_packet_get_int (packet, "length", &length))
    g_variant_dict_insert (&metadata, "mpris:length", "x", length);

  if (valent_packet_get_string (packet, "albumArtUrl", &url))
    valent_mpris_plugin_request_album_art (self, player, url, &metadata);

  /* Playback Status */
  is_playing = valent_packet_check_field (packet, "isPlaying");

  if (!valent_packet_get_int (packet, "pos", &position))
    position = 0;

  if (valent_packet_get_int (packet, "volume", &volume))
    volume_level = volume / 100;

  valent_mpris_remote_update_player (remote,
                                     flags,
                                     g_variant_dict_end (&metadata),
                                     is_playing ? "Playing" : "Paused",
                                     position,
                                     volume_level);
}

static void
valent_mpris_plugin_handle_mpris (ValentMprisPlugin *self,
                                  JsonNode          *packet)
{
  JsonArray *player_list;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));
  g_assert (VALENT_IS_PACKET (packet));

  if (valent_packet_get_array (packet, "playerList", &player_list))
    valent_mpris_plugin_handle_player_list (self, player_list);

  else if (valent_packet_get_string (packet, "player", NULL))
    valent_mpris_plugin_handle_player_update (self, packet);
}

/*
 * ValentDevicePlugin
 */
static void
valent_mpris_plugin_enable (ValentDevicePlugin *plugin)
{
  ValentMprisPlugin *self = VALENT_MPRIS_PLUGIN (plugin);

  self->media = valent_media_get_default ();
}

static void
valent_mpris_plugin_disable (ValentDevicePlugin *plugin)
{
  ValentMprisPlugin *self = VALENT_MPRIS_PLUGIN (plugin);

  valent_mpris_plugin_watch_media (self, FALSE);
  self->media = NULL;
}

static void
valent_mpris_plugin_update_state (ValentDevicePlugin *plugin,
                                  ValentDeviceState   state)
{
  ValentMprisPlugin *self = VALENT_MPRIS_PLUGIN (plugin);
  gboolean available;

  g_assert (VALENT_IS_MPRIS_PLUGIN (self));

  available = (state & VALENT_DEVICE_STATE_CONNECTED) != 0 &&
              (state & VALENT_DEVICE_STATE_PAIRED) != 0;

  if (available)
    {
      valent_mpris_plugin_watch_media (self, TRUE);
      valent_mpris_plugin_request_player_list (self);
      valent_mpris_plugin_send_player_list (self);
    }
  else
    {
      valent_mpris_plugin_watch_media (self, FALSE);
      g_hash_table_remove_all (self->remotes);
    }
}

static void
valent_mpris_plugin_handle_packet (ValentDevicePlugin *plugin,
                                   const char         *type,
                                   JsonNode           *packet)
{
  ValentMprisPlugin *self = VALENT_MPRIS_PLUGIN (plugin);

  g_assert (VALENT_IS_MPRIS_PLUGIN (plugin));
  g_assert (type != NULL);
  g_assert (VALENT_IS_PACKET (packet));

  if (strcmp (type, "kdeconnect.mpris") == 0)
    valent_mpris_plugin_handle_mpris (self, packet);

  else if (strcmp (type, "kdeconnect.mpris.request") == 0)
    valent_mpris_plugin_handle_mpris_request (self, packet);

  else
    g_assert_not_reached ();
}

/*
 * GObject
 */
static void
valent_mpris_plugin_finalize (GObject *object)
{
  ValentMprisPlugin *self = VALENT_MPRIS_PLUGIN (object);

  g_clear_pointer (&self->artwork_transfers, g_hash_table_unref);
  g_clear_pointer (&self->remotes, g_hash_table_unref);

  G_OBJECT_CLASS (valent_mpris_plugin_parent_class)->finalize (object);
}

static void
valent_mpris_plugin_class_init (ValentMprisPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ValentDevicePluginClass *plugin_class = VALENT_DEVICE_PLUGIN_CLASS (klass);

  object_class->finalize = valent_mpris_plugin_finalize;

  plugin_class->enable = valent_mpris_plugin_enable;
  plugin_class->disable = valent_mpris_plugin_disable;
  plugin_class->handle_packet = valent_mpris_plugin_handle_packet;
  plugin_class->update_state = valent_mpris_plugin_update_state;
}

static void
valent_mpris_plugin_init (ValentMprisPlugin *self)
{
  self->artwork_transfers = g_hash_table_new_full (g_str_hash,
                                                   g_str_equal,
                                                   g_free,
                                                   g_object_unref);
  self->remotes = g_hash_table_new_full (g_str_hash,
                                         g_str_equal,
                                         g_free,
                                         g_object_unref);
}

