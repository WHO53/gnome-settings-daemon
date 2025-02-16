/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2018 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"
#include <stdlib.h>

#include "gsd-backlight.h"
#include "gpm-common.h"
#include "gsd-power-constants.h"
#include "gsd-power-manager.h"

#include <gbinder.h>

#ifdef __linux__
#include <gudev/gudev.h>
#endif /* __linux__ */

struct _GsdBacklight
{
        GObject object;

        gint brightness_min;
        gint brightness_max;
        gint brightness_val;
        gint brightness_target;
        gint brightness_step;

#ifdef __linux__
        GDBusProxy *logind_proxy;

        GUdevClient *udev;
        GUdevDevice *udev_device;

        GTask *active_task;
        GQueue tasks;

        gint idle_update;
#endif /* __linux__ */

        GBinderServiceManager *service_manager;
        GBinderRemoteObject *remote;
        GBinderClient *client;
        gboolean use_binder;

        GnomeRRScreen *rr_screen;
        gboolean builtin_display_disabled;
};

enum {
        PROP_RR_SCREEN = 1,
        PROP_BRIGHTNESS,
        PROP_LAST,
};

#define SYSTEMD_DBUS_NAME                       "org.freedesktop.login1"
#define SYSTEMD_DBUS_PATH                       "/org/freedesktop/login1/session/auto"
#define SYSTEMD_DBUS_INTERFACE                  "org.freedesktop.login1.Session"

#define BINDER_LIGHT_AIDL_DEVICE "/dev/binder"
#define BINDER_LIGHT_AIDL_IFACE "android.hardware.light.ILights"

#define BINDER_LIGHT_HIDL_DEVICE "/dev/hwbinder"
#define BINDER_LIGHT_HIDL_IFACE "android.hardware.light@2.0::ILight"

#define BINDER_LIGHT_SLOT "default"
#define BINDER_LIGHT_SET_LIGHT 1

typedef struct {
    int32_t color;
    int32_t flashMode;
    int32_t flashOnMs;
    int32_t flashOffMs;
    int32_t brightnessMode;
} LightState;

static GParamSpec *props[PROP_LAST];

static void     gsd_backlight_initable_iface_init (GInitableIface  *iface);
static gboolean gsd_backlight_initable_init       (GInitable       *initable,
                                                   GCancellable    *cancellable,
                                                   GError         **error);


G_DEFINE_TYPE_EXTENDED (GsdBacklight, gsd_backlight, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                               gsd_backlight_initable_iface_init);)

static gboolean
gsd_backlight_init_binder (GsdBacklight *backlight, const gchar *device, const gchar *iface)
{
        backlight->service_manager = gbinder_servicemanager_new(device);
      
        if(!backlight->service_manager) {
          g_warning("Failed to get create service manager");
          return FALSE;
        }
      
        backlight->remote = gbinder_servicemanager_get_service_sync(backlight->service_manager,
            g_strdup_printf("%s/%s", iface, BINDER_LIGHT_SLOT),
            NULL);
        
        if(!backlight->remote) {
          g_warning("Failed to get light service");
          return FALSE;
        }
      
        backlight->client = gbinder_client_new(backlight->remote, iface);
        if (!backlight->client) {
          g_warning("Failed to create binder client");
          return FALSE;
        }
      
        backlight->brightness_min = 1;
        backlight->brightness_max = 255;
        backlight->use_binder = TRUE;
        return TRUE;
}

#ifdef __linux__
static GUdevDevice*
gsd_backlight_udev_get_type (GList *devices, const gchar *type)
{
        const gchar *type_tmp;
        GList *d;

        for (d = devices; d != NULL; d = d->next) {
                type_tmp = g_udev_device_get_sysfs_attr (d->data, "type");
                if (g_strcmp0 (type_tmp, type) == 0)
                        return G_UDEV_DEVICE (g_object_ref (d->data));
        }
        return NULL;
}

/*
 * Search for a raw backlight interface, raw backlight interfaces registered
 * by the drm driver will have the drm-connector as their parent, check the
 * drm-connector's enabled sysfs attribute so that we pick the right LCD-panel
 * connector on laptops with hybrid-gfx. Fall back to just picking the first
 * raw backlight interface if no enabled interface is found.
 */
