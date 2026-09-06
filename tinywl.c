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

// For various protocols
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>

enum tinywl_cursor_mode {
  TINYWL_CURSOR_PASSTHROUGH,
  TINYWL_CURSOR_MOVE,
  TINYWL_CURSOR_RESIZE,
};

enum tinywl_surface_type {
  TINYWL_SURFACE_POPUP,
  TINYWL_SURFACE_TOPLEVEL,
  TINYWL_SURFACE_LAYER,
};

struct tinywl_toplevel_marker {
  struct tinywl_server *server;
  struct wlr_xdg_surface *xdg_surface;
  struct wl_listener map;
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
  struct wlr_xcursor_manager *cursor_mgr;
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

  // For resizing "wireframe layout"
  struct wlr_scene_rect *preview_rect;

  // To be attached to scene, for wlr_layer_shell_unstable_v1
  struct wlr_scene_tree *scene_background;
  struct wlr_scene_tree *scene_bottom;
  struct wlr_scene_tree *scene_normal;
  struct wlr_scene_tree *scene_top;
  struct wlr_scene_tree *scene_overlay;

  struct wlr_scene_output_layout *scene_layout;

  // for DnD
  struct wlr_drag *current_drag;
  struct wlr_scene_tree *drag_icon_tree;
  bool next_commit_at_cursor;
  struct wl_listener request_start_drag;
  struct wl_listener start_drag;
  struct wl_listener destroy_drag;

  // For xdg_activation_v1
  struct wlr_xdg_activation_v1 *xdg_activation;
  struct wl_listener request_activation;

  // For wlr_idle_notifier_v1
  struct wlr_idle_notifier_v1 *idle_notifier;
  struct wl_listener idle_away;
  struct wl_listener idle_resume;

  // For wlr_xdg_decoration_manager_v1
  struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
  struct wl_listener new_toplevel_decoration;

  // For move/resize interactions
  double grabbed_cursor_x;
  double grabbed_cursor_y;
  uint32_t grabbed_active_button;
  uint32_t grabbed_cancel_button;

  // For wlr_layer_shell_unstable_v1
  struct wlr_layer_shell_v1 *layer_shell;
  struct wl_list layer_surfaces;
  struct wl_listener new_layer_surface; 
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
  enum tinywl_surface_type type;  // must go first
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *scene_tree; // added

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener request_move;
  struct wl_listener request_resize;

  // added listeners for maximize, fullscreen, asynchronous resize
  struct wl_listener request_maximize;
  struct wl_listener request_fullscreen;

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

struct tinywl_popup {
  enum tinywl_surface_type type; // first!
  struct wlr_xdg_popup *xdg_popup;
  struct wlr_scene_tree *scene_tree;
  struct tinywl_server *server;
  struct wl_listener destroy;
  struct wl_listener reposition;
};

struct tinywl_layer_surface {
  enum tinywl_surface_type type; // first!
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_layer_surface_v1 *wlr_layer_surface;
  struct wlr_scene_layer_surface_v1 *scene_layer_surface;

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener commit;
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

// ----- POPUPS -----

static void handle_popup_destroy(struct wl_listener *listener, void *data) {
  struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);
  wl_list_remove(&popup->destroy.link);
  wl_list_remove(&popup->reposition.link);
  free(popup);
}

static void handle_popup_reposition(struct wl_listener *listener, void *data) {
  struct tinywl_popup *popup = wl_container_of(listener, popup, reposition);
  // Update the scene node to the browser's newly requested coordinates
  wlr_scene_node_set_position(&popup->scene_tree->node,
                              popup->xdg_popup->current.geometry.x,
                              popup->xdg_popup->current.geometry.y);
  // TO DO: again prevent it spilling off-screen
}


// Handle new popup owned by any kind of surface
static void server_new_popup(struct tinywl_server *server,
                             struct wlr_xdg_popup *xdg_popup) {
  // calloc my custom wrapper
  struct tinywl_popup *popup = calloc(1, sizeof(*popup));
  if (popup == NULL) {
    wlr_log(WLR_ERROR, "Failed to allocate memory for popup tracking");
    return;
  }

  // Find the appropriate tree, depending on the parent type
  struct wlr_scene_tree *parent_tree = NULL;

  struct wlr_xdg_surface *parent_xdg =
      wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
  struct wlr_layer_surface_v1 *parent_layer =
      wlr_layer_surface_v1_try_from_wlr_surface(xdg_popup->parent);

  if (parent_xdg != NULL) {
    parent_tree = parent_xdg->surface->data;
  } else if (parent_layer != NULL) {
    parent_tree = parent_layer->surface->data;
  }

  if (!parent_tree) {
    wlr_log(WLR_DEBUG, "POPUP: fall back to attaching it to root.");
    parent_tree = &server->scene->tree;
  }

  // Let the scene graph attach it automatically
  popup->scene_tree =
      wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
  if (!popup->scene_tree) {
    wlr_log(WLR_ERROR, "POPUP: failed to attach to the scene tree.");
    free(popup);
    return;
  }

  // Hook up the server back-pointer 
  popup->server = server;
  popup->xdg_popup = xdg_popup;

  // Cross-link data pointers
  xdg_popup->base->data = popup;
  popup->scene_tree->node.data = popup;

  // Prevent the popup from spilling off-screen
  struct wlr_output *output = wlr_output_layout_output_at(
      server->output_layout, server->cursor->x, server->cursor->y);
  if (output != NULL) {
    struct wlr_box output_box;
    wlr_output_layout_get_box(server->output_layout, output, &output_box);
    // TO DO: worry about offsets, especially if multi-monitor
    wlr_xdg_popup_unconstrain_from_box(xdg_popup, &output_box);
  }

