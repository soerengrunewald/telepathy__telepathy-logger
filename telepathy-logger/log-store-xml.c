/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2003-2007 Imendio AB
 * Copyright (C) 2007-2011 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 *          Jonny Lamb <jonny.lamb@collabora.co.uk>
 *          Cosimo Alfarano <cosimo.alfarano@collabora.co.uk>
 */

#include "config.h"
#include "log-store-xml-internal.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>

#include <glib-object.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <telepathy-glib/account.h>
#include <telepathy-glib/account-manager.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/defs.h>
#include <telepathy-glib/util.h>

#include <telepathy-logger/event-internal.h>
#include <telepathy-logger/text-event.h>
#include <telepathy-logger/text-event-internal.h>
#include <telepathy-logger/log-manager.h>
#include <telepathy-logger/log-store-internal.h>
#include <telepathy-logger/log-manager-internal.h>

#define DEBUG_FLAG TPL_DEBUG_LOG_STORE
#include <telepathy-logger/entity-internal.h>
#include <telepathy-logger/datetime-internal.h>
#include <telepathy-logger/debug-internal.h>
#include <telepathy-logger/util-internal.h>

#define LOG_DIR_CREATE_MODE       (S_IRUSR | S_IWUSR | S_IXUSR)
#define LOG_FILE_CREATE_MODE      (S_IRUSR | S_IWUSR)
#define LOG_DIR_CHATROOMS         "chatrooms"
#define LOG_FILENAME_SUFFIX       ".log"
#define LOG_TIME_FORMAT_FULL      "%Y%m%dT%H:%M:%S"
#define LOG_TIME_FORMAT           "%Y%m%d"
#define LOG_HEADER \
    "<?xml version='1.0' encoding='utf-8'?>\n" \
    "<?xml-stylesheet type=\"text/xsl\" href=\"log-store-xml.xsl\"?>\n" \
    "<log>\n"

#define LOG_FOOTER \
    "</log>\n"


struct _TplLogStoreXmlPriv
{
  gchar *basedir;
  gchar *name;
  gboolean readable;
  gboolean writable;
  gboolean empathy_legacy;
  gboolean test_mode;
  TpAccountManager *account_manager;
};

enum {
    PROP_0,
    PROP_NAME,
    PROP_READABLE,
    PROP_WRITABLE,
    PROP_BASEDIR,
    PROP_EMPATHY_LEGACY,
    PROP_TESTMODE
};

static void log_store_iface_init (gpointer g_iface, gpointer iface_data);
static void tpl_log_store_xml_get_property (GObject *object, guint param_id, GValue *value,
    GParamSpec *pspec);
static void tpl_log_store_xml_set_property (GObject *object, guint param_id, const GValue *value,
    GParamSpec *pspec);
static const gchar *log_store_xml_get_name (TplLogStore *store);
static void log_store_xml_set_name (TplLogStoreXml *self, const gchar *data);
static const gchar *log_store_xml_get_basedir (TplLogStoreXml *self);
static void log_store_xml_set_basedir (TplLogStoreXml *self,
    const gchar *data);
static void log_store_xml_set_writable (TplLogStoreXml *self, gboolean data);
static void log_store_xml_set_readable (TplLogStoreXml *self, gboolean data);


G_DEFINE_TYPE_WITH_CODE (TplLogStoreXml, _tpl_log_store_xml,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TPL_TYPE_LOG_STORE, log_store_iface_init))

static void
log_store_xml_dispose (GObject *object)
{
  TplLogStoreXml *self = TPL_LOG_STORE_XML (object);
  TplLogStoreXmlPriv *priv = self->priv;

  /* FIXME See TP-bug #25569, when dispose a non prepared TP_AM, it
     might segfault.
     To avoid it, a *klduge*, a reference in the TplObserver to
     the TplLogManager is kept, so that until TplObserver is instanced,
     there will always be a TpLogManager reference and it won't be
     diposed */
  if (priv->account_manager != NULL)
    {
      g_object_unref (priv->account_manager);
      priv->account_manager = NULL;
    }

  G_OBJECT_CLASS (_tpl_log_store_xml_parent_class)->dispose (object);
}


static void
log_store_xml_finalize (GObject *object)
{
  TplLogStoreXml *self = TPL_LOG_STORE_XML (object);
  TplLogStoreXmlPriv *priv = self->priv;

  if (priv->basedir != NULL)
    {
      g_free (priv->basedir);
      priv->basedir = NULL;
    }
  if (priv->name != NULL)
    {
      g_free (priv->name);
      priv->name = NULL;
    }
}


static void
tpl_log_store_xml_get_property (GObject *object,
    guint param_id,
    GValue *value,
    GParamSpec *pspec)
{
  TplLogStoreXmlPriv *priv = TPL_LOG_STORE_XML (object)->priv;

  switch (param_id)
    {
      case PROP_NAME:
        g_value_set_string (value, priv->name);
        break;
      case PROP_WRITABLE:
        g_value_set_boolean (value, priv->writable);
        break;
      case PROP_READABLE:
        g_value_set_boolean (value, priv->readable);
        break;
      case PROP_BASEDIR:
        g_value_set_string (value, priv->basedir);
        break;
      case PROP_EMPATHY_LEGACY:
        g_value_set_boolean (value, priv->empathy_legacy);
        break;
      case PROP_TESTMODE:
        g_value_set_boolean (value, priv->test_mode);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    };
}


