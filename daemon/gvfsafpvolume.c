 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <glib/gi18n.h>

#include "gvfsafpserver.h"

#include "gvfsafpvolume.h"



G_DEFINE_TYPE (GVfsAfpVolume, g_vfs_afp_volume, G_TYPE_OBJECT);

struct _GVfsAfpVolumePrivate
{
  GVfsAfpServer *server;
  gboolean mounted;

  guint16 attributes;
  guint16 volume_id;
};

static void
g_vfs_afp_volume_init (GVfsAfpVolume *volume)
{
  GVfsAfpVolumePrivate *priv;
  
  volume->priv = priv = G_TYPE_INSTANCE_GET_PRIVATE (volume, G_VFS_TYPE_AFP_VOLUME,
                                                     GVfsAfpVolumePrivate);
  priv->mounted = FALSE;
}

static void
g_vfs_afp_volume_finalize (GObject *object)
{
  /* TODO: Add deinitalization code here */

  G_OBJECT_CLASS (g_vfs_afp_volume_parent_class)->finalize (object);
}

static void
g_vfs_afp_volume_class_init (GVfsAfpVolumeClass *klass)
{
  GObjectClass* object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = g_vfs_afp_volume_finalize;

  g_type_class_add_private (klass, sizeof (GVfsAfpVolumePrivate));
}

GVfsAfpVolume *
g_vfs_afp_volume_new (GVfsAfpServer *server)
{
  GVfsAfpVolume *volume;
  GVfsAfpVolumePrivate *priv;

  g_return_val_if_fail (G_VFS_IS_AFP_SERVER (server), NULL);
  
  volume = g_object_new (G_VFS_TYPE_AFP_VOLUME, NULL);
  priv = volume->priv;

  priv->server = server;

  return volume;
}

gboolean
g_vfs_afp_volume_mount_sync (GVfsAfpVolume *volume,
                             const char    *volume_name,
                             GCancellable  *cancellable,
                             GError       **error)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GVfsAfpReply *reply;
  gboolean res;
  AfpResultCode res_code;

  g_return_val_if_fail (G_VFS_IS_AFP_VOLUME (volume), FALSE);
  g_return_val_if_fail (volume_name != NULL, FALSE);

  priv = volume->priv;
  
  /* Open Volume */
  comm = g_vfs_afp_command_new (AFP_COMMAND_OPEN_VOL);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume Bitmap */
  g_vfs_afp_command_put_uint16 (comm, AFP_VOLUME_BITMAP_VOL_ID_BIT | AFP_VOLUME_BITMAP_ATTRIBUTE_BIT);

  /* VolumeName */
  g_vfs_afp_command_put_pascal (comm, volume_name);

  /* TODO: password? */

  res = g_vfs_afp_connection_send_command_sync (priv->server->conn, comm, cancellable,
                                                error);
  g_object_unref (comm);
  if (!res)
    return FALSE;

  reply = g_vfs_afp_connection_read_reply_sync (priv->server->conn, cancellable, error);
  if (!reply)
    return FALSE;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    goto generic_error;
  }
  
  /* Volume Bitmap */
  g_vfs_afp_reply_read_uint16 (reply, NULL);
  /* Volume Attributes Bitmap */
  g_vfs_afp_reply_read_uint16 (reply, &priv->attributes);
  /* Volume ID */
  g_vfs_afp_reply_read_uint16 (reply, &priv->volume_id);
  
  g_object_unref (reply);

  priv->mounted = TRUE;
  return TRUE;

generic_error:
  /* Translators: first %s is volumename and second servername */ 
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               _("Couldn't mount AFP volume %s on %s"), volume_name,
               priv->server->server_name);
  return FALSE;
}

guint16
g_vfs_afp_volume_get_attributes (GVfsAfpVolume *volume)
{
  GVfsAfpVolumePrivate *priv = volume->priv;

  g_return_val_if_fail (priv->mounted, 0);
  
  return priv->attributes; 
}

guint16
g_vfs_afp_volume_get_id (GVfsAfpVolume *volume)
{
  GVfsAfpVolumePrivate *priv = volume->priv;

  g_return_val_if_fail (priv->mounted, 0);
  
  return priv->volume_id; 
}

static void
get_vol_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *connection = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));
  GVfsAfpVolumePrivate *priv = volume->priv;

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  
  guint16 vol_bitmap;
  GFileInfo *info;

  reply = g_vfs_afp_connection_send_command_finish (connection, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
    goto done;
  }

  g_vfs_afp_reply_read_uint16 (reply, &vol_bitmap);

  info = g_file_info_new ();

  if (vol_bitmap & AFP_VOLUME_BITMAP_ATTRIBUTE_BIT)
  {
    guint16 vol_attrs_bitmap;
    
    g_vfs_afp_reply_read_uint16 (reply, &vol_attrs_bitmap);
    
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY,
                                       vol_attrs_bitmap & AFP_VOLUME_ATTRIBUTES_BITMAP_READ_ONLY);
  }

  if (vol_bitmap & AFP_VOLUME_BITMAP_CREATE_DATE_BIT)
  {
    gint32 create_date;
    gint64 create_date_local;

    g_vfs_afp_reply_read_int32 (reply, &create_date);

    create_date_local = g_vfs_afp_server_time_to_local_time (priv->server, create_date);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED,
                                      create_date_local);
  }

  if (vol_bitmap & AFP_VOLUME_BITMAP_MOD_DATE_BIT)
  {
    gint32 mod_date;
    gint64 mod_date_local;

    g_vfs_afp_reply_read_int32 (reply, &mod_date);

    mod_date_local = g_vfs_afp_server_time_to_local_time (priv->server, mod_date);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                      mod_date_local);
  }

  if (vol_bitmap & AFP_VOLUME_BITMAP_EXT_BYTES_FREE_BIT)
  {
    guint64 bytes_free;

    g_vfs_afp_reply_read_uint64 (reply, &bytes_free);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                      bytes_free);
  }

  if (vol_bitmap & AFP_VOLUME_BITMAP_EXT_BYTES_TOTAL_BIT)
  {
    guint64 bytes_total;

    g_vfs_afp_reply_read_uint64 (reply, &bytes_total);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
                                      bytes_total);
  }

  g_object_unref (reply);
  
  g_simple_async_result_set_op_res_gpointer (simple, info, g_object_unref);

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_get_parms:
 * 
 * @volume: a #GVfsAfpVolume
 * @vol_bitmap: bitmap describing the parameters that should be received.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously retrieves the parameters specified by @vol_bitmap from @volume
 */