static GUdevDevice*
gsd_backlight_udev_get_raw (GList *devices)
{
        GUdevDevice *parent;
        const gchar *attr;
        GList *d;

        for (d = devices; d != NULL; d = d->next) {
                attr = g_udev_device_get_sysfs_attr (d->data, "type");
                if (g_strcmp0 (attr, "raw") != 0)
                        continue;

                parent = g_udev_device_get_parent (d->data);
                if (!parent)
                        continue;

                attr = g_udev_device_get_sysfs_attr (parent, "enabled");
                if (!attr || g_strcmp0 (attr, "enabled") != 0)
                        continue;

                return G_UDEV_DEVICE (g_object_ref (d->data));
        }

        return gsd_backlight_udev_get_type (devices, "raw");
}

static void
gsd_backlight_udev_resolve (GsdBacklight *backlight)
{
        g_autolist(GUdevDevice) devices = NULL;

        g_assert (backlight->udev != NULL);

        devices = g_udev_client_query_by_subsystem (backlight->udev, "backlight");
        if (devices == NULL)
                return;

        /* Search the backlight devices and prefer the types:
         * firmware -> platform -> raw */
        backlight->udev_device = gsd_backlight_udev_get_type (devices, "firmware");
        if (backlight->udev_device != NULL)
                return;

        backlight->udev_device = gsd_backlight_udev_get_type (devices, "platform");
        if (backlight->udev_device != NULL)
                return;

        backlight->udev_device = gsd_backlight_udev_get_raw (devices);
        if (backlight->udev_device != NULL)
                return;
}

static gboolean
gsd_backlight_udev_idle_update_cb (GsdBacklight *backlight)
{
        g_autoptr(GError) error = NULL;
        gint brightness;
        g_autofree gchar *path = NULL;
        g_autofree gchar *contents = NULL;
        backlight->idle_update = 0;

        /* If we are active again now, just stop. */
        if (backlight->active_task)
                return FALSE;

        path = g_build_filename (g_udev_device_get_sysfs_path (backlight->udev_device), "brightness", NULL);
        if (!g_file_get_contents (path, &contents, NULL, &error)) {
                g_warning ("Could not get brightness from sysfs: %s", error->message);
                return FALSE;
        }
        brightness = g_ascii_strtoll (contents, NULL, 0);

        /* e.g. brightness lower than our minimum. */
        brightness = CLAMP (brightness, backlight->brightness_min, backlight->brightness_max);

        /* Only notify if brightness has changed. */
        if (brightness == backlight->brightness_val)
                return FALSE;

        backlight->brightness_val = brightness;
        backlight->brightness_target = brightness;
        g_object_notify_by_pspec (G_OBJECT (backlight), props[PROP_BRIGHTNESS]);

        return FALSE;
}

static void
gsd_backlight_udev_idle_update (GsdBacklight *backlight)
{
        if (backlight->idle_update)
                return;

        backlight->idle_update = g_idle_add ((GSourceFunc) gsd_backlight_udev_idle_update_cb, backlight);
}


static void
gsd_backlight_udev_uevent (GUdevClient *client, const gchar *action, GUdevDevice *device, gpointer user_data)
{
        GsdBacklight *backlight = GSD_BACKLIGHT (user_data);

        if (g_strcmp0 (action, "change") != 0)
                return;

        /* We are going to update our state after processing the tasks anyway. */
        if (!g_queue_is_empty (&backlight->tasks))
                return;

        if (g_strcmp0 (g_udev_device_get_sysfs_path (device),
                       g_udev_device_get_sysfs_path (backlight->udev_device)) != 0)
                return;

        g_debug ("GsdBacklight: Got uevent");

        gsd_backlight_udev_idle_update (backlight);
}