static void
tpl_log_store_xml_set_property (GObject *object,
    guint param_id,
    const GValue *value,
    GParamSpec *pspec)
{
  TplLogStoreXml *self = TPL_LOG_STORE_XML (object);

  switch (param_id)
    {
      case PROP_NAME:
        log_store_xml_set_name (self, g_value_get_string (value));
        break;
      case PROP_READABLE:
        log_store_xml_set_readable (self, g_value_get_boolean (value));
        break;
      case PROP_WRITABLE:
        log_store_xml_set_writable (self, g_value_get_boolean (value));
        break;
      case PROP_EMPATHY_LEGACY:
        self->priv->empathy_legacy = g_value_get_boolean (value);
        break;
      case PROP_BASEDIR:
        log_store_xml_set_basedir (self, g_value_get_string (value));
        break;
      case PROP_TESTMODE:
        self->priv->test_mode = g_value_get_boolean (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    };
}


static void
_tpl_log_store_xml_class_init (TplLogStoreXmlClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *param_spec;

  object_class->finalize = log_store_xml_finalize;
  object_class->dispose = log_store_xml_dispose;
  object_class->get_property = tpl_log_store_xml_get_property;
  object_class->set_property = tpl_log_store_xml_set_property;

  g_object_class_override_property (object_class, PROP_NAME, "name");
  g_object_class_override_property (object_class, PROP_READABLE, "readable");
  g_object_class_override_property (object_class, PROP_WRITABLE, "writable");

  /**
   * TplLogStoreXml:basedir:
   *
   * The log store's basedir.
   */
  param_spec = g_param_spec_string ("basedir",
      "Basedir",
      "The TplLogStore implementation's name",
      NULL, G_PARAM_READABLE | G_PARAM_WRITABLE |
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_BASEDIR, param_spec);

  /**
   * TplLogStoreXml:empathy-legacy:
   *
   * If %TRUE, the logstore pointed by TplLogStoreXml::base-dir will be
   * considered formatted as an Empathy's LogStore (pre telepathy-logger).
   * Xml: %FALSE.
   */
  param_spec = g_param_spec_boolean ("empathy-legacy",
      "EmpathyLegacy",
      "Enables compatibility with old Empathy's logs",
      FALSE, G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_EMPATHY_LEGACY,
      param_spec);

  param_spec = g_param_spec_boolean ("testmode",
      "TestMode",
      "Wheter the logstore is in testmode, for testsuite use only",
      FALSE, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_TESTMODE, param_spec);

  g_type_class_add_private (object_class, sizeof (TplLogStoreXmlPriv));
}


static void
_tpl_log_store_xml_init (TplLogStoreXml *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      TPL_TYPE_LOG_STORE_XML, TplLogStoreXmlPriv);
  self->priv->account_manager = tp_account_manager_dup ();
}


static gchar *
log_store_account_to_dirname (TpAccount *account)
{
  const gchar *name;

  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);

  name = tp_proxy_get_object_path (account);
  if (g_str_has_prefix (name, TP_ACCOUNT_OBJECT_PATH_BASE))
    name += strlen (TP_ACCOUNT_OBJECT_PATH_BASE);

  return g_strdelimit (g_strdup (name), "/", '_');
}


/* id can be NULL, but if present have to be a non zero-lenght string.
 * If NULL, the returned dir will be composed until the account part.
 * If non-NULL, the returned dir will be composed until the id part */
static gchar *
log_store_xml_get_dir (TplLogStoreXml *self,
    TpAccount *account,
    TplEntity *target)
{
  gchar *basedir;
  gchar *escaped_account;
  gchar *escaped_id = NULL;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);

  escaped_account = log_store_account_to_dirname (account);

  if (target)
    {
      /* FIXME This may be source of bug (does that case still exist ?)
       * avoid that 1-1 conversation generated from a chatroom, having id similar
       * to room@conference.domain/My_Alias (in XMPP) are treated as a directory
       * path, creating My_Alias as a subdirectory of room@conference.domain */
      escaped_id = g_strdelimit (
          g_strdup (tpl_entity_get_identifier (target)),
          "/", '_');
    }

  if (target != NULL
      && tpl_entity_get_entity_type (target) == TPL_ENTITY_ROOM)
    basedir = g_build_path (G_DIR_SEPARATOR_S,
        log_store_xml_get_basedir (self), escaped_account, LOG_DIR_CHATROOMS,
        escaped_id, NULL);
  else
    basedir = g_build_path (G_DIR_SEPARATOR_S,
        log_store_xml_get_basedir (self), escaped_account, escaped_id, NULL);

  g_free (escaped_account);
  g_free (escaped_id);

  return basedir;
}