void
g_vfs_afp_volume_get_parms (GVfsAfpVolume       *volume,
                            guint16              vol_bitmap,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_VOL_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, priv->volume_id);
  /* Volume Bitmap */
  g_vfs_afp_command_put_uint16 (comm, vol_bitmap);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_get_parms);
                                      
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL, get_vol_parms_cb,
                                     cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_get_parms_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_get_parms.
 * 
 * Returns: (transfer full): A #GFileInfo with the requested parameters or %NULL
 * on error.
 */
GFileInfo *
g_vfs_afp_volume_get_parms_finish (GVfsAfpVolume  *volume,
                                   GAsyncResult   *result,
                                   GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_get_parms),
                        NULL);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  return g_object_ref (g_simple_async_result_get_op_res_gpointer (simple));
}

typedef struct
{
  gint16 fork_refnum;
  GFileInfo *info;
} OpenForkData;

static void
open_fork_data_free (OpenForkData *data)
{
  g_object_unref (data->info);

  g_slice_free (OpenForkData, data);
}

static void
open_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  
  GVfsAfpVolume *volume;
  GVfsAfpVolumePrivate *priv;
  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  OpenForkData *data;
  guint16 file_bitmap;

  volume = G_VFS_AFP_VOLUME (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));
  priv = volume->priv;
  
  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("File doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_TYPE_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                         _("File is directory"));
        break;
      case AFP_RESULT_TOO_MANY_FILES_OPEN:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                                         _("Too many files open"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

  data = g_slice_new (OpenForkData);
  
  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_int16  (reply, &data->fork_refnum);

  data->info = g_file_info_new ();
  g_vfs_afp_server_fill_info (priv->server, data->info, reply, FALSE, file_bitmap);
  g_object_unref (reply);

  g_simple_async_result_set_op_res_gpointer (simple, data,
                                             (GDestroyNotify)open_fork_data_free);

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_open_fork:
 * 
 * @volume: a #GVfsAfpVolume
 * @filename: file to open fork for.
 * @access_mode:
 * @bitmap:
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously opens a fork corresponding to @filename with the requested
 * access rights.
 */
void
g_vfs_afp_volume_open_fork (GVfsAfpVolume      *volume,
                            const char         *filename,
                            guint16             access_mode,
                            guint16             bitmap,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  if (is_root (filename))
  {
    g_simple_async_report_error_in_idle (G_OBJECT (volume), callback,
                                         user_data, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                         _("File is directory"));
    return;
  }

  comm = g_vfs_afp_command_new (AFP_COMMAND_OPEN_FORK);
  /* data fork */
  g_vfs_afp_command_put_byte (comm, 0);

  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* Directory ID */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* Bitmap */
  g_vfs_afp_command_put_uint16 (comm, bitmap);

  /* AccessMode */
  g_vfs_afp_command_put_uint16 (comm, access_mode);

  /* Pathname */
  g_vfs_afp_command_put_pathname (comm, filename);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback,
                                      user_data, g_vfs_afp_volume_open_fork);
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     open_fork_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_open_fork_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @fork_refnum: (out) the reference id of the newly opened fork.
 * @info: (out callee-allocates) a #GFileInfo containing the requested parameters.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_open_fork.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_open_fork_finish (GVfsAfpVolume  *volume,
                                   GAsyncResult   *res,
                                   gint16         *fork_refnum,
                                   GFileInfo      **info,
                                   GError         **error)
{
  GSimpleAsyncResult *simple;
  OpenForkData *data;

  g_return_val_if_fail (g_simple_async_result_is_valid (res,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_open_fork),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  if (fork_refnum)
    *fork_refnum = data->fork_refnum;
  if (info)
    *info = g_object_ref (data->info);

  return TRUE;
}

static void
close_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  
  if (res_code != AFP_RESULT_NO_ERROR)
    g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_close_fork:
 * 
 * @volume: a #GVfsAfpVolume.
 * @fork_refnum: the reference id of the fork which is to be closed.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously closes an open fork specified by @fork_refnum.
 */
void
g_vfs_afp_volume_close_fork (GVfsAfpVolume       *volume,
                             gint16               fork_refnum,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_CLOSE_FORK);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, fork_refnum);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_close_fork);
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     close_fork_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_close_fork_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_close_fork.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_close_fork_finish (GVfsAfpVolume  *volume,
                                    GAsyncResult   *result,
                                    GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_close_fork),
                        FALSE);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static void
delete_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_FILE_BUSY:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_BUSY,
                                         _("Target file is open"));
        break;                           
      case AFP_RESULT_DIR_NOT_EMPTY:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY,
                                  _("Directory not empty"));
        break;
      case AFP_RESULT_OBJECT_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("Target object is marked as not deletable (DeleteInhibit)"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("Target object doesn't exist"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Volume is read-only"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_delete:
 * 
 * @volume: a #GVfsAfpVolume.
 * @filename: file to delete.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously deletes the file @filename.
 */
void
g_vfs_afp_volume_delete (GVfsAfpVolume       *volume,
                         const char          *filename,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_DELETE);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* Directory ID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* Pathname */
  g_vfs_afp_command_put_pathname (comm, filename);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback,
                                      user_data, g_vfs_afp_volume_delete);
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     delete_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_delete_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_delete.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_delete_finish (GVfsAfpVolume  *volume,
                                GAsyncResult   *result,
                                GError         **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_delete),
                        FALSE);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

