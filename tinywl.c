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

/* For brevity's sake, struct members are annotated where they are used. */
enum tinywl_cursor_mode {
  TINYWL_CURSOR_PASSTHROUGH,
  TINYWL_CURSOR_MOVE,
  TINYWL_CURSOR_RESIZE,
};

struct tinywl_server {
  struct wl_display *wl_display;
  struct wlr_backend *backend;
  struct wlr_session *session; // added
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
  double grabbed_cursor_x, grabbed_cursor_y;  // cursor position when resize/move started
  struct wlr_box grabbed_geobox;

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
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *scene_tree; // added

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener request_move;
  struct wl_listener request_resize;

  // For asynchronous resizing
  uint32_t resize_serial;
  int pending_x, pending_y;

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

  // For keeping track of keypresses not told to the client
  uint32_t grabbed_keycode;
};

// Struct for baby popup windows that have not yet been configured
struct tinywl_unconfigured {
  struct wlr_xdg_surface *xdg_surface;
  struct tinywl_server *server;
  struct wl_listener commit;
  struct wl_listener destroy;
};

// Struct to manage the popup's lifecycle
struct tinywl_popup {
    struct wlr_xdg_surface *xdg_surface;
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
                                                   double lx, double ly,
                                                   struct wlr_surface **surface,
                                                   double *sx, double *sy) {
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
  while (tree != NULL && tree->node.data == NULL) {
    tree = tree->node.parent;
  }
  
  // If we hit the root of the tree without finding a window structure,
  if (tree == NULL) {
      return NULL;
  }
  
  return tree->node.data;
}

