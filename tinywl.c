#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

// I added this for BTN_LEFT
#include <linux/input-event-codes.h>

// For TTY switching
#include <wlr/backend/session.h>

// for tap-to-click
#include <libinput.h>
#include <wlr/backend/libinput.h>

// For wlr-screencopy-unstable-v1 (screenshots)
#include <wlr/types/wlr_screencopy_v1.h>

// For zxdg_output_manager_v1
#include <wlr/types/wlr_xdg_output_v1.h>

// For xdg-activation-v1
#include <wlr/types/wlr_xdg_activation_v1.h>

// For wlr_idle_notifier_v1
#include <wlr/types/wlr_idle_notify_v1.h>

// For wlr_xdg_decoration_manager_v1
#include <wlr/types/wlr_xdg_decoration_v1.h>

/* For brevity's sake, struct members are annotated where they are used. */
enum tinywl_cursor_mode {
  TINYWL_CURSOR_PASSTHROUGH,
  TINYWL_CURSOR_MOVE,
  TINYWL_CURSOR_RESIZE,
};

enum tinywl_surface_type {
  TINYWL_SURFACE_TOPLEVEL,
  TINYWL_SURFACE_POPUP,
};

struct tinywl_server {
  struct wl_display *wl_display;
  struct wlr_backend *backend;
  struct wlr_session *session; // added for TTY switching
  struct wlr_renderer *renderer;

  struct wlr_xdg_shell *xdg_shell;
  struct wl_listener new_xdg_surface;
  struct wl_list toplevels;

  struct wlr_cursor *cursor;
  struct wlr_xcursor_manager *cursor_manager;
  struct wl_listener cursor_motion;
  struct wl_listener cursor_motion_absolute;
  struct wl_listener cursor_button;
  struct wl_listener cursor_axis;
  struct wl_listener cursor_frame;

  struct wlr_seat *seat;
  struct wl_listener new_input;
  struct wl_listener request_cursor;
  struct wl_listener request_set_selection;
  struct wl_list keyboards;
  enum tinywl_cursor_mode cursor_mode;
  uint32_t resize_edges;
  struct tinywl_toplevel *grabbed_toplevel;

  struct wlr_output_layout *output_layout;
  struct wl_list outputs;
  struct wl_listener new_output;

  struct wlr_allocator *allocator;
  struct wlr_scene *scene;
  struct wlr_scene_output_layout *scene_layout;

  // for DnD
  struct wlr_drag *current_drag;
  struct wlr_scene_tree *drag_icon_tree;
  bool drag_just_ended;
  struct wl_listener request_start_drag;
  struct wl_listener start_drag;
  struct wl_listener destroy_drag;

  // For xdg-activation-v1.xml
  struct wlr_xdg_activation_v1 *xdg_activation;
  struct wl_listener request_activation;

  // For wlr_idle_notifier_v1
  struct wlr_idle_notifier_v1 *idle_notifier;
  struct wl_listener idle_away;
  struct wl_listener idle_resume;

  // For wlr_xdg_decoration_manager_v1
  struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
  struct wl_listener new_toplevel_decoration;

  // For drag or resize
  double grabbed_cursor_x;
  double grabbed_cursor_y;

  // Up to two grabbed mouse buttons - to start and cancel an interaction
  uint32_t grabbed_active_button;
  uint32_t grabbed_cancel_button;
};

struct tinywl_output {
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_output *wlr_output;
  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;
};

struct tinywl_toplevel {
  enum tinywl_surface_type type;  // added
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *scene_tree; // added

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener request_move;
  struct wl_listener request_resize;

  // Added for moving windows, or resizing them throttled to the frame rate
  struct wlr_box initial_geom;
  int pending_width;
  int pending_height;
  bool resize_is_pending;

  // added listeners for maximize, fullscreen, asynchronous resize
  struct wl_listener request_maximize;
  struct wl_listener request_fullscreen;
  struct wl_listener commit;

  // Box to save properties before going fullscreen
  struct wlr_box saved_geometry;

  // Track state
  bool is_maximized;
  bool is_fullscreen;
};

struct tinywl_keyboard {
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_keyboard *wlr_keyboard;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;

  uint32_t grabbed_keycode; // keypress not told to the client
};

// For xdg_popup
struct tinywl_popup {
  enum tinywl_surface_type type;
  struct wlr_xdg_popup *xdg_popup;
  struct wlr_scene_tree *scene_tree;
  struct wl_listener commit;
  struct wl_listener destroy;
};

// spawn a shell process
static void spawn(const char *cmd) {
  // A standard doule-fork trick makes systemd deal with cleanup.
  // The grandchild becomes an orphan, so gets adopted by systemd.
  if (fork() == 0) {
    if (fork() == 0) {
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      wlr_log(WLR_ERROR, "spawn: execl failed: %s", strerror(errno));
      _exit(1); // if execl fails for some reason (rare)
    }
    _exit(0);
  }
}

// For wlr_idle_notifier_v1
static void handle_idle_away(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, idle_away);
  
  // Loop through all connected monitors/outputs and turn them off
  struct tinywl_output *output;
  wl_list_for_each(output, &server->outputs, link) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, false); // Disable display output
    wlr_output_commit_state(output->wlr_output, &state);
    wlr_output_state_finish(&state);
  }
}

// For wlr_idle_notifier_v1
static void handle_idle_resume(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, idle_resume);
   
  // The user moved the mouse! Turn all monitors back on
  struct tinywl_output *output;
  wl_list_for_each(output, &server->outputs, link) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true); // Re-enable display output
    wlr_output_commit_state(output->wlr_output, &state);
    wlr_output_state_finish(&state);
    
    // Schedule a frame redraw immediately so the screen isn't blank
    wlr_output_schedule_frame(output->wlr_output);
  }
}

// Return the toplevel responsible for a given pixel.
// Also set surface and relative coordinates.
static struct tinywl_toplevel *desktop_toplevel_at(struct tinywl_server *server,
    double lx, double ly, struct wlr_surface **surface, double *sx, double *sy) {
  struct wlr_scene_node *node =
      wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
  if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
    return NULL;
  }
  struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
  struct wlr_scene_surface *scene_surface =
      wlr_scene_surface_try_from_buffer(scene_buffer);
  if (!scene_surface) {
    return NULL;
  }

  *surface = scene_surface->surface;
  
  // Climb the tree to find our custom tinywl_toplevel data pointer
  struct wlr_scene_tree *tree = node->parent;
  while (tree != NULL) {
    if (tree->node.data != NULL) {
      // Get the type. This is safe since it's the first member of both structs
      enum tinywl_surface_type *surface_type = tree->node.data;
      if (*surface_type == TINYWL_SURFACE_TOPLEVEL) {
        return tree->node.data;
      }
    }
    tree = tree->node.parent;
  }
  
  return NULL;
}