typedef struct
{
  char *filename;
  gboolean hard_create;
  GCancellable *cancellable;
} CreateFileData;

static void
create_file_data_free (CreateFileData *cfd)
{
  g_free (cfd->filename);
  if (cfd->cancellable)
    g_object_unref (cfd->cancellable);

  g_slice_free (CreateFileData, cfd);
}

static void
create_file_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *conn = G_VFS_AFP_CONNECTION (object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  
  reply = g_vfs_afp_connection_send_command_finish (conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                  _("Not enough space on volume"));
        break;
      case AFP_RESULT_FILE_BUSY:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                  _("Target file is open"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                  _("Target file already exists"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("Ancestor directory doesn't exist"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Volume is read-only"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
create_file_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsAfpVolumePrivate *priv = volume->priv;
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  CreateFileData *cfd = g_simple_async_result_get_op_res_gpointer (simple);
  
  GFileInfo *info;
  GError *err = NULL;

  guint32 dir_id;
  char *basename;
  GVfsAfpCommand *comm;

  info = g_vfs_afp_volume_get_filedir_parms_finish (volume, res, &err);
  if (!info)
  {
    g_simple_async_result_take_error (simple, err);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
    return;
  }

  dir_id = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_NODE_ID);
  g_object_unref (info);

  comm = g_vfs_afp_command_new (AFP_COMMAND_CREATE_FILE);
  /* soft/hard create */
  g_vfs_afp_command_put_byte (comm, cfd->hard_create ? 0x80 : 0x00);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* Directory ID */
  g_vfs_afp_command_put_uint32 (comm, dir_id);

  /* Pathname */
  basename = g_path_get_basename (cfd->filename);
  g_vfs_afp_command_put_pathname (comm, basename);
  g_free (basename);

  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     create_file_cb, cfd->cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_create_file:
 * 
 * @volume: a #GVfsAfpVolume.
 * @filename: path to the new file to create.
 * @hard_create: if %TRUE this call will overwrite an already existing file.
 * If %FALSE it will error out instead.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously creates a file at @filename.
 */
void
g_vfs_afp_volume_create_file (GVfsAfpVolume      *volume,
                              const char         *filename,
                              gboolean            hard_create,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
  CreateFileData *cfd;
  GSimpleAsyncResult *simple;
  char *dirname;

  cfd = g_slice_new0 (CreateFileData);
  cfd->filename = g_strdup (filename);
  cfd->hard_create = hard_create;
  if (cancellable)
    cfd->cancellable = g_object_ref (cancellable);
  
  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_create_file);
  g_simple_async_result_set_op_res_gpointer (simple, cfd,
                                             (GDestroyNotify)create_file_data_free);

  dirname = g_path_get_dirname (filename);
  g_vfs_afp_volume_get_filedir_parms (volume, dirname, 0, AFP_DIR_BITMAP_NODE_ID_BIT,
                                      cancellable, create_file_get_filedir_parms_cb, simple);
  g_free (dirname);
}

/*
 * g_vfs_afp_volume_create_file_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_create_file.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_create_file_finish (GVfsAfpVolume  *volume,
                                     GAsyncResult   *result,
                                     GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_create_file),
                        FALSE);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

typedef struct
{
  char *basename;
  GCancellable *cancellable;
} CreateDirData;

static void
create_dir_data_free (CreateDirData *cdd)
{
  g_free (cdd->basename);
  g_object_unref (cdd->cancellable);

  g_slice_free (CreateDirData, cdd);
}

static void
make_directory_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);

  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Permission denied"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                  _("Not enough space on volume"));
        break;
      case AFP_RESULT_FLAT_VOL:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                  _("Volume is flat and doesn't support directories"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("Ancestor directory doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                  _("Target directory already exists"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Volume is read-only"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
create_directory_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  CreateDirData *cdd = g_simple_async_result_get_op_res_gpointer (simple);

  GFileInfo *info = NULL;
  GError *err = NULL;

  guint32 dir_id;
  GVfsAfpCommand *comm;
  
  info = g_vfs_afp_volume_get_filedir_parms_finish (volume, res, &err);
  if (!info)
    goto error;

  if (g_cancellable_set_error_if_cancelled (cdd->cancellable, &err))
    goto error;
  
  dir_id = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_NODE_ID);
  g_object_unref (info);

  comm = g_vfs_afp_command_new (AFP_COMMAND_CREATE_DIR);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* Directory ID */
  g_vfs_afp_command_put_uint32 (comm, dir_id);

  /* Pathname */
  g_vfs_afp_command_put_pathname (comm, cdd->basename);
  
  g_vfs_afp_connection_send_command (volume->priv->server->conn, comm, NULL,
                                     make_directory_cb, cdd->cancellable, simple);
  g_object_unref (comm);
  return;

error:
  g_clear_object (info);
  g_simple_async_result_take_error (simple, err);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_create_directory:
 * 
 * @volume: a #GVfsAfpVolume.
 * @directory: path to the new directory to create.
 * @hard_create: if %TRUE this call will overwrite an already existing file.
 * If %FALSE it will error out instead.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously creates a directory at @directory.
 */
void
g_vfs_afp_volume_create_directory (GVfsAfpVolume      *volume,
                                   const char         *directory,
                                   GCancellable       *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
  GSimpleAsyncResult *simple;
  CreateDirData *cdd;
  char *dirname;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_create_directory);

  cdd = g_slice_new (CreateDirData);
  cdd->basename = g_path_get_basename (directory);
  cdd->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  
  g_simple_async_result_set_op_res_gpointer (simple, cdd,
                                             (GDestroyNotify)create_dir_data_free);

  dirname = g_path_get_dirname (directory);
  g_vfs_afp_volume_get_filedir_parms (volume, dirname, 0,
                                      AFP_DIR_BITMAP_NODE_ID_BIT,
                                      cancellable,
                                      create_directory_get_filedir_parms_cb,
                                      simple);
  g_free (dirname);
}

/*
 * g_vfs_afp_volume_create_directory_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_create_directory.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_create_directory_finish (GVfsAfpVolume  *volume,
                                          GAsyncResult   *result,
                                          GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_create_directory),
                        FALSE);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

typedef struct
{
  char *filename;
  char *new_name;
  GCancellable *cancellable;
} RenameData;

static void
rename_data_free (RenameData *rd)
{
  g_free (rd->filename);
  g_free (rd->new_name);
  g_object_unref (rd->cancellable);

  g_slice_free (RenameData, rd);
}

static void
rename_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);

  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_CANT_RENAME:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                                  _("Can't rename volume"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                  _("Object with that name already exists"));
        break;
      case AFP_RESULT_OBJECT_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("Target object is marked as not renameable (RenameInhibit)"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("Target object doesn't exist"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                  _("Volume is read-only"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
rename_get_filedir_parms_cb (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  RenameData *rd = g_simple_async_result_get_op_res_gpointer (simple);

  GFileInfo *info;
  GError *err = NULL;

  guint32 dir_id;
  GVfsAfpCommand *comm;
  char *basename;

  info = g_vfs_afp_volume_get_filedir_parms_finish (volume, res, &err);
  if (!info)
  {
    g_simple_async_result_take_error (simple, err);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
    return;
  }

  dir_id = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_PARENT_DIR_ID);
  g_object_unref (info);

  comm = g_vfs_afp_command_new (AFP_COMMAND_RENAME);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* Directory ID */
  g_vfs_afp_command_put_uint32 (comm, dir_id);

  /* Pathname */
  basename = g_path_get_basename (rd->filename);
  g_vfs_afp_command_put_pathname (comm, basename);
  g_free (basename);

  /* NewName */
  g_vfs_afp_command_put_pathname (comm, rd->new_name);

  g_vfs_afp_connection_send_command (volume->priv->server->conn, comm, NULL,
                                     rename_cb, rd->cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_rename:
 * 
 * @volume: a #GVfsAfpVolume.
 * @filename: path to file to rename.
 * @new_name: the new name of the file.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously renames the file at @filename to @new_name.
 */
void
g_vfs_afp_volume_rename (GVfsAfpVolume      *volume,
                         const char         *filename,
                         const char         *new_name,
                         GCancellable       *cancellable,
                         GAsyncReadyCallback callback,
                         gpointer            user_data)
{
  GSimpleAsyncResult *simple;
  RenameData *rd;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_rename);

  rd = g_slice_new (RenameData);
  rd->filename = g_strdup (filename);
  rd->new_name = g_strdup (new_name);
  rd->cancellable = g_object_ref (cancellable);
  g_simple_async_result_set_op_res_gpointer (simple, rd,
                                             (GDestroyNotify)rename_data_free);
  
  g_vfs_afp_volume_get_filedir_parms (volume, filename,
                                      AFP_FILEDIR_BITMAP_PARENT_DIR_ID_BIT,
                                      AFP_FILEDIR_BITMAP_PARENT_DIR_ID_BIT,
                                      cancellable, rename_get_filedir_parms_cb,
                                      simple);
}

/*
 * g_vfs_afp_volume_rename_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_move_and_rename.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_rename_finish (GVfsAfpVolume  *volume,
                                GAsyncResult   *res,
                                GError        **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_rename),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static void
move_and_rename_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;

  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_CANT_MOVE:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                                         _("Can't move directory into one of its descendants"));
        break;
      case AFP_RESULT_INSIDE_SHARE_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Can't move sharepoint into a shared directory"));
        break;
      case AFP_RESULT_INSIDE_TRASH_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Can't move a shared directory into the Trash"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                         _("Target file already exists"));
        break;
      case AFP_RESULT_OBJECT_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Object being moved is marked as not renameable (RenameInhibit)"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Object being moved doesn't exist"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_move_and_rename:
 * 
 * @volume: a #GVfsAfpVolume.
 * @source: the source path of the file to move.
 * @destination: destination path.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously moves (and renames) the file at @source to @destination.
 */
void
g_vfs_afp_volume_move_and_rename (GVfsAfpVolume      *volume,
                                  const char         *source,
                                  const char         *destination,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  char *dirname, *basename;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_MOVE_AND_RENAME);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* VolumeID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));

  /* SourceDirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);
  /* DestDirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* SourcePathname */
  g_vfs_afp_command_put_pathname (comm, source);

  /* DestPathname */
  dirname = g_path_get_dirname (destination);
  g_vfs_afp_command_put_pathname (comm, dirname);
  g_free (dirname);

  /* NewName */
  basename = g_path_get_basename (destination);
  g_vfs_afp_command_put_pathname (comm, basename);
  g_free (basename);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback,
                                      user_data, g_vfs_afp_volume_move_and_rename);
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     move_and_rename_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_move_and_rename_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_move_and_rename.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_move_and_rename_finish (GVfsAfpVolume  *volume,
                                         GAsyncResult   *res,
                                         GError        **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_move_and_rename),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static void