  // Connect listeners
  popup->destroy.notify = handle_popup_destroy;
  wl_signal_add(&xdg_popup->base->events.destroy, &popup->destroy);

  popup->reposition.notify = handle_popup_reposition;
  wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);
}

// ----- LAYERS -----
// --- wlr_layer_shell_unstable_v1 ---

// Anchor layers to the sides and calculate usable space
static void arrange_layers(struct tinywl_server *server) {
  if (wl_list_empty(&server->outputs)) {
    return; 
  }

  // TO DO: think about multi-monitor
  struct tinywl_output *tinywl_output =
      wl_container_of(server->outputs.next, tinywl_output, link);
  struct wlr_output *output = tinywl_output->wlr_output;

  // Initialize the bounding boxes
  struct wlr_box full_area;
  wlr_output_effective_resolution(output, &full_area.width, &full_area.height);
  full_area.x = 0;
  full_area.y = 0;

  // Usable_area starts full
  struct wlr_box usable_area = full_area;

  struct tinywl_layer_surface *layer_surface;

  // --- PASS 1: Calculate Usable Area ---
  wl_list_for_each(layer_surface, &server->layer_surfaces, link) {
    struct wlr_layer_surface_v1 *wlr_surface = layer_surface->wlr_layer_surface;
    struct wlr_layer_surface_v1_state *state = &wlr_surface->pending;

    // Skip if it doesn't request an exclusive zone
    if (state->exclusive_zone <= 0) {
      continue;
    }

    // Shrink usable_area along one edge
    // It is anchored to that edge but not its opposite edge
    if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) && 
       !(state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
      usable_area.y += state->exclusive_zone;
      usable_area.height -= state->exclusive_zone;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) && 
               !(state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
      usable_area.height -= state->exclusive_zone;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) && 
               !(state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
      usable_area.x += state->exclusive_zone;
      usable_area.width -= state->exclusive_zone;
    } else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) && 
          !(state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
      usable_area.width -= state->exclusive_zone;
    }
  }

  // --- PASS 2: Position and Configure Surfaces ---
  wl_list_for_each(layer_surface, &server->layer_surfaces, link) {
    struct wlr_layer_surface_v1 *wlr_surface = layer_surface->wlr_layer_surface;
    struct wlr_layer_surface_v1_state *state = &wlr_surface->pending;

    // Default to requested sizes
    int width = state->desired_width;
    int height = state->desired_height;

    // Check horizontal anchor stretching
    if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) &&
        (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
      width = full_area.width; // Wallpapers/overlays span full screen width
      if (state->exclusive_zone > 0) {
        width = usable_area.width; // Exclusive panels match usable bounds
      }
    }

    // Check vertical anchor stretching
    if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) &&
        (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
      height = full_area.height;
      if (state->exclusive_zone > 0) {
        height = usable_area.height;
      }
    }

    // Protect against passing 0 dimensions to the client
    if (width <= 0) width = state->desired_width ? state->desired_width : 100;
    if (height <= 0) height = state->desired_height ? state->desired_height : 100;

    // Determine absolute position coordinates (x, y)
    int x = full_area.x;
    int y = full_area.y;

    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
      x = full_area.x + full_area.width - width;
    }
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
      y = full_area.y + full_area.height - height;
    }

    // Apply margins
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
      x += state->margin.left;
    }
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
      x -= state->margin.right;
    }
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
      y += state->margin.top;
    }
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
      y -= state->margin.bottom;
    }

    // Update the scene node position
    wlr_scene_node_set_position(
        &layer_surface->scene_layer_surface->tree->node, x, y);

    // Send the configure event to the client
    wlr_layer_surface_v1_configure(wlr_surface, width, height);

    // Update the scene-graph element
    wlr_scene_layer_surface_v1_configure(layer_surface->scene_layer_surface,
                                         &full_area, &usable_area);
  }
}

static void handle_layer_map(struct wl_listener *listener, void *data) {
  struct tinywl_layer_surface *surface =
      wl_container_of(listener, surface, map);
  struct tinywl_server *server = surface->server;
  wlr_scene_node_set_enabled(&surface->scene_layer_surface->tree->node, true);
  arrange_layers(server);

  // Let it grab focus
  struct wlr_layer_surface_v1 *wlr_layer_surface =
      surface->scene_layer_surface->layer_surface;
  
  // Check if the client requested an exclusive keyboard grab
  if (wlr_layer_surface->current.keyboard_interactive ==
      ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
      
    // Tell the wlroots seat to route keyboard events to this surface
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard != NULL) {
      wlr_seat_keyboard_notify_enter(
          server->seat, 
          wlr_layer_surface->surface, 
          keyboard->keycodes, 
          keyboard->num_keycodes, 
          &keyboard->modifiers);
    }
  }
}

static void handle_layer_unmap(struct wl_listener *listener, void *data) {
  struct tinywl_layer_surface *surface =
      wl_container_of(listener, surface, unmap);
  wlr_scene_node_set_enabled(&surface->scene_layer_surface->tree->node, false);
  arrange_layers(surface->server);
}

static void handle_layer_destroy(struct wl_listener *listener, void *data) {
  struct tinywl_layer_surface *surface =
      wl_container_of(listener, surface, destroy);
  struct tinywl_server *server = surface->server;

  wl_list_remove(&surface->commit.link);
  wl_list_remove(&surface->map.link);
  wl_list_remove(&surface->unmap.link);
  wl_list_remove(&surface->destroy.link);

  // Remove from the layout list if it hasn't already been removed
  if (!wl_list_empty(&surface->link)) {
    wl_list_remove(&surface->link);
    wl_list_init(&surface->link);
  }

  free(surface);
  arrange_layers(server);
}