static gchar *
log_store_xml_get_timestamp_filename (void)
{
  time_t t;
  gchar *time_str;
  gchar *filename;

  t = _tpl_time_get_current ();
  time_str = _tpl_time_to_string_local (t, LOG_TIME_FORMAT);
  filename = g_strconcat (time_str, LOG_FILENAME_SUFFIX, NULL);

  g_free (time_str);

  return filename;
}


static gchar *
log_store_xml_get_timestamp_from_event (TplEvent *event)
{
  time_t t;

  t = tpl_event_get_timestamp (event);

  /* We keep the timestamps in the events as UTC */
  return _tpl_time_to_string_utc (t, LOG_TIME_FORMAT_FULL);
}


static gchar *
log_store_xml_get_filename (TplLogStoreXml *self,
    TpAccount *account,
    TplEntity *target)
{
  gchar *id_dir;
  gchar *timestamp;
  gchar *filename;

  id_dir = log_store_xml_get_dir (self, account, target);
  timestamp = log_store_xml_get_timestamp_filename ();
  filename = g_build_filename (id_dir, timestamp, NULL);

  g_free (id_dir);
  g_free (timestamp);

  return filename;
}


/* this is a method used at the end of the add_event process, used by any
 * Event<Type> instance. it should the only method allowed to write to the
 * store */
static gboolean
_log_store_xml_write_to_store (TplLogStoreXml *self,
    TpAccount *account,
    TplEntity *target,
    const gchar *event,
    GError **error)
{
  FILE *file;
  gchar *filename;
  gchar *basedir;
  gboolean ret = TRUE;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), FALSE);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), FALSE);
  g_return_val_if_fail (TPL_IS_ENTITY (target), FALSE);

  filename = log_store_xml_get_filename (self, account, target);
  basedir = g_path_get_dirname (filename);

  if (!g_file_test (basedir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))
    {
      DEBUG ("Creating directory: '%s'", basedir);
      g_mkdir_with_parents (basedir, LOG_DIR_CREATE_MODE);
    }

  g_free (basedir);

  if (!g_file_test (filename, G_FILE_TEST_EXISTS))
    {
      file = g_fopen (filename, "w+");
      if (file != NULL)
        g_fprintf (file, LOG_HEADER);

      g_chmod (filename, LOG_FILE_CREATE_MODE);
    }
  else
    {
      file = g_fopen (filename, "r+");
      if (file != NULL)
        fseek (file, -strlen (LOG_FOOTER), SEEK_END);
    }

  if (file == NULL)
    {
      g_set_error (error, TPL_LOG_STORE_ERROR,
          TPL_LOG_STORE_ERROR_FAILED,
          "Couldn't open log file: %s", filename);
      ret = FALSE;
      goto out;
    }

  g_fprintf (file, "%s", event);
  DEBUG ("%s: written: %s", filename, event);

  fclose (file);
 out:
  g_free (filename);
  return ret;
}


static gboolean
add_text_event (TplLogStoreXml *self,
    TplTextEvent *message,
    GError **error)
{
  gboolean ret = FALSE;
  TpDBusDaemon *bus_daemon;
  TpAccount *account;
  TplEntity *sender;
  const gchar *body_str;
  gchar *avatar_token = NULL;
  gchar *body;
  gchar *timestamp;
  gchar *contact_name = NULL;
  gchar *contact_id;
  gchar *event;
  TpChannelTextMessageType msg_type;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), FALSE);
  g_return_val_if_fail (TPL_IS_TEXT_EVENT (message), FALSE);

  bus_daemon = tp_dbus_daemon_dup (error);
  if (bus_daemon == NULL)
    {
      DEBUG ("Error acquiring bus daemon: %s", (*error)->message);
      goto out;
    }

  account =  tpl_event_get_account (TPL_EVENT (message));

  body_str = tpl_text_event_get_message (message);
  if (TPL_STR_EMPTY (body_str))
    goto out;

  body = g_markup_escape_text (body_str, -1);
  msg_type = tpl_text_event_get_message_type (message);
  timestamp = log_store_xml_get_timestamp_from_event (
      TPL_EVENT (message));

  sender = tpl_event_get_sender (TPL_EVENT (message));
  contact_id = g_markup_escape_text (tpl_entity_get_identifier (sender), -1);
  if (tpl_entity_get_alias (sender) != NULL)
    contact_name = g_markup_escape_text (tpl_entity_get_alias (sender), -1);
  if (tpl_entity_get_avatar_token (sender) != NULL)
    avatar_token = g_markup_escape_text (tpl_entity_get_avatar_token
        (sender), -1);

  event = g_strdup_printf ("<message time='%s' cm_id='%s' id='%s' name='%s' "
      "token='%s' isuser='%s' type='%s'>"
      "%s</message>\n" LOG_FOOTER, timestamp,
      _tpl_event_get_log_id (TPL_EVENT (message)),
      contact_id, contact_name,
      avatar_token ? avatar_token : "",
      tpl_entity_get_entity_type (sender)
          == TPL_ENTITY_SELF ? "true" : "false",
      _tpl_text_event_message_type_to_str (msg_type),
      body);

  DEBUG ("writing %s from %s (ts %s)",
      _tpl_event_get_log_id (TPL_EVENT (message)),
      contact_id, timestamp);

  ret = _log_store_xml_write_to_store (self, account,
      _tpl_event_get_target (TPL_EVENT (message)), event, error);