// Reset the cursor mode to passthrough.
static void reset_cursor_mode(struct tinywl_server *server) {
  if (server->cursor_mode == TINYWL_CURSOR_PASSTHROUGH) {
    wlr_log(WLR_DEBUG, "Trying to reset an already-reset cursor mode.");
    return;
  }

  // If a window is currently bound to the grab, clear it
  if (server->grabbed_toplevel &&
      !wl_list_empty(&server->grabbed_toplevel->link)) {
    server->grabbed_toplevel->resize_serial = 0;
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

// Move the grabbed toplevel
static void process_cursor_move(struct tinywl_server *server, uint32_t time) {
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;
  if (toplevel == NULL) {
    wlr_log(WLR_ERROR, "Trying to drag a NULL.");
    return;
  }

  if (wl_list_empty(&toplevel->link)) {
    wlr_log(WLR_DEBUG, "Grabbed window must have been destroyed mid-move.");
    reset_cursor_mode(server);
    return;
  }

  if (!toplevel->scene_tree) {
    wlr_log(WLR_ERROR, "Trying to drag a NULL scene_tree.");
    return;
  }

  // Coordinate math
  int x = server->grabbed_geobox.x;
  int y = server->grabbed_geobox.y;

  int dx = server->cursor->x - server->grabbed_cursor_x;
  int dy = server->cursor->y - server->grabbed_cursor_y;

  x += dx;
  y += dy;

  // Re-position the window in your compositor layout tree
  wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
}

// Resize the grabbed toplevel at any corner or edge.
static void process_cursor_resize(struct tinywl_server *server, uint32_t time) {
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;

  if (toplevel->resize_serial != 0) {
    // Waiting for the client to commit
    return;
  }

  // Sanity checks
  if (toplevel == NULL) {
    wlr_log(WLR_ERROR, "Trying to resize a NULL.");
    return;
  }

  if (wl_list_empty(&toplevel->link)) {
    wlr_log(WLR_DEBUG, "Grabbed window must have been destroyed mid-resize.");
    reset_cursor_mode(server);
    return;
  }

  if (!toplevel->scene_tree) {
    wlr_log(WLR_ERROR, "Trying to resize a NULL scene_tree.");
    return;
  }
  
  // Position and geometry when the drag started
  int x = server->grabbed_geobox.x;
  int y = server->grabbed_geobox.y;
  int width = server->grabbed_geobox.width;
  int height = server->grabbed_geobox.height;

  // Mouse movement since the drag started
  int dx = server->cursor->x - server->grabbed_cursor_x;
  int dy = server->cursor->y - server->grabbed_cursor_y;

  if (server->resize_edges & WLR_EDGE_TOP) {
    y += dy;
    height -= dy;
  } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
    height += dy;
  }
  if (server->resize_edges & WLR_EDGE_LEFT) {
    x += dx;
    width -= dx;
  } else if (server->resize_edges & WLR_EDGE_RIGHT) {
    width += dx;
  }
    
  int min_width = 50;
  int min_height = 50;
  // TO DO: set these to client preferences, with sanity checks
  // int client_min_width = toplevel->xdg_toplevel->current.min_width; ...

  if (height < min_height) {
    if (server->resize_edges & WLR_EDGE_TOP) {
      y -= (min_height - height);
    }
    height = min_height;
  }

  if (width < min_width) {
    if (server->resize_edges & WLR_EDGE_LEFT) {
      x -= (min_width - width);
    }
    width = min_width;
  }

  toplevel->resize_serial = 
      wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
  // Don't position until the client resizes
  toplevel->pending_x = x;
  toplevel->pending_y = y;
}

static void process_cursor_motion(struct tinywl_server *server, uint32_t time) {
  // If the mode is non-passthrough, delegate to those functions.
  if (server->cursor_mode == TINYWL_CURSOR_MOVE) {
    process_cursor_move(server, time);
    return;
  } else if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
    process_cursor_resize(server, time);
    return;
  }

  // Otherwise, find the toplevel under the pointer and send the event along.
  double sx, sy; 
  struct wlr_seat *seat = server->seat;
  struct wlr_surface *surface = NULL;
  struct tinywl_toplevel *toplevel = desktop_toplevel_at(
      server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
  
  if (!toplevel) {
    // Over empty space, set the cursor image to a default arrow
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
  }

  if (surface) {
    // Enter event gives the surface "pointer focus".
    // wlroots will avoid sending duplicate enter/motion events.
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
  } else {
    // The cursor is either on empty space OR window borders.
    // Clear client-side focus without breaking compositor graphics states.
    wlr_seat_pointer_clear_focus(seat);
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

// Unfocus the old stuff, move the new window to the top, and tell it
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
      
  // Safe to activate and pass to the seat engine now
  wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

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

// This function is triggered by a relative pointer motion event
static void server_cursor_motion(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_motion);
  struct wlr_pointer_motion_event *event = data;
  wlr_cursor_move(server->cursor, &event->pointer->base,
                  event->delta_x, event->delta_y);
  process_cursor_motion(server, event->time_msec);
  update_drag_icon_position(server); // for DnD
  wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
}

// This function is triggered by an absolute pointer motion event
// eg. enter the window when tinywl is running nested in a window
static void server_cursor_motion_absolute(struct wl_listener *listener,
                                          void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_motion_absolute);
  struct wlr_pointer_motion_absolute_event *event = data;
  wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
                           event->x, event->y);
  process_cursor_motion(server, event->time_msec);
  wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
}

// Set up an interactive move/resize, where the compositor grabs mouse events
static void begin_interactive(struct tinywl_toplevel *toplevel,
                              enum tinywl_cursor_mode mode, uint32_t edges) {
  struct tinywl_server *server = toplevel->server;
  struct wlr_surface *focused_surface =
      server->seat->pointer_state.focused_surface;

  // Sanity checks
  if (toplevel->xdg_toplevel->base->surface !=
      wlr_surface_get_root_surface(focused_surface)) {
    wlr_log(WLR_ERROR, "Trying to interact with a toplevel and unrelated surface");
    return;
  }

  if (toplevel->is_fullscreen) {
    wlr_log(WLR_INFO, "Trying to interact with a fullscreen");
    return;
  }

  // Unmaximize. Maximized windows do not (currently) remember their previous size.
  if (toplevel->is_maximized) {
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
    toplevel->is_maximized = false;
  }

  server->grabbed_toplevel = toplevel;
  server->cursor_mode = mode;
  server->resize_edges = edges;

  struct wlr_box geom;
  wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geom);

  server->grabbed_geobox.x = toplevel->scene_tree->node.x;
  server->grabbed_geobox.y = toplevel->scene_tree->node.y;
  server->grabbed_geobox.width = geom.width;
  server->grabbed_geobox.height = geom.height;

  server->grabbed_cursor_x = server->cursor->x;
  server->grabbed_cursor_y = server->cursor->y;
}