static gboolean
gsd_backlight_udev_init (GsdBacklight *backlight)
{
        const gchar* const subsystems[] = {"backlight", NULL};
        gint brightness_val;

        backlight->udev = g_udev_client_new (subsystems);
        gsd_backlight_udev_resolve (backlight);
        if (backlight->udev_device == NULL)
                return FALSE;

        backlight->brightness_max = g_udev_device_get_sysfs_attr_as_int (backlight->udev_device,
                                                                         "max_brightness");
        backlight->brightness_min = MAX (1, backlight->brightness_max * 0.01);

        /* If the interface has less than 100 possible values, and it is of type
         * raw, then assume that 0 does not turn off the backlight completely. */
        if (backlight->brightness_max < 99 &&
            g_strcmp0 (g_udev_device_get_sysfs_attr (backlight->udev_device, "type"), "raw") == 0)
                backlight->brightness_min = 0;

        /* Ignore a backlight which has no steps. */
        if (backlight->brightness_min >= backlight->brightness_max) {
                g_warning ("Resolved kernel backlight has an unusable maximum brightness (%d)", backlight->brightness_max);
                g_clear_object (&backlight->udev_device);
                return FALSE;
        }

        brightness_val = g_udev_device_get_sysfs_attr_as_int (backlight->udev_device,
                                                              "brightness");
        backlight->brightness_val = CLAMP (brightness_val,
                                           backlight->brightness_min,
                                           backlight->brightness_max);
        g_debug ("Using udev device with brightness from %i to %i. Current brightness is %i.",
                 backlight->brightness_min, backlight->brightness_max, backlight->brightness_val);

        g_signal_connect_object (backlight->udev, "uevent",
                                 G_CALLBACK (gsd_backlight_udev_uevent),
                                 backlight, 0);

        return TRUE;
}


typedef struct {
        int value;
        char *value_str;
} BacklightHelperData;

static void gsd_backlight_process_taskqueue (GsdBacklight *backlight);

static void
backlight_task_data_destroy (gpointer data)
{
        BacklightHelperData *task_data = (BacklightHelperData*) data;

        g_free (task_data->value_str);
        g_free (task_data);
}

static void
gsd_backlight_set_helper_return (GsdBacklight *backlight, GTask *task, gint result, const GError *error)
{
        GTask *finished_task;
        gint percent = ABS_TO_PERCENTAGE (backlight->brightness_min, backlight->brightness_max, result);

        if (error)
                g_warning ("Error executing backlight helper: %s", error->message);

        /* If the queue will be empty then update the current value. */
        if (task == g_queue_peek_tail (&backlight->tasks)) {
                if (error == NULL) {
                        g_assert (backlight->brightness_target == result);

                        backlight->brightness_val = backlight->brightness_target;
                        g_debug ("New brightness value is in effect %i (%i..%i)",
                                 backlight->brightness_val, backlight->brightness_min, backlight->brightness_max);
                        g_object_notify_by_pspec (G_OBJECT (backlight), props[PROP_BRIGHTNESS]);
                }

                /* The udev handler won't read while a write is pending, so queue an
                 * update in case we have missed some events. */
                gsd_backlight_udev_idle_update (backlight);
        }

        /* Return all the pending tasks up and including the one we actually
         * processed. */
        do {
                finished_task = g_queue_pop_head (&backlight->tasks);

                if (error)
                        g_task_return_error (finished_task, g_error_copy (error));
                else
                        g_task_return_int (finished_task, percent);

                g_object_unref (finished_task);
        } while (finished_task != task);
}

static void
gsd_backlight_set_helper_finish (GObject *obj, GAsyncResult *res, gpointer user_data)
{
        g_autoptr(GSubprocess) proc = G_SUBPROCESS (obj);
        GTask *task = G_TASK (user_data);
        BacklightHelperData *data = g_task_get_task_data (task);
        GsdBacklight *backlight = g_task_get_source_object (task);
        g_autoptr(GError) error = NULL;

        g_assert (task == backlight->active_task);
        backlight->active_task = NULL;

        g_subprocess_wait_check_finish (proc, res, &error);

        if (error)
                goto done;

done:
        gsd_backlight_set_helper_return (backlight, task, data->value, error);
        /* Start processing any tasks that were added in the meantime. */
        gsd_backlight_process_taskqueue (backlight);
}