out:
  g_free (contact_id);
  g_free (contact_name);
  g_free (timestamp);
  g_free (body);
  g_free (event);
  g_free (avatar_token);

  if (bus_daemon != NULL)
    g_object_unref (bus_daemon);

  return ret;
}


/* First of two phases selection: understand the type Event */
static gboolean
log_store_xml_add_event (TplLogStore *store,
    TplEvent *event,
    GError **error)
{
  TplLogStoreXml *self = TPL_LOG_STORE_XML (store);

  g_return_val_if_fail (TPL_IS_EVENT (event), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (TPL_IS_TEXT_EVENT (event))
    return add_text_event (self, TPL_TEXT_EVENT (event), error);

  DEBUG ("TplEntry not handled by this LogStore (%s). "
      "Ignoring Event", log_store_xml_get_name (store));
  /* do not consider it an error, this LogStore simply do not want/need
   * this Event */
  return TRUE;
}


static gboolean
log_store_xml_exists (TplLogStore *store,
    TpAccount *account,
    TplEntity *target)
{
  TplLogStoreXml *self = (TplLogStoreXml *) store;
  gchar *dir;
  gboolean exists;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), FALSE);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), FALSE);
  g_return_val_if_fail (TPL_IS_ENTITY (target), FALSE);

  dir = log_store_xml_get_dir (self, account, target);
  exists = g_file_test (dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR);
  g_free (dir);

  return exists;
}

static GDate *
create_date_from_string (const gchar *str)
{
  GDate *date;
  guint u;
  guint day, month, year;

  if (sscanf (str, "%u", &u) != 1)
    return NULL;

  day = (u % 100);
  month = ((u / 100) % 100);
  year = (u / 10000);

  if (!g_date_valid_dmy (day, month, year))
    return NULL;

  date = g_date_new_dmy (day, month, year);

  return date;
}

static GList *
log_store_xml_get_dates (TplLogStore *store,
    TpAccount *account,
    TplEntity *target)
{
  TplLogStoreXml *self = (TplLogStoreXml *) store;
  GList *dates = NULL;
  gchar *directory;
  GDir *dir;
  const gchar *filename;
  const gchar *p;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (TPL_IS_ENTITY (target), NULL);

  directory = log_store_xml_get_dir (self, account, target);
  dir = g_dir_open (directory, 0, NULL);
  if (!dir)
    {
      DEBUG ("Could not open directory:'%s'", directory);
      g_free (directory);
      return NULL;
    }

  DEBUG ("Collating a list of dates in:'%s'", directory);

  while ((filename = g_dir_read_name (dir)) != NULL)
    {
      gchar *str;
      GDate *date;

      if (!g_str_has_suffix (filename, LOG_FILENAME_SUFFIX))
        continue;

      p = strstr (filename, LOG_FILENAME_SUFFIX);
      str = g_strndup (filename, p - filename);

      if (str == NULL)
        continue;

      date = create_date_from_string (str);
      if (date != NULL)
       dates = g_list_insert_sorted (dates, date,
           (GCompareFunc) g_date_compare);

      g_free (str);
    }

  g_free (directory);
  g_dir_close (dir);

  DEBUG ("Parsed %d dates", g_list_length (dates));

  return dates;
}


static gchar *
log_store_xml_get_filename_for_date (TplLogStoreXml *self,
    TpAccount *account,
    TplEntity *target,
    const GDate *date)
{
  gchar *basedir;
  gchar *timestamp;
  gchar *filename;
  gchar str[9];

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (TPL_IS_ENTITY (target), NULL);
  g_return_val_if_fail (date != NULL, NULL);

  g_date_strftime (str, 9, "%Y%m%d", date);

  basedir = log_store_xml_get_dir (self, account, target);
  timestamp = g_strconcat (str, LOG_FILENAME_SUFFIX, NULL);
  filename = g_build_filename (basedir, timestamp, NULL);

  g_free (basedir);
  g_free (timestamp);

  return filename;
}