// Reset the cursor mode to passthrough.
static void end_interactive(struct tinywl_server *server) {
  if (server->cursor_mode == TINYWL_CURSOR_PASSTHROUGH) {
    return;
  }

  // Clear out the server tracking state fields
  server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
  server->grabbed_toplevel = NULL;
  
  // Find what sits directly under the mouse pointer
  double sx, sy;
  struct wlr_surface *surface = NULL;
  struct tinywl_toplevel *hover_toplevel = desktop_toplevel_at(
      server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

  if (!hover_toplevel) {
    // Over completely empty space, restore default cursor arrow graphics
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
  }

  if (surface) {
    // Restore pointer focus immediately
    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
  } else {
    // Clean slate if dropping the window layout structure over empty space
    wlr_seat_pointer_clear_focus(server->seat);
  }
}

// Set up an interactive move/resize, where the compositor grabs mouse events
static void begin_interactive(struct tinywl_toplevel *toplevel,
                              enum tinywl_cursor_mode mode, uint32_t edges) {
  struct tinywl_server *server = toplevel->server;
  struct wlr_surface *focused_surface =
      server->seat->pointer_state.focused_surface;

  // Sanity check
  if (toplevel->xdg_toplevel->base->surface !=
      wlr_surface_get_root_surface(focused_surface)) {
    wlr_log(WLR_ERROR, "Trying to interact with a toplevel and unrelated surface");
    return;
  }

  // fullscreen
  if (toplevel->is_fullscreen) {
    return;
  }

  // Unmaximize. Maximized windows do not remember their previous size.
  if (toplevel->is_maximized) {
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
    toplevel->is_maximized = false;
  }

  server->grabbed_toplevel = toplevel;
  server->cursor_mode = mode;
  server->resize_edges = edges;

  struct wlr_box geom;
  wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geom);

  // save the initial state of the window and cursor
  toplevel->initial_geom.x = toplevel->scene_tree->node.x;
  toplevel->initial_geom.y = toplevel->scene_tree->node.y;
  toplevel->initial_geom.width = geom.width;
  toplevel->initial_geom.height = geom.height;
  server->grabbed_cursor_x = server->cursor->x;
  server->grabbed_cursor_y = server->cursor->y;
}


// Move the grabbed toplevel
static void handle_interactive_move(struct tinywl_server *server, uint32_t time) {
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
  if (toplevel == NULL) {
    wlr_log(WLR_ERROR, "Trying to drag a NULL.");
    return;
  }

  if (wl_list_empty(&toplevel->link)) {
    wlr_log(WLR_INFO, "Grabbed window must have been destroyed mid-move.");
    end_interactive(server);
    return;
  }

  if (!toplevel->scene_tree) {
    wlr_log(WLR_ERROR, "Trying to drag a NULL scene_tree.");
    return;
  }

  // Coordinate math
  int x = toplevel->initial_geom.x;
  int y = toplevel->initial_geom.y;

  int dx = server->cursor->x - server->grabbed_cursor_x;
  int dy = server->cursor->y - server->grabbed_cursor_y;

  x += dx;
  y += dy;

  // Re-position the window in your compositor layout tree
  wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
}

// Resize the grabbed toplevel at any corner or edge.
static void handle_interactive_resize(struct tinywl_server *server, uint32_t time) {
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;

  wlr_seat_pointer_notify_motion(server->seat, time, server->cursor->x, server->cursor->y);

  // Sanity checks
  if (toplevel == NULL) {
    wlr_log(WLR_ERROR, "Trying to resize a NULL.");
    return;
  }
  if (wl_list_empty(&toplevel->link)) {
    wlr_log(WLR_DEBUG, "Grabbed window must have been destroyed mid-resize.");
    end_interactive(server);
    return;
  }
  if (!toplevel->scene_tree) {
    wlr_log(WLR_ERROR, "Trying to resize a NULL scene_tree.");
    return;
  }

  // Mouse movement since the drag started
  int dx = server->cursor->x - server->grabbed_cursor_x;
  int dy = server->cursor->y - server->grabbed_cursor_y;

  int width = toplevel->initial_geom.width;
  int height = toplevel->initial_geom.height;

  if (server->resize_edges & WLR_EDGE_TOP) {
    height -= dy;
  } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
    height += dy;
  }
  if (server->resize_edges & WLR_EDGE_LEFT) {
    width -= dx;
  } else if (server->resize_edges & WLR_EDGE_RIGHT) {
    width += dx;
  }
  
  // Clamp
  int min_width = toplevel->xdg_toplevel->current.min_width;
  int min_height = toplevel->xdg_toplevel->current.min_height;
  if (height < min_height) {
    height = min_height;
  }
  if (width < min_width) {
    width = min_width;
  }

  toplevel->pending_width = width;
  toplevel->pending_height = height;
  toplevel->resize_is_pending = true;

  // I'm throttling client resize requests to the frame rate.
  // This is a hack to make sure frames occasionally happen when running nested.
  // It could be made more subtle in the case of multiple monitors.
  struct tinywl_output *output;
  wl_list_for_each(output, &server->outputs, link) {
    wlr_output_schedule_frame(output->wlr_output);
  }
}

static void handle_cursor_motion(struct tinywl_server *server, uint32_t time) {
  // If the mode is non-passthrough, delegate to those functions.
  switch (server->cursor_mode) {
    case TINYWL_CURSOR_MOVE:
      handle_interactive_move(server, time);
      return;
    case TINYWL_CURSOR_RESIZE:
      handle_interactive_resize(server, time);
      return;
    default:
      break;
  }

  // Find the exact scene graph node under the cursor
  double sx, sy;
  struct wlr_seat *seat = server->seat;
  struct wlr_surface *surface = NULL;

  // Use the scene graph root directly to see absolutely everything rendered
  struct wlr_scene_node *node = wlr_scene_node_at(
      &server->scene->tree.node, server->cursor->x, server->cursor->y, &sx, &sy);

  if (node && node->type == WLR_SCENE_NODE_BUFFER) {
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    
    if (scene_surface) {
      surface = scene_surface->surface;
    }
  }

  if (!surface) {
    // Over empty space, set the cursor image to a default arrow
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
    wlr_seat_pointer_clear_focus(seat);
  } else {
    // wlroots handles duplicate protection internally.
    // sx and sy are already transformed into the local surface coordinate space.
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
  }
}


// Unfocus
static void unfocus_toplevel(struct tinywl_server *server) {
  struct wlr_seat *seat = server->seat;
  struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

  // Clear grabbed keys
  struct tinywl_keyboard *kbd;
  wl_list_for_each(kbd, &server->keyboards, link) {
    kbd->grabbed_keycode = 0;
  }

  // Visually deactivate the old window if one was focused
  if (prev_surface) {
    struct wlr_xdg_toplevel *prev_toplevel =
        wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
    if (prev_toplevel != NULL) {
      wlr_xdg_toplevel_set_activated(prev_toplevel, false);
    }
  }

  // Notify the seat and underlying wlroots architecture
  wlr_seat_keyboard_notify_clear_focus(seat);
}

// Raise in our tracker list and the scene tree
static void raise_toplevel(struct tinywl_toplevel *toplevel) {
  struct tinywl_server *server = toplevel->server;
  if (wl_list_empty(&toplevel->link) ||
      toplevel->link.next == NULL ||
      toplevel->link.prev == NULL) {
    return;
  }

  // Raise it in our tracker list
  wl_list_remove(&toplevel->link);
  wl_list_insert(&server->toplevels, &toplevel->link);

  // Raise it in the scene tree
  if (toplevel->scene_tree) {
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
  }
}