static void handle_layer_commit(struct wl_listener *listener, void *data) {
  (void)data;
  struct tinywl_layer_surface *layer_surface =
      wl_container_of(listener, layer_surface, commit);
  struct wlr_layer_surface_v1 *wlr_layer_surface =
      layer_surface->wlr_layer_surface;

  // An initial commit is the client accepting the configure size
  if (wlr_layer_surface->initial_commit) {
    arrange_layers(layer_surface->server);
    return;
  }

  // Do nothing if the surface isn't mapped yet
  if (!wlr_layer_surface->surface->mapped) {
    return;
  }

  // Re-arrange the layers of properties have changed
  if (wlr_layer_surface->current.committed != 0) {
    arrange_layers(layer_surface->server);
  }

  // TO DO: handle a request to change layers on the fly
}


static void server_new_layer_surface(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, new_layer_surface);
  struct wlr_layer_surface_v1 *wlr_layer_surface = data;

  // Safety, if no output
  if (wlr_layer_surface->output == NULL) {
    if (wl_list_empty(&server->outputs)) {
      wlr_log(WLR_ERROR, "No outputs so cannot create layer surface");
      wlr_layer_surface_v1_destroy(wlr_layer_surface);
      return;
    }
    struct tinywl_output *output =
        wl_container_of(server->outputs.next, output, link);
    wlr_layer_surface->output = output->wlr_output;
  }

  // Allocate our tracker
  struct tinywl_layer_surface *layer_surface =
      calloc(1, sizeof(*layer_surface));
  layer_surface->server = server;
  layer_surface->wlr_layer_surface = wlr_layer_surface;
  wlr_layer_surface->data = layer_surface;

  // Add it to the scene tree for the requested layer
  struct wlr_scene_tree *tree;
  switch (wlr_layer_surface->pending.layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
      tree = server->scene_background;
      break;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
      tree = server->scene_bottom;
      break;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
      tree = server->scene_top;
      break;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
      tree = server->scene_overlay;
      break;
    default:
      tree = server->scene_top;
      break;
  }
  layer_surface->scene_layer_surface =
      wlr_scene_layer_surface_v1_create(tree, wlr_layer_surface);
  if (!layer_surface->scene_layer_surface) {
    wlr_log(WLR_ERROR, "Failed to create scene layer surface");
    free(layer_surface);
    return;
  }

  // Add a pointer back to our wrapper
  layer_surface->scene_layer_surface->tree->node.data = layer_surface;

  // Add it to our custom tracker
  wl_list_insert(&server->layer_surfaces, &layer_surface->link);

  // Connect listeners
  layer_surface->commit.notify = handle_layer_commit;
  wl_signal_add(&wlr_layer_surface->surface->events.commit,
                &layer_surface->commit);

  layer_surface->map.notify = handle_layer_map;
  wl_signal_add(&wlr_layer_surface->surface->events.map,
                &layer_surface->map);

  layer_surface->unmap.notify = handle_layer_unmap;
  wl_signal_add(&wlr_layer_surface->surface->events.unmap,
                &layer_surface->unmap);

  layer_surface->destroy.notify = handle_layer_destroy;
  wl_signal_add(&wlr_layer_surface->events.destroy, &layer_surface->destroy);
}

// ----- PROTOCOLS -----

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

// For wlr_xdg_decoration_manager_v1
static void handle_new_toplevel_decoration(struct wl_listener *listener,
                                           void *data) {
  struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
  // Tell the client to draw its own window decorations
  wlr_xdg_toplevel_decoration_v1_set_mode(decoration, 
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}
// Invoked when an application tells the compositor a context menu has closed
static void xdg_popup_handle_destroy(struct wl_listener *listener, void *data) {
  struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);
  wl_list_remove(&popup->destroy.link);
  free(popup);
}


// Find the generic surface wrapper responsible for a given pixel
// Also set its type, and relative coordinates.
static void *desktop_surface_at(struct tinywl_server *server,
    double x, double y, struct wlr_surface **surface, 
    enum tinywl_surface_type *type, double *sx, double *sy) {
  
  // Let wlroots find the scene node for that pixel
  struct wlr_scene_node *node =
      wlr_scene_node_at(&server->scene->tree.node, x, y, sx, sy);
  if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
    return NULL;
  }

  // Get the wlroots surface
  struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
  struct wlr_scene_surface *scene_surface =
      wlr_scene_surface_try_from_buffer(scene_buffer);
  if (scene_surface == NULL) {
    return NULL;
  }
  *surface = scene_surface->surface;

  // Climb the tree
  struct wlr_scene_tree *tree = node->parent;
  while (tree != NULL) {
    if (tree->node.data != NULL) {
      enum tinywl_surface_type *surface_type = tree->node.data;
      if (*surface_type == TINYWL_SURFACE_TOPLEVEL ||
          *surface_type == TINYWL_SURFACE_POPUP    ||
          *surface_type == TINYWL_SURFACE_LAYER) {
        *type = *surface_type;
        return tree->node.data;
      }
    }
    tree = tree->node.parent;
  }

  return NULL;
}

