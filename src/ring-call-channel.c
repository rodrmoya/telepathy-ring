/*
 * ring-call-channel.c - Source for RingCallChannel
 * Handles peer-to-peer calls
 *
 * Copyright (C) 2007-2009 Nokia Corporation
 *   @author Pekka Pessi <first.surname@nokia.com>
 *
 * This work is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This work is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * Based on telepathy-glib/examples/cm/echo/chan.c:
 *
 * """
 * Copyright (C) 2007 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2007 Nokia Corporation
 *
 * Copying and distribution of this file, with or without modification,
 * are permitted in any medium without royalty provided the copyright
 * notice and this notice are preserved.
 * """
 */

#include "config.h"

#define DEBUG_FLAG RING_DEBUG_MEDIA
#include "ring-debug.h"

#include "ring-call-channel.h"
#include "ring-member-channel.h"
#include "ring-call-content.h"
#include "ring-emergency-service.h"

#include "ring-util.h"

#include "modem/service.h"
#include "modem/call.h"
#include "modem/tones.h"
#include "modem/errors.h"

#include <dbus/dbus-glib.h>

#include <telepathy-glib/channel-iface.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/dbus-properties-mixin.h>
#include <telepathy-glib/errors.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/intset.h>
#include <telepathy-glib/svc-channel.h>
#include <telepathy-glib/svc-generic.h>

#include <ring-extensions/ring-extensions.h>

#include "ring-connection.h"
#include "ring-media-manager.h"

#include "ring-param-spec.h"

#if !defined(TP_CHANNEL_CALL_STATE_CONFERENCE_HOST)
/* Added in tp-spec 0.19.11 */
#define TP_CHANNEL_CALL_STATE_CONFERENCE_HOST (32)
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct _RingCallChannelPrivate
{
  guint anon_modes;
  char *dial2nd;
  char *emergency_service;
  char *initial_emergency_service;

  TpHandle peer_handle, initial_remote;
  TpCallMemberFlags peer_flags;

  char *accepted;

  ModemRequest *creating_call;

  struct {
    char *message;
    TpHandle actor;
    TpChannelGroupChangeReason reason;
    guchar causetype, cause;
  } release;

  struct {
    RingConferenceChannel *conference;
    TpHandle handle;
  } member;

  uint8_t state;

  unsigned constructed:1, released:1, shutting_down:1, disposed:1;

  unsigned call_instance_seen:1;

  unsigned originating:1, terminating:1;

  unsigned :0;

  struct {
    gulong emergency;
    gulong waiting, on_hold, forwarded;
    gulong notify_multiparty;
    gulong state;
  } signals;

  struct {
    uint8_t state;
    uint8_t reason;
    uint8_t requested;          /* Hold state requested by client */
  } hold;

  ModemRequest *control;
  guint playing;
  ModemTones *tones;

  GQueue *requests;
};

/* properties */
enum
{
  PROP_NONE,

  PROP_ANON_MODES,

  PROP_CURRENT_SERVICE_POINT, /* o.f.T.C.I.ServicePoint.CurrentServicePoint */
  PROP_INITIAL_SERVICE_POINT, /* o.f.T.C.I.ServicePoint.InitialServicePoint */

  /* ring-specific properties */
  PROP_EMERGENCY_SERVICE,
  PROP_INITIAL_EMERGENCY_SERVICE,

  PROP_TERMINATING,
  PROP_ORIGINATING,

  PROP_MEMBER,
  PROP_MEMBER_MAP,
  PROP_CONFERENCE,

  PROP_PEER,
  PROP_INITIAL_REMOTE,
  PROP_CALL_INSTANCE,
  PROP_TONES,

  PROP_HARDWARE_STREAMING, /* ofdT.Channel.Type.Call1.HardwareStreaming */

  LAST_PROPERTY
};

static TpDBusPropertiesMixinIfaceImpl
ring_call_channel_dbus_property_interfaces[];
static void ring_channel_hold_iface_init(gpointer, gpointer);
static void ring_channel_splittable_iface_init(gpointer, gpointer);

static void ring_call_channel_add_content (
  RingCallChannel *self, const gchar *name,
  TpCallContentDisposition disposition);
static ModemRequest *ring_call_channel_create(RingCallChannel *,
  GError **error);

G_DEFINE_TYPE_WITH_CODE(
  RingCallChannel, ring_call_channel, TP_TYPE_BASE_CALL_CHANNEL,
  G_IMPLEMENT_INTERFACE(RING_TYPE_MEMBER_CHANNEL, NULL);
  G_IMPLEMENT_INTERFACE(TP_TYPE_SVC_CHANNEL_INTERFACE_SERVICE_POINT,
    NULL);
  G_IMPLEMENT_INTERFACE(TP_TYPE_SVC_CHANNEL_INTERFACE_HOLD,
    ring_channel_hold_iface_init);
  G_IMPLEMENT_INTERFACE(RING_TYPE_SVC_CHANNEL_INTERFACE_SPLITTABLE,
    ring_channel_splittable_iface_init));

const char *ring_call_channel_interfaces[] = {
  TP_IFACE_CHANNEL_INTERFACE_SERVICE_POINT,
  TP_IFACE_CHANNEL_INTERFACE_HOLD,
  RING_IFACE_CHANNEL_INTERFACE_SPLITTABLE,
  NULL
};

static void ring_call_channel_fill_immutable_properties(TpBaseChannel *base,
    GHashTable *props);

static void ring_call_channel_play_error_tone(RingCallChannel *self,
  guint state, guint causetype, guint cause);
static void ring_call_channel_close(TpBaseChannel *self);
static void ring_call_channel_hangup(TpBaseCallChannel *self, guint reason,
  const gchar *detailed_reason, const gchar *message);
static void ring_call_channel_set_call_instance(RingCallChannel *_self,
  ModemCall *ci);

static ModemCallService *ring_call_channel_get_call_service (RingCallChannel *self);
static guint ring_call_channel_get_member_handle(RingCallChannel *self);

static void on_modem_call_state(ModemCall *, ModemCallState, RingCallChannel *self);
static void on_modem_call_state_dialing(RingCallChannel *self);
static void on_modem_call_state_incoming(RingCallChannel *self);
static void on_modem_call_state_waiting(RingCallChannel *self);
static void on_modem_call_state_mo_alerting(RingCallChannel *self);
#ifdef nomore
static void on_modem_call_state_mt_alerting(RingCallChannel *self);
static void on_modem_call_state_answered(RingCallChannel *self);
#endif
static void on_modem_call_state_active(RingCallChannel *self);
static void on_modem_call_state_mo_release(RingCallChannel *, guint causetype, guint cause);
#ifdef nomore
static void on_modem_call_state_mt_release(RingCallChannel *, guint causetype, guint cause);
static void on_modem_call_state_terminated(RingCallChannel *, guint causetype, guint cause);
#endif

static void ring_call_channel_released(RingCallChannel *self,
  TpHandle actor, TpChannelGroupChangeReason reason, char const *message,
  GError *error, char const *debug);

static void on_modem_call_emergency(ModemCall *, char const *, RingCallChannel *);
static void on_modem_call_on_hold(ModemCall *, int onhold, RingCallChannel *);
static void on_modem_call_forwarded(ModemCall *, RingCallChannel *);
static void on_modem_call_notify_multiparty(ModemCall *ci, GParamSpec *pspec, gpointer user_data);
static void on_modem_call_waiting(ModemCall *, RingCallChannel *);

/* ====================================================================== */
/* GObject interface */

static void
ring_call_channel_init(RingCallChannel *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE(
    self, RING_TYPE_CALL_CHANNEL, RingCallChannelPrivate);
}