static TplLogSearchHit *
log_store_xml_search_hit_new (TplLogStoreXml *self,
    const gchar *filename)
{
  TplLogSearchHit *hit;
  gchar *account_name;
  const gchar *end;
  gchar **strv;
  guint len;
  GList *accounts, *l;
  gchar *tmp;
  TpAccount *account = NULL;
  GDate *date;
  const gchar *chat_id;
  gboolean is_chatroom;
  TplEntity *target;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (!TPL_STR_EMPTY (filename), NULL);
  g_return_val_if_fail (g_str_has_suffix (filename, LOG_FILENAME_SUFFIX),
      NULL);

  strv = g_strsplit (filename, G_DIR_SEPARATOR_S, -1);
  len = g_strv_length (strv);

  end = strstr (strv[len - 1], LOG_FILENAME_SUFFIX);
  tmp = g_strndup (strv[len - 1], end - strv[len - 1]);
  date = create_date_from_string (tmp);
  g_free (tmp);
  chat_id = strv[len - 2];
  is_chatroom = (strcmp (strv[len - 3], LOG_DIR_CHATROOMS) == 0);

  if (is_chatroom)
    account_name = strv[len - 4];
  else
    account_name = strv[len - 3];

  /* FIXME: This assumes the account manager is prepared, but the
   * synchronous API forces this. See bug #599189. */
  accounts = tp_account_manager_get_valid_accounts (
      self->priv->account_manager);

  for (l = accounts; l != NULL && account == NULL; l = g_list_next (l))
    {
      TpAccount *acc = TP_ACCOUNT (l->data);
      gchar *name;

      name = log_store_account_to_dirname (acc);
      if (!tp_strdiff (name, account_name))
        account = acc;
      g_free (name);
    }
  g_list_free (accounts);

  if (is_chatroom)
    target = tpl_entity_new_from_room_id (chat_id);
  else
    target = tpl_entity_new (chat_id, TPL_ENTITY_CONTACT, NULL, NULL);

  hit = _tpl_log_manager_search_hit_new (account, target, date);

  g_strfreev (strv);
  g_date_free (date);
  g_object_unref (target);

  return hit;
}

/* returns a Glist of TplEvent instances */
static GList *
log_store_xml_get_events_for_file (TplLogStoreXml *self,
    TpAccount *account,
    const gchar *filename)
{
  GList *events = NULL;
  xmlParserCtxtPtr ctxt;
  xmlDocPtr doc;
  xmlNodePtr log_node;
  xmlNodePtr node;
  gboolean is_room;
  gchar *dirname;
  gchar *tmp;
  gchar *target_id;
  gchar *self_id;
  GError *error = NULL;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (!TPL_STR_EMPTY (filename), NULL);

  DEBUG ("Attempting to parse filename:'%s'...", filename);

  if (!g_file_test (filename, G_FILE_TEST_EXISTS))
    {
      DEBUG ("Filename:'%s' does not exist", filename);
      return NULL;
    }

  if (!tp_account_parse_object_path (
        tp_proxy_get_object_path (TP_PROXY (account)),
        NULL, NULL, &self_id, &error))
    {
      DEBUG ("Cannot get self identifier from account: %s",
          error->message);
      g_error_free (error);
      return NULL;
    }

  /* Create parser. */
  ctxt = xmlNewParserCtxt ();

  /* Parse and validate the file. */
  doc = xmlCtxtReadFile (ctxt, filename, NULL, 0);
  if (!doc)
    {
      g_warning ("Failed to parse file:'%s'", filename);
      xmlFreeParserCtxt (ctxt);
      g_free (self_id);
      return NULL;
    }

  /* The root node, presets. */
  log_node = xmlDocGetRootElement (doc);
  if (!log_node)
    {
      xmlFreeDoc (doc);
      xmlFreeParserCtxt (ctxt);
      g_free (self_id);
      return NULL;
    }

  /* Guess the target based on directory name */
  dirname = g_path_get_dirname (filename);
  target_id = g_path_get_basename (dirname);

  /* Determine if it's a chatroom */
  tmp = dirname;
  dirname = g_path_get_dirname (tmp);
  g_free (tmp);
  tmp = g_path_get_basename (dirname);
  is_room = g_strcmp0 ("chatroom", tmp);
  g_free (dirname);
  g_free (tmp);

  /* Now get the events. */
  for (node = log_node->children; node; node = node->next)
    {
      TplTextEvent *event;
      TplEntity *sender;
      TplEntity *receiver;
      gchar *time_str;
      time_t timestamp;
      gchar *sender_id;
      gchar *sender_name;
      gchar *sender_avatar_token;
      gchar *body;
      gchar *is_user_str;
      gboolean is_user = FALSE;
      gchar *msg_type_str;
      gchar *log_id;
      TpChannelTextMessageType msg_type = TP_CHANNEL_TEXT_MESSAGE_TYPE_NORMAL;

      if (strcmp ((const gchar *) node->name, "message") != 0)
        continue;

      body = (gchar *) xmlNodeGetContent (node);
      time_str = (gchar *) xmlGetProp (node, (const xmlChar *) "time");
      sender_id = (gchar *) xmlGetProp (node, (const xmlChar *) "id");
      sender_name = (gchar *) xmlGetProp (node, (const xmlChar *) "name");
      sender_avatar_token = (gchar *) xmlGetProp (node,
          (const xmlChar *) "token");
      is_user_str = (gchar *) xmlGetProp (node, (const xmlChar *) "isuser");
      msg_type_str = (gchar *) xmlGetProp (node, (const xmlChar *) "type");
      /* in XML the attr is still cm_id to keep legacy Empathy LogStore
       * compatibility, but actually it stores the log-id when not in
       * legacy-mode */
      log_id = (gchar *) xmlGetProp (node, (const xmlChar *) "cm_id");

      if (is_user_str != NULL)
        is_user = (!tp_strdiff (is_user_str, "true"));

      if (msg_type_str != NULL)
        msg_type = _tpl_text_event_message_type_from_str (msg_type_str);

      timestamp = _tpl_time_parse (time_str);

      if (is_room)
        receiver = tpl_entity_new_from_room_id (target_id);
      else if (is_user)
        receiver = tpl_entity_new (target_id, TPL_ENTITY_CONTACT, NULL, NULL);
      else
        receiver = tpl_entity_new (self_id, TPL_ENTITY_SELF,
            tp_account_get_nickname (account), NULL);

      sender = tpl_entity_new (sender_id,
          is_user ? TPL_ENTITY_SELF : TPL_ENTITY_CONTACT,
          sender_name, sender_avatar_token);

      if (self->priv->empathy_legacy)
        {
          /* in legacy Empathy LogStore there is no concept of log-id as a unique
           * token, so I'll create, just for it to be present, an ad hoc unique
           * token. */
          gchar *instead_of_channel_path;
          gchar *saved_log_id = log_id;
          /* In legacy the log-id is in fact the pending-msg-id */
          gint pending_msg_id = atoi (log_id);

          instead_of_channel_path = g_strconcat (
              tp_proxy_get_object_path (account), sender_id, NULL);

          log_id = _tpl_create_message_token (instead_of_channel_path,
              timestamp, pending_msg_id);

          g_free (instead_of_channel_path);
          xmlFree (saved_log_id);
        }

      event = g_object_new (TPL_TYPE_TEXT_EVENT,
          /* TplEvent */
          "account", account,
          /* MISSING: "channel-path", channel_path, */
          "log-id", log_id,
          "receiver", receiver,
          "sender", sender,
          "timestamp", timestamp,
          /* TplTextEvent */
          "message-type", msg_type,
          "message", body,
          /* As the pending-msg-id is not maintained in XML store, we simply
           * assume the message to be acknowledge. */
          "pending-msg-id", TPL_TEXT_EVENT_MSG_ID_ACKNOWLEDGED,
          NULL);

      events = g_list_append (events, event);

      g_object_unref (sender);
      g_object_unref (receiver);
      g_free (target_id);
      xmlFree (log_id);
      xmlFree (time_str);
      xmlFree (sender_id);
      xmlFree (sender_name);
      xmlFree (body);
      xmlFree (is_user_str);
      xmlFree (msg_type_str);
      xmlFree (sender_avatar_token);
    }

  DEBUG ("Parsed %d events", g_list_length (events));

  xmlFreeDoc (doc);
  xmlFreeParserCtxt (ctxt);

  return events;
}