// Cursor motion has four cases:
// interactive move/resize, pointer grab, DnD, passthrough
static void handle_cursor_motion(struct tinywl_server *server, uint32_t time) {
  struct wlr_seat *seat = server->seat;
  double x = server->cursor->x;
  double y = server->cursor->y;
  enum tinywl_cursor_mode mode = server->cursor_mode;
  struct wlr_surface *focused = seat->pointer_state.focused_surface;

  // TO DO: set up a listener for DnD icon surface's commit signal

  // interactive move or resize
  if (mode != TINYWL_CURSOR_PASSTHROUGH) {
    // The toplevel has not actually moved
    struct tinywl_toplevel *toplevel = server->grabbed_toplevel;

    // How much the cursor has moved
    double dx = x - server->grabbed_cursor_x;
    double dy = y - server->grabbed_cursor_y;

    double left = server->grabbed_toplevel->scene_tree->node.x;
    double top = server->grabbed_toplevel->scene_tree->node.y;

    if (mode == TINYWL_CURSOR_MOVE) {
      left += dx;
      top += dy;
      // Same width and height, so that's all we need to know
      wlr_scene_node_set_position(&server->preview_rect->node,
                                  (int)left, (int)top);
      return;
    }
  
    // Now the mode is TINYWL_CURSOR_RESIZE:
    struct wlr_box geom;
    wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geom);
    double right = left + geom.width;
    double bottom = top + geom.height;

    int min_width = toplevel->xdg_toplevel->current.min_width;
    if (min_width < 10) min_width = 50;
    int min_height = toplevel->xdg_toplevel->current.min_height;
    if (min_height < 10) min_height = 50;

    if (server->resize_edges & WLR_EDGE_TOP) {
      top += dy;
      if (bottom - top < min_height) top = bottom - min_height;
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
      bottom += dy;
      if (bottom - top < min_height) bottom = top + min_height;
    }

    if (server->resize_edges & WLR_EDGE_LEFT) {
      left -= dx;
      if (right - left < min_width) left = right - min_width;
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
      right += dx;
      if (right - left < min_width) right = left + min_width;
    }

    wlr_scene_node_set_position(&server->preview_rect->node,
                                (int)left, (int)top);
    wlr_scene_rect_set_size(server->preview_rect,
                            (int)(right-left), (int)(bottom-top));
    return;
  }


  // Implicit grab
  if ((seat->pointer_state.grab != NULL) &&
      (seat->pointer_state.grab->interface != NULL) &&
      (seat->pointer_state.button_count > 0)) {
    seat->pointer_state.grab->interface->motion(seat->pointer_state.grab,
                                                time, x, y);
    wlr_seat_pointer_notify_frame(seat);
    return;
  }

  // Normal motion

  double sx, sy;
  struct wlr_surface *surface = NULL;
  enum tinywl_surface_type type;

  void *wrapper = desktop_surface_at(server, y, y, &surface, &type, &sx, &sy);

  // notify that surface, if there is one
  if (surface) {
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
  } else {
    wlr_seat_pointer_clear_focus(seat);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
  }
    
  wlr_seat_pointer_notify_frame(seat);
}

// End an interaction and reset the cursor mode to passthrough.
static void end_interactive(struct tinywl_server *server) {
  if (server->cursor_mode == TINYWL_CURSOR_PASSTHROUGH) {
    return;
  }
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;

  // Put the window wherever the preview rectangle is
  if (server->preview_rect == NULL) {
    wlr_log(WLR_ERROR, "Interactive: end interaction with no rectangle.");
    return;
  }

  // Get the geometry
  int x = server->preview_rect->node.x;
  int y = server->preview_rect->node.y;
  int width = server->preview_rect->width;
  int height = server->preview_rect->height;

  // Immediately reposition
  wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);

  // Request the app update the size
  if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
  }

  // Destroy the preview rectangle
  wlr_scene_node_destroy(&server->preview_rect->node);
  server->preview_rect = NULL;

  // Clear the server fields that track state
  server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
  server->grabbed_toplevel = NULL;

  // Recalculate pointer focus
  handle_cursor_motion(server, 0);

  server->grabbed_toplevel = NULL;
}

// Set up an interactive move/resize
static void begin_interactive(struct tinywl_toplevel *toplevel,
                              enum tinywl_cursor_mode mode, uint32_t edges) {
  struct tinywl_server *server = toplevel->server;
  struct wlr_surface *focused_surface =
      server->seat->pointer_state.focused_surface;

  // fullscreen
  if (toplevel->is_fullscreen) {
    return;
  }

  // Unmaximize. Maximized windows do not remember their previous size.
  if (toplevel->is_maximized) {
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
    toplevel->is_maximized = false;
  }

  // Save initial data for the interaction
  server->grabbed_toplevel = toplevel;
  server->cursor_mode = mode;
  server->resize_edges = edges;
  server->grabbed_cursor_x = server->cursor->x;
  server->grabbed_cursor_y = server->cursor->y;

  // Draw a preview rectangle
  struct wlr_box geom;
  wlr_xdg_surface_get_geometry(toplevel->xdg_toplevel->base, &geom);
  server->preview_rect = wlr_scene_rect_create(
      server->scene_normal,
      geom.width, 
      geom.height,
      (float[]){0.13f, 0.67f, 0.6f, 0.4f}
  );
  wlr_scene_node_set_position(&server->preview_rect->node,
                              toplevel->scene_tree->node.x,
                              toplevel->scene_tree->node.y);
}

// For ending a DnD
static void handle_destroy_drag(struct wl_listener *listener,
                                       void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, destroy_drag);

  // Clear our active drag pointer
  server->current_drag = NULL;

  // Trigger the flag so the next mapped window maps at the cursor
  server->next_commit_at_cursor = true;

  // Disconnect this temporary listener until the next drag happens
  wl_list_remove(&server->destroy_drag.link);
  wl_list_init(&server->destroy_drag.link); // eliminate dangling pointers
}

// For starting a DnD
static void handle_start_drag(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, start_drag);
  struct wlr_drag *drag = data;
  struct wlr_drag_icon *icon = drag->icon;

  server->current_drag = drag;
  server->next_commit_at_cursor = false;

  if (icon) {
    // Generate the scene node and save its reference in the icon's data slot
    struct wlr_scene_tree *icon_tree =
        wlr_scene_drag_icon_create(&server->scene->tree, icon);
    icon->data = &icon_tree->node;
  }

  server->destroy_drag.notify = handle_destroy_drag;
  wl_signal_add(&drag->events.destroy, &server->destroy_drag);
}