// This function is triggered by a pointer button event
static void server_cursor_button(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, cursor_button);
  struct wlr_pointer_button_event *event = data;

  // Button release while dragging or resizing a window
  if (server->cursor_mode == TINYWL_CURSOR_MOVE ||
      server->cursor_mode == TINYWL_CURSOR_RESIZE) {
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
      reset_cursor_mode(server);
      return; // Do not notify wlroots or the client
    }
  }

  // Get active keyboard hardware modifier flags
  uint32_t modifiers =
      wlr_keyboard_get_modifiers(wlr_seat_get_keyboard(server->seat));

  // Get the surface and toplevel under the cursor
  double sx, sy;
  struct wlr_surface *surface = NULL;
  struct tinywl_toplevel *toplevel = desktop_toplevel_at(
      server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

  // LOGO + Left Click (Interactive Window Move)
  if ((modifiers & WLR_MODIFIER_LOGO) && (event->button == BTN_LEFT)) {
    focus_toplevel(toplevel, surface);
    begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
    return; // Do not notify wlroots or the client
  }

  // LOGO + Right Click (Interactive Window Resize)
  if ((modifiers & WLR_MODIFIER_LOGO) && (event->button == BTN_RIGHT)) {
    focus_toplevel(toplevel, surface);
    begin_interactive(toplevel, TINYWL_CURSOR_RESIZE,
                      WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
    return; // Do not notify wlroots or the client
  }

  // click on empty space or not-LOGO click on a window
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (toplevel == NULL) { // click on empty space
      unfocus_toplevel(server);
      wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                     event->button, event->state);
    } else {  // click on a surface
      wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                     event->button, event->state);
      focus_toplevel(toplevel, surface);
    }
    return;
  }

  // Remaining case: release event in the usual "passthrough" state
  wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                 event->button, event->state);
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
  struct wlr_scene *scene = output->server->scene;

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
  float background_color[4] = {0.25f, 0.05f, 0.05f, 1.0f};
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
    reset_cursor_mode(server);
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

// This function is called every time the client updates its surface buffer.
// Handle a commit of a resize that now needs to be positioned.
static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
  (void)data;

  if (toplevel->resize_serial == 0) {
    return;
  }

  struct wlr_xdg_surface *xdg_surface = toplevel->xdg_toplevel->base;
  
  if (xdg_surface->current.configure_serial >= toplevel->resize_serial) {
    // We are guaranteed to have a valid pending position because resize_serial was > 0
    wlr_scene_node_set_position(&toplevel->scene_tree->node, 
                                toplevel->pending_x, 
                                toplevel->pending_y);
    
    // This single assignment resets the state machine and unblocks the cursor handler
    toplevel->resize_serial = 0; 
  }
}

static void handle_new_toplevel(struct tinywl_server *server,
                                struct wlr_xdg_surface *xdg_surface) {
  struct tinywl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
  toplevel->server = server;
  toplevel->xdg_toplevel = xdg_surface->toplevel;

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

static void handle_popup_destroy(struct wl_listener *listener, void *data) {
    struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void handle_new_popup(struct tinywl_server *server,
                             struct wlr_xdg_surface *xdg_surface) {
  // Find the parent surface's scene tree node
  struct wlr_xdg_surface *parent = 
    wlr_xdg_surface_try_from_wlr_surface(xdg_surface->popup->parent);

  if (parent == NULL || parent->data == NULL) {
    wlr_log(WLR_DEBUG, "Popup parent is missing a scene node.");
    return;
  }
  struct wlr_scene_tree *parent_tree = parent->data;

  // Allocate a tracking structure for this popup's lifecycle
  struct tinywl_popup *popup = calloc(1, sizeof(*popup));
  popup->xdg_surface = xdg_surface;

  // Create the scene node and save it to xdg_surface->data
  struct wlr_scene_tree *popup_tree = wlr_scene_xdg_surface_create(parent_tree,
                                                                   xdg_surface);
  xdg_surface->data = popup_tree;

  // Setup the destroy listener
  popup->destroy.notify = handle_popup_destroy;
  wl_signal_add(&xdg_surface->events.destroy, &popup->destroy);
}

// Handle windows that have not yet been configured
static void handle_unconfigured_commit(struct wl_listener *listener, void *data) {
  struct tinywl_unconfigured *unconfigured = wl_container_of(listener, unconfigured, commit);
  struct wlr_xdg_surface *xdg_surface = unconfigured->xdg_surface;

  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_NONE) {
    return; // Keep waiting
  }

  wl_list_remove(&unconfigured->commit.link);
  wl_list_init(&unconfigured->commit.link);
  wl_list_remove(&unconfigured->destroy.link);
  wl_list_init(&unconfigured->destroy.link);

  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    handle_new_toplevel(unconfigured->server, xdg_surface);
  } else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
    handle_new_popup(unconfigured->server, xdg_surface);
  }

  free(unconfigured);
}