copy_file_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;

  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);

  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_CALL_NOT_SUPPORTED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                         _("Server doesn't support the FPCopyFile operation"));
        break;
      case AFP_RESULT_DENY_CONFLICT:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Unable to open source file for reading"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                         _("Not enough space on volume"));
        break;
      case AFP_RESULT_OBJECT_EXISTS:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                         _("Target file already exists"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Source file and/or destination directory doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_TYPE_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                         _("Source file is a directory"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
  }
        
done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_copy_file:
 * 
 * @volume: a #GVfsAfpVolume.
 * @source: the source path of the file to copy.
 * @destination: destination path.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously copies the file at @source to @destination.
 */
void
g_vfs_afp_volume_copy_file (GVfsAfpVolume      *volume,
                            const char         *source,
                            const char         *destination,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
  GVfsAfpVolumePrivate *priv;
  
  GVfsAfpCommand *comm;
  char *dirname, *basename;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_COPY_FILE);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* SourceVolumeID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* SourceDirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* DestVolumeID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* DestDirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* SourcePathname */
  g_vfs_afp_command_put_pathname (comm, source);

  /* DestPathname */
  dirname = g_path_get_dirname (destination);
  g_vfs_afp_command_put_pathname (comm, dirname);
  g_free (dirname);

  /* NewName */
  basename = g_path_get_basename (destination);
  g_vfs_afp_command_put_pathname (comm, basename);
  g_free (basename);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback,
                                      user_data, g_vfs_afp_volume_copy_file);

  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     copy_file_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_copy_file_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_copy_file.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_copy_file_finish (GVfsAfpVolume *volume,
                                   GAsyncResult  *res,
                                   GError       **error)
{
  GSimpleAsyncResult *simple;

  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_copy_file),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

