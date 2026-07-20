/*
 * Copyright (C) 2024 Igalia, S.L.
 *
 * Author: Nick Yamane <nickdiego@igalia.com>
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
 *
 */

#include "config.h"

#include <glib.h>
#include <wayland-server.h>

#include "wayland/meta-wayland-toplevel-drag.h"

#include "clutter/clutter.h"
#include "compositor/compositor-private.h"
#include "core/window-private.h"
#include "meta/common.h"
#include "meta/types.h"
#include "meta/util.h"
#include "meta/window.h"
#include "wayland/meta-wayland-data-device.h"
#include "wayland/meta-wayland-data-source.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-seat.h"
#include "wayland/meta-wayland-surface.h"
#include "wayland/meta-wayland-types.h"
#include "wayland/meta-wayland-versions.h"
#include "wayland/meta-wayland-xdg-shell.h"

#include "xdg-toplevel-drag-v1-server-protocol.h"

static void toplevel_drag_end_grab_op (MetaWaylandToplevelDrag *toplevel_drag);

static void
xdg_toplevel_drag_destructor (struct wl_resource *resource)
{
  MetaWaylandToplevelDrag *toplevel_drag = wl_resource_get_user_data (resource);
  g_assert (toplevel_drag);
  meta_topic (META_DEBUG_EVENTS, "Destroying xdg_toplevel_drag#%u",
              wl_resource_get_id (resource));

  meta_wayland_toplevel_drag_end (toplevel_drag);
  g_free (toplevel_drag);
}

static void
on_dragged_window_unmanaging (MetaWindow              *window,
                              MetaWaylandToplevelDrag *toplevel_drag)
{
  meta_topic (META_DEBUG_EVENTS, "Dragged window destroyed.");
  g_clear_signal_handler (&toplevel_drag->window_unmanaging_handler_id, window);
  g_clear_signal_handler (&toplevel_drag->window_shown_handler_id, window);
  toplevel_drag->dragged_surface = NULL;
}

static void
on_data_source_destroyed (MetaWaylandDataSource   *data_source,
                          MetaWaylandToplevelDrag *toplevel_drag)
{
  meta_topic (META_DEBUG_EVENTS,
              "Data source destroyed before xdg_toplevel_drag#%d",
              wl_resource_get_id (toplevel_drag->resource));

  g_clear_signal_handler (&toplevel_drag->source_destroyed_handler_id,
                          data_source);
  meta_wayland_toplevel_drag_end (toplevel_drag);
}

static void
add_window_geometry_origin (MetaWaylandSurface *dragged_surface,
                            int                *x_offset,
                            int                *y_offset)
{
  MetaRectangle toplevel_geometry;
  toplevel_geometry = meta_wayland_xdg_surface_get_window_geometry (
    META_WAYLAND_XDG_SURFACE (dragged_surface->role));

  if (x_offset)
    *x_offset = *x_offset + toplevel_geometry.x;
  if (y_offset)
    *y_offset = *y_offset + toplevel_geometry.y;
}