static void
gsd_backlight_run_set_helper (GsdBacklight *backlight, GTask *task)
{
        GSubprocess *proc = NULL;
        BacklightHelperData *data = g_task_get_task_data (task);
        const gchar *gsd_backlight_helper = NULL;
        GError *error = NULL;

        g_assert (backlight->active_task == NULL);
        backlight->active_task = task;

        if (data->value_str == NULL)
                data->value_str = g_strdup_printf ("%d", data->value);

        /* This is solely for use by the test environment. If given, execute
         * this helper instead of the internal helper using pkexec */
        gsd_backlight_helper = g_getenv ("GSD_BACKLIGHT_HELPER");
        if (!gsd_backlight_helper) {
                proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
                                         &error,
                                         "pkexec",
                                         LIBEXECDIR "/gsd-backlight-helper",
                                         g_udev_device_get_sysfs_path (backlight->udev_device),
                                         data->value_str, NULL);
        } else {
                proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
                                         &error,
                                         gsd_backlight_helper,
                                         g_udev_device_get_sysfs_path (backlight->udev_device),
                                         data->value_str, NULL);
        }

        if (proc == NULL) {
                gsd_backlight_set_helper_return (backlight, task, -1, error);
                return;
        }

        g_subprocess_wait_check_async (proc, g_task_get_cancellable (task),
                                       gsd_backlight_set_helper_finish,
                                       task);
}

static void
gsd_backlight_process_taskqueue (GsdBacklight *backlight)
{
        GTask *to_run;

        /* There is already a task active, nothing to do. */
        if (backlight->active_task)
                return;

        /* Get the last added task, thereby compressing the updates into one. */
        to_run = G_TASK (g_queue_peek_tail (&backlight->tasks));
        if (to_run == NULL)
                return;

        /* And run it! */
        gsd_backlight_run_set_helper (backlight, to_run);
}
#endif /* __linux__ */

static GnomeRROutput*
gsd_backlight_rr_find_output (GsdBacklight *backlight, gboolean controllable)
{
        GnomeRROutput *output = NULL;
        GnomeRROutput **outputs;
        guint i;

        /* search all X11 outputs for the device id */
        outputs = gnome_rr_screen_list_outputs (backlight->rr_screen);
        if (outputs == NULL)
                goto out;

        for (i = 0; outputs[i] != NULL; i++) {
                gboolean builtin = gnome_rr_output_is_builtin_display (outputs[i]);
                gint backlight = gnome_rr_output_get_backlight (outputs[i]);

                g_debug("Output %d: %s, backlight %d", i, builtin ? "builtin" : "external", backlight);
                if (builtin && (!controllable || backlight >= 0)) {
                        output = outputs[i];
                        break;
                }
        }
out:
        return output;
}

/**
 * gsd_backlight_get_brightness
 * @backlight: a #GsdBacklight
 * @target: Output parameter for the value the target value of pending set operations.
 *
 * The backlight value returns the last known stable value. This value will
 * only update once all pending operations to set a new value have finished.
 *
 * As such, this function may return a different value from the return value
 * of the async brightness setter. This happens when another set operation was
 * queued after it was already running.
 *
 * If the internal display is detected as disabled, then the function will
 * instead return -1.
 *
 * Returns: The last stable backlight value or -1 if the internal display is disabled.
 **/
gint
gsd_backlight_get_brightness (GsdBacklight *backlight, gint *target)
{
        if (backlight->builtin_display_disabled)
                return -1;

        if (target)
                *target = ABS_TO_PERCENTAGE (backlight->brightness_min, backlight->brightness_max, backlight->brightness_target);

        return ABS_TO_PERCENTAGE (backlight->brightness_min, backlight->brightness_max, backlight->brightness_val);
}

static gboolean
gsd_backlight_set_brightness_binder (GsdBacklight *backlight, gint value, GError **error)
{
      GBinderLocalRequest *req;
      GBinderRemoteReply *reply;
      GBinderWriter writer;
      LightState *light_state;
      int status;
  
      req = gbinder_client_new_request(backlight->client);
      gbinder_local_request_init_writer(req, &writer);
      
      light_state = gbinder_writer_new0(&writer, LightState);
      light_state->color = (0xff << 24) | (value << 16) | (value << 8) | value;
      light_state->brightnessMode = 1;
  
      gbinder_writer_append_int32(&writer, 0);
      gbinder_writer_append_buffer_object(&writer, light_state, sizeof(*light_state));
  
      reply = gbinder_client_transact_sync_reply(backlight->client, BINDER_LIGHT_SET_LIGHT, req, &status);
      gbinder_local_request_unref(req);
  
      if (status != GBINDER_STATUS_OK) {
          g_set_error(error, GSD_POWER_MANAGER_ERROR, GSD_POWER_MANAGER_ERROR_FAILED,
                      "Failed to set display brightness");
          return FALSE;
      }

      gbinder_remote_reply_unref(reply);
  
      return TRUE;
}