typedef struct
{
  AfpMapIDFunction function;
  char *name;
} MapIDData;

static void
map_id_data_free (MapIDData *mid)
{
  g_free (mid->name);

  g_slice_free (MapIDData, mid);
}

static void
map_id_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;

  AfpResultCode res_code;
  MapIDData *map_data;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    g_simple_async_result_complete (simple);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ITEM_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("ID not found"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }

    g_simple_async_result_complete (simple);
    return;
  }

  map_data = g_simple_async_result_get_op_res_gpointer (simple);
  
  if (map_data->function == AFP_MAP_ID_FUNCTION_USER_UUID_TO_UTF8_NAME ||
      map_data->function == AFP_MAP_ID_FUNCTION_GROUP_UUID_TO_UTF8_NAME)
  {
    /* objType */
    g_vfs_afp_reply_read_uint32 (reply, NULL);
    /* id */
    g_vfs_afp_reply_read_uint32 (reply, NULL);
  }

  if (map_data->function == AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME ||
      map_data->function == AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME)
  {
    g_vfs_afp_reply_read_pascal (reply, &map_data->name);
  }
  else
  {
    GVfsAfpName *afp_name;

    g_vfs_afp_reply_read_afp_name (reply, FALSE, &afp_name);
    map_data->name = g_vfs_afp_name_get_string (afp_name);
    g_vfs_afp_name_unref (afp_name);
  }

  g_simple_async_result_complete (simple);
}

/*
 * g_vfs_afp_volume_map_id:
 * 
 * @volume: a #GVfsAfpVolume.
 * @map_function: a #AfpMapIDFunction.
 * @id: the id to be mapped to a name.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously maps a user id, group id or uuid to a name.
 */
void
g_vfs_afp_volume_map_id (GVfsAfpVolume       *volume,
                         AfpMapIDFunction     map_function,
                         gint64               id,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;
  MapIDData *map_data;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_MAP_ID);

  /* SubFunction*/
  g_vfs_afp_command_put_byte (comm, map_function);

  /* ID */
  if (map_function == AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME ||
      map_function == AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME)
    g_vfs_afp_command_put_int32 (comm, id);
  else
    g_vfs_afp_command_put_int64 (comm, id);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback,
                                      user_data, g_vfs_afp_volume_map_id);

  map_data = g_slice_new0 (MapIDData);
  map_data->function = map_function;
  g_simple_async_result_set_op_res_gpointer (simple, map_data,
                                             (GDestroyNotify)map_id_data_free);
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     map_id_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_map_id_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @map_function: (out) optional out parameter to get the #AfpMapIDFunction
 * which was used, %NULL to ignore.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_map_id.
 * 
 * Returns: (transfer full): A string with the name of the id or %NULL
 * on error.
 */
char *
g_vfs_afp_volume_map_id_finish (GVfsAfpVolume   *volume,
                                GAsyncResult     *res,
                                AfpMapIDFunction *map_function,
                                GError          **error)
{
  GSimpleAsyncResult *simple;
  MapIDData *map_data;

  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_map_id),
                        NULL);

  simple = (GSimpleAsyncResult *)res;

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  map_data = g_simple_async_result_get_op_res_gpointer (simple);

  if (map_function)
    *map_function = map_data->function;
  
  return g_strdup (map_data->name);
}

static void
get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  guint16 file_bitmap, dir_bitmap, bitmap;
  guint8 FileDir;
  gboolean directory;
  GFileInfo *info;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    
    switch (res_code)
    {
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("File doesn't exist"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_uint16 (reply, &dir_bitmap);

  g_vfs_afp_reply_read_byte (reply, &FileDir);
  /* Pad Byte */
  g_vfs_afp_reply_read_byte (reply, NULL);
  
  directory = (FileDir & 0x80); 
  bitmap =  directory ? dir_bitmap : file_bitmap;

  info = g_file_info_new ();
  g_vfs_afp_server_fill_info (volume->priv->server, info, reply, directory, bitmap);
  
  g_object_unref (reply);

  g_simple_async_result_set_op_res_gpointer (simple, info, g_object_unref);

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_get_filedir_parms:
 * 
 * @volume: a #GVfsAfpVolume.
 * @filename: file or directory whose parameters should be retreived.
 * @file_bitmap: bitmap describing the parameters to retrieve if @filename is a
 * file.
 * @dir_bitmap: bitmap describing the parameters to retrieve if @filename is a
 * directory.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously retrieves the parameters described by @file_bitmap or
 * @dir_bitmap of the file/directory at @filename.
 */
void
g_vfs_afp_volume_get_filedir_parms (GVfsAfpVolume       *volume,
                                    const char          *filename,
                                    guint16              file_bitmap,
                                    guint16              dir_bitmap,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_FILE_DIR_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* VolumeID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* Directory ID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);
  /* FileBitmap */  
  g_vfs_afp_command_put_uint16 (comm, file_bitmap);
  /* DirectoryBitmap */  
  g_vfs_afp_command_put_uint16 (comm, dir_bitmap);
  /* PathName */
  g_vfs_afp_command_put_pathname (comm, filename);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_get_filedir_parms);
                                      

  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     get_filedir_parms_cb, cancellable,
                                     simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_get_filedir_parms_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_get_fork_parms.
 * 
 * Returns: (transfer full): A #GFileInfo with the requested parameters or %NULL
 * on error.
 */
GFileInfo *
g_vfs_afp_volume_get_filedir_parms_finish (GVfsAfpVolume  *volume,
                                           GAsyncResult   *result,
                                           GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_get_filedir_parms),
                        NULL);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  return g_object_ref (g_simple_async_result_get_op_res_gpointer (simple));
}