// Unfocus the old stuff, move the new window to the top, and notify it
static void focus_toplevel(struct tinywl_toplevel *toplevel,
                           struct wlr_surface *surface) {
  if (toplevel == NULL || surface == NULL) {
    wlr_log(WLR_ERROR, "Trying to focus NULL");
    return;
  }
  
  struct tinywl_server *server = toplevel->server;
  struct wlr_seat *seat = server->seat;
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
  struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

  if (prev_surface == surface) {
    return; // Already focused
  }

  // Clean slate: Unfocus the old window state first
  unfocus_toplevel(server);

  // Raise it
  raise_toplevel(toplevel);

  // Safety check
  if (toplevel->xdg_toplevel == NULL) {
    wlr_log(WLR_ERROR, "Attempted to focus an orphan!");
    unfocus_toplevel(server);
    return;
  }
      
  // For dialog boxes
  struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(surface);
  if (xdg_surface && xdg_surface->toplevel) {
    // If you clicked the dialog box, this activates the dialog box.
    // If you clicked the main window, this activates the main window.
    wlr_xdg_toplevel_set_activated(xdg_surface->toplevel, true);
  } else {
    // Fallback security measure
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
  }

  if (keyboard == NULL) {
    wlr_log(WLR_INFO, "Focusing with no keyboard.");
    wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
  } else {
    wlr_seat_keyboard_notify_enter(seat, surface,
                                   keyboard->keycodes,
                                   keyboard->num_keycodes,
                                   &keyboard->modifiers);
  }
}

// For DnD
static void update_drag_icon_position(struct tinywl_server *server) {
  if (!server->current_drag || !server->drag_icon_tree) {
    return;
  }

  // Explicitly cast coordinates to clean integers (matching pixels)
  int x = (int)server->cursor->x;
  int y = (int)server->cursor->y;

  struct wlr_scene_node *node = &server->drag_icon_tree->node;

  // If the drag icon tree node is at this layout coordinate, do nothing
  if (node->x == x && node->y == y) {
    return;
  }

  // Only update and re-render if the cursor actually jumped to a new pixel
  wlr_scene_node_set_position(node, x, y);
}

// Triggered by a relative pointer motion event
static void server_cursor_motion(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_motion);
  struct wlr_pointer_motion_event *event = data;
  wlr_cursor_move(server->cursor, &event->pointer->base,
                  event->delta_x, event->delta_y);
  handle_cursor_motion(server, event->time_msec);
  update_drag_icon_position(server); // for DnD
  wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
}

// Triggered by an absolute pointer motion event,
// eg. enter the window when tinywl is running nested in a window
static void server_cursor_motion_absolute(struct wl_listener *listener,
                                          void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_motion_absolute);
  struct wlr_pointer_motion_absolute_event *event = data;
  wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
                           event->x, event->y);
  handle_cursor_motion(server, event->time_msec);
  wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
}

static bool is_popup_at(struct tinywl_server *server, double lx, double ly) {
  double sx, sy;
  // Ask the scene graph what node is at the cursor coordinates 
  struct wlr_scene_node *node =
    wlr_scene_node_at(&server->scene->tree.node, lx, ly, &sx, &sy);
    
  if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
    return false;
  }

  // Climb the scene tree to check if this buffer belongs to a popup 
  struct wlr_scene_tree *tree = node->parent;
  while (tree != NULL) {
    if (tree->node.data != NULL) {
      enum tinywl_surface_type *surface_type = tree->node.data;
      if (*surface_type == TINYWL_SURFACE_POPUP) {
        return true;
      }
      if (*surface_type == TINYWL_SURFACE_TOPLEVEL) {
        return false;
      }
    }
    tree = tree->node.parent;
  }

  return false;
}

    
// Triggered by a mouse click or release
static void server_cursor_button(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, cursor_button);
  struct wlr_pointer_button_event *event = data;
  uint32_t modifiers =
      wlr_keyboard_get_modifiers(wlr_seat_get_keyboard(server->seat));
  double sx, sy;
  struct wlr_surface *surface = NULL;
  struct tinywl_toplevel *toplevel = desktop_toplevel_at(
      server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

  // release the button that started a mod+click interaction
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) && // redundant
      (event->button == server->grabbed_active_button)) {
    // note it must be button release
    server->grabbed_active_button = 0;
    if (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH) {
      end_interactive(server);
    }
    return;
  }
    
  // release the button that canceled a mod+click interaction
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) && // redundant
      (event->button == server->grabbed_cancel_button)) {
    server->grabbed_cancel_button = 0;
    return;
  }

  // a second press that cancels a mod+click interaction
  // TO DO: maybe restore the pre-interaction geometry?
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) &&
      (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH)) {
    server->grabbed_cancel_button = event->button;
    end_interactive(server);
    return;
  }

  // LOGO + Left Click (Interactive Window Move)
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) &&
      (modifiers & WLR_MODIFIER_LOGO) &&
      (event->button == BTN_LEFT) && toplevel) {
    server->grabbed_active_button = event->button;
    focus_toplevel(toplevel, surface);
    begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
    return;
  }

  // LOGO + Right Click (Interactive Window Resize)
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) &&
      (modifiers & WLR_MODIFIER_LOGO) &&
      (event->button == BTN_RIGHT) && toplevel) {
    server->grabbed_active_button = event->button;
    focus_toplevel(toplevel, surface);
    begin_interactive(toplevel, TINYWL_CURSOR_RESIZE,
                      WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
    return;
  }

  // NOTIFY THE SEAT IN EVERY REMAINING CASE

  // Any release that was not grabbed
  if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    end_interactive(server);
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    wlr_seat_pointer_notify_frame(server->seat);
    return;
  }

  // button press on a popup - I think it must already have focus, so do nothing else
  if (is_popup_at(server, server->cursor->x, server->cursor->y)) {
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    wlr_seat_pointer_notify_frame(server->seat);
    return;
  }

  // It is a normal click that is not on a popup

  // Dismiss all popups
  struct tinywl_toplevel *top;
  wl_list_for_each(top, &server->toplevels, link) {
    struct wl_list *pops = &top->xdg_toplevel->base->popups;
    while (!wl_list_empty(pops)) {
      struct wlr_xdg_popup *pop =
          wl_container_of(pops->next, pop, link);
      wlr_xdg_popup_destroy(pop);
    }
    wlr_log(WLR_DEBUG, "Popups dismissed.");
  }

  // Set keyboard focus
  if (toplevel == NULL) {
    unfocus_toplevel(server);
  } else {
    focus_toplevel(toplevel, surface);
  }

  // Notify
  wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                 event->button, event->state);
  wlr_seat_pointer_notify_frame(server->seat);
} 

// Function triggered by a pointer axis event, eg. scroll wheel.
static void server_cursor_axis(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, cursor_axis);
  struct wlr_pointer_axis_event *event = data;
  wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
                               event->orientation, event->delta,
                               event->delta_discrete, event->source);
}