static void
gsd_backlight_set_brightness_val_async (GsdBacklight *backlight,
                                        int value,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
        GError *error = NULL;
        GTask *task = NULL;
        GnomeRROutput *output;
        gint percent;

        value = MIN(backlight->brightness_max, value);
        value = MAX(backlight->brightness_min, value);

        backlight->brightness_target = value;

        task = g_task_new (backlight, cancellable, callback, user_data);

    if (backlight->use_binder) {
      if (gsd_backlight_set_brightness_binder(backlight, value, &error)) {
        backlight->brightness_val = value;
        g_object_notify_by_pspec (G_OBJECT (backlight), props[PROP_BRIGHTNESS]);
        g_task_return_int (task, gsd_backlight_get_brightness (backlight, NULL));
      } else {
        g_task_return_error(task, error);

      }
      g_object_unref (task);
      return;
    }

#ifdef __linux__
        if (backlight->udev_device != NULL) {
                BacklightHelperData *task_data;

                if (backlight->logind_proxy) {
                        g_dbus_proxy_call (backlight->logind_proxy,
                                           "SetBrightness",
                                           g_variant_new ("(ssu)",
                                                          "backlight",
                                                          g_udev_device_get_name (backlight->udev_device),
                                                          backlight->brightness_target),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1, NULL,
                                           NULL, NULL);

                        percent = ABS_TO_PERCENTAGE (backlight->brightness_min,
                                                     backlight->brightness_max,
                                                     backlight->brightness_target);
                        g_task_return_int (task, percent);
                } else {
                        task_data = g_new0 (BacklightHelperData, 1);
                        task_data->value = backlight->brightness_target;
                        g_task_set_task_data (task, task_data, backlight_task_data_destroy);

                        /* Task is set up now. Queue it and ensure we are working something. */
                        g_queue_push_tail (&backlight->tasks, task);
                        gsd_backlight_process_taskqueue (backlight);
                }

                return;
        }
#endif /* __linux__ */

        /* Fallback to setting via GNOME RR/X11 */
        output = gsd_backlight_rr_find_output (backlight, TRUE);
        if (output) {
                if (!gnome_rr_output_set_backlight (output, value, &error)) {
                        g_task_return_error (task, error);
                        g_object_unref (task);
                        return;
                }
                backlight->brightness_val = gnome_rr_output_get_backlight (output);
                g_object_notify_by_pspec (G_OBJECT (backlight), props[PROP_BRIGHTNESS]);
                g_task_return_int (task, gsd_backlight_get_brightness (backlight, NULL));
                g_object_unref (task);

                return;
        }

        g_assert_not_reached ();

        g_task_return_new_error (task, GSD_POWER_MANAGER_ERROR,
                                 GSD_POWER_MANAGER_ERROR_FAILED,
                                 "No method to set brightness!");
        g_object_unref (task);
}

void
gsd_backlight_set_brightness_async (GsdBacklight *backlight,
                                    gint percent,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
        /* Overflow/underflow is handled by gsd_backlight_set_brightness_val_async. */
        gsd_backlight_set_brightness_val_async (backlight,
                                                PERCENTAGE_TO_ABS (backlight->brightness_min, backlight->brightness_max, percent),
                                                cancellable,
                                                callback,
                                                user_data);
}

/**
 * gsd_backlight_set_brightness_finish
 * @backlight: a #GsdBacklight
 * @res: the #GAsyncResult passed to the callback
 * @error: #GError return address
 *
 * Finish an operation started by gsd_backlight_set_brightness_async(). Will
 * return the value that was actually set (which may be different because of
 * rounding or as multiple set actions were queued up).
 *
 * Please note that a call to gsd_backlight_get_brightness() may not in fact
 * return the same value if further operations to set the value are pending.
 *
 * Returns: The brightness in percent that was set.
 **/
gint
gsd_backlight_set_brightness_finish (GsdBacklight *backlight,
                                     GAsyncResult *res,
                                     GError **error)
{
        return g_task_propagate_int (G_TASK (res), error);
}