static void
ring_call_channel_constructed(GObject *object)
{
  RingCallChannel *self = RING_CALL_CHANNEL(object);
  RingCallChannelPrivate *priv = self->priv;
  TpBaseChannel *base = TP_BASE_CHANNEL (self);

  if (G_OBJECT_CLASS(ring_call_channel_parent_class)->constructed)
    G_OBJECT_CLASS(ring_call_channel_parent_class)->constructed(object);

  g_assert (priv->peer_handle == tp_base_channel_get_target_handle (base));
  g_assert (tp_base_call_channel_has_initial_audio(
    TP_BASE_CALL_CHANNEL (self), NULL));

  tp_base_call_channel_update_member_flags (TP_BASE_CALL_CHANNEL (object),
    priv->peer_handle, priv->peer_flags,
    0, TP_CALL_STATE_CHANGE_REASON_PROGRESS_MADE, "", "");

  ring_call_channel_add_content(self,
      "Audio", TP_CALL_CONTENT_DISPOSITION_INITIAL);

  tp_base_channel_register(base);

  priv->constructed = 1;
}

static void
ring_call_channel_get_property(GObject *obj,
  guint property_id,
  GValue *value,
  GParamSpec *pspec)
{
  RingCallChannel *self = RING_CALL_CHANNEL(obj);
  RingCallChannelPrivate *priv = self->priv;

  switch (property_id) {
    case PROP_ANON_MODES:
      g_value_set_uint(value, priv->anon_modes);
      break;
    case PROP_ORIGINATING:
      g_value_set_boolean(value, priv->originating);
      break;
    case PROP_TERMINATING:
      g_value_set_boolean(value, priv->terminating);
      break;
    case PROP_MEMBER:
      g_value_set_uint(value, ring_call_channel_get_member_handle(self));
      break;
    case PROP_MEMBER_MAP:
      g_value_take_boxed(
        value, ring_member_channel_get_handlemap(RING_MEMBER_CHANNEL(self)));
      break;
    case PROP_CONFERENCE:
      {
        char *object_path = NULL;
        if (priv->member.conference) {
          g_object_get(priv->member.conference, "object-path", &object_path, NULL);
        }
        else {
          object_path = strdup("/");
        }
        g_value_take_boxed(value, object_path);
      }
      break;
    case PROP_INITIAL_SERVICE_POINT:
      g_value_take_boxed(value,
        ring_emergency_service_new(priv->initial_emergency_service));
      break;
    case PROP_CURRENT_SERVICE_POINT:
      g_value_take_boxed(value,
        ring_emergency_service_new(priv->emergency_service));
      break;
    case PROP_EMERGENCY_SERVICE:
      g_value_set_string(value, priv->emergency_service);
      break;
    case PROP_INITIAL_EMERGENCY_SERVICE:
      g_value_set_string(value, priv->initial_emergency_service);
      break;
    case PROP_PEER:
      g_value_set_uint(value, priv->peer_handle);
      break;
    case PROP_INITIAL_REMOTE:
      g_value_set_uint(value, priv->initial_remote);
      break;
    case PROP_HARDWARE_STREAMING:
      g_value_set_boolean(value, TRUE);
      break;
    case PROP_CALL_INSTANCE:
      g_value_set_pointer(value, self->call_instance);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, property_id, pspec);
      break;
  }
}