// Function triggered by a pointer frame event, grouping multiple events.
static void server_cursor_frame(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_frame);
  wlr_seat_pointer_notify_frame(server->seat);
}

// Function called every time an output is ready to display a frame, eg 60Hz
static void output_frame(struct wl_listener *listener, void *data) {
  struct tinywl_output *output = wl_container_of(listener, output, frame);
  struct tinywl_server *server = output->server;
  struct wlr_scene *scene = server->scene;

  // Send resizing requests here, so they "only" get sent at the frame rate
  if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
    struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
    if (toplevel->resize_is_pending) {
      wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->pending_width,
                                toplevel->pending_height);
      wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
      toplevel->resize_is_pending = false;
    }
  }

  struct wlr_scene_output *scene_output =
      wlr_scene_get_scene_output(scene, output->wlr_output);

  /* Render the scene if needed and commit the output */
  wlr_scene_output_commit(scene_output, NULL);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(scene_output, &now);
}

// Function called when the backend requests a new state for the output,
// eg. when the output window is resized.
static void output_request_state(struct wl_listener *listener, void *data) {
  struct tinywl_output *output =
      wl_container_of(listener, output, request_state);
  const struct wlr_output_event_request_state *event = data;
  wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
  struct tinywl_output *output = wl_container_of(listener, output, destroy);

  wl_list_remove(&output->frame.link);
  wl_list_init(&output->frame.link);
  wl_list_remove(&output->request_state.link);
  wl_list_init(&output->request_state.link);
  wl_list_remove(&output->destroy.link);
  wl_list_init(&output->destroy.link);
  wl_list_remove(&output->link);
  wl_list_init(&output->link);
  free(output);
}

// Function called by the backend when a new output becomes available
static void server_new_output(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, new_output);
  struct wlr_output *wlr_output = data;

  wlr_output_init_render(wlr_output, server->allocator, server->renderer);

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);

  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode != NULL) {
    wlr_output_state_set_mode(&state, mode);
  }

  /* Automically applies the new output state. */
  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  /* Allocates and configures our state for this output */
  struct tinywl_output *output = calloc(1, sizeof(*output));
  output->wlr_output = wlr_output;
  output->server = server;

  /* Sets up a listener for the frame event. */
  output->frame.notify = output_frame;
  wl_signal_add(&wlr_output->events.frame, &output->frame);

  /* Sets up a listener for the state request event. */
  output->request_state.notify = output_request_state;
  wl_signal_add(&wlr_output->events.request_state, &output->request_state);

  /* Sets up a listener for the destroy event. */
  output->destroy.notify = output_destroy;
  wl_signal_add(&wlr_output->events.destroy, &output->destroy);

  wl_list_insert(&server->outputs, &output->link);

  // Adds this to the output layout.
  // Simple, so use the default: arrange outputs left to right.
  struct wlr_output_layout_output *l_output =
      wlr_output_layout_add_auto(server->output_layout, wlr_output);
  struct wlr_scene_output *scene_output =
      wlr_scene_output_create(server->scene, wlr_output);

  // Background color
  float background_color[4] = {0.02f, 0.20f, 0.20f, 1.0f};
  wlr_scene_rect_create(&server->scene->tree, 
                        wlr_output->width, 
                        wlr_output->height, 
                        background_color);

  wlr_scene_output_layout_add_output(server->scene_layout, l_output,
                                     scene_output);
}

// Function called when the surface is mapped, ready for display.
static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, map);
  struct tinywl_server *server = toplevel->server;

  // Intercept placement due to a finished drag, added for DnD and tearoff tabs
  if (server->drag_just_ended) {
    server->drag_just_ended = false;
    int spawn_x = (int)server->cursor->x;
    int spawn_y = (int)server->cursor->y;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, spawn_x, spawn_y);
  }

  wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
  focus_toplevel(toplevel, toplevel->xdg_toplevel->base->surface);
}

// Function called when the surface is unmapped (visually hidden/closed).
static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
  struct tinywl_server *server = toplevel->server;

  // Clear cursor grab if this window was being manipulated
  if (toplevel == server->grabbed_toplevel) {
    end_interactive(server);
  }

  // Check if this unmapping window currently has keyboard focus
  struct wlr_surface *focused_surface = server->seat->keyboard_state.focused_surface;
  bool was_focused = false;
  if (focused_surface) {
    was_focused = (toplevel->xdg_toplevel->base->surface ==
                  wlr_surface_get_root_surface(focused_surface));
  }

  // Remove it from the compositor's window list
  wl_list_remove(&toplevel->link);
  wl_list_init(&toplevel->link);

  // Evacuate focus safely BEFORE the window data structures are destroyed
  if (was_focused) {
    if (wl_list_empty(&server->toplevels)) {
      unfocus_toplevel(server);
    } else {
      struct tinywl_toplevel *next_toplevel = wl_container_of(
          server->toplevels.next, next_toplevel, link);
      focus_toplevel(next_toplevel, next_toplevel->xdg_toplevel->base->surface);
    }
  }
}

// Function called when a toplevel is destroyed.
static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
  (void)data;
  struct tinywl_toplevel *toplevel = wl_container_of(listener,
                                                     toplevel, destroy);
  struct tinywl_server *server = toplevel->server;
  bool was_focused = (server->seat->keyboard_state.focused_surface ==
                      toplevel->xdg_toplevel->base->surface);

  // If this view was being moved or resized, reset the cursor mode.
  if (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH &&
      server->grabbed_toplevel == toplevel) {
    server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
  }

  wl_list_remove(&toplevel->map.link);
  wl_list_init(&toplevel->map.link);
  wl_list_remove(&toplevel->unmap.link);
  wl_list_init(&toplevel->unmap.link);
  wl_list_remove(&toplevel->destroy.link);
  wl_list_init(&toplevel->destroy.link);
  wl_list_remove(&toplevel->request_move.link);
  wl_list_init(&toplevel->request_move.link);
  wl_list_remove(&toplevel->request_resize.link);
  wl_list_init(&toplevel->request_resize.link);

  // added for fullscreen, maximize, commit
  wl_list_remove(&toplevel->request_maximize.link);
  wl_list_init(&toplevel->request_maximize.link);
  wl_list_remove(&toplevel->request_fullscreen.link);
  wl_list_init(&toplevel->request_fullscreen.link);
  wl_list_remove(&toplevel->commit.link);
  wl_list_init(&toplevel->commit.link);

  // If the closed window was focused, find a replacement!
  if (was_focused) {
    if (wl_list_empty(&server->toplevels)) {
      wlr_log(WLR_DEBUG, "No more windows, so unfocus.");
      unfocus_toplevel(server);
    } else {
      struct tinywl_toplevel *next_toplevel = wl_container_of(
          server->toplevels.next, next_toplevel, link);
      focus_toplevel(next_toplevel, next_toplevel->xdg_toplevel->base->surface);
    }
  }
  free(toplevel);
}

// Function triggered when a client requests to begin an interactive move.
// TO DO: could check it's a reasonable request.
static void xdg_toplevel_request_move(struct wl_listener *listener,
                                      void *data) {
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_move);
  begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
}