/* If dir is NULL, basedir will be used instead.
 * Used to make possible the full search vs. specific subtrees search */
static GList *
log_store_xml_get_all_files (TplLogStoreXml *self,
    const gchar *dir)
{
  GDir *gdir;
  GList *files = NULL;
  const gchar *name;
  const gchar *basedir;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  /* dir can be NULL, do not check */

  basedir = (dir != NULL) ? dir : log_store_xml_get_basedir (self);

  gdir = g_dir_open (basedir, 0, NULL);
  if (!gdir)
    return NULL;

  while ((name = g_dir_read_name (gdir)) != NULL)
    {
      gchar *filename;

      filename = g_build_filename (basedir, name, NULL);
      if (g_str_has_suffix (filename, LOG_FILENAME_SUFFIX))
        {
          files = g_list_prepend (files, filename);
          continue;
        }

      if (g_file_test (filename, G_FILE_TEST_IS_DIR))
        {
          /* Recursively get all log files */
          files = g_list_concat (files,
              log_store_xml_get_all_files (self,
                filename));
        }

      g_free (filename);
    }

  g_dir_close (gdir);

  return files;
}


static GList *
_log_store_xml_search_in_files (TplLogStoreXml *self,
    const gchar *text,
    GList *files)
{
  GList *l;
  GList *hits = NULL;
  gchar *text_casefold;
  gchar *escaped_text;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (!TPL_STR_EMPTY (text), NULL);

  escaped_text = g_markup_escape_text (text, -1);
  text_casefold = g_utf8_casefold (escaped_text, -1);

  for (l = files; l; l = g_list_next (l))
    {
      gchar *filename;
      GMappedFile *file;
      gsize length;
      gchar *contents = NULL;
      gchar *contents_casefold = NULL;

      filename = l->data;

      file = g_mapped_file_new (filename, FALSE, NULL);
      if (file == NULL)
        goto fail;

      length = g_mapped_file_get_length (file);
      contents = g_mapped_file_get_contents (file);

      if (length == 0 || contents == NULL)
        goto fail;

      contents_casefold = g_utf8_casefold (contents, length);

      if (strstr (contents_casefold, text_casefold))
        {
          TplLogSearchHit *hit;

          hit = log_store_xml_search_hit_new (self, filename);
          if (hit != NULL)
            {
              hits = g_list_prepend (hits, hit);
              DEBUG ("Found text:'%s' in file:'%s' on date: %04u-%02u-%02u",
                  text, filename, g_date_get_year (hit->date),
                  g_date_get_month (hit->date), g_date_get_day (hit->date));
            }
        }

fail:
      if (file != NULL)
        g_mapped_file_unref (file);

      g_free (contents_casefold);
      g_free (filename);
    }

  g_list_free (files);
  g_free (text_casefold);
  g_free (escaped_text);

  return hits;
}