static void
get_fork_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));
  GVfsAfpVolumePrivate *priv = volume->priv;

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  guint16 file_bitmap;
  GFileInfo *info;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);

    g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
    goto done;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);

  info = g_file_info_new ();
  g_vfs_afp_server_fill_info (priv->server, info, reply, FALSE, file_bitmap);

  g_object_unref (reply);

  g_simple_async_result_set_op_res_gpointer (simple, info, g_object_unref);

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_get_fork_parms:
 * 
 * @volume: a #GVfsAfpVolume.
 * @fork_refnume: the reference id of the fork.
 * @file_bitmap: bitmap describing the parameters to retrieve.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously retrieves the parameters described by @file_bitmap of the fork
 * with reference id @fork_refnum.
 */
void
g_vfs_afp_volume_get_fork_parms (GVfsAfpVolume       *volume,
                                 gint16               fork_refnum,
                                 guint16              file_bitmap,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_FORK_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, fork_refnum);
  /* Bitmap */  
  g_vfs_afp_command_put_uint16 (comm, file_bitmap);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_get_fork_parms);
                                      

  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     get_fork_parms_cb, cancellable,
                                     simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_get_fork_parms_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_get_fork_parms.
 * 
 * Returns: (transfer full): A #GFileInfo with the requested parameters or %NULL
 * on error.
 */
GFileInfo *
g_vfs_afp_volume_get_fork_parms_finish (GVfsAfpVolume  *volume,
                                        GAsyncResult   *result,
                                        GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_get_fork_parms),
                        NULL);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;

  return g_object_ref (g_simple_async_result_get_op_res_gpointer (simple));
}

static void
set_fork_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                         _("Not enough space on volume"));
        break;
      case AFP_RESULT_LOCK_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Range lock conflict exists"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_set_fork_size:
 * 
 * @volume: a #GVfsAfpVolume.
 * @fork_refnume: the reference id of the fork.
 * @size: the new size of the fork,
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously sets the size of the fork referenced by @fork_refnum.
 */
void
g_vfs_afp_volume_set_fork_size (GVfsAfpVolume       *volume,
                                gint16               fork_refnum,
                                gint64               size,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_SET_FORK_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, fork_refnum);
  /* Bitmap */
  g_vfs_afp_command_put_uint16 (comm, AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT);
  /* ForkLen */
  g_vfs_afp_command_put_int64 (comm, size);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_set_fork_size);
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     set_fork_parms_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_set_fork_size_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_set_fork_size.
 * 
 * Returns: (transfer full): %TRUE on success, %FALSE otherwise.
 */
gboolean
g_vfs_afp_volume_set_fork_size_finish (GVfsAfpVolume  *volume,
                                       GAsyncResult   *result,
                                       GError         **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        G_OBJECT (volume),
                                                        g_vfs_afp_volume_set_fork_size),
                        FALSE);

  simple = (GSimpleAsyncResult *)result;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static void
set_unix_privs_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Target object doesn't exist"));
        break;
      case AFP_RESULT_VOL_LOCKED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Volume is read-only"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_set_unix_privs:
 * 
 * @volume: a #GVfsAfpVolume.
 * @filename: file or directory whose unix privileges should be set.
 * @uid: the new user id of the file.
 * @gid: the new group id of the file.
 * @permissions: the new unix permissions of the file.
 * @ua_permissions: the new AFP access right of the file.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously sets new unix permissions on the file/directory pointed to by
 * @filename.
 */
void
g_vfs_afp_volume_set_unix_privs (GVfsAfpVolume       *volume,
                                 const char          *filename,
                                 guint32              uid,
                                 guint32              gid,
                                 guint32              permissions,
                                 guint32              ua_permissions,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_SET_FILEDIR_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* VolumeID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* DirectoryID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);
  /* Bitmap */
  g_vfs_afp_command_put_uint16 (comm, AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT);
  /* Pathname */
  g_vfs_afp_command_put_pathname (comm, filename);
  /* pad to even */
  g_vfs_afp_command_pad_to_even (comm);

  /* UID */
  g_vfs_afp_command_put_uint32 (comm, uid);
  /* GID */
  g_vfs_afp_command_put_uint32 (comm, gid);
  /* Permissions */
  g_vfs_afp_command_put_uint32 (comm, permissions);
  /* UAPermissions */
  g_vfs_afp_command_put_uint32 (comm, ua_permissions);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback,
                                      user_data, g_vfs_afp_volume_set_unix_privs);

  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     set_unix_privs_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_set_unix_privs_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_set_unix_privs.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_set_unix_privs_finish (GVfsAfpVolume  *volume,
                                        GAsyncResult   *res,
                                        GError        **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_set_unix_privs),
                        FALSE);

  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  return TRUE;
}

static const gint16 ENUMERATE_REQ_COUNT           = G_MAXINT16;
static const gint16 ENUMERATE_EXT_MAX_REPLY_SIZE  = G_MAXINT16; 
static const gint32 ENUMERATE_EXT2_MAX_REPLY_SIZE = G_MAXINT32;