// Clear keyboard focus from whatever surface currently holds it
static void unfocus_keyboard(struct tinywl_server *server) {
  struct wlr_seat *seat = server->seat;
  struct wlr_surface *focused_surface = seat->keyboard_state.focused_surface;

  // Clear key grabs, even if there is more than one keyboard
  struct tinywl_keyboard *kbd;
  wl_list_for_each(kbd, &server->keyboards, link) {
    kbd->grabbed_keycode = 0;
  }

  if (focused_surface == NULL) return;

  wlr_seat_keyboard_clear_focus(seat);

  // Deactivate visually based on surface type
  struct wlr_xdg_surface *xdg_surface =
      wlr_xdg_surface_try_from_wlr_surface(focused_surface);
  if (xdg_surface != NULL) {
    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL &&
        xdg_surface->toplevel != NULL) {
      wlr_xdg_toplevel_set_activated(xdg_surface->toplevel, false);
    }
  }
}

// focus_toplevel - only for keyboard focus
static void focus_toplevel(struct tinywl_toplevel *toplevel) {
  if ((toplevel == NULL) ||
      (toplevel->xdg_toplevel == NULL) ||
      (toplevel->xdg_toplevel->base == NULL)) {
    wlr_log(WLR_ERROR, "Trying to focus an invalid toplevel.");
    return;
  }
  struct tinywl_server *server = toplevel->server;

  // Unfocus the old window
  unfocus_keyboard(server);
  
  struct wlr_seat *seat = server->seat;
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
  struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
  struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

  if (prev_surface == surface) {
    return;
  }

  // Raise the new window
  wl_list_remove(&toplevel->link);
  wl_list_insert(&server->toplevels, &toplevel->link);
  if (toplevel->scene_tree) {
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
  }

  // Protocol-level activation, so dialogs can draw active borders/titles
  wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

  if (keyboard != NULL) {
    wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
        keyboard->num_keycodes, &keyboard->modifiers);
  } else {
    wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
  }
}

// Triggered by a relative pointer motion event
static void server_cursor_motion(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_motion);
  struct wlr_pointer_motion_event *event = data;
  wlr_cursor_move(server->cursor, &event->pointer->base,
                  event->delta_x, event->delta_y);
  handle_cursor_motion(server, event->time_msec);
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
}

// Triggered by a mouse click or release
static void server_cursor_button(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, cursor_button);
  struct wlr_pointer_button_event *event = data;
  uint32_t modifiers =
      wlr_keyboard_get_modifiers(wlr_seat_get_keyboard(server->seat));
  double sx, sy;
  struct wlr_surface *surface = NULL;
  void *wrapper = NULL;
  enum tinywl_surface_type type;

  wlr_log(WLR_DEBUG, "CURSOR: BUTTON EVENT");
  // Button releases whose corresponding press was grabbed

  // Release the button that started a mod+click interaction
  if (event->button == server->grabbed_active_button) {
    wlr_log(WLR_DEBUG, "CURSOR: grabbed button release");
    // it must be a button release, no need to check
    server->grabbed_active_button = 0;
    if (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH) {
      end_interactive(server);
    }
    return;
  }

  // Release the button that ended a mod+click interaction
  if (event->button == server->grabbed_cancel_button) {
    wlr_log(WLR_DEBUG, "CURSOR: cancel button release");
    server->grabbed_cancel_button = 0;
    return;
  }

  // a second press anywhere, that ends a mod+click interaction
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) &&
      (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH)) {
    wlr_log(WLR_DEBUG, "CURSOR: cancel interaction");
    server->grabbed_cancel_button = event->button;
    end_interactive(server);
    return;
  }

  // Set wrapper, surface and type.
  wrapper = desktop_surface_at(server, server->cursor->x, server->cursor->y,
      &surface, &type, &sx, &sy);

  // grab LOGO+click on a toplevel
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) &&
      (modifiers & WLR_MODIFIER_LOGO) &&
      (type == TINYWL_SURFACE_TOPLEVEL)) {
    wlr_log(WLR_DEBUG, "CURSOR: mod+click");
    server->grabbed_active_button = event->button;
    struct tinywl_toplevel *toplevel = wrapper;
    focus_toplevel(toplevel);
    if (event->button == BTN_LEFT) {
      begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
    } else {
      begin_interactive(toplevel, TINYWL_CURSOR_RESIZE,
                        WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
    }
    return;
  }

  // NOTIFY THE SEAT IN EVERY REMAINING CASE

  // Button release
  if (event->state == WLR_BUTTON_RELEASED) {
    wlr_log(WLR_DEBUG, "CURSOR: button release");
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);
    wlr_seat_pointer_notify_frame(server->seat);
    if (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH) {
      end_interactive(server);
    }
    return;
  }

  // Click on empty space
  if ((wrapper == NULL) || (surface == NULL)) {
    wlr_log(WLR_DEBUG, "CURSOR: click on empty space");
    wlr_seat_pointer_clear_focus(server->seat);
    unfocus_keyboard(server);
    // Bonus: click on a completely empty screen to get foot
    if (wl_list_empty(&server->toplevels)) spawn("foot");
    wlr_seat_pointer_notify_frame(server->seat);
    return;
  }

  // On toplevel
  if (type == TINYWL_SURFACE_TOPLEVEL) {
    wlr_log(WLR_DEBUG, "CURSOR: click on toplevel");
    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
    wlr_seat_pointer_notify_button(
        server->seat, event->time_msec, event->button, event->state);
    wlr_seat_pointer_notify_frame(server->seat);
    struct tinywl_toplevel *toplevel = wrapper;
    focus_toplevel(toplevel);
    return;
  }

  // On popup
  if (type == TINYWL_SURFACE_POPUP) {
    wlr_log(WLR_DEBUG, "CURSOR: click on popup");
    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
    wlr_seat_pointer_notify_button(
        server->seat, event->time_msec, event->button, event->state);
    wlr_seat_pointer_notify_frame(server->seat);
    return;
  }

  // On layer surface
  if (type == TINYWL_SURFACE_LAYER) {
    wlr_log(WLR_DEBUG, "CURSOR: click on layer");
    struct tinywl_layer_surface *layer = wrapper;

    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);

    // Give it keyboard focus if it wants
    if (layer->wlr_layer_surface->current.keyboard_interactive !=
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
      
      struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
      if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(
            server->seat, surface, keyboard->keycodes,
            keyboard->num_keycodes, &keyboard->modifiers);
      } else {
        wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
      }
    }
    wlr_seat_pointer_notify_button(
        server->seat, event->time_msec, event->button, event->state);
    wlr_seat_pointer_notify_frame(server->seat);
    return;
  }
  wlr_log(WLR_ERROR, "Click on surface of unknown type.");
}
  