static void
ring_call_channel_set_property(GObject *obj,
  guint property_id,
  const GValue *value,
  GParamSpec *pspec)
{
  RingCallChannel *self = RING_CALL_CHANNEL(obj);
  RingCallChannelPrivate *priv = self->priv;

  switch (property_id) {
    case PROP_ANON_MODES:
      priv->anon_modes = g_value_get_uint(value);
      break;
    case PROP_ORIGINATING:
      priv->originating = g_value_get_boolean(value);
      break;
    case PROP_TERMINATING:
      priv->terminating = g_value_get_boolean(value);
      break;
    case PROP_INITIAL_EMERGENCY_SERVICE:
      priv->initial_emergency_service = g_value_dup_string(value);
      break;
    case PROP_PEER:
      priv->peer_handle = g_value_get_uint(value);
      break;
    case PROP_INITIAL_REMOTE:
      priv->initial_remote = g_value_get_uint(value);
      break;
    case PROP_TONES:
      /* media manager owns tones as well as a reference to this channel */
      priv->tones = g_value_get_object(value);
      break;
    case PROP_CALL_INSTANCE:
      ring_call_channel_set_call_instance(self, g_value_get_pointer (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, property_id, pspec);
      break;
  }
}

static void
ring_call_channel_dispose(GObject *object)
{
  RingCallChannel *self = RING_CALL_CHANNEL(object);
  RingCallChannelPrivate *priv = self->priv;

  if (self->priv->disposed)
    return;
  self->priv->disposed = TRUE;

  priv->member.handle = 0;

  if (priv->playing)
    modem_tones_stop(priv->tones, priv->playing);

  /* If still holding on to a call instance, disconnect */
  if (self->call_instance)
    ring_call_channel_set_call_instance (self, NULL);

  ((GObjectClass *)ring_call_channel_parent_class)->dispose(object);
}


static void
ring_call_channel_finalize(GObject *object)
{
  RingCallChannel *self = RING_CALL_CHANNEL(object);
  RingCallChannelPrivate *priv = self->priv;

  g_free(priv->emergency_service);
  g_free(priv->initial_emergency_service);
  g_free(priv->dial2nd);
  g_free(priv->accepted);
  g_free(priv->release.message);

  G_OBJECT_CLASS(ring_call_channel_parent_class)->finalize(object);

  DEBUG("exit");
}

static GPtrArray *
ring_call_channel_get_interfaces (TpBaseChannel *self)
{
  GPtrArray *interfaces;
  gint i;

  interfaces = TP_BASE_CHANNEL_CLASS (ring_call_channel_parent_class)->get_interfaces (self);

  for (i = 0; i < G_N_ELEMENTS (ring_call_channel_interfaces); i++) {
    g_ptr_array_add (interfaces, (gpointer) ring_call_channel_interfaces[i]);
  }

  return interfaces;
}

void reply_to_answer (ModemCall *call_instance,
                      ModemRequest *request,
                      GError *error,
                      gpointer user_data);

static void
ring_call_channel_accept (TpBaseCallChannel *_self)
{
  guint state = 0;
  RingCallChannel *self = RING_CALL_CHANNEL (_self);

  if (tp_base_channel_is_requested (TP_BASE_CHANNEL (self))) {
    char const *destination;
    GError *error = NULL;
    TpHandle handle = tp_base_channel_get_target_handle(TP_BASE_CHANNEL(_self));

    DEBUG("sending outgoing call");

    destination = ring_connection_inspect_contact(
      RING_CONNECTION(tp_base_channel_get_connection (TP_BASE_CHANNEL (_self))),
      handle);

    DEBUG("Trying to start call to %u=\"%s\"", handle, destination);
    if (!modem_call_is_valid_address(destination)) {
      tp_base_call_channel_set_state (TP_BASE_CALL_CHANNEL (self),
        TP_CALL_STATE_ENDED,
        0, TP_CALL_STATE_CHANGE_REASON_INVALID_CONTACT,
        TP_ERROR_STR_INVALID_HANDLE, "Invalid destination");
      return;
    }

    if (ring_call_channel_create (self, &error) == NULL) {
      /* Only errors if the contact was invalid */
      tp_base_call_channel_set_state (TP_BASE_CALL_CHANNEL (self),
        TP_CALL_STATE_ENDED,
        0, TP_CALL_STATE_CHANGE_REASON_INVALID_CONTACT,
        TP_ERROR_STR_INVALID_HANDLE, error->message);
      g_error_free (error);
    }

  } else {
    DEBUG("accepting incoming call");

    if (self->call_instance == NULL) {
      g_warning("Missing call instance");
      return;
    }

    g_object_get (self->call_instance, "state", &state, NULL);
    if (state == MODEM_CALL_STATE_DISCONNECTED) {
      g_warning("Invalid call state");
      return;
    }

    if (!self->priv->accepted)
      self->priv->accepted = g_strdup("Call accepted");

    modem_call_request_answer(self->call_instance, reply_to_answer,
        g_strdup(self->nick));
  }
}

static gchar *
ring_call_channel_get_object_path_suffix (TpBaseChannel *base)
{
  return g_strdup_printf ("CallChannel%p", base);
}


static void
ring_call_channel_add_content (RingCallChannel *self,
  const gchar *name,
  TpCallContentDisposition disposition)
{
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  gchar *object_path;
  TpBaseCallContent *content;
  gchar *escaped;

  /* FIXME could clash when other party in a one-to-one call creates a stream
   * with the same media type and name */
  escaped = tp_escape_as_identifier (name);
  object_path = g_strdup_printf ("%s/Content_%s",
      tp_base_channel_get_object_path (base),
      escaped);
  g_free (escaped);

  content = g_object_new (RING_TYPE_CALL_CONTENT,
    "connection", tp_base_channel_get_connection (base),
    "object-path", object_path,
    "disposition", disposition,
    "name", name,
    NULL);

  g_free (object_path);

  tp_base_call_channel_add_content (TP_BASE_CALL_CHANNEL (self),
    content);

  ring_call_content_add_stream(RING_CALL_CONTENT(content));
}


/* ====================================================================== */
/* GObjectClass */

static void
ring_call_channel_class_init(RingCallChannelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  TpBaseChannelClass *base_chan_class = TP_BASE_CHANNEL_CLASS (klass);
  TpBaseCallChannelClass *base_call_class = TP_BASE_CALL_CHANNEL_CLASS (klass);

  g_type_class_add_private(klass, sizeof (RingCallChannelPrivate));

  object_class->constructed = ring_call_channel_constructed;
  object_class->get_property = ring_call_channel_get_property;
  object_class->set_property = ring_call_channel_set_property;
  object_class->dispose = ring_call_channel_dispose;
  object_class->finalize = ring_call_channel_finalize;

  base_chan_class->target_handle_type = TP_HANDLE_TYPE_CONTACT;
  base_chan_class->get_interfaces = ring_call_channel_get_interfaces;
  base_chan_class->close = ring_call_channel_close;
  base_chan_class->fill_immutable_properties = ring_call_channel_fill_immutable_properties;
  base_chan_class->get_object_path_suffix = ring_call_channel_get_object_path_suffix;

  base_call_class->accept = ring_call_channel_accept;
  base_call_class->hangup = ring_call_channel_hangup;

  klass->dbus_properties_class.interfaces =
    ring_call_channel_dbus_property_interfaces;
  tp_dbus_properties_mixin_class_init(object_class,
    G_STRUCT_OFFSET(RingCallChannelClass, dbus_properties_class));

  g_object_class_install_property(
    object_class, PROP_ANON_MODES, ring_param_spec_anon_modes());

  g_object_class_install_property(
    object_class, PROP_INITIAL_SERVICE_POINT,
    g_param_spec_boxed("initial-service-point",
      "Initial Service Point",
      "The service point initially associated with this channel",
      TP_STRUCT_TYPE_SERVICE_POINT,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_CURRENT_SERVICE_POINT,
    g_param_spec_boxed("current-service-point",
      "Current Service Point",
      "The service point currently associated with this channel",
      TP_STRUCT_TYPE_SERVICE_POINT,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_EMERGENCY_SERVICE,
    g_param_spec_string("emergency-service",
      "Emergency Service",
      "Emergency service associated with this channel",
      "",
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_INITIAL_EMERGENCY_SERVICE,
    g_param_spec_string("initial-emergency-service",
      "Initial Emergency Service",
      "Emergency service initially associated with this channel",
      NULL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_ORIGINATING,
    g_param_spec_boolean("originating",
      "Mobile-Originated Call",
      "Call associated with this channel is mobile-originated",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_TERMINATING,
    g_param_spec_boolean("terminating",
      "Mobile-Terminating Call",
      "Call associated with this channel is mobile-terminating",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_MEMBER,
    g_param_spec_uint("member-handle",
      "Member Handle",
      "Handle representing the channel target in conference",
      0, G_MAXUINT, 0,
      G_PARAM_READABLE |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_MEMBER_MAP,
    g_param_spec_boxed("member-map",
      "Mapping from peer to member handle",
      "Mapping from peer to member handle",
      TP_HASH_TYPE_HANDLE_OWNER_MAP,
      G_PARAM_READABLE |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_CONFERENCE,
    g_param_spec_boxed("member-conference",
      "Conference Channel",
      "Conference Channel this object is associated with",
      DBUS_TYPE_G_OBJECT_PATH,
      G_PARAM_READABLE |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_PEER,
    g_param_spec_uint("peer",
      "Peer handle",
      "Peer handle for this channel",
      0, G_MAXUINT, 0,
      G_PARAM_READWRITE |
      G_PARAM_CONSTRUCT |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_INITIAL_REMOTE,
    g_param_spec_uint("initial-remote",
      "Initial Remote Handle",
      "Handle added to the remote pending set initially",
      0, G_MAXUINT32, 0,    /* min, max, default */
      G_PARAM_READWRITE |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
    object_class, PROP_TONES,
    g_param_spec_object("tones",
      "ModemTones Object",
      "ModemTones for this channel",
      MODEM_TYPE_TONES,
      G_PARAM_WRITABLE |
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class,
      PROP_CALL_INSTANCE,
      g_param_spec_pointer ("call-instance",
          "ModemCall Object",
          "ModemCall instance for this channel",
          /* MODEM_TYPE_CALL, */
          G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));

  g_object_class_override_property (object_class, PROP_HARDWARE_STREAMING,
      "hardware-streaming");
}

/* ====================================================================== */
/**
 * org.freedesktop.DBus properties
 */

/* Properties for o.f.T.Channel.Interface.ServicePoint */
static TpDBusPropertiesMixinPropImpl service_point_properties[] = {
  { "InitialServicePoint", "initial-service-point" },
  { "CurrentServicePoint", "current-service-point" },
  { NULL }
};

static TpDBusPropertiesMixinIfaceImpl
ring_call_channel_dbus_property_interfaces[] = {
  {
    TP_IFACE_CHANNEL_INTERFACE_SERVICE_POINT,
    tp_dbus_properties_mixin_getter_gobject_properties,
    NULL,
    service_point_properties,
  },
  { NULL }
};

static void
ring_call_channel_fill_immutable_properties(TpBaseChannel *base,
    GHashTable *props)
{
  RingCallChannel *self = RING_CALL_CHANNEL (base);
  GObject *obj = (GObject *) self;

  TP_BASE_CHANNEL_CLASS (ring_call_channel_parent_class)->fill_immutable_properties (
      base, props);

  tp_dbus_properties_mixin_fill_properties_hash (obj, props,
    TP_IFACE_CHANNEL_INTERFACE_SERVICE_POINT, "CurrentServicePoint",
    NULL);

  if (!RING_STR_EMPTY(self->priv->initial_emergency_service))
    tp_dbus_properties_mixin_fill_properties_hash (obj, props,
      TP_IFACE_CHANNEL_INTERFACE_SERVICE_POINT, "InitialServicePoint",
      NULL);
}

/* ====================================================================== */
/* RingMediaChannel implementation */

ModemRequest *
ring_call_channel_queue_request (RingCallChannel *self,
  ModemRequest *request)
{
  if (request)
    g_queue_push_tail(self->priv->requests, request);
  return request;
}

ModemRequest *
ring_call_channel_dequeue_request(RingCallChannel *self,
  ModemRequest *request)
{
  if (request)
    g_queue_remove(self->priv->requests, request);
  return request;
}

/* Shutdown modem */
static void
ring_call_channel_shutdown_modem(RingCallChannel *self, const gchar *message)
{
  RingCallChannelPrivate *priv = RING_CALL_CHANNEL(self)->priv;

  DEBUG ("Shutting down the modem call");

  priv->shutting_down = 1;

  if (priv->playing)
    modem_tones_stop(priv->tones, priv->playing);

  if (priv->member.conference) {
    TpHandle actor = priv->release.actor;
    TpChannelGroupChangeReason reason = priv->release.reason;

    ring_conference_channel_emit_channel_removed(
      priv->member.conference, RING_MEMBER_CHANNEL(self),
      message,
      actor, reason);

    /* above emit calls ring_member_channel_left() */
    g_assert(priv->member.conference == NULL);
  }

  if (priv->requests) {
    while (!g_queue_is_empty(priv->requests))
      modem_request_cancel(g_queue_pop_head(priv->requests));

    g_queue_free(priv->requests);
    priv->requests = NULL;
  }

  if (self->call_instance) {
    if (!priv->release.message)
      priv->release.message = g_strdup(message);
    modem_call_request_release(self->call_instance, NULL, NULL);
  }
  else if (priv->creating_call) {
    modem_request_cancel(priv->creating_call);
    priv->creating_call = NULL;
    g_object_unref(self);
  }
}

/** Hangup **/
static void
ring_call_channel_hangup (TpBaseCallChannel *base,
    guint reason,
    const gchar *detailed_reason,
    const gchar *message)
{
  RingCallChannel *self = RING_CALL_CHANNEL (base);

  DEBUG ("Hanging up channel");
  ring_call_channel_shutdown_modem (self, message);

  if (TP_BASE_CALL_CHANNEL_CLASS (ring_call_channel_parent_class)->hangup
      != NULL)
    TP_BASE_CALL_CHANNEL_CLASS (ring_call_channel_parent_class)->hangup
      ( base, reason, detailed_reason, message);
}


/** Close channel */
static void
ring_call_channel_close(TpBaseChannel *_self)
{
  RingCallChannel *self = RING_CALL_CHANNEL (_self);
  ring_call_channel_shutdown_modem (self, "Channel closed");

  TP_BASE_CHANNEL_CLASS (ring_call_channel_parent_class)->close (_self);
}

/* ---------------------------------------------------------------------- */

static void
ring_call_channel_stopped_playing(ModemTones *tones,
  guint source,
  gpointer _self)
{
  RingCallChannel *self = RING_CALL_CHANNEL(_self);
  RingCallChannelPrivate *priv = self->priv;

  if (priv->playing == source) {
    priv->playing = 0;

    if (!self->call_instance) {
      DEBUG("tone ended, closing");
      ring_call_channel_close(TP_BASE_CHANNEL(self));
    }
  }

  g_object_unref(self);
}

static void
ring_call_channel_play_tone(RingCallChannel *self,
  int tone,
  int volume,
  unsigned duration)
{
  RingCallChannelPrivate *priv = self->priv;

  if (priv->shutting_down)
    return;

  if (1)
    /* XXX - no tones so far */
    return;

  if ((tone >= 0 && !modem_tones_is_playing(priv->tones, 0))
    || priv->playing) {
    priv->playing = modem_tones_start_full(priv->tones,
                    tone, volume, duration,
                    ring_call_channel_stopped_playing,
                    g_object_ref(self));
  }
}

void
ring_call_channel_update_state(RingCallChannel *self,
  guint state, guint causetype, guint cause)
{
  RingCallChannelPrivate *priv = self->priv;

  switch (state) {
    case MODEM_CALL_STATE_DIALING:
    case MODEM_CALL_STATE_INCOMING:
    case MODEM_CALL_STATE_WAITING:
    case MODEM_CALL_STATE_ACTIVE:
      if (priv->playing)
        modem_tones_stop(priv->tones, priv->playing);
      break;
    case MODEM_CALL_STATE_ALERTING:
      ring_call_channel_play_tone(self, TONES_EVENT_RINGING, 0, 0);
      break;
    case MODEM_CALL_STATE_DISCONNECTED:
      ring_call_channel_play_error_tone(self, state, causetype, cause);
      break;

#if nomore
    case MODEM_CALL_STATE_TERMINATED:
      if (!priv->released) {
        ring_call_channel_play_error_tone(self, state, causetype, cause);
        break;
      }
      /* FALLTHROUGH */
#endif
    case MODEM_CALL_STATE_INVALID:
      if (priv->playing) {
        int event = modem_tones_playing_event(priv->tones, priv->playing);
        if (event < TONES_EVENT_RADIO_PATH_ACK &&
            modem_tones_is_playing(priv->tones, priv->playing) > 1200)
          modem_tones_stop(priv->tones, priv->playing);
      }
      break;

    default:
      break;
  }

  switch (state) {
    case MODEM_CALL_STATE_DIALING: on_modem_call_state_dialing(self); break;
    case MODEM_CALL_STATE_INCOMING: on_modem_call_state_incoming(self); break;
    case MODEM_CALL_STATE_ALERTING: on_modem_call_state_mo_alerting(self); break;
    case MODEM_CALL_STATE_WAITING: on_modem_call_state_waiting(self); break;
    case MODEM_CALL_STATE_ACTIVE: on_modem_call_state_active(self); break;
    case MODEM_CALL_STATE_DISCONNECTED: on_modem_call_state_mo_release(self, causetype, cause); break;
      /*case MODEM_CALL_STATE_TERMINATED: on_modem_call_state_terminated(self, causetype, cause); break;*/
    default:
      break;
  }
}

static void
ring_call_channel_play_error_tone(RingCallChannel *self,
  guint state, guint causetype, guint cause)
{
  int event_tone;
  guint duration = 5000;
  int volume = 0;

  guint hold;

  if (!self->call_instance)
    return;

  event_tone = modem_call_event_tone(state, causetype, cause);

  hold = TP_LOCAL_HOLD_STATE_UNHELD;
  g_object_get(self, "hold-state", &hold, NULL);

  if (hold != TP_LOCAL_HOLD_STATE_UNHELD &&
    hold != TP_LOCAL_HOLD_STATE_PENDING_UNHOLD) {
    /* XXX - dropped tone damped 3dB if call was on hold */
    event_tone = TONES_EVENT_DROPPED, duration = 1200, volume = -3;
  }

  ring_call_channel_play_tone(self, event_tone, volume, duration);
}

static void
ring_call_channel_set_call_instance(RingCallChannel *self,
  ModemCall *ci)
{
  RingCallChannelPrivate *priv = self->priv;
  ModemCall *old = self->call_instance;

  if (ci == old)
    return;

  if (ci) {
    modem_call_set_handler(ci, self);

    priv->call_instance_seen = 1;

#define CONNECT(n, f)                                                   \
    g_signal_connect(ci, n, G_CALLBACK(on_modem_call_ ## f), self)

    priv->signals.state = CONNECT("state", state);
    priv->signals.waiting = CONNECT("waiting", waiting);
    priv->signals.emergency = CONNECT("emergency", emergency);
    priv->signals.on_hold = CONNECT("on-hold", on_hold);
    priv->signals.forwarded = CONNECT("forwarded", forwarded);
    priv->signals.notify_multiparty = CONNECT("notify::multiparty", notify_multiparty);
#undef CONNECT
  }
  else {
    modem_call_set_handler(old, NULL);

#define DISCONNECT(n)                                           \
    if (priv->signals.n &&                                      \
      g_signal_handler_is_connected(old, priv->signals.n)) {     \
      g_signal_handler_disconnect(old, priv->signals.n);         \
    } (priv->signals.n = 0)

    DISCONNECT(state);
    DISCONNECT(waiting);
    DISCONNECT(emergency);
    DISCONNECT(on_hold);
    DISCONNECT(forwarded);
    DISCONNECT(notify_multiparty);
#undef DISCONNECT
  }

  if (old)
    g_object_unref (old);
  self->call_instance = ci;
  if (ci)
    g_object_ref (ci);

  if (ci == NULL && !priv->playing)
    ring_call_channel_close(TP_BASE_CHANNEL(self));
}

static void reply_to_modem_call_request_dial(ModemCallService *_service,
  ModemRequest *request,
  ModemCall *instance,
  GError *error,
  gpointer _channel);

static ModemRequest *
ring_call_channel_create(RingCallChannel *self, GError **error)
{
  RingCallChannelPrivate *priv = self->priv;
  TpHandle handle = priv->peer_handle;
  char const *destination;
  ModemClirOverride clir;
  char *number = NULL;
  ModemCallService *service;
  ModemRequest *request;

  destination = ring_connection_inspect_contact (
    RING_CONNECTION(tp_base_channel_get_connection(TP_BASE_CHANNEL(self))),
    handle);

  if (RING_STR_EMPTY(destination)) {
    g_set_error(error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT, "Invalid handle");
    return NULL;
  }

  if (priv->anon_modes & TP_ANONYMITY_MODE_CLIENT_INFO)
    clir = MODEM_CLIR_OVERRIDE_ENABLED;
  else if (priv->anon_modes & TP_ANONYMITY_MODE_SHOW_CLIENT_INFO)
    clir = MODEM_CLIR_OVERRIDE_DISABLED;
  else
    clir = MODEM_CLIR_OVERRIDE_DEFAULT;

  if (priv->dial2nd)
    g_free(priv->dial2nd), priv->dial2nd = NULL;

  modem_call_split_address(destination, &number, &priv->dial2nd, &clir);
  if (priv->dial2nd)
    DEBUG("2nd stage dialing: \"%s\"", priv->dial2nd);

  service = ring_call_channel_get_call_service (RING_CALL_CHANNEL (self));

  request = modem_call_request_dial (service, number, clir,
            reply_to_modem_call_request_dial, self);

  g_free(number);

  if (request) {
    priv->creating_call = request;
    g_object_ref(self);
  }

  return request;
}

static void
reply_to_modem_call_request_dial(ModemCallService *_service,
  ModemRequest *request,
  ModemCall *ci,
  GError *error,
  gpointer _channel)
{
  RingCallChannel *self = RING_CALL_CHANNEL(_channel);
  RingCallChannelPrivate *priv = self->priv;
  GError *error0 = NULL;
  TpChannelGroupChangeReason reason;
  char *debug;

  if (request == priv->creating_call) {
    priv->creating_call = NULL;
    g_object_unref(self);
  }

  if (ci) {
    g_assert(self->call_instance == NULL);
    g_object_set(self, "call-instance", ci, NULL);
    if (priv->release.message == NULL)
      ring_call_channel_update_state(self,
        MODEM_CALL_STATE_DIALING, 0, 0);
    else
      modem_call_request_release(ci, NULL, NULL);
    return;
  }

  ring_call_channel_play_tone(self, modem_call_error_tone(error), 0, 4000);

  reason = ring_channel_group_error_reason(error);

  ring_warning("Call.Dial: message=\"%s\" reason=%s (%u) cause=%s.%s",
    error->message, ring_util_reason_name(reason), reason,
    modem_error_domain_prefix(error->domain),
    modem_error_name(error, NULL, 0));
  debug = g_strdup_printf("Dial() failed: reason=%s (%u) cause=%s.%s",
          ring_util_reason_name(reason), reason,
          modem_error_domain_prefix(error->domain),
          modem_error_name(error, NULL, 0));

  ring_call_channel_released(self,
    priv->peer_handle, reason, error->message, error, debug);

  if (error0)
    g_error_free(error0);
  g_free(debug);
}

/* ---------------------------------------------------------------------- */
/* Implement org.freedesktop.Telepathy.Channel.Interface.Hold */

static int ring_update_hold (RingCallChannel *self, int hold, int reason);

static
void get_hold_state(TpSvcChannelInterfaceHold *iface,
  DBusGMethodInvocation *context)
{
  RingCallChannel *self = RING_CALL_CHANNEL(iface);
  RingCallChannelPrivate *priv = self->priv;

  if (self->call_instance == NULL)
    {
      GError *error = NULL;
      g_set_error(&error, TP_ERROR, TP_ERROR_DISCONNECTED,
          "Channel is not connected");
      dbus_g_method_return_error(context, error);
      g_error_free(error);
    }
  else
    {
      tp_svc_channel_interface_hold_return_from_get_hold_state (context,
          priv->hold.state, priv->hold.reason);
    }
}

static ModemCallReply response_to_hold;

static
void request_hold (TpSvcChannelInterfaceHold *iface,
                   gboolean hold,
                   DBusGMethodInvocation *context)
{
  RingCallChannel *self = RING_CALL_CHANNEL (iface);
  RingCallChannelPrivate *priv = self->priv;
  ModemCall *instance = self->call_instance;

  GError *error = NULL;
  uint8_t state, next;
  ModemCallState expect;

  DEBUG ("(%u) on %s", hold, self->nick);

  if (hold)
    {
      expect = MODEM_CALL_STATE_ACTIVE;
      state = TP_LOCAL_HOLD_STATE_HELD;
      next = TP_LOCAL_HOLD_STATE_PENDING_HOLD;
    }
  else
    {
      expect = MODEM_CALL_STATE_HELD;
      state = TP_LOCAL_HOLD_STATE_UNHELD;
      next = TP_LOCAL_HOLD_STATE_PENDING_UNHOLD;
    }

  if (instance == NULL)
    {
      g_set_error(&error, TP_ERROR, TP_ERROR_DISCONNECTED,
          "Channel is not connected");
    }
  else if (state == priv->hold.state || next == priv->hold.state)
    {
      priv->hold.reason = TP_LOCAL_HOLD_STATE_REASON_REQUESTED;
      tp_svc_channel_interface_hold_return_from_request_hold(context);
      return;
    }
  else if (priv->state != expect)
    {
      g_set_error (&error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "Invalid call state %s",
          modem_call_get_state_name(priv->state));
    }
  else if (priv->control)
    {
      g_set_error (&error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
          "Call control operation pending");
    }
  else
    {
      tp_svc_channel_interface_hold_return_from_request_hold(context);

      g_object_ref (self);

      priv->control = modem_call_request_hold (instance, hold,
          response_to_hold, self);
      ring_call_channel_queue_request (self, priv->control);

      priv->hold.requested = state;

      ring_update_hold (self, next, TP_LOCAL_HOLD_STATE_REASON_REQUESTED);
      return;
    }

  DEBUG ("request_hold(%u) on %s: %s", hold, self->nick, error->message);
  dbus_g_method_return_error (context, error);
  g_clear_error (&error);
}

static void
response_to_hold (ModemCall *ci,
                  ModemRequest *request,
                  GError *error,
                  gpointer _self)
{
  RingCallChannel *self = RING_CALL_CHANNEL (_self);
  RingCallChannelPrivate *priv = self->priv;

  if (priv->control == request)
    priv->control = NULL;

  ring_call_channel_dequeue_request (self, request);

  if (error && priv->hold.requested != -1)
    {
      uint8_t next;

      DEBUG ("%s: %s", self->nick, error->message);

      if (priv->hold.requested)
        next = TP_LOCAL_HOLD_STATE_UNHELD;
      else
        next = TP_LOCAL_HOLD_STATE_HELD;

      ring_update_hold (self, next,
          TP_LOCAL_HOLD_STATE_REASON_RESOURCE_NOT_AVAILABLE);

      priv->hold.requested = -1;
    }

  ring_update_hold(self, priv->hold.requested, 0);
  g_object_unref (self);
}

static void ring_channel_hold_iface_init(gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelInterfaceHoldClass *klass = g_iface;

#define IMPLEMENT(x) tp_svc_channel_interface_hold_implement_##x(       \
    klass, x)
  IMPLEMENT(get_hold_state);
  IMPLEMENT(request_hold);
#undef IMPLEMENT
}

static int
ring_update_hold (RingCallChannel *self,
                  int hold,
                  int reason)
{
  RingCallChannelPrivate *priv = self->priv;
  unsigned old = priv->hold.state;
  char const *name;

  if (hold == old)
    return 0;

  switch (hold) {
    case TP_LOCAL_HOLD_STATE_UNHELD:
      name = "Unheld";
      if (reason)
        ;
      else if (hold == priv->hold.requested)
        reason = TP_LOCAL_HOLD_STATE_REASON_REQUESTED;
      else if (old == TP_LOCAL_HOLD_STATE_PENDING_HOLD)
        reason = TP_LOCAL_HOLD_STATE_REASON_RESOURCE_NOT_AVAILABLE;
      else
        reason = TP_LOCAL_HOLD_STATE_REASON_NONE;
      priv->hold.requested = -1;
      break;
    case TP_LOCAL_HOLD_STATE_HELD:
      name = "Held";
      if (reason)
        ;
      else if (hold == priv->hold.requested)
        reason = TP_LOCAL_HOLD_STATE_REASON_REQUESTED;
      else if (old == TP_LOCAL_HOLD_STATE_PENDING_UNHOLD)
        reason = TP_LOCAL_HOLD_STATE_REASON_RESOURCE_NOT_AVAILABLE;
      else
        reason = TP_LOCAL_HOLD_STATE_REASON_NONE;
      priv->hold.requested = -1;
      break;
    case TP_LOCAL_HOLD_STATE_PENDING_HOLD:
      name = "Pending_Hold";
      break;
    case TP_LOCAL_HOLD_STATE_PENDING_UNHOLD:
      name = "Pending_Unhold";
      break;
    default:
      name = "Unknown";
      DEBUG("unknown %s(%d)", "HoldStateChanged", hold);
      return -1;
  }

  g_object_set(self,
    "hold-state", hold,
    "hold-state-reason", reason,
    NULL);

  DEBUG("emitting %s(%s) for %s", "HoldStateChanged", name, self->nick);

  tp_svc_channel_interface_hold_emit_hold_state_changed(
    (TpSvcChannelInterfaceHold *)self,
    hold, reason);

  return 0;
}

static void
on_modem_call_state(ModemCall *ci,
  ModemCallState state,
  RingCallChannel *self)
{
  /* Eh */
  ring_call_channel_update_state(
    RING_CALL_CHANNEL(self), state, 0, 0);
}

static void
ring_call_channel_set_peer_flags (RingCallChannel *self,
   TpCallMemberFlags flag, gboolean set)
{
  RingCallChannelPrivate *priv = self->priv;
  TpCallMemberFlags flags = priv->peer_flags;

  if (set)
    flags |= flag;
  else
    flags &= ~flag;

  if (flags == priv->peer_flags)
    return;

  tp_base_call_channel_update_member_flags (TP_BASE_CALL_CHANNEL (self),
    priv->peer_handle, priv->peer_flags,
    priv->peer_handle, TP_CALL_STATE_CHANGE_REASON_USER_REQUESTED, "", "");
}

/** Remote end has put us on hold */
static void
on_modem_call_on_hold(ModemCall *ci,
  gboolean onhold,
  RingCallChannel *self)
{
  ring_call_channel_set_peer_flags (self, TP_CALL_MEMBER_FLAG_HELD,
    onhold);
}

/** This call has been forwarded */
static void
on_modem_call_forwarded(ModemCall *ci,
  RingCallChannel *self)
{
  /* TODO signal forwarded */
}

static void
on_modem_call_notify_multiparty(ModemCall *ci, GParamSpec *pspec, gpointer user_data)
{
  RingCallChannel *self = RING_CALL_CHANNEL (user_data);
  RingCallChannelPrivate *priv = self->priv;
  gboolean multiparty_member;

  DEBUG ("");

  g_object_get(ci, "multiparty", &multiparty_member, NULL);

  /*
   * This does _not_ cover membership in peer hosted conferences
   * (i.e. when there is no local conference channel).
   **/

  if (priv->member.conference && multiparty_member == FALSE) {
    TpHandle actor = 0; /* unknown actor */
    TpChannelGroupChangeReason reason = TP_CHANNEL_GROUP_CHANGE_REASON_SEPARATED;

    ring_conference_channel_emit_channel_removed(
      priv->member.conference, RING_MEMBER_CHANNEL(self),
      "Conference call split", actor, reason);

    /* above emit calls ring_member_channel_left() */
    g_assert(priv->member.conference == NULL);
  }
}

/* MO call is waiting */
static void
on_modem_call_waiting(ModemCall *ci,
  RingCallChannel *self)
{
  /* TODO signal queued */
}

/* Invoked when MO call targets an emergency service */
static void
on_modem_call_emergency(ModemCall *ci,
  char const *emergency_service,
  RingCallChannel *self)
{
  RingCallChannelPrivate *priv = self->priv;

  DEBUG("%s", emergency_service);

  if (g_strcmp0 (emergency_service, priv->emergency_service) != 0) {
    RingEmergencyService *esp;

    g_free(priv->emergency_service);
    priv->emergency_service = g_strdup(emergency_service);
    g_object_notify(G_OBJECT(self), "emergency-service");

    DEBUG("emitting ServicePointChanged");

    esp = ring_emergency_service_new(emergency_service);
    tp_svc_channel_interface_service_point_emit_service_point_changed(
      (TpSvcChannelInterfaceServicePoint *)self, esp);
    ring_emergency_service_free(esp);
  }
}

/* ---------------------------------------------------------------------- */
void
reply_to_answer (ModemCall *call_instance,
                 ModemRequest *request,
                 GError *error,
                 gpointer user_data)
{
  DEBUG ("%s: %s", (char *)user_data, error ? error->message : "ok");

  g_free (user_data);
}

static void on_modem_call_state_incoming(RingCallChannel *self)
{
  RingCallChannelPrivate *priv = self->priv;
  if (!priv->terminating)
    g_object_set(self, "terminating", TRUE, NULL);
}

static void on_modem_call_state_dialing(RingCallChannel *self)
{
  RingCallChannelPrivate *priv = self->priv;
  if (!priv->originating)
    g_object_set(self, "originating", TRUE, NULL);
}

static void on_modem_call_state_mo_alerting(RingCallChannel *self)
{
  ring_call_channel_set_peer_flags (self, TP_CALL_MEMBER_FLAG_RINGING, TRUE);
}

static void on_modem_call_state_waiting(RingCallChannel *self)
{
}

static gboolean
ring_call_channel_send_dialstring(RingCallChannel *self,
  guint id,
  char const *dialstring,
  guint duration,
  guint pause,
  GError **error)
{
  DEBUG("(%u, \"%s\", %u, %u) for %s",
    id, dialstring, duration, pause, self->nick);

  if (self->call_instance == NULL) {
    g_set_error(error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
      "Channel is not connected");
    return FALSE;
  }
  else if (modem_call_send_dtmf(self->call_instance, dialstring, NULL, NULL) == NULL) {
    g_set_error(error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      "Bad dial string");
    return FALSE;
  }

  return TRUE;
}

static void on_modem_call_state_active(RingCallChannel *self)
{
  RingCallChannelPrivate *priv = self->priv;
  TpHandle actor;

  if (priv->dial2nd) {
    /* p equals 0b1100, 0xC, or "DTMF Control Digits Separator" in the 3GPP
       TS 11.11 section 10.5.1 "Extended BCD coding" table,

       According to 3GPP TS 02.07 appendix B.3.4, 'p', or DTMF Control
       Digits Separator is used "to distinguish between the addressing
       digits (i.e. the phone number) and the DTMF digits." According to the
       B.3.4, "upon the called party answering the ME shall send the DTMF
       digits automatically to the network after a delay of 3 seconds. Upon
       subsequent occurrences of the separator, the ME shall pause again for
       3 seconds (± 20 %) before sending any further DTMF digits."

       According to 3GPP TS 11.11 section 10.5.1 note 6, "A second or
       subsequent 'C'" will be interpreted as a 3 second pause.
    */

    if (!ring_call_channel_send_dialstring(self,
        1, priv->dial2nd, 0, 0, NULL)) {
      DEBUG("Ignoring dialstring \"%s\"", priv->dial2nd);
    }

    g_free(priv->dial2nd), priv->dial2nd = NULL;
  }

  if (tp_base_channel_is_requested (TP_BASE_CHANNEL (self)))
    actor = tp_base_channel_get_target_handle (TP_BASE_CHANNEL (self));
  else
    actor = tp_base_channel_get_self_handle (TP_BASE_CHANNEL (self));

  tp_base_call_channel_set_state (TP_BASE_CALL_CHANNEL (self),
      TP_CALL_STATE_ACTIVE, actor,
      TP_CALL_STATE_CHANGE_REASON_USER_REQUESTED,
      "", "call state change to active");
}

static void
on_modem_call_state_mo_release(RingCallChannel *self,
  guint causetype,
  guint cause)
{
  RingCallChannelPrivate *priv = self->priv;
  char const *message = priv->release.message;
  TpHandle actor = priv->release.actor;
  TpChannelGroupChangeReason reason = priv->release.reason;
  GError *error = NULL;
  char *debug;
  int details = 0;

  error = modem_call_new_error(causetype, cause, NULL);

  if (!actor) {
    if (/*MODEM_CALL_CAUSE_FROM_GSM(causetype)*/1) {
      /* Cancelled by modem for unknown reasons? */
      message = error->message;
      reason = ring_channel_group_release_reason(causetype, cause);
      details = causetype && cause &&
        reason != TP_CHANNEL_GROUP_CHANGE_REASON_BUSY &&
        reason != TP_CHANNEL_GROUP_CHANGE_REASON_NONE;
    }
    else {
      /* Call cancelled by MO but did not come from GSM; likely intentional */
      message = "";
      actor = tp_base_channel_get_self_handle (TP_BASE_CHANNEL (self));
      reason = 0;
      details = 0;
    }
  }

  DEBUG("MO_RELEASE: message=\"%s\" reason=%s (%u) cause=%s.%s (%u.%u)",
    message, ring_util_reason_name(reason), reason,
    modem_error_domain_prefix(error->domain),
    modem_error_name(error, NULL, 0),
    causetype, cause);
  debug = g_strdup_printf("mo-release: reason=%s (%u) cause=%s.%s (%u.%u)",
          ring_util_reason_name(reason), reason,
          modem_error_domain_prefix(error->domain),
          modem_error_name(error, NULL, 0),
          causetype, cause);

  ring_call_channel_released(self, actor, reason, message,
    details ? error : NULL, debug);

  g_error_free(error);
  g_free(debug);
}

#ifdef nomore
static void
on_modem_call_state_mt_release(RingCallChannel *self,
  guint causetype,
  guint cause)
{
  char const *message;
  TpHandle actor;
  TpChannelGroupChangeReason reason;
  GError *error;
  char *debug;
  int details;

  message = "Call released";
  actor = self->priv->peer_handle;

  reason = ring_channel_group_release_reason(causetype, cause);
  error = modem_call_new_error(causetype, cause, NULL);

  details = causetype && cause &&
    reason != TP_CHANNEL_GROUP_CHANGE_REASON_BUSY &&
    reason != TP_CHANNEL_GROUP_CHANGE_REASON_NONE;

  message = error->message;

  DEBUG("MT_RELEASE: message=\"%s\" reason=%s (%u) cause=%s.%s (%u.%u)",
    message, ring_util_reason_name(reason), reason,
    modem_error_domain_prefix(error->domain),
    modem_error_name(error, NULL, 0),
    causetype, cause);
  debug = g_strdup_printf("mt-release: reason=%s (%u) cause=%s.%s (%u.%u)",
          ring_util_reason_name(reason), reason,
          modem_error_domain_prefix(error->domain),
          modem_error_name(error, NULL, 0),
          causetype, cause);

  ring_call_channel_released(self, actor, reason, message,
    details ? error : NULL, debug);

  g_error_free(error);
  g_free(debug);
}

static void
on_modem_call_state_terminated(RingCallChannel *self,
  guint causetype,
  guint cause)
{
  RingCallChannelPrivate *priv = self->priv;
  char const *message;
  TpHandle actor;
  TpChannelGroupChangeReason reason;
  GError *error;
  char *debug;
  int details;

  if (priv->released)
    return;

  message = "Call terminated";
  actor = priv->peer_handle;

  reason = ring_channel_group_release_reason(causetype, cause);
  error = modem_call_new_error(causetype, cause, NULL);

  details = causetype && cause &&
    reason != TP_CHANNEL_GROUP_CHANGE_REASON_BUSY &&
    reason != TP_CHANNEL_GROUP_CHANGE_REASON_NONE;

  message = error->message;

  DEBUG("TERMINATED: message=\"%s\" reason=%s (%u) cause=%s.%s (%u.%u)",
    message, ring_util_reason_name(reason), reason,
    modem_error_domain_prefix(error->domain),
    modem_error_name(error, NULL, 0),
    causetype, cause);
  debug = g_strdup_printf("terminated: reason=%s (%u) cause=%s.%s (%u.%u)",
          ring_util_reason_name(reason), reason,
          modem_error_domain_prefix(error->domain),
          modem_error_name(error, NULL, 0),
          causetype, cause);

  ring_call_channel_released(self, actor, reason, message,
    details ? error : NULL, debug);

  g_error_free(error);
  g_free(debug);
}
#endif

static void
ring_call_channel_released(RingCallChannel *self,
  TpHandle actor,
  TpChannelGroupChangeReason reason,
  char const *message,
  GError *error,
  char const *debug)
{
  char *dbus_error = NULL;

  if (self->priv->released)
    return;
  self->priv->released = 1;

  if (error)
    dbus_error = modem_error_fqn(error);

  if (self->priv->member.conference) {
    ring_conference_channel_emit_channel_removed(
      self->priv->member.conference, RING_MEMBER_CHANNEL(self),
      message, actor, reason);
    /* above emit calls ring_member_channel_left() */
    g_assert(self->priv->member.conference == NULL);
  }

  /* In case of error END it explictilty */
  if (tp_base_call_channel_get_state (TP_BASE_CALL_CHANNEL (self))
      != TP_CALL_STATE_ENDED) {
    tp_base_call_channel_set_state (TP_BASE_CALL_CHANNEL (self),
      TP_CALL_STATE_ENDED,
      actor,
      error == NULL ?  TP_CALL_STATE_CHANGE_REASON_PROGRESS_MADE : TP_CALL_STATE_CHANGE_REASON_INTERNAL_ERROR,
      dbus_error != NULL ? dbus_error : "",
      debug != NULL ? debug : "");
  }

  g_free(dbus_error);
}

/* ---------------------------------------------------------------------- */
/* Conference member */

gboolean
ring_member_channel_is_in_conference(RingMemberChannel const *iface)
{
  return iface && RING_CALL_CHANNEL(iface)->priv->member.conference != NULL;
}


RingConferenceChannel *
ring_member_channel_get_conference(RingMemberChannel const *iface)
{
  if (!iface || !RING_IS_CALL_CHANNEL(iface))
    return FALSE;

  RingCallChannel const *self = RING_CALL_CHANNEL(iface);
  RingCallChannelPrivate const *priv = self->priv;

  return priv->member.conference;
}


gboolean
ring_member_channel_can_become_member(RingMemberChannel const *iface,
  GError **error)
{
  if (!iface || !RING_IS_CALL_CHANNEL(iface)) {
    g_set_error(error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      "Member is not valid channel object");
    return FALSE;
  }

  RingCallChannel const *self = RING_CALL_CHANNEL(iface);
  RingCallChannelPrivate const *priv = self->priv;

  if (!priv->peer_handle) {
    g_set_error(error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      "Member channel has no target");
    return FALSE;
  }
  if (priv->member.conference) {
    g_set_error(error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      "Member channel is already in conference");
    return FALSE;
  }

  if (!self->call_instance) {
    g_set_error(error, TP_ERROR, TP_ERROR_INVALID_ARGUMENT,
      "Member channel has no ongoing call");
    return FALSE;
  }

  if (!modem_call_can_join(self->call_instance)) {
    g_set_error(error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
      "Member channel in state %s",
      modem_call_get_state_name(
        modem_call_get_state(self->call_instance)));
    return FALSE;
  }

  return TRUE;
}

static ModemCallService *
ring_call_channel_get_call_service (RingCallChannel *self)
{
  TpBaseChannel *base = TP_BASE_CHANNEL (self);
  TpBaseConnection *base_connection;
  RingConnection *connection;
  ModemOface *oface;

  base_connection = tp_base_channel_get_connection (base);
  connection = RING_CONNECTION (base_connection);
  oface = ring_connection_get_modem_interface (connection,
      MODEM_OFACE_CALL_MANAGER);

  if (oface)
    return MODEM_CALL_SERVICE (oface);
  else
    return NULL;
}

static guint
ring_call_channel_get_member_handle(RingCallChannel *self)
{
  RingCallChannelPrivate *priv = self->priv;
  TpHandle handle = priv->member.handle;
  TpHandle owner = priv->peer_handle;

  if (handle == 0) {
    TpHandleRepoIface *repo = tp_base_connection_get_handles(
      tp_base_channel_get_connection(TP_BASE_CHANNEL(self)),
      TP_HANDLE_TYPE_CONTACT);
    gpointer context = ring_network_normalization_context();
    char *object_path, *unique, *membername;

    g_object_get(self, "object-path", &object_path, NULL);
    unique = strrchr(object_path, '/');

    membername = g_strdup_printf(
      "%s/%s", tp_handle_inspect(repo, owner),
      unique + strcspn(unique, "0123456789"));

    handle = tp_handle_ensure(repo, membername, context, NULL);

    priv->member.handle = handle;

    g_free(object_path);
    g_free(membername);
  }

  return handle;
}


GHashTable *
ring_member_channel_get_handlemap(RingMemberChannel *iface)
{
  RingCallChannel *self = RING_CALL_CHANNEL(iface);
  RingCallChannelPrivate const *priv = self->priv;
  TpHandle handle = ring_call_channel_get_member_handle(self);
  TpHandle owner = priv->peer_handle;

  GHashTable *handlemap = g_hash_table_new(NULL, NULL);

  g_hash_table_replace(handlemap,
    GUINT_TO_POINTER(handle),
    GUINT_TO_POINTER(owner));

  return handlemap;
}

gboolean
ring_member_channel_release(RingMemberChannel *iface,
  const char *message,
  TpChannelGroupChangeReason reason,
  GError **error)
{
  RingCallChannel *self = RING_CALL_CHANNEL(iface);
  RingCallChannelPrivate *priv = self->priv;

  if (priv->release.message) {
    g_set_error(error, TP_ERROR, TP_ERROR_NOT_AVAILABLE, "already releasing");
    return FALSE;
  }

  if (self->call_instance == NULL) {
    g_set_error(error, TP_ERROR, TP_ERROR_NOT_AVAILABLE, "no call instance");
    return FALSE;
  }

  g_assert(message);

  priv->release.message = g_strdup(message ? message : "Call Released");
  priv->release.actor = tp_base_channel_get_self_handle (TP_BASE_CHANNEL (self));
  priv->release.reason = reason;

  modem_call_request_release(self->call_instance, NULL, NULL);

  return TRUE;
}

void
ring_member_channel_joined(RingMemberChannel *iface,
  RingConferenceChannel *conference)
{
  RingCallChannel *self = RING_CALL_CHANNEL(iface);
  RingCallChannelPrivate *priv = self->priv;

  if (priv->member.conference) {
    DEBUG("switching to a new conference");
    if (priv->member.conference == conference)
      return;
    ring_conference_channel_emit_channel_removed(
      priv->member.conference, iface,
      "Joined new conference",
      tp_base_channel_get_self_handle (TP_BASE_CHANNEL (self)),
      TP_CHANNEL_GROUP_CHANGE_REASON_INVITED);
    /* above emit calls ring_member_channel_left() */
    g_assert(priv->member.conference == NULL);
  }

  g_assert(priv->member.conference == NULL);

  priv->member.conference = g_object_ref(conference);

  DEBUG("%s joined conference %s",
    self->nick,
    RING_CONFERENCE_CHANNEL(conference)->nick);
}

void
ring_member_channel_left(RingMemberChannel *iface)
{
  RingCallChannel *self = RING_CALL_CHANNEL(iface);
  RingCallChannelPrivate *priv = self->priv;

  if (priv->member.conference) {
    g_object_unref(priv->member.conference);
    priv->member.conference = NULL;

    DEBUG("Leaving conference");
  }
  else {
    DEBUG("got Left but not in conference");
  }
}

/* ---------------------------------------------------------------------- */
/* org.freedesktop.Telepathy.Channel.Interface.Splittable */

static ModemCallReply ring_call_channel_request_split_reply;

static void
ring_member_channel_method_split(
  RingSvcChannelInterfaceSplittable *iface,
  DBusGMethodInvocation *context)
{
  RingCallChannel *self = RING_CALL_CHANNEL(iface);
  GError *error;

  DEBUG("enter");

  if (ring_member_channel_is_in_conference(RING_MEMBER_CHANNEL(self))) {
    RingConferenceChannel *conference = self->priv->member.conference;

    if (ring_conference_channel_has_members(conference) <= 1) {

      /*
       * This is handles a race condition between two last members
       * of a conference. If one is currently leaving, and client
       * tries to Split() out the other, this branch is hit. We try
       * to follow Split() semantics even in this case.
       */
      ring_warning("Only one member left in conference unable to split");

      /*
       * Make sure the remaining call is unheld to follow
       * Split() semantics the callerexpects.
       **/
      modem_call_request_hold(self->call_instance,
          0, NULL, context);

      return;
    }

    ModemRequest *request;
    request = modem_call_request_split(self->call_instance,
              ring_call_channel_request_split_reply,
              context);
    modem_request_add_data_full(request,
      "tp-request",
      context,
      ring_method_return_internal_error);
    return;
  }

  g_set_error(&error, TP_ERROR, TP_ERROR_NOT_AVAILABLE,
    "Not a member channel");
  dbus_g_method_return_error(context, error);
  g_error_free(error);
}

static void
ring_call_channel_request_split_reply(ModemCall *instance,
  ModemRequest *request,
  GError *error,
  gpointer _context)
{
  DBusGMethodInvocation *context;

  context = modem_request_steal_data(request, "tp-request");

  g_assert(context);
  g_assert(context == _context);

  if (error) {
    DEBUG("split failed: " GERROR_MSG_FMT, GERROR_MSG_CODE(error));

    GError tperror[1] = {{
        TP_ERROR, TP_ERROR_NOT_AVAILABLE,
        "Cannot LeaveConference"
      }};

    dbus_g_method_return_error(context, tperror);
  }
  else {
    DEBUG("enter");
    ring_svc_channel_interface_splittable_return_from_split
      (context);
  }
}

static void
ring_channel_splittable_iface_init(gpointer g_iface, gpointer iface_data)
{
  RingSvcChannelInterfaceSplittableClass *klass = g_iface;

#define IMPLEMENT(x)                                    \
  ring_svc_channel_interface_splittable_implement_##x   \
    (klass, ring_member_channel_method_ ## x)
  IMPLEMENT(split);
#undef IMPLEMENT
}