void
gsd_backlight_step_up_async (GsdBacklight *backlight,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
        gint value;

        /* Overflows are handled by gsd_backlight_set_brightness_val_async. */
        value = backlight->brightness_target + backlight->brightness_step;

        gsd_backlight_set_brightness_val_async (backlight,
                                                value,
                                                cancellable,
                                                callback,
                                                user_data);
}

/**
 * gsd_backlight_step_up_finish
 * @backlight: a #GsdBacklight
 * @res: the #GAsyncResult passed to the callback
 * @error: #GError return address
 *
 * Finish an operation started by gsd_backlight_step_up_async(). Will return
 * the value that was actually set (which may be different because of rounding
 * or as multiple set actions were queued up).
 *
 * Please note that a call to gsd_backlight_get_brightness() may not in fact
 * return the same value if further operations to set the value are pending.
 *
 * For simplicity it is also valid to call gsd_backlight_set_brightness_finish()
 * allowing sharing the callback routine for calls to
 * gsd_backlight_set_brightness_async(), gsd_backlight_step_up_async(),
 * gsd_backlight_step_down_async() and gsd_backlight_cycle_up_async().
 *
 * Returns: The brightness in percent that was set.
 **/
gint
gsd_backlight_step_up_finish (GsdBacklight *backlight,
                              GAsyncResult *res,
                              GError **error)
{
        return g_task_propagate_int (G_TASK (res), error);
}

void
gsd_backlight_step_down_async (GsdBacklight *backlight,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
        gint value;

        /* Underflows are handled by gsd_backlight_set_brightness_val_async. */
        value = backlight->brightness_target - backlight->brightness_step;

        gsd_backlight_set_brightness_val_async (backlight,
                                                value,
                                                cancellable,
                                                callback,
                                                user_data);
}

/**
 * gsd_backlight_step_down_finish
 * @backlight: a #GsdBacklight
 * @res: the #GAsyncResult passed to the callback
 * @error: #GError return address
 *
 * Finish an operation started by gsd_backlight_step_down_async(). Will return
 * the value that was actually set (which may be different because of rounding
 * or as multiple set actions were queued up).
 *
 * Please note that a call to gsd_backlight_get_brightness() may not in fact
 * return the same value if further operations to set the value are pending.
 *
 * For simplicity it is also valid to call gsd_backlight_set_brightness_finish()
 * allowing sharing the callback routine for calls to
 * gsd_backlight_set_brightness_async(), gsd_backlight_step_up_async(),
 * gsd_backlight_step_down_async() and gsd_backlight_cycle_up_async().
 *
 * Returns: The brightness in percent that was set.
 **/
gint
gsd_backlight_step_down_finish (GsdBacklight *backlight,
                                GAsyncResult *res,
                                GError **error)
{
        return g_task_propagate_int (G_TASK (res), error);
}

/**
 * gsd_backlight_cycle_up_async
 * @backlight: a #GsdBacklight
 * @cancellable: an optional #GCancellable, NULL to ignore
 * @callback: the #GAsyncReadyCallback invoked for cycle up to be finished
 * @user_data: the #gpointer passed to the callback
 *
 * Start a brightness cycle up operation by gsd_backlight_cycle_up_async().
 * The brightness will be stepped up if it is not already at the maximum.
 * If it is already at the maximum, it will be set to the minimum brightness.
 **/
void
gsd_backlight_cycle_up_async (GsdBacklight *backlight,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
        if (backlight->brightness_target < backlight->brightness_max)
                gsd_backlight_step_up_async (backlight,
                                             cancellable,
                                             callback,
                                             user_data);
        else
                gsd_backlight_set_brightness_val_async (backlight,
                                                        backlight->brightness_min,
                                                        cancellable,
                                                        callback,
                                                        user_data);
}

/**
 * gsd_backlight_cycle_up_finish
 * @backlight: a #GsdBacklight
 * @res: the #GAsyncResult passed to the callback
 * @error: #GError return address
 *
 * Finish an operation started by gsd_backlight_cycle_up_async(). Will return
 * the value that was actually set (which may be different because of rounding
 * or as multiple set actions were queued up).
 *
 * Please note that a call to gsd_backlight_get_brightness() may not in fact
 * return the same value if further operations to set the value are pending.
 *
 * For simplicity it is also valid to call gsd_backlight_set_brightness_finish()
 * allowing sharing the callback routine for calls to
 * gsd_backlight_set_brightness_async(), gsd_backlight_step_up_async(),
 * gsd_backlight_step_down_async() and gsd_backlight_cycle_up_async().
 *
 * Returns: The brightness in percent that was set.
 **/