// Function triggered by a pointer axis event, eg. scroll wheel.
static void server_cursor_axis(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, cursor_axis);
  struct wlr_pointer_axis_event *event = data;
  wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
                               event->orientation, event->delta,
                               event->delta_discrete, event->source);
  wlr_seat_pointer_notify_frame(server->seat);
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

  // Automically applies the new output state. 
  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  // Allocates and configures our state for this output.
  struct tinywl_output *output = calloc(1, sizeof(*output));
  output->wlr_output = wlr_output;
  output->server = server;

  // Set up listeners.
  output->frame.notify = output_frame;
  wl_signal_add(&wlr_output->events.frame, &output->frame);

  output->request_state.notify = output_request_state;
  wl_signal_add(&wlr_output->events.request_state, &output->request_state);

  output->destroy.notify = output_destroy;
  wl_signal_add(&wlr_output->events.destroy, &output->destroy);

  wl_list_insert(&server->outputs, &output->link);

  // Add this to the output layout.
  // Keep the simple default: arrange outputs left to right.
  struct wlr_output_layout_output *l_output =
      wlr_output_layout_add_auto(server->output_layout, wlr_output);
  struct wlr_scene_output *scene_output =
      wlr_scene_output_create(server->scene, wlr_output);

  wlr_scene_output_layout_add_output(server->scene_layout, l_output,
                                     scene_output);
}

// Function called when the surface is mapped, ready for display.
static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, map);
  struct tinywl_server *server = toplevel->server;

  // Position the window
  if (server->next_commit_at_cursor) {
    server->next_commit_at_cursor = false;
    int x = (int)server->cursor->x;
    int y = (int)server->cursor->y;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
  } else {
    wlr_scene_node_set_position(&toplevel->scene_tree->node, 50, 50);
  }

  // Add to our tracking list
  wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
  
  // Explicitly enable the window's visual node.
  wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);

  // Focus the window
  focus_toplevel(toplevel);
}


static void keyboard_refocus(struct tinywl_server *server) {
  struct wlr_seat *seat = server->seat;
  struct wlr_surface *focused_surface = seat->keyboard_state.focused_surface;

  // If someone is already focused
  if (focused_surface != NULL && focused_surface->mapped) {
    return;
  }

  struct tinywl_toplevel *toplevel;
  wl_list_for_each(toplevel, &server->toplevels, link) {
    if (!toplevel->scene_tree) {
      continue;
    }
    focus_toplevel(toplevel);
    return;
  }
  wlr_seat_keyboard_clear_focus(seat);
}

// Function called when the surface is unmapped (visually hidden/closed).
static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
  (void)data;
  struct tinywl_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
  struct tinywl_server *server = toplevel->server;

  // Always clear cursor grab if this window was being manipulated
  if (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH &&
      toplevel == server->grabbed_toplevel) {
    end_interactive(server);
  }

  // Always clear keyboard focus if it had it
  struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
  if (focused && focused == toplevel->xdg_toplevel->base->surface) {
    wlr_seat_keyboard_clear_focus(server->seat);
  }

  // Remove it from the compositor's list, if it hasn't been already
  if (!wl_list_empty(&toplevel->link)) {
    wl_list_remove(&toplevel->link);
    wl_list_init(&toplevel->link);
  }

  // TO DO: keyboard_refocus(server);
  // TO DO: same for mouse
}

// Function called when a toplevel is destroyed.
static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, destroy);
  struct tinywl_server *server = toplevel->server;

  // disconnect all listeners first, so they never fire again
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
  wl_list_remove(&toplevel->request_maximize.link);
  wl_list_init(&toplevel->request_maximize.link);
  wl_list_remove(&toplevel->request_fullscreen.link);
  wl_list_init(&toplevel->request_fullscreen.link);

  // clear cursor grab if this window was being manipulated
  if (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH &&
      toplevel == server->grabbed_toplevel) {
    end_interactive(server);
  }

  // clear keyboard focus if it had it
  struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
  if (focused && focused == toplevel->xdg_toplevel->base->surface) {
    wlr_seat_keyboard_clear_focus(server->seat);
  }

  // Remove it from the compositor's list, if it hasn't been already
  if (!wl_list_empty(&toplevel->link)) {
    wl_list_remove(&toplevel->link);
    wl_list_init(&toplevel->link);
  }

  free(toplevel);
}

// Function triggered when a client requests to begin an interactive move.
// TO DO: check it's a reasonable request?
static void xdg_toplevel_request_move(struct wl_listener *listener,
                                      void *data) {
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_move);
  begin_interactive(toplevel, TINYWL_CURSOR_MOVE, 0);
}