static GList *
log_store_xml_search_new (TplLogStore *store,
    const gchar *text)
{
  TplLogStoreXml *self = (TplLogStoreXml *) store;
  GList *files;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (!TPL_STR_EMPTY (text), NULL);

  files = log_store_xml_get_all_files (self, NULL);
  DEBUG ("Found %d log files in total", g_list_length (files));

  return _log_store_xml_search_in_files (self, text, files);
}

/* Returns: (GList *) of (TplLogSearchHit *) */
static GList *
log_store_xml_get_entities_for_dir (TplLogStoreXml *self,
    const gchar *dir,
    gboolean is_chatroom,
    TpAccount *account)
{
  GDir *gdir;
  GList *entities = NULL;
  const gchar *name;
  GError *error = NULL;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (!TPL_STR_EMPTY (dir), NULL);

  gdir = g_dir_open (dir, 0, &error);
  if (!gdir)
    {
      DEBUG ("Failed to open directory: %s, error: %s", dir, error->message);
      g_error_free (error);
      return NULL;
    }

  while ((name = g_dir_read_name (gdir)) != NULL)
    {
      TplEntity *entity;

      if (!is_chatroom && strcmp (name, LOG_DIR_CHATROOMS) == 0)
        {
          gchar *filename = g_build_filename (dir, name, NULL);
          entities = g_list_concat (entities,
              log_store_xml_get_entities_for_dir (self, filename, TRUE, account));
          g_free (filename);
          continue;
        }

      if (is_chatroom)
        entity = tpl_entity_new_from_room_id (name);
      else
        entity = tpl_entity_new (name, TPL_ENTITY_CONTACT, NULL, NULL);

      entities = g_list_prepend (entities, entity);
    }

  g_dir_close (gdir);

  return entities;
}


/* returns a Glist of TplEvent instances */
static GList *
log_store_xml_get_events_for_date (TplLogStore *store,
    TpAccount *account,
    TplEntity *target,
    const GDate *date)
{
  TplLogStoreXml *self = (TplLogStoreXml *) store;
  gchar *filename;
  GList *events;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (TPL_IS_ENTITY (target), NULL);
  g_return_val_if_fail (date != NULL, NULL);

  filename = log_store_xml_get_filename_for_date (self, account, target,
      date);
  events = log_store_xml_get_events_for_file (self, account, filename);
  g_free (filename);

  return events;
}


static GList *
log_store_xml_get_entities (TplLogStore *store,
    TpAccount *account)
{
  TplLogStoreXml *self = (TplLogStoreXml *) store;
  gchar *dir;
  GList *entities;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);

  dir = log_store_xml_get_dir (self, account, NULL);
  entities = log_store_xml_get_entities_for_dir (self, dir, FALSE, account);
  g_free (dir);

  return entities;
}


static const gchar *
log_store_xml_get_name (TplLogStore *store)
{
  TplLogStoreXml *self = (TplLogStoreXml *) store;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);

  return self->priv->name;
}


/* returns am absolute path for the base directory of LogStore */
static const gchar *
log_store_xml_get_basedir (TplLogStoreXml *self)
{
  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);

  /* set default based on name if NULL, see prop's comment about it in
   * class_init method */
  if (self->priv->basedir == NULL)
    {
      gchar *dir;
      const char *user_data_dir;
      const char *name;

      if (self->priv->test_mode && g_getenv ("TPL_TEST_LOG_DIR") != NULL)
        {
          user_data_dir = g_getenv ("TPL_TEST_LOG_DIR");
          name = self->priv->empathy_legacy ? "Empathy" : "TpLogger";
        }
      else
        {
          user_data_dir = g_get_user_data_dir ();
          name =  log_store_xml_get_name ((TplLogStore *) self);
        }

      dir = g_build_path (G_DIR_SEPARATOR_S, user_data_dir, name, "logs",
          NULL);
      log_store_xml_set_basedir (self, dir);
      g_free (dir);
    }

  return self->priv->basedir;
}


static void
log_store_xml_set_name (TplLogStoreXml *self,
    const gchar *data)
{
  g_return_if_fail (TPL_IS_LOG_STORE_XML (self));
  g_return_if_fail (!TPL_STR_EMPTY (data));
  g_return_if_fail (self->priv->name == NULL);

  self->priv->name = g_strdup (data);
}