// Function triggered when a client requests to begin an interactive resize.
// TO DO: could check it's a reasonable request.
static void xdg_toplevel_request_resize(struct wl_listener *listener,
                                        void *data) {
  struct wlr_xdg_toplevel_resize_event *event = data;
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_resize);
  begin_interactive(toplevel, TINYWL_CURSOR_RESIZE, event->edges);
}

// Toggle fullscreen on client request or a quick-key.
static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
                                            void *data) {
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
  struct tinywl_server *server = toplevel->server;

  // ENTER fullscreen
  if (toplevel->xdg_toplevel->requested.fullscreen) {
    if (toplevel->is_fullscreen) {
      return;
    }

    // Save state
    toplevel->saved_geometry.x = toplevel->scene_tree->node.x;
    toplevel->saved_geometry.y = toplevel->scene_tree->node.y;
    toplevel->saved_geometry.width =
        toplevel->xdg_toplevel->base->current.geometry.width;
    toplevel->saved_geometry.height =
        toplevel->xdg_toplevel->base->current.geometry.height;

    // Query the monitor size
    struct wlr_output *output = wlr_output_layout_output_at(
        server->output_layout, server->cursor->x, server->cursor->y);
    if (!output) {
      return;
    }

    // Find where this specific monitor starts in the virtual grid layout
    struct wlr_output_layout_output *layout_output = 
        wlr_output_layout_get(server->output_layout, output);
        
    // Position it at the screen's top-left origin, and span its pixel size
    wlr_scene_node_set_position(&toplevel->scene_tree->node,
                                layout_output->x, layout_output->y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                                output->width, output->height);
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);

    // Mark the window status and tell the client it is now fullscreen
    toplevel->is_fullscreen = true;
    wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
  }
  // LEAVE fullscreen
  else {
    if (!toplevel->is_fullscreen) return;

    // Put the window node back at its original coordinates and size
    wlr_scene_node_set_position(&toplevel->scene_tree->node, 
                                toplevel->saved_geometry.x, 
                                toplevel->saved_geometry.y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 
                              toplevel->saved_geometry.width, 
                              toplevel->saved_geometry.height);
    // Clear the tracking flags
    toplevel->is_fullscreen = false;
    wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
  }
}

// Maximize. Assume there are no status bars.
// Do not save the previous size.
static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
  struct tinywl_server *server = toplevel->server;

  // Look up the monitor under the cursor
  struct wlr_output *output = wlr_output_layout_output_at(
      server->output_layout, server->cursor->x, server->cursor->y);
  if (!output) {
    return;
  }

  // find this monitor in the virtual grid
  struct wlr_output_layout_output *layout_output =
    wlr_output_layout_get(server->output_layout, output);
  if (!layout_output) {
    return;
  }

  // Snap the window to the top-left corner and set it to full screen size
  wlr_scene_node_set_position(&toplevel->scene_tree->node,
                              layout_output->x, layout_output->y);
  wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                            output->width, output->height);
}

// Called every time the client updates its surface buffer.
// Adjust the boundaries of the scene graph node so the frame can be drawn.
static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
  struct tinywl_server *server = toplevel->server;
  struct wlr_xdg_surface *xdg_surface = toplevel->xdg_toplevel->base;

  // Do nothing on an initial commit
  if (xdg_surface->initial_commit) {
    return;
  }

  // Extract the actual geometry the application just committed

  // Do nothing UNLESS we are actively resizing from top/left edges
  if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
    struct wlr_box geom;
    wlr_xdg_surface_get_geometry(xdg_surface, &geom);

    if (server->resize_edges & (WLR_EDGE_LEFT | WLR_EDGE_TOP)) {
      int32_t x = toplevel->initial_geom.x;
      int32_t y = toplevel->initial_geom.y;

      if (server->resize_edges & WLR_EDGE_LEFT) {
        x += toplevel->initial_geom.width - geom.width;
      }
      if (server->resize_edges & WLR_EDGE_TOP) {
        y += toplevel->initial_geom.height - geom.height;
      }

      // Reposition the window node on screen to keep the anchor point pinned
      wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
    }
  }
}

static void handle_new_toplevel(struct tinywl_server *server,
                                struct wlr_xdg_surface *xdg_surface) {
  struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
  toplevel->server = server;
  toplevel->xdg_toplevel = xdg_surface->toplevel;
  toplevel->type = TINYWL_SURFACE_TOPLEVEL;

  // Attach to the scene graph
  toplevel->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree,
                                                       xdg_surface);
  toplevel->scene_tree->node.data = toplevel;
  xdg_surface->data = toplevel->scene_tree;

  // Set up listeners
  // Core Surface Layer Events (map, unmap, commit)
  toplevel->map.notify = xdg_toplevel_map;
  wl_signal_add(&xdg_surface->surface->events.map, &toplevel->map);

  toplevel->unmap.notify = xdg_toplevel_unmap;
  wl_signal_add(&xdg_surface->surface->events.unmap, &toplevel->unmap);

  toplevel->commit.notify = xdg_toplevel_commit;
  wl_signal_add(&xdg_surface->surface->events.commit, &toplevel->commit);

  // Shell Management Layer Event (destroy)
  toplevel->destroy.notify = xdg_toplevel_destroy;
  wl_signal_add(&xdg_surface->events.destroy, &toplevel->destroy);

  // Window Type Interaction Events (move, resize, maximize, fullscreen)
  toplevel->request_move.notify = xdg_toplevel_request_move;
  wl_signal_add(&xdg_surface->toplevel->events.request_move, &toplevel->request_move);

  toplevel->request_resize.notify = xdg_toplevel_request_resize;
  wl_signal_add(&xdg_surface->toplevel->events.request_resize, &toplevel->request_resize);

  toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
  wl_signal_add(&xdg_surface->toplevel->events.request_maximize, &toplevel->request_maximize);

  toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
  wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void popup_handle_commit(struct wl_listener *listener, void *data) {
    // In wlroots 0.17, committing a popup surface requires scheduling a repaint
    struct tinywl_popup *popup = wl_container_of(listener, popup, commit);
    wlr_log(WLR_DEBUG, "popup handle commit.");
    
    if (popup->xdg_popup->base->initial_commit) {
        // First commit, nothing to damage yet
        return;
    }
    
    // Optional: If you implemented scene-graph or manual tracking, 
    // you would trigger a frame damage event here. 
    // For raw tinywl, the main output frame loop handles general rendering.
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
  wlr_log(WLR_DEBUG, "popup handle destroy.");
  struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);

  wl_list_remove(&popup->commit.link);
  wl_list_remove(&popup->destroy.link);
  
  free(popup);
}


static void handle_new_popup(struct tinywl_server *server, struct wlr_xdg_popup *xdg_popup) {
  wlr_log(WLR_DEBUG, "popup handle destroy.");
  struct tinywl_popup *popup = calloc(1, sizeof(struct tinywl_popup));
  popup->type = TINYWL_SURFACE_POPUP;
  if (!popup) {
    return;
  }

  popup->xdg_popup = xdg_popup;

  // Attach to the scene graph tree. wlroots 0.17 handles nested submenus 
  // automatically if we pass the primary scene tree root.
  popup->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree, xdg_popup->base);
  if (!popup->scene_tree) {
    free(popup);
    return;
  }

  popup->scene_tree->node.data = popup;

  popup->commit.notify = popup_handle_commit;
  wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

  popup->destroy.notify = popup_handle_destroy;
  wl_signal_add(&xdg_popup->base->events.destroy, &popup->destroy);
}