// Function triggered when a client requests to begin an interactive resize.
// TO DO: check it's a reasonable request?
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
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_fullscreen);
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

// Maximize. TO DO: check the usable area
// Do not save the previous size.
static void xdg_toplevel_request_maximize(struct wl_listener *listener,
                                          void *data) {
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_maximize);
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

// A new toplevel is created, or finally assigned its role
static void server_new_toplevel(struct tinywl_server *server,
                                struct wlr_xdg_toplevel *xdg_toplevel) {
  struct wlr_xdg_surface *xdg_surface = xdg_toplevel->base;

  struct tinywl_toplevel *toplevel = calloc(1, sizeof(struct tinywl_toplevel));
  if (toplevel == NULL) {
    wlr_log(WLR_ERROR, "Failed to allocate memory for toplevel tracking.");
    return;
  }

  toplevel->type = TINYWL_SURFACE_TOPLEVEL;
  toplevel->server = server;
  toplevel->xdg_toplevel = xdg_toplevel;

  // Attach to the root scene tree
  toplevel->scene_tree =
      wlr_scene_xdg_surface_create(server->scene_normal, xdg_surface);

  // Make compositor/wlr wrappers point to each other
  xdg_surface->data = toplevel;
  toplevel->scene_tree->node.data = toplevel;

  // Add to the our tracking list 
  wl_list_insert(&server->toplevels, &toplevel->link);

  // Connect listeners
  // Core Surface Layer Events (map, unmap)
  toplevel->map.notify = xdg_toplevel_map;
  wl_signal_add(&xdg_surface->surface->events.map, &toplevel->map);

  toplevel->unmap.notify = xdg_toplevel_unmap;
  wl_signal_add(&xdg_surface->surface->events.unmap, &toplevel->unmap);

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

// --------------

static void server_handle_xdg_map(struct wl_listener *listener, void *data) {
  // This fires when the client window physically draws its initial content buffer
  struct tinywl_toplevel_marker *marker = wl_container_of(listener, marker, map);
  struct wlr_xdg_surface *xdg_surface = marker->xdg_surface;
  struct tinywl_server *server = marker->server;

  // Now that it's mapping, the role is 100% guaranteed to be defined
  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    server_new_toplevel(server, xdg_surface->toplevel);
  } else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
    server_new_popup(server, xdg_surface->popup);
  }

  // Disconnect the temporary map listener and free the microscopic tracking label
  wl_list_remove(&marker->map.link);
  free(marker);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, new_xdg_surface);
  struct wlr_xdg_surface *xdg_surface = data;

  // If the app was fast and declared its role immediately, route it instantly
  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    server_new_toplevel(server, xdg_surface->toplevel);
    return;
  }
  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
    server_new_popup(server, xdg_surface->popup);
    return;
  }

  // If the role is unassigned, we track ONLY the map signal (NO commit loops!)
  struct tinywl_toplevel_marker *marker = calloc(1, sizeof(*marker));
  if (!marker) return;

  marker->server = server;
  marker->xdg_surface = xdg_surface;
  marker->map.notify = server_handle_xdg_map;
  wl_signal_add(&xdg_surface->surface->events.map, &marker->map);
}

// ----


// For xdg_activation_v1 - eg., click on a link opens a browser
static void server_request_activation(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = 
      wl_container_of(listener, server, request_activation);
  struct wlr_xdg_activation_v1_request_activate_event *event = data;

  // Verify the surface exists and is an XDG surface
  if (!event->surface ||
      !wlr_xdg_surface_try_from_wlr_surface(event->surface)) {
    return;
  }

  struct wlr_xdg_surface *xdg_surface = 
      wlr_xdg_surface_try_from_wlr_surface(event->surface);

  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg_surface->data) {
    struct tinywl_toplevel *toplevel = xdg_surface->data;
    struct wlr_scene_tree *scene_tree = toplevel->scene_tree;
  
    if (toplevel) {
      // Focus the window that the link is trying to open
      focus_toplevel(toplevel);
    }
  }
}