static void
enumerate_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));
  GVfsAfpVolumePrivate *priv = volume->priv;

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  
  guint16 file_bitmap;
  guint16  dir_bitmap;
  gint16 count, i;
  GPtrArray *infos;

  reply = g_vfs_afp_connection_send_command_finish (conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    
    switch (res_code)
    {
      case AFP_RESULT_OBJECT_NOT_FOUND:
        g_simple_async_result_set_op_res_gpointer (simple, NULL, NULL);
        break;
        
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_DIR_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("Directory doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_TYPE_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                                         _("Target object is not a directory"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_uint16 (reply, &dir_bitmap);

  g_vfs_afp_reply_read_int16 (reply, &count);
  infos = g_ptr_array_new_full (count, g_object_unref);
  
  for (i = 0; i < count; i++)
  {
    goffset start_pos;
    guint16 struct_length;
    guint8 FileDir;
    
    gboolean directory;
    guint16 bitmap;
    GFileInfo *info;

    start_pos = g_vfs_afp_reply_get_pos (reply);
    
    g_vfs_afp_reply_read_uint16 (reply, &struct_length);
    g_vfs_afp_reply_read_byte (reply, &FileDir);
    /* pad byte */
    g_vfs_afp_reply_read_byte (reply, NULL);

    directory = (FileDir & 0x80); 
    bitmap =  directory ? dir_bitmap : file_bitmap;
    
    info = g_file_info_new ();
    g_vfs_afp_server_fill_info (priv->server, info, reply, directory, bitmap);
    g_ptr_array_add (infos, info);

    g_vfs_afp_reply_seek (reply, start_pos + struct_length, G_SEEK_SET);
  }
  g_object_unref (reply);

  g_simple_async_result_set_op_res_gpointer (simple, infos,
                                             (GDestroyNotify)g_ptr_array_unref);
  
done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_enumerate:
 * 
 * @volume: a #GVfsAfpVolume.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously enumerates the files in @directory starting at index
 * @start_index.
 */
void
g_vfs_afp_volume_enumerate (GVfsAfpVolume       *volume,
                            const char          *directory,
                            gint64               start_index,
                            guint16              file_bitmap,
                            guint16              dir_bitmap,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  gint32 max;
  
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;

  simple = g_simple_async_result_new (G_OBJECT (volume), callback,
                                      user_data, g_vfs_afp_volume_enumerate);
  
  max = (priv->server->version >= AFP_VERSION_3_1) ? G_MAXINT32 : G_MAXINT16;
  /* Can't enumerate any more files */
  if (start_index > max)
  {
    g_simple_async_result_set_op_res_gpointer (simple, NULL, NULL);
    g_simple_async_result_complete_in_idle (simple);
    return;
  }
  
  if (priv->server->version >= AFP_VERSION_3_1)
    comm = g_vfs_afp_command_new (AFP_COMMAND_ENUMERATE_EXT2);
  else
    comm = g_vfs_afp_command_new (AFP_COMMAND_ENUMERATE_EXT);
  
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* Directory ID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* File Bitmap */
  g_vfs_afp_command_put_uint16 (comm, file_bitmap);
  
  /* Dir Bitmap */
  g_vfs_afp_command_put_uint16 (comm, dir_bitmap);

  /* Req Count */
  g_vfs_afp_command_put_int16 (comm, ENUMERATE_REQ_COUNT);

  
  /* StartIndex and MaxReplySize */
  if (priv->server->version >= AFP_VERSION_3_1)
  {
    g_vfs_afp_command_put_int32 (comm, start_index);
    g_vfs_afp_command_put_int32 (comm, ENUMERATE_EXT2_MAX_REPLY_SIZE);
  }
  else
  {
    g_vfs_afp_command_put_int16 (comm, start_index);
    g_vfs_afp_command_put_int16 (comm, ENUMERATE_EXT_MAX_REPLY_SIZE);
  }
  
  /* Pathname */
  g_vfs_afp_command_put_pathname (comm, directory);
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     enumerate_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_enumerate_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @infos: (out) (element-type G.FileInfo): array of #GFileInfo objects or %NULL
 * when no more files could be found.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_enumerate.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_enumerate_finish (GVfsAfpVolume  *volume,
                                   GAsyncResult   *res,
                                   GPtrArray      **infos,
                                   GError        **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_enumerate),
                        FALSE);
  g_return_val_if_fail (infos != NULL, FALSE);
  
  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  *infos = g_simple_async_result_get_op_res_gpointer (simple);
  if (*infos)
    g_ptr_array_ref (*infos);
  
  return TRUE;
}

static void
close_replace_exchange_files_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }
  
  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);
  
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Permission denied"));
        break;
      case AFP_RESULT_ID_NOT_FOUND:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                         _("File doesn't exist"));
        break;
      case AFP_RESULT_OBJECT_TYPE_ERR:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                         _("File is directory"));
        break;   
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}
  
/*
 * g_vfs_afp_volume_exchange_files:
 * 
 * @volume: a #GVfsAfpVolume.
 * @source: path to source file to exchange.
 * @destination: path to destination file to exchange.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously exchanges the file system metadata of the two files @source
 * and @destination.
 */
void
g_vfs_afp_volume_exchange_files (GVfsAfpVolume       *volume,
                                 const char          *source,
                                 const char          *destination,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  GVfsAfpVolumePrivate *priv;
  GVfsAfpCommand *comm;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));

  priv = volume->priv;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_EXCHANGE_FILES);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* Volume ID */
  g_vfs_afp_command_put_uint16 (comm, g_vfs_afp_volume_get_id (volume));
  /* SourceDirectory ID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);
  /* DestDirectory ID 2 == / */
  g_vfs_afp_command_put_uint32 (comm, 2);

  /* SourcePath */
  g_vfs_afp_command_put_pathname (comm, source);
  /* DestPath */
  g_vfs_afp_command_put_pathname (comm, destination);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_exchange_files);
  
  g_vfs_afp_connection_send_command (priv->server->conn, comm, NULL,
                                     close_replace_exchange_files_cb,
                                     cancellable, simple);
  g_object_unref (comm); 
}