// For wlr_xdg_decoration_manager_v1
static void handle_new_toplevel_decoration(struct wl_listener *listener, void *data) {
  struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
  // Tell the client to draw its own window decorations
  wlr_xdg_toplevel_decoration_v1_set_mode(decoration, 
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

// Event raised when client sends a new xdg surface to wlr_xdg_shell
static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, new_xdg_surface);
  struct wlr_xdg_surface *xdg_surface = data;

  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    handle_new_toplevel(server, xdg_surface); 
  } else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
    handle_new_popup(server, xdg_surface->popup);
  } else {
    wlr_log(WLR_ERROR, "new surface with no ROLE.");
  }
}

// For xdg-activation-v1.xml - when you click on a link to open a browser
static void server_request_activation(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = 
      wl_container_of(listener, server, request_activation);
  struct wlr_xdg_activation_v1_request_activate_event *event = data;

  // Verify the surface exists and is an XDG surface
  if (!event->surface || !wlr_xdg_surface_try_from_wlr_surface(event->surface)) {
    return;
  }

  struct wlr_xdg_surface *xdg_surface = 
      wlr_xdg_surface_try_from_wlr_surface(event->surface);

  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg_surface->data) {
    struct wlr_scene_tree *scene_tree = xdg_surface->data;
    struct tinywl_toplevel *toplevel = scene_tree->node.data;
  
    if (toplevel) {
      // Focus the window that the link is trying to open
      focus_toplevel(toplevel, event->surface); 
    }
  }
}

static void server_handle_request_start_drag(struct wl_listener *listener,
                                             void *data) {
  struct wlr_seat_request_start_drag_event *event = data;
  struct tinywl_server *server =
      wl_container_of(listener, server, request_start_drag);

  // Validate the drag request (check if the serial matches an implicit mouse
  // grab)
  if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin,
                                            event->serial)) {
    wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
  } else {
    // Reject the drag if the serial is invalid or outdated
    wlr_data_source_destroy(event->drag->source);
  }
}

// For DnD
static void handle_destroy_drag(struct wl_listener *listener, void *data) {
    struct tinywl_server *server = wl_container_of(listener, server, destroy_drag);

    // Completely destroy the scene tree (removes it from screen graphics)
    if (server->drag_icon_tree) {
        wlr_scene_node_destroy(&server->drag_icon_tree->node);
        server->drag_icon_tree = NULL;
    }

    server->drag_just_ended = true; // for tag tearing

    server->current_drag = NULL;
    wl_list_remove(&server->destroy_drag.link);
    wl_list_init(&server->destroy_drag.link);
}

// For DnD
static void server_handle_start_drag(struct wl_listener *listener, void *data) {
    struct tinywl_server *server = wl_container_of(listener, server, start_drag);
    struct wlr_drag *drag = data;

    server->current_drag = drag;

    server->drag_just_ended = false; // for tag tearing

    // Create a new scene node tree for the drag icon attached to the root scene
    server->drag_icon_tree = wlr_scene_tree_create(&server->scene->tree);

    if (drag->icon) {
        // Automatically link the drag icon's surface to our scene tree
        wlr_scene_drag_icon_create(server->drag_icon_tree, drag->icon);
    }

    // Set up the destroy listener to clean it all up when done
    server->destroy_drag.notify = handle_destroy_drag;
    wl_signal_add(&drag->events.destroy, &server->destroy_drag);
    
    // Instantly position the icon under the mouse right now
    update_drag_icon_position(server);
}

// Function triggered when a modifier key is pressed.
static void keyboard_handle_modifiers(struct wl_listener *listener,
                                      void *data) {
  struct tinywl_keyboard *keyboard =
      wl_container_of(listener, keyboard, modifiers);
  wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
  wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                     &keyboard->wlr_keyboard->modifiers);
}