gint
gsd_backlight_cycle_up_finish (GsdBacklight *backlight,
                               GAsyncResult *res,
                               GError **error)
{
        return g_task_propagate_int (G_TASK (res), error);
}

/**
 * gsd_backlight_get_connector
 * @backlight: a #GsdBacklight
 *
 * Return the connector for the display that is being controlled by the
 * #GsdBacklight object. This connector can be passed to gnome-shell to show
 * the on screen display only on the affected screen.
 *
 * Returns: The connector of the controlled output or NULL if unknown.
 **/
const char*
gsd_backlight_get_connector (GsdBacklight *backlight)
{
        GnomeRROutput *output;

        output = gsd_backlight_rr_find_output (backlight, FALSE);
        if (output == NULL)
                return NULL;

        return gnome_rr_output_get_name (output);
}

static void
gsd_backlight_rr_screen_changed_cb (GnomeRRScreen *screen,
                                    gpointer data)
{
        GsdBacklight *backlight = GSD_BACKLIGHT (data);
        GnomeRROutput *output;
        gboolean builtin_display_disabled = FALSE;

        /* NOTE: Err on the side of assuming the backlight controlls something
         *       even if we cannot find the output that belongs to it.
         *       This might backfire on us obviously if the hardware claims it
         *       can control a non-existing screen.
         */
        output = gsd_backlight_rr_find_output (backlight, FALSE);
        if (output)
                builtin_display_disabled = !gnome_rr_output_get_crtc (output);

        if (builtin_display_disabled != backlight->builtin_display_disabled) {
                backlight->builtin_display_disabled = builtin_display_disabled;
                g_object_notify_by_pspec (G_OBJECT (backlight), props[PROP_BRIGHTNESS]);
        }
}

static void
gsd_backlight_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
        GsdBacklight *backlight = GSD_BACKLIGHT (object);

        switch (prop_id) {
        case PROP_RR_SCREEN:
                g_value_set_object (value, backlight->rr_screen);
                break;

        case PROP_BRIGHTNESS:
                g_value_set_int (value, gsd_backlight_get_brightness (backlight, NULL));
                break;

        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gsd_backlight_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
        GsdBacklight *backlight = GSD_BACKLIGHT (object);

        switch (prop_id) {
        case PROP_RR_SCREEN:
                backlight->rr_screen = g_value_dup_object (value);

                g_signal_connect_object (backlight->rr_screen, "changed",
                                         G_CALLBACK (gsd_backlight_rr_screen_changed_cb),
                                         object, 0);
                gsd_backlight_rr_screen_changed_cb (backlight->rr_screen, object);

                break;

        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static gboolean
gsd_backlight_initable_init (GInitable       *initable,
                             GCancellable    *cancellable,
                             GError         **error)
{
        GsdBacklight *backlight = GSD_BACKLIGHT (initable);
  
    if (gsd_backlight_init_binder(backlight, BINDER_LIGHT_AIDL_DEVICE, BINDER_LIGHT_AIDL_IFACE)) {
      g_debug("Yep! AIDL Binderrrr");
      return TRUE;
    }

    if (gsd_backlight_init_binder(backlight, BINDER_LIGHT_HIDL_DEVICE, BINDER_LIGHT_HIDL_IFACE)) {
      g_debug("Yep! HIDL Binderrrr");
      return TRUE;
    }

        GnomeRROutput* output = NULL;
        GError *logind_error = NULL;

        if (cancellable != NULL) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                     "GsdBacklight does not support cancelling initialization.");
                return FALSE;
        }

#ifdef __linux__
        backlight->logind_proxy =
                g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                               0,
                                               NULL,
                                               SYSTEMD_DBUS_NAME,
                                               SYSTEMD_DBUS_PATH,
                                               SYSTEMD_DBUS_INTERFACE,
                                               NULL, &logind_error);
        if (backlight->logind_proxy) {
                /* Check that the SetBrightness method does exist */
                g_dbus_proxy_call_sync (backlight->logind_proxy,
                                        "SetBrightness", NULL,
                                        G_DBUS_CALL_FLAGS_NONE, -1,
                                        NULL, &logind_error);

                if (g_error_matches (logind_error, G_DBUS_ERROR,
                                     G_DBUS_ERROR_INVALID_ARGS)) {
                        /* We are calling the method with no arguments, so
                         * this is expected.
                         */
                        g_clear_error (&logind_error);
                } else if (g_error_matches (logind_error, G_DBUS_ERROR,
                                            G_DBUS_ERROR_UNKNOWN_METHOD)) {
                        /* systemd version is too old, so ignore.
                         */
                        g_clear_error (&logind_error);
                        g_clear_object (&backlight->logind_proxy);
                } else {
                        /* Fail on anything else */
                        g_clear_object (&backlight->logind_proxy);
                }
        }

        if (logind_error) {
                g_warning ("No logind found: %s", logind_error->message);
                g_error_free (logind_error);
        }

        /* Try finding a udev device. */
        if (gsd_backlight_udev_init (backlight))
                goto found;