// For DnD
static void server_handle_request_start_drag(struct wl_listener *listener,
                                             void *data) {
  struct wlr_seat_request_start_drag_event *event = data;
  struct tinywl_server *server =
      wl_container_of(listener, server, request_start_drag);
  if (!wlr_seat_validate_pointer_grab_serial(server->seat, event->origin,
                                            event->serial)) {
    // click state does not match active seat state
    wlr_data_source_destroy(event->drag->source);
    return;
  }
  wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
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
// Ctrl+Alt+Fn is mapped to XKB_KEY_XF86Switch_VT_n
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

// Assume LOGO is held down and handle another key pressed.
static bool handle_quick_key(struct tinywl_server *server, uint32_t sym) {
  switch (sym) {
    case XKB_KEY_Return:
      spawn("foot");
      return true;
    case XKB_KEY_Escape:
      wl_display_terminate(server->wl_display);
      return true;
    case XKB_KEY_d:
      spawn("wofi --show drun");
      return true;
    case XKB_KEY_g:
      spawn("play -n synth square 600 trim 0 0.3 gain -20");
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

// Called when a client provides a cursor image
static void seat_request_cursor(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, request_cursor);
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  struct wlr_seat_client *focused_client =
      server->seat->pointer_state.focused_client;
  if (focused_client == event->seat_client) {
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
  // Capabilities
  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&server->keyboards)) {
    caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  }
  wlr_seat_set_capabilities(server->seat, caps);
}

int main(int argc, char *argv[]) {
  wlr_log_init(WLR_DEBUG, NULL);

  // Command line argument parser
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

  // Initialize server
  struct tinywl_server server = {0};

  server.wl_display = wl_display_create();
  if (!server.wl_display) {
    wlr_log(WLR_ERROR, "Failed to create Wayland display");
    return 1;
  }

  server.backend = wlr_backend_autocreate(server.wl_display, &server.session);
  if (server.backend == NULL) {
    wlr_log(WLR_ERROR, "Failed to create wlr_backend");
    return 1;
  }

  server.renderer = wlr_renderer_autocreate(server.backend);
  if (server.renderer == NULL) {
    wlr_log(WLR_ERROR, "Failed to create wlr_renderer");
    return 1;
  }
  wlr_renderer_init_wl_display(server.renderer, server.wl_display);

  server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
  if (server.allocator == NULL) {
    wlr_log(WLR_ERROR, "Failed to create wlr_allocator");
    return 1;
  }

  // Data Device Manager Protocol, for copy/paste and DnD
  wlr_data_device_manager_create(server.wl_display);

  // wlroots interface for clients to allocate surfaces.
  wlr_compositor_create(server.wl_display, 5, server.renderer);

  // wlroots interface for subsurfaces
  wlr_subcompositor_create(server.wl_display);

  // wlroots utility to arrange monitor screens
  server.output_layout = wlr_output_layout_create();
  wl_list_init(&server.outputs);
  server.new_output.notify = server_new_output;
  wl_signal_add(&server.backend->events.new_output, &server.new_output);

  // Create a scene graph, used by wlr_scene_output_commit() to render
  server.scene = wlr_scene_create();
  if (server.scene ==  NULL) {
    wlr_log(WLR_ERROR, "Failed to create scene graph");
    return 1;
  }

  // Create stacked Z-index buckets, for wlr_layer_shell_unstable_v1
  server.scene_background = wlr_scene_tree_create(&server.scene->tree);
  server.scene_bottom     = wlr_scene_tree_create(&server.scene->tree);
  server.scene_normal     = wlr_scene_tree_create(&server.scene->tree);
  server.scene_top        = wlr_scene_tree_create(&server.scene->tree);
  server.scene_overlay    = wlr_scene_tree_create(&server.scene->tree);

  // Initialize tracking list
  wl_list_init(&server.toplevels);

  // Activate
  wlr_scene_node_set_enabled(&server.scene_background->node, true);
  wlr_scene_node_set_enabled(&server.scene_bottom->node, true);
  wlr_scene_node_set_enabled(&server.scene_normal->node, true);
  wlr_scene_node_set_enabled(&server.scene_top->node, true);
  wlr_scene_node_set_enabled(&server.scene_overlay->node, true);

  // Configure the cursor
  server.cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
  server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
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

  // Configure the seat
  wl_list_init(&server.keyboards);
  server.new_input.notify = server_new_input;
  wl_signal_add(&server.backend->events.new_input, &server.new_input);
  server.seat = wlr_seat_create(server.wl_display, "seat0");
  server.request_cursor.notify = seat_request_cursor;
  wl_signal_add(&server.seat->events.request_set_cursor,
                &server.request_cursor);
  server.request_set_selection.notify = seat_request_set_selection;
  wl_signal_add(&server.seat->events.request_set_selection,
                &server.request_set_selection);

  // Drag-and-Drop (DnD) Input Intercept Engine
  server.request_start_drag.notify = server_handle_request_start_drag;
  wl_signal_add(&server.seat->events.request_start_drag,
                &server.request_start_drag);
  server.start_drag.notify = handle_start_drag;
  wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

  // PROTOCOLS

  // wlr_xdg_shell
  server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
  if (!server.xdg_shell) {
    wlr_log(WLR_ERROR, "Unable to create the XDG shell interface.");
    return 1;
  }
  
  server.new_xdg_surface.notify = server_new_xdg_surface;
  wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

  // xdg_activation_v1.xml
  server.xdg_activation = wlr_xdg_activation_v1_create(server.wl_display);
  server.request_activation.notify = server_request_activation;
  wl_signal_add(&server.xdg_activation->events.request_activate, 
                &server.request_activation);

  // wlr-screencopy-unstable-v1
  wlr_screencopy_manager_v1_create(server.wl_display);

  // zxdg_output_manager_v1
  wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

  // wlr_xdg_decoration_manager_v1
  server.xdg_decoration_manager =
      wlr_xdg_decoration_manager_v1_create(server.wl_display);
  server.new_toplevel_decoration.notify = handle_new_toplevel_decoration;
  wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration, 
                &server.new_toplevel_decoration);

  // wlr_idle_notifier_v1
  server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);

  // wlr_layer_shell_unstable_v1
  wl_list_init(&server.layer_surfaces);
  server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
  server.new_layer_surface.notify = server_new_layer_surface;
  wl_signal_add(&server.layer_shell->events.new_surface,
                &server.new_layer_surface);

  // Automatically link monitors to the virtual desktop tree
  // This belongs at the end of the initialization sequence
  server.scene_layout =
      wlr_scene_attach_output_layout(server.scene, server.output_layout);

  // Add a Unix socket to the Wayland display.
  const char *socket = wl_display_add_socket_auto(server.wl_display);
  if (!socket) {
    wlr_backend_destroy(server.backend);
    return 1;
  }

  // Start the backend.
  // This will enumerate outputs and inputs, become the DRM master, etc. 
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
  // Run the Wayland event loop.
  wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
  wl_display_run(server.wl_display);

  // Destroy all clients then shut down the server. 
  wl_display_destroy_clients(server.wl_display);
  wlr_scene_node_destroy(&server.scene->tree.node);
  wlr_xcursor_manager_destroy(server.cursor_mgr);
  wlr_output_layout_destroy(server.output_layout);
  wl_display_destroy(server.wl_display);
  return 0;
}