static void close_focused_toplevel(struct tinywl_server *server) {
  // Exit if there are no windows
  if (wl_list_empty(&server->toplevels)) {
    return;
  }

  // Grab the first structure out of the list
  struct tinywl_toplevel *toplevel = wl_container_of(
      server->toplevels.next, toplevel, link);
  if (!toplevel) {
    return;
  }

  wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

static void toggle_focused_fullscreen(struct tinywl_server *server) {
  // Exit if there are no windows
  if (wl_list_empty(&server->toplevels)) {
    return;
  }

  // Grab the first structure container out of the list
  struct tinywl_toplevel *toplevel = wl_container_of(
    server->toplevels.next, toplevel, link);
  if (!toplevel) {
    return;
  }

  // Toggle fullscreen
  toplevel->xdg_toplevel->requested.fullscreen = !toplevel->is_fullscreen;

  xdg_toplevel_request_fullscreen(&toplevel->request_fullscreen, NULL);
}

static bool handle_media_key(uint32_t sym) {
  switch (sym) {
    case XKB_KEY_XF86AudioMute:
      spawn("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
      return true;
    case XKB_KEY_XF86AudioLowerVolume:
      spawn("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-");
      return true;
    case XKB_KEY_XF86AudioRaiseVolume:
      spawn("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+ -l 1.0");
      return true;
    case XKB_KEY_XF86AudioMicMute:
      spawn("amixer set Capture toggle");
      return true;
    case XKB_KEY_XF86MonBrightnessDown:
      spawn("brightnessctl set 1%-");
      return true;
    case XKB_KEY_XF86MonBrightnessUp:
      spawn("brightnessctl set 1%+");
      return true;
    case XKB_KEY_XF86Favorites:
      spawn("playerctl play-pause");
      return true;
    default:
      return false; // Not a handled media key
  }
}
// Ctrl+Alt+Fn is mapped to XKB_KEY_XF86Switch_VT_n, and switches virtual terminals
static bool handle_switch_vt_key(struct tinywl_server *server, uint32_t sym) {
  // VT_1 through VT_4 are perfectly contiguous
  if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_4) {
    uint32_t vt_number = 1 + (sym - XKB_KEY_XF86Switch_VT_1);

    if (server->session != NULL) {
      wlr_log(WLR_INFO, "Switching to TTY %d via server state", vt_number);
      wlr_session_change_vt(server->session, vt_number);
    }
    return true; // Swallowed
  }
  return false;
}

// LOGO+Tab
static void cycle_toplevels(struct tinywl_server *server) {
  struct wl_list *toplevels = &server->toplevels;

  if (wl_list_length(toplevels) <= 1) {
    return;
  }

  // Get front and back windows
  struct tinywl_toplevel *front =
      wl_container_of(toplevels->next, front, link);
  struct tinywl_toplevel *back =
      wl_container_of(toplevels->prev, back, link);

  // Cycle in the scene tree and our tracking list
  wlr_scene_node_place_below(&front->scene_tree->node, 
                             &back->scene_tree->node);
  wl_list_remove(&front->link);
  wl_list_insert(toplevels->prev, &front->link);

  // Get the new front and focus it
  front = wl_container_of(toplevels->next, front, link);
  focus_toplevel(front, front->xdg_toplevel->base->surface);
}

// Assume LOGO is held down and handle another key pressed.
static bool handle_quick_key(struct tinywl_server *server, uint32_t sym) {
  switch (sym) {
    case XKB_KEY_Return:
      spawn("foot");
      return true;
    case XKB_KEY_Escape:
      wl_display_terminate(server->wl_display);
      return true;
    case XKB_KEY_Tab:
      cycle_toplevels(server);
      return true;
    case XKB_KEY_d:
      spawn("wofi --show drun");
      return true;
    case XKB_KEY_f:
      toggle_focused_fullscreen(server);
      return true;
    case XKB_KEY_g:
      spawn("play -n synth square 600 trim 0 0.3 gain -20");
      return true;
    case XKB_KEY_q:
      close_focused_toplevel(server);
      return true;
    default:
      break;
  }
  return false;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
  struct tinywl_keyboard *keyboard = wl_container_of(listener, keyboard, key);
  struct wlr_keyboard_key_event *event = data;

  // sanity check
  if (keyboard == NULL || event == NULL) {
    wlr_log(WLR_ERROR, "keyboard fail");
    return;
  }
  if (event->keycode == 0) {
    wlr_log(WLR_ERROR, "keycode 0");
    return;
  }

  struct tinywl_server *server = keyboard->server;

  if (server == NULL) return;
  if (keyboard->wlr_keyboard == NULL) return;
  if (keyboard->wlr_keyboard->xkb_state == NULL) return;

  wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);

  // Ensure the seat knows this hardware keyboard is active.
  wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    
  bool grab = false;

  // Handle a key release
  if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED &&
      event->keycode == keyboard->grabbed_keycode) {
    keyboard->grabbed_keycode = 0;
    grab = true;
  }

  // See if the compositor should grab and act on a keypress 
  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
      keyboard->grabbed_keycode == 0) {
    uint32_t sym = xkb_state_key_get_one_sym(keyboard->wlr_keyboard->xkb_state,
                                             event->keycode + 8);
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    if (handle_media_key(sym)) {
      grab = true;
    } else if (handle_switch_vt_key(server, sym)) {
      grab = true;
    } else if ((modifiers & WLR_MODIFIER_LOGO) == WLR_MODIFIER_LOGO) {
      if (handle_quick_key(server, sym)) {
        grab = true;
      }
    }
    if (grab) {
      keyboard->grabbed_keycode = event->keycode;
    }
  }

  if (!grab) {
    wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
                                 event->keycode, event->state);
  }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
  /* This event is raised by the keyboard base wlr_input_device to signal
   * the destruction of the wlr_keyboard. It will no longer receive events
   * and should be destroyed.
   */
  struct tinywl_keyboard *keyboard =
      wl_container_of(listener, keyboard, destroy);

  wl_list_remove(&keyboard->modifiers.link);
  wl_list_init(&keyboard->modifiers.link);
  wl_list_remove(&keyboard->key.link);
  wl_list_init(&keyboard->key.link);
  wl_list_remove(&keyboard->destroy.link);
  wl_list_init(&keyboard->destroy.link);
  wl_list_remove(&keyboard->link);
  wl_list_init(&keyboard->link);
  free(keyboard);
}

static void server_new_keyboard(struct tinywl_server *server,
                                struct wlr_input_device *device) {
  struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

  struct tinywl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
  if (!keyboard) {
    return;
  }
  keyboard->server = server;
  keyboard->wlr_keyboard = wlr_keyboard;

  // Initialize XKB context and standard keymap
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  
  struct xkb_rule_names rules = {
    .options = "compose:ralt"
  };

  struct xkb_keymap *keymap =
      xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (keymap) {
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
  }
  xkb_context_unref(context);
  
  // Set typematic key repeat parameters
  wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);
  
  // Hook up listeners
  keyboard->modifiers.notify = keyboard_handle_modifiers;
  wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
  keyboard->key.notify = keyboard_handle_key;
  wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
  keyboard->destroy.notify = keyboard_handle_destroy;
  wl_signal_add(&device->events.destroy, &keyboard->destroy);

  // Set this newly plugged-in device as the primary keyboard
  wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

  // Inject our tracking instance into the server's tracking array
  wl_list_insert(&server->keyboards, &keyboard->link);
}

// New pointer does not allocate memory or store any custom state,
// so no need for a handle_pointer_destroy
static void server_new_pointer(struct tinywl_server *server,
                               struct wlr_input_device *device) {
  wlr_cursor_attach_input_device(server->cursor, device);

  /* Configure Tap-to-Click if this is a libinput device (like a touchpad) */
  if (wlr_input_device_is_libinput(device)) {
    struct libinput_device *libinput_dev =
        wlr_libinput_get_device_handle(device);
    if (libinput_device_config_tap_get_finger_count(libinput_dev) > 0) {
      libinput_device_config_tap_set_enabled(libinput_dev,
                                             LIBINPUT_CONFIG_TAP_ENABLED);
    }
  }
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, request_cursor);
  /* This event is raised by the seat when a client provides a cursor image */
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  struct wlr_seat_client *focused_client =
      server->seat->pointer_state.focused_client;
  /* This can be sent by any client, so we check to make sure this one is
   * actually has pointer focus first. */
  if (focused_client == event->seat_client) {
    /* Once we've vetted the client, we can tell the cursor to use the
     * provided surface as the cursor image. It will set the hardware cursor
     * on the output that it's currently on and continue to do so as the
     * cursor moves between outputs. */
    wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
                           event->hotspot_y);
  }
}

// The seat raises a request when the client wants to set the selection
static void seat_request_set_selection(struct wl_listener *listener,
                                       void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, request_set_selection);
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void server_new_input(struct wl_listener *listener, void *data) {
  /* This event is raised by the backend when a new input device becomes
   * available. */
  struct tinywl_server *server = wl_container_of(listener, server, new_input);
  struct wlr_input_device *device = data;
  switch (device->type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    server_new_keyboard(server, device);
    break;
  case WLR_INPUT_DEVICE_POINTER:
    server_new_pointer(server, device);
    break;
  default:
    break;
  }
  /* We need to let the wlr_seat know what our capabilities are, which is
   * communiciated to the client. In TinyWL we always have a cursor, even if
   * there are no pointer devices, so we always include that capability. */
  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&server->keyboards)) {
    caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  }
  wlr_seat_set_capabilities(server->seat, caps);
}