static void
log_store_xml_set_basedir (TplLogStoreXml *self,
    const gchar *data)
{
  g_return_if_fail (TPL_IS_LOG_STORE_XML (self));
  g_return_if_fail (self->priv->basedir == NULL);
  /* data may be NULL when the class is initialized and the default value is
   * set */

  self->priv->basedir = g_strdup (data);

  /* at install_spec time, default value is set to NULL, ignore it */
  if (self->priv->basedir != NULL)
    DEBUG ("logstore set to dir: %s", data);
}


static void
log_store_xml_set_readable (TplLogStoreXml *self,
    gboolean data)
{
  g_return_if_fail (TPL_IS_LOG_STORE_XML (self));

  self->priv->readable = data;
}


static void
log_store_xml_set_writable (TplLogStoreXml *self,
    gboolean data)
{
  g_return_if_fail (TPL_IS_LOG_STORE_XML (self));

  self->priv->writable = data;
}


static GList *
log_store_xml_get_filtered_events (TplLogStore *store,
    TpAccount *account,
    TplEntity *target,
    guint num_events,
    TplLogEventFilter filter,
    gpointer user_data)
{
  TplLogStoreXml *self = (TplLogStoreXml *) store;
  GList *dates, *l, *events = NULL;
  guint i = 0;

  g_return_val_if_fail (TPL_IS_LOG_STORE_XML (self), NULL);
  g_return_val_if_fail (TP_IS_ACCOUNT (account), NULL);
  g_return_val_if_fail (TPL_IS_ENTITY (target), NULL);

  dates = log_store_xml_get_dates (store, account, target);

  for (l = g_list_last (dates); l != NULL && i < num_events;
       l = g_list_previous (l))
    {
      GList *new_events, *n, *next;

      /* FIXME: We should really restrict the event parsing to get only
       * the newest num_events. */
      new_events = log_store_xml_get_events_for_date (store, account,
          target, l->data);

      n = new_events;
      while (n != NULL)
        {
          next = g_list_next (n);
          if (filter != NULL && !filter (n->data, user_data))
            {
              g_object_unref (n->data);
              new_events = g_list_delete_link (new_events, n);
            }
          else
            i++;
          n = next;
        }
      events = g_list_concat (events, new_events);
    }

  g_list_foreach (dates, (GFunc) g_free, NULL);
  g_list_free (dates);

  return events;
}


static void
log_store_xml_clear (TplLogStore *store)
{
  TplLogStoreXml *self = TPL_LOG_STORE_XML (store);
  const gchar *basedir;

  /* We need to use the getter otherwise the basedir might not be set yet */
  basedir = log_store_xml_get_basedir (self);

  DEBUG ("Clear all logs from XML store in: %s", basedir);

  _tpl_rmdir_recursively (basedir);
}


static void
log_store_xml_clear_account (TplLogStore *store,
    TpAccount *account)
{
  TplLogStoreXml *self = TPL_LOG_STORE_XML (store);
  gchar *account_dir;

  account_dir = log_store_xml_get_dir (self, account, NULL);

  if (account_dir)
    {
      DEBUG ("Clear account logs from XML store in: %s",
          account_dir);
      _tpl_rmdir_recursively (account_dir);
      g_free (account_dir);
    }
  else
    DEBUG ("Nothing to clear in account: %s",
        tp_proxy_get_object_path (TP_PROXY (account)));
}


static void
log_store_xml_clear_entity (TplLogStore *store,
    TpAccount *account,
    TplEntity *entity)
{
  TplLogStoreXml *self = TPL_LOG_STORE_XML (store);
  gchar *entity_dir;

  entity_dir = log_store_xml_get_dir (self, account, entity);

  if (entity_dir)
    {
      DEBUG ("Clear entity logs from XML store in: %s",
          entity_dir);

      _tpl_rmdir_recursively (entity_dir);
      g_free (entity_dir);
    }
  else
    DEBUG ("Nothing to clear for account/entity: %s/%s",
        tp_proxy_get_object_path (TP_PROXY (account)),
        tpl_entity_get_identifier (entity));
}


static void
log_store_iface_init (gpointer g_iface,
    gpointer iface_data)
{
  TplLogStoreInterface *iface = (TplLogStoreInterface *) g_iface;

  iface->get_name = log_store_xml_get_name;
  iface->exists = log_store_xml_exists;
  iface->add_event = log_store_xml_add_event;
  iface->get_dates = log_store_xml_get_dates;
  iface->get_events_for_date = log_store_xml_get_events_for_date;
  iface->get_entities = log_store_xml_get_entities;
  iface->search_new = log_store_xml_search_new;
  iface->get_filtered_events = log_store_xml_get_filtered_events;
  iface->clear = log_store_xml_clear;
  iface->clear_account = log_store_xml_clear_account;
  iface->clear_entity = log_store_xml_clear_entity;
}