// If a window gets destroyed before it was configured and committed
static void handle_unconfigured_destroy(struct wl_listener *listener, void *data) {
    struct tinywl_unconfigured *unconfigured = wl_container_of(listener, unconfigured, destroy);
    wl_list_remove(&unconfigured->commit.link);
    wl_list_init(&unconfigured->commit.link);
    wl_list_remove(&unconfigured->destroy.link);
    wl_list_init(&unconfigured->destroy.link);
    free(unconfigured);
}

// Handle a new unconfigured window - give it a listener until it is configured
static void handle_new_unconfigured_surface(struct tinywl_server *server,
                                            struct wlr_xdg_surface *xdg_surface) {
    struct tinywl_unconfigured *unconfigured = calloc(1, sizeof(*unconfigured));
    unconfigured->xdg_surface = xdg_surface;
    unconfigured->server = server;

    unconfigured->commit.notify = handle_unconfigured_commit;
    wl_signal_add(&xdg_surface->surface->events.commit, &unconfigured->commit);

    unconfigured->destroy.notify = handle_unconfigured_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &unconfigured->destroy);
}

// Event raised when client sends a new xdg surface to wlr_xdg_shell
static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, new_xdg_surface);
  struct wlr_xdg_surface *xdg_surface = data;

  switch (xdg_surface->role) {
    case WLR_XDG_SURFACE_ROLE_NONE:
      handle_new_unconfigured_surface(server, xdg_surface);
      break;
    case WLR_XDG_SURFACE_ROLE_POPUP:
      handle_new_popup(server, xdg_surface);
      break;
    case WLR_XDG_SURFACE_ROLE_TOPLEVEL:
      handle_new_toplevel(server, xdg_surface);
      break;
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

  // 1. Grab the top window we want to move
  struct tinywl_toplevel *current =
      wl_container_of(toplevels->next, current, link);

  // 2. Find the window that is currently at the very back of our stack
  // (toplevels->prev points to the last item in a standard Wayland wl_list)
  struct tinywl_toplevel *back_toplevel =
      wl_container_of(toplevels->prev, back_toplevel, link);

  // 3. Move the data container to the back of the memory list
  wl_list_remove(&current->link);
  wl_list_insert(toplevels->prev, &current->link);

  // 4. Move the scene graphics directly underneath that back window!
  // This guarantees it stays in the normal window layer and doesn't get clipped.
  if (current != back_toplevel) {
    wlr_scene_node_place_below(&current->scene_tree->node, 
                               &back_toplevel->scene_tree->node);
  }

  // 5. Focus the window that has now inherited the top position
  struct tinywl_toplevel *next =
      wl_container_of(toplevels->next, next, link);

  focus_toplevel(next, next->xdg_toplevel->base->surface);
}

// This function assumes LOGO is held down and a key is pressed
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
    uint32_t sym = xkb_state_key_get_one_sym(keyboard->wlr_keyboard->xkb_state, event->keycode + 8);
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

static void seat_request_set_selection(struct wl_listener *listener,
                                       void *data) {
  /* This event is raised by the seat when a client wants to set the selection,
   * usually when the user copies something. wlroots allows compositors to
   * ignore such requests if they so choose, but in tinywl we always honor
   */
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

  /* Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which is
   * used for application windows. For more detail on shells, refer to
   * https://drewdevault.com/2018/07/29/Wayland-shells.html.
   */
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