static void
xdg_toplevel_drag_destroy (struct wl_client   *client,
                           struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static MetaWaylandSurface *
surface_from_xdg_toplevel_resource (struct wl_resource *resource)
{
  MetaWaylandSurfaceRole *surface_role = wl_resource_get_user_data (resource);

  if (!META_IS_WAYLAND_SURFACE_ROLE (surface_role))
    return NULL;

  return meta_wayland_surface_role_get_surface (surface_role);
}

static void
toplevel_drag_grab_focus (MetaWaylandPointerGrab *grab,
                          MetaWaylandSurface     *surface)
{
  /* During a toplevel drag, we don't change focus - the dragged window
   * follows the cursor via the grab op mechanism. */
}

static void
toplevel_drag_grab_motion (MetaWaylandPointerGrab *grab,
                           const ClutterEvent     *event)
{
  MetaWaylandToplevelDrag *toplevel_drag =
    wl_container_of (grab, toplevel_drag, pointer_grab);

  if (!toplevel_drag->grab_active || !toplevel_drag->dragged_surface)
    return;

  /* The grab op mechanism in meta_window_handle_mouse_grab_op_event
   * handles the actual window movement. We just need to forward the
   * motion event so the grab op can track it. The event is already
   * being processed by the main event handler since we set up a
   * META_GRAB_OP_MOVING, so this is mainly for completeness. */
}

static void
toplevel_drag_grab_button (MetaWaylandPointerGrab *grab,
                           const ClutterEvent     *event)
{
  MetaWaylandToplevelDrag *toplevel_drag =
    wl_container_of (grab, toplevel_drag, pointer_grab);
  ClutterEventType event_type = clutter_event_type (event);

  if (!toplevel_drag->grab_active)
    return;

  /* End the grab op on button release */
  if (event_type == CLUTTER_BUTTON_RELEASE)
    {
      toplevel_drag_end_grab_op (toplevel_drag);
    }
}

static const MetaWaylandPointerGrabInterface toplevel_drag_grab_interface = {
  toplevel_drag_grab_focus,
  toplevel_drag_grab_motion,
  toplevel_drag_grab_button,
};

static void
toplevel_drag_end_grab_op (MetaWaylandToplevelDrag *toplevel_drag)
{
  MetaDisplay *display;

  if (!toplevel_drag->grab_active)
    return;

  display = meta_get_display ();
  if (display && display->grab_op != META_GRAB_OP_NONE)
    {
      meta_display_end_grab_op (display,
                                meta_display_get_current_time_roundtrip (display));
    }

  toplevel_drag->grab_active = FALSE;
}

static void
start_window_drag (MetaWindow              *dragged_window,
                   MetaWaylandToplevelDrag *toplevel_drag,
                   graphene_point_t        *offset_hint)
{
  MetaWaylandSeat *seat;
  MetaWaylandDragGrab *drag_grab;
  MetaSurfaceActor *surface_actor;
  uint32_t timestamp;
  MetaDisplay *display;

  g_assert (toplevel_drag);
  g_assert (toplevel_drag->data_source);
  g_assert (toplevel_drag->dragged_surface);

  seat = meta_wayland_data_source_get_seat (toplevel_drag->data_source);
  if (!seat)
    return;

  drag_grab = meta_wayland_data_device_get_current_grab (&seat->data_device);
  if (!drag_grab ||
      toplevel_drag->data_source !=
        meta_wayland_drag_grab_get_data_source (drag_grab))
    {
      meta_topic (META_DEBUG_EVENTS, "No drag grab found, earlying out.");
      return;
    }

  /* Disable events on the dragged surface, so drag enter and leave events
   * can be detected for other surfaces. */
  surface_actor = meta_wayland_surface_get_actor (toplevel_drag->dragged_surface);
  clutter_actor_set_reactive (CLUTTER_ACTOR (surface_actor), FALSE);

  meta_topic (META_DEBUG_EVENTS, "Starting window drag. window=%s offset=(%.0f, %.0f)",
              dragged_window->desc,
              (offset_hint ? offset_hint->x : -1),
              (offset_hint ? offset_hint->y : -1));

  display = meta_get_display ();
  timestamp = meta_display_get_current_time_roundtrip (display);

  /* Start the grab op for window moving */
  {
    int root_x, root_y;

    if (offset_hint)
      {
        root_x = (int) offset_hint->x;
        root_y = (int) offset_hint->y;
      }
    else
      {
        /* Get current pointer position */
        graphene_point_t pos;
        clutter_input_device_get_coords (seat->pointer->device, NULL, &pos);
        root_x = (int) pos.x;
        root_y = (int) pos.y;
      }

    if (!meta_display_begin_grab_op (display,
                                     dragged_window,
                                     META_GRAB_OP_MOVING,
                                     TRUE,  /* pointer_already_grabbed */
                                     TRUE,  /* frame_action */
                                     1,     /* button */
                                     0,     /* modmask */
                                     timestamp,
                                     root_x,
                                     root_y))
      {
        meta_topic (META_DEBUG_EVENTS, "Failed to start grab op for window drag.");
        clutter_actor_set_reactive (CLUTTER_ACTOR (surface_actor), TRUE);
        return;
      }
  }

  toplevel_drag->grab_active = TRUE;

  /* Start the pointer grab to intercept events */
  toplevel_drag->pointer_grab.interface = &toplevel_drag_grab_interface;
  toplevel_drag->pointer_grab.pointer = seat->pointer;
  meta_wayland_pointer_start_grab (seat->pointer, &toplevel_drag->pointer_grab);

  meta_topic (META_DEBUG_EVENTS, "Window drag started successfully.");
}

static void
on_dragged_window_shown (MetaWindow              *window,
                         MetaWaylandToplevelDrag *toplevel_drag)
{
  g_assert (window->mapped);
  g_clear_signal_handler (&toplevel_drag->window_shown_handler_id, window);
  if (toplevel_drag->data_source && toplevel_drag->dragged_surface)
    start_window_drag (window, toplevel_drag, NULL);
}

static void
xdg_toplevel_drag_attach (struct wl_client   *client,
                          struct wl_resource *resource,
                          struct wl_resource *toplevel,
                          int32_t             x_offset,
                          int32_t             y_offset)
{
  MetaWaylandSurface *dragged_surface;
  MetaWindow *dragged_window;
  float screen_x, screen_y;
  MetaWaylandToplevelDrag *toplevel_drag = wl_resource_get_user_data (resource);

  /* Toplevel drag becomes inert if the associated data source is destroyed */
  if (!toplevel_drag->data_source)
    return;

  dragged_surface = surface_from_xdg_toplevel_resource (toplevel);
  dragged_window = meta_wayland_surface_get_window (dragged_surface);
  g_return_if_fail (dragged_window != NULL);

  if (toplevel_drag->dragged_surface != NULL)
    {
      wl_resource_post_error (
        resource, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED,
        "toplevel drag already has a surface attached");
      return;
    }

  meta_topic (META_DEBUG_EVENTS,
              "Attaching xdg_toplevel#%u to xdg_toplevel_drag#%u "
              "data_source#%p window=%s drag_offset=(%d, %d)",
              wl_resource_get_id (toplevel), wl_resource_get_id (resource),
              toplevel_drag->data_source, dragged_window->desc, x_offset, y_offset);

  toplevel_drag->dragged_surface = dragged_surface;
  toplevel_drag->x_offset = x_offset;
  toplevel_drag->y_offset = y_offset;
  toplevel_drag->window_unmanaging_handler_id =
    g_signal_connect (dragged_window,
                      "unmanaging",
                      G_CALLBACK (on_dragged_window_unmanaging),
                      toplevel_drag);

  if (dragged_window->mapped)
    {
      /* {x,y}_offset values are relative to the toplevel geometry. */
      add_window_geometry_origin (dragged_surface, &x_offset, &y_offset);
      meta_wayland_surface_get_absolute_coordinates (dragged_surface,
                                                     (float) x_offset,
                                                     (float) y_offset,
                                                     &screen_x, &screen_y);
      start_window_drag (dragged_window, toplevel_drag,
                         &GRAPHENE_POINT_INIT (screen_x, screen_y));
    }
  else
    {
      meta_topic (META_DEBUG_EVENTS, "Window not mapped yet, monitoring.");
      toplevel_drag->window_shown_handler_id =
        g_signal_connect (dragged_window,
                          "shown",
                          G_CALLBACK (on_dragged_window_shown),
                          toplevel_drag);
    }
}

static const struct xdg_toplevel_drag_v1_interface meta_wayland_toplevel_drag_interface = {
  .destroy = xdg_toplevel_drag_destroy,
  .attach = xdg_toplevel_drag_attach,
};

static void
xdg_toplevel_drag_manager_destroy (struct wl_client   *client,
                                   struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
xdg_toplevel_drag_manager_get_toplevel_drag (struct wl_client   *client,
                                             struct wl_resource *resource,
                                             uint32_t            toplevel_drag_id,
                                             struct wl_resource *data_source_resource)
{
  MetaWaylandDataSource *data_source;
  MetaWaylandToplevelDrag *toplevel_drag;
  struct wl_resource *toplevel_drag_resource;

  data_source = wl_resource_get_user_data (data_source_resource);
  toplevel_drag = meta_wayland_data_source_get_toplevel_drag (data_source);

  if (toplevel_drag)
    {
      wl_resource_post_error (
        resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
        "toplevel drag resource already exists on data source");
      return;
    }

  toplevel_drag_resource = wl_resource_create (client,
                                               &xdg_toplevel_drag_v1_interface,
                                               wl_resource_get_version (resource),
                                               toplevel_drag_id);

  toplevel_drag = g_new0 (MetaWaylandToplevelDrag, 1);
  toplevel_drag->resource = toplevel_drag_resource;
  toplevel_drag->data_source = data_source;
  toplevel_drag->source_destroyed_handler_id =
    g_signal_connect (data_source,
                      "destroy",
                      G_CALLBACK (on_data_source_destroyed),
                      toplevel_drag);
  meta_wayland_data_source_set_toplevel_drag (data_source,
                                              toplevel_drag);

  wl_resource_set_implementation (toplevel_drag_resource,
                                  &meta_wayland_toplevel_drag_interface,
                                  toplevel_drag,
                                  xdg_toplevel_drag_destructor);
}

static const struct xdg_toplevel_drag_manager_v1_interface meta_wayland_toplevel_drag_manager_interface = {
  .destroy = xdg_toplevel_drag_manager_destroy,
  .get_xdg_toplevel_drag = xdg_toplevel_drag_manager_get_toplevel_drag,
};

static void
xdg_toplevel_drag_bind (struct wl_client *client,
                        void             *data,
                        uint32_t          version,
                        uint32_t          id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
                                 &xdg_toplevel_drag_manager_v1_interface,
                                 version,
                                 id);
  wl_resource_set_implementation (resource,
                                  &meta_wayland_toplevel_drag_manager_interface,
                                  data,
                                  NULL);
}

void
meta_wayland_init_toplevel_drag (MetaWaylandCompositor *compositor)
{
  if (wl_global_create (compositor->wayland_display,
                        &xdg_toplevel_drag_manager_v1_interface,
                        META_XDG_TOPLEVEL_DRAG_V1_VERSION,
                        compositor,
                        xdg_toplevel_drag_bind) == NULL)
    g_error ("Failed to register a global xdg_toplevel_drag object");
}

gboolean
meta_wayland_toplevel_drag_calc_origin_for_dragged_window (MetaWaylandToplevelDrag *toplevel_drag,
                                                           MetaRectangle           *bounds_out)
{
  MetaWaylandSeat *seat;
  graphene_point_t coords;

  g_assert (toplevel_drag);
  g_assert (bounds_out);

  seat = meta_wayland_data_source_get_seat (toplevel_drag->data_source);
  if (!seat)
    return FALSE;

  /* Get current pointer position using ClutterInputDevice */
  clutter_input_device_get_coords (seat->pointer->device, NULL, &coords);

  meta_topic (META_DEBUG_EVENTS,
              "Calculated position for the dragged window. "
              "offset=(%d, %d) new_origin=(%.0f, %.0f)",
              toplevel_drag->x_offset, toplevel_drag->y_offset, coords.x, coords.y);
  bounds_out->x = (int) coords.x - toplevel_drag->x_offset;
  bounds_out->y = (int) coords.y - toplevel_drag->y_offset;
  return TRUE;
}

void
meta_wayland_toplevel_drag_end (MetaWaylandToplevelDrag *toplevel_drag)
{
  MetaWindow *window;
  MetaSurfaceActor *surface_actor;

  g_return_if_fail (toplevel_drag != NULL);
  meta_topic (META_DEBUG_EVENTS, "Ending toplevel drag.");

  /* End the grab op if active */
  if (toplevel_drag->grab_active)
    {
      toplevel_drag_end_grab_op (toplevel_drag);
    }

  if (toplevel_drag->dragged_surface)
    {
      surface_actor = meta_wayland_surface_get_actor (toplevel_drag->dragged_surface);
      if (surface_actor)
        clutter_actor_set_reactive (CLUTTER_ACTOR (surface_actor), TRUE);

      window = meta_wayland_surface_get_window (toplevel_drag->dragged_surface);
      if (window)
        {
          g_clear_signal_handler (&toplevel_drag->window_unmanaging_handler_id, window);
          g_clear_signal_handler (&toplevel_drag->window_shown_handler_id, window);
        }
      toplevel_drag->dragged_surface = NULL;
    }

  if (toplevel_drag->data_source)
    {
      g_clear_signal_handler (&toplevel_drag->source_destroyed_handler_id,
                              toplevel_drag->data_source);
      meta_wayland_data_source_set_toplevel_drag (toplevel_drag->data_source, NULL);
      toplevel_drag->data_source = NULL;
    }
}
