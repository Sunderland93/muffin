/*
 * Copyright (C) 2026 Linux Mint
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "wayland/meta-wayland-idle-notify.h"

#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-versions.h"

#include "ext-idle-notify-v1-server-protocol.h"

static void
ext_idle_notifier_get_idle_notification (struct wl_client   *client,
                                         struct wl_resource *resource,
                                         uint32_t            id,
                                         uint32_t            timeout,
                                         struct wl_resource *seat)
{
}

static void
ext_idle_notifier_get_input_idle_notification (struct wl_client   *client,
                                               struct wl_resource *resource,
                                               uint32_t            id,
                                               uint32_t            timeout,
                                               struct wl_resource *seat)
{
}

static void
ext_idle_notifier_destroy (struct wl_client   *client,
                           struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct ext_idle_notifier_v1_interface ext_idle_notifier_v1_impl = {
  ext_idle_notifier_destroy,
  ext_idle_notifier_get_idle_notification,
  ext_idle_notifier_get_input_idle_notification,
};

static void
bind_ext_idle_notifier (struct wl_client *client,
                        void             *data,
                        uint32_t          version,
                        uint32_t          id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
                                 &ext_idle_notifier_v1_interface,
                                 version,
                                 id);
  wl_resource_set_implementation (resource,
                                  &ext_idle_notifier_v1_impl,
                                  NULL,
                                  NULL);
}

void
meta_wayland_idle_notify_init (MetaWaylandCompositor *compositor)
{
  if (wl_global_create (compositor->wayland_display,
      &ext_idle_notifier_v1_interface,
      META_EXT_IDLE_NOTIFY_V1_VERSION,
      NULL,
      bind_ext_idle_notifier) == NULL)
      g_error ("Failed to register a global ext-idle-notify object");
}