#endif /* __linux__ */

        /* Try GNOME RR as a fallback. */
        output = gsd_backlight_rr_find_output (backlight, TRUE);
        if (output) {
                g_debug ("Using GNOME RR (mutter) for backlight.");
                backlight->brightness_min = 1;
                backlight->brightness_max = 100;
                backlight->brightness_val = gnome_rr_output_get_backlight (output);
                backlight->brightness_step = gnome_rr_output_get_min_backlight_step (output);

                goto found;
        }

        g_debug ("No usable backlight found.");

        g_set_error_literal (error, GSD_POWER_MANAGER_ERROR, GSD_POWER_MANAGER_ERROR_NO_BACKLIGHT,
                             "No usable backlight could be found!");

        return FALSE;

found:
        backlight->brightness_target = backlight->brightness_val;
        backlight->brightness_step = MAX(backlight->brightness_step, BRIGHTNESS_STEP_AMOUNT(backlight->brightness_max - backlight->brightness_min + 1));

        g_debug ("Step size for backlight is %i.", backlight->brightness_step);

        return TRUE;
}

static void
gsd_backlight_finalize (GObject *object)
{
        GsdBacklight *backlight = GSD_BACKLIGHT (object);

#ifdef __linux__
        g_assert (backlight->active_task == NULL);
        g_assert (g_queue_is_empty (&backlight->tasks));
        g_clear_object (&backlight->logind_proxy);
        g_clear_object (&backlight->udev);
        g_clear_object (&backlight->udev_device);
        if (backlight->idle_update) {
                g_source_remove (backlight->idle_update);
                backlight->idle_update = 0;
        }
#endif /* __linux__ */

        g_clear_object (&backlight->rr_screen);
}

static void
gsd_backlight_initable_iface_init (GInitableIface *iface)
{
  iface->init = gsd_backlight_initable_init;
}

static void
gsd_backlight_class_init (GsdBacklightClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = gsd_backlight_finalize;
        object_class->get_property = gsd_backlight_get_property;
        object_class->set_property = gsd_backlight_set_property;

        props[PROP_RR_SCREEN] = g_param_spec_object ("rr-screen", "GnomeRRScreen",
                                                     "GnomeRRScreen usable for backlight control.",
                                                     GNOME_TYPE_RR_SCREEN,
                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

        props[PROP_BRIGHTNESS] = g_param_spec_int ("brightness", "The display brightness",
                                                   "The brightness of the internal display in percent.",
                                                   0, 100, 100,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

        g_object_class_install_properties (object_class, PROP_LAST, props);
}


static void
gsd_backlight_init (GsdBacklight *backlight)
{
        backlight->brightness_target = -1;
        backlight->brightness_min = -1;
        backlight->brightness_max = -1;
        backlight->brightness_val = -1;
        backlight->brightness_step = 1;

#ifdef __linux__
        backlight->active_task = NULL;
        g_queue_init (&backlight->tasks);
#endif /* __linux__ */
}

GsdBacklight *
gsd_backlight_new (GnomeRRScreen  *rr_screen,
                   GError        **error)
{
        return GSD_BACKLIGHT (g_initable_new (GSD_TYPE_BACKLIGHT, NULL, error,
                                              "rr-screen", rr_screen,
                                              NULL));
}