/*
 * g_vfs_afp_volume_exchange_files_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_exchange_files.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_exchange_files_finish (GVfsAfpVolume  *volume,
                                        GAsyncResult   *res,
                                        GError        **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_exchange_files),
                        FALSE);
  
  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  
  return TRUE;
}

static void
write_ext_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  gint64 *last_written;

  reply = g_vfs_afp_connection_send_command_finish (conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (!(res_code == AFP_RESULT_NO_ERROR || res_code == AFP_RESULT_LOCK_ERR))
  {
    g_object_unref (reply);

    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("File is not open for write access"));
        break;
      case AFP_RESULT_DISK_FULL:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                  _("Not enough space on volume"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

  last_written = g_new (gint64, 1);
  g_vfs_afp_reply_read_int64 (reply, last_written);
  g_object_unref (reply);
  
  g_simple_async_result_set_op_res_gpointer (simple, last_written, g_free); 

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_write_to_fork:
 * 
 * @volume: a #GVfsAfpVolume.
 * @fork_refnume: reference id of the fork to write to.
 * @buffer: buffer containing the data to write. Must be valid during the whole
 * call.
 * @buffer_size: size of @buffer.
 * @offset: offset in file where the data should be written.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously writes the data in @buffer to the fork referenced by
 * @fork_refnum.
 */
void
g_vfs_afp_volume_write_to_fork (GVfsAfpVolume       *volume,
                                guint16              fork_refnum,
                                char                *buffer,
                                gsize                buffer_size,
                                gint64               offset,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  GVfsAfpCommand *comm;
  guint32 req_count;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_WRITE_EXT);
  /* StartEndFlag = 0 */
  g_vfs_afp_command_put_byte (comm, 0);

  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, fork_refnum);
  /* Offset */
  g_vfs_afp_command_put_int64 (comm, offset);
  /* ReqCount */
  req_count = MIN (buffer_size, G_MAXUINT32);
  g_vfs_afp_command_put_int64 (comm, req_count);

  g_vfs_afp_command_set_buffer (comm, buffer, buffer_size);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_write_to_fork);
  
  g_vfs_afp_connection_send_command (volume->priv->server->conn, comm, NULL,
                                     write_ext_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_write_to_fork_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @last_written: (out) (allow-none): offset of the last written byte.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_write_to_fork.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_write_to_fork_finish (GVfsAfpVolume  *volume,
                                       GAsyncResult   *res,
                                       gint64         *last_written,
                                       GError        **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_write_to_fork),
                        FALSE);
  
  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  if (last_written)
    *last_written = *((gint64 *)g_simple_async_result_get_op_res_gpointer (simple));
  
  return TRUE;
}

static void
read_ext_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *conn = G_VFS_AFP_CONNECTION (source_object);
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;

  reply = g_vfs_afp_connection_send_command_finish (conn, res, &err);
  if (!reply)
  {
    g_simple_async_result_take_error (simple, err);
    goto done;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (!(res_code == AFP_RESULT_NO_ERROR || res_code == AFP_RESULT_LOCK_ERR ||
        res_code == AFP_RESULT_EOF_ERR))
  {
    g_object_unref (reply);

    switch (res_code)
    {
      case AFP_RESULT_ACCESS_DENIED:
        g_simple_async_result_set_error (simple, G_IO_ERROR, G_IO_ERROR_FAILED,
                                  _("File is not open for read access"));
        break;
      default:
        g_simple_async_result_take_error (simple, afp_result_code_to_gerror (res_code));
        break;
    }
    goto done;
  }

  g_simple_async_result_set_op_res_gssize (simple, g_vfs_afp_reply_get_size (reply));
  g_object_unref (reply);

done:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

/*
 * g_vfs_afp_volume_read_from_fork:
 * 
 * @volume: a #GVfsAfpVolume.
 * @fork_refnume: reference id of the fork to write to.
 * @buffer: buffer to read data into. Must be valid during the whole call. 
 * @bytes_requested: number of bytes that should be read.
 * @offset: offset in file from where the data should be read.
 * @cancellable: optional #GCancellable object, %NULL to ignore.
 * @callback: callback to call when the request is satisfied.
 * @user_data: the data to pass to callback function.
 * 
 * Asynchronously reads data from the fork referenced by @fork_refnum.
 */
void
g_vfs_afp_volume_read_from_fork (GVfsAfpVolume       *volume,
                                 guint16              fork_refnum,
                                 char                *buffer,
                                 gsize                bytes_requested,
                                 gint64               offset,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  GVfsAfpCommand *comm;
  guint32 req_count;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_VFS_IS_AFP_VOLUME (volume));
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_READ_EXT);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);

  /* OForkRefNum */
  g_vfs_afp_command_put_int16 (comm, fork_refnum);
  /* Offset */
  g_vfs_afp_command_put_int64 (comm, offset);
  /* ReqCount */
  req_count = MIN (bytes_requested, G_MAXUINT32);
  g_vfs_afp_command_put_int64 (comm, req_count);

  simple = g_simple_async_result_new (G_OBJECT (volume), callback, user_data,
                                      g_vfs_afp_volume_read_from_fork);
  
  g_vfs_afp_connection_send_command (volume->priv->server->conn, comm, buffer,
                                     read_ext_cb, cancellable, simple);
  g_object_unref (comm);
}

/*
 * g_vfs_afp_volume_read_from_fork_finish:
 * 
 * @volume: a #GVfsAfpVolume.
 * @result: a #GAsyncResult.
 * @bytes_read: (out) (allow-none): the number of bytes that were read.
 * @error: a #GError, %NULL to ignore.
 * 
 * Finalizes the asynchronous operation started by
 * g_vfs_afp_volume_read_from_fork.
 * 
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
g_vfs_afp_volume_read_from_fork_finish (GVfsAfpVolume  *volume,
                                        GAsyncResult   *res,
                                        gsize          *bytes_read,
                                        GError        **error)
{
  GSimpleAsyncResult *simple;
  
  g_return_val_if_fail (g_simple_async_result_is_valid (res, G_OBJECT (volume),
                                                        g_vfs_afp_volume_read_from_fork),
                        FALSE);
  
  simple = (GSimpleAsyncResult *)res;
  
  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;

  if (bytes_read)
    *bytes_read = g_simple_async_result_get_op_res_gssize (simple);
  
  return TRUE;
}