int main(int argc, char *argv[]) {
  wlr_log_init(WLR_DEBUG, NULL);
  char *startup_cmd = NULL;

  int c;
  while ((c = getopt(argc, argv, "s:h")) != -1) {
    switch (c) {
    case 's':
      startup_cmd = optarg;
      break;
    default:
      printf("Usage: %s [-s startup command]\n", argv[0]);
      return 0;
    }
  }
  if (optind < argc) {
    printf("Usage: %s [-s startup command]\n", argv[0]);
    return 0;
  }

  struct tinywl_server server = {0};
  server.wl_display = wl_display_create();

  // Choose the most suitable backend based on the current environment
  server.backend = wlr_backend_autocreate(server.wl_display, &server.session);
  if (server.backend == NULL) {
    wlr_log(WLR_ERROR, "Failed to create wlr_backend");
    return 1;
  }

  // Create a renderer
  server.renderer = wlr_renderer_autocreate(server.backend);
  if (server.renderer == NULL) {
    wlr_log(WLR_ERROR, "Failed to create wlr_renderer");
    return 1;
  }
  wlr_renderer_init_wl_display(server.renderer, server.wl_display);

  // Create an allocator - the bridge between renderer and backend
  server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
  if (server.allocator == NULL) {
    wlr_log(WLR_ERROR, "Failed to create wlr_allocator");
    return 1;
  }

  // Instantiate the data device manager for copy/paste and DnD
  struct wlr_data_device_manager *data_device_manager =
      wlr_data_device_manager_create(server.wl_display);
  if (data_device_manager == NULL) {
    wlr_log(WLR_ERROR, "Failed to create data device manager");
    return 1;
  }

  /* This creates some hands-off wlroots interfaces. The compositor is
   * necessary for clients to allocate surfaces, the subcompositor allows to
   * assign the role of subsurfaces to surfaces and the data device manager
   * handles the clipboard. Each of these wlroots interfaces has room for you
   * to dig your fingers in and play with their behavior if you want. Note that
   * the clients cannot set the selection directly without compositor approval,
   * see the handling of the request_set_selection event below.*/
  wlr_compositor_create(server.wl_display, 5, server.renderer);
  wlr_subcompositor_create(server.wl_display);

  /* Creates an output layout, which a wlroots utility for working with an
   * arrangement of screens in a physical layout. */
  server.output_layout = wlr_output_layout_create();

  /* Configure a listener to be notified when new outputs are available on the
   * backend. */
  wl_list_init(&server.outputs);
  server.new_output.notify = server_new_output;
  wl_signal_add(&server.backend->events.new_output, &server.new_output);

  /* Create a scene graph. This is a wlroots abstraction that handles all
   * rendering and damage tracking. All the compositor author needs to do
   * is add things that should be rendered to the scene graph at the proper
   * positions and then call wlr_scene_output_commit() to render a frame if
   * necessary.
   */
  server.scene = wlr_scene_create();
  server.scene_layout =
      wlr_scene_attach_output_layout(server.scene, server.output_layout);

  // For wlr-screencopy-unstable-v1
  wlr_screencopy_manager_v1_create(server.wl_display);

  // For zxdg_output_manager_v1
  wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

  // For xdg-shell version 3.
  wl_list_init(&server.toplevels);
  server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
  server.new_xdg_surface.notify = server_new_xdg_surface;
  wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

  /*
   * Creates a cursor, which is a wlroots utility for tracking the cursor
   * image shown on screen.
   */
  server.cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

  /* Creates an xcursor manager, another wlroots utility which loads up
   * Xcursor themes to source cursor images from and makes sure that cursor
   * images are available at all scale factors on the screen (necessary for
   * HiDPI support). */
  server.cursor_manager = wlr_xcursor_manager_create(NULL, 24);

  /*
   * wlr_cursor *only* displays an image on screen. It does not move around
   * when the pointer moves. However, we can attach input devices to it, and
   * it will generate aggregate events for all of them. In these events, we
   * can choose how we want to process them, forwarding them to clients and
   * moving the cursor around. More detail on this process is described in
   * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html.
   *
   * And more comments are sprinkled throughout the notify functions above.
   */
  server.cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
  server.cursor_motion.notify = server_cursor_motion;
  wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
  server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
  wl_signal_add(&server.cursor->events.motion_absolute,
                &server.cursor_motion_absolute);
  server.cursor_button.notify = server_cursor_button;
  wl_signal_add(&server.cursor->events.button, &server.cursor_button);
  server.cursor_axis.notify = server_cursor_axis;
  wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
  server.cursor_frame.notify = server_cursor_frame;
  wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

  // For wlr_xdg_decoration_manager_v1
  server.xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server.wl_display);
  server.new_toplevel_decoration.notify = handle_new_toplevel_decoration;
  wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration, 
                &server.new_toplevel_decoration);

  // For wlr_idle_notifier_v1
  server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);

  /*
   * Configures a seat, which is a single "seat" at which a user sits and
   * operates the computer. This conceptually includes up to one keyboard,
   * pointer, touch, and drawing tablet device. We also rig up a listener to
   * let us know when new input devices are available on the backend.
   */
  wl_list_init(&server.keyboards);
  server.new_input.notify = server_new_input;
  wl_signal_add(&server.backend->events.new_input, &server.new_input);
  server.seat = wlr_seat_create(server.wl_display, "seat0");

  // For DnD
  server.start_drag.notify = server_handle_start_drag;
  wl_signal_add(&server.seat->events.start_drag, &server.start_drag);
  server.request_start_drag.notify = server_handle_request_start_drag;
  wl_signal_add(&server.seat->events.request_start_drag,
                &server.request_start_drag);
  server.start_drag.notify = server_handle_start_drag;
  wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

  server.request_cursor.notify = seat_request_cursor;
  wl_signal_add(&server.seat->events.request_set_cursor,
                &server.request_cursor);
  server.request_set_selection.notify = seat_request_set_selection;
  wl_signal_add(&server.seat->events.request_set_selection,
                &server.request_set_selection);


  // For xdg-activation-v1.xml
  server.xdg_activation = wlr_xdg_activation_v1_create(server.wl_display);
  server.request_activation.notify = server_request_activation;
  wl_signal_add(&server.xdg_activation->events.request_activate, 
                &server.request_activation);

  /* Add a Unix socket to the Wayland display. */
  const char *socket = wl_display_add_socket_auto(server.wl_display);
  if (!socket) {
    wlr_backend_destroy(server.backend);
    return 1;
  }

  /* Start the backend. This will enumerate outputs and inputs, become the DRM
   * master, etc */
  if (!wlr_backend_start(server.backend)) {
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.wl_display);
    return 1;
  }

  /* Set the WAYLAND_DISPLAY environment variable to our socket and run the
   * startup command if requested. */
  setenv("WAYLAND_DISPLAY", socket, true);
  if (startup_cmd) {
    if (fork() == 0) {
      execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
    }
  }
  /* Run the Wayland event loop. This does not return until you exit the
   * compositor. Starting the backend rigged up all of the necessary event
   * loop configuration to listen to libinput events, DRM events, generate
   * frame events at the refresh rate, and so on. */
  wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
  wl_display_run(server.wl_display);

  /* Once wl_display_run returns, we destroy all clients then shut down the
   * server. */
  wl_display_destroy_clients(server.wl_display);
  wlr_scene_node_destroy(&server.scene->tree.node);
  wlr_xcursor_manager_destroy(server.cursor_manager);
  wlr_output_layout_destroy(server.output_layout);
  wl_display_destroy(server.wl_display);
  return 0;
}
