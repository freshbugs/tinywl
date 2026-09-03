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

// For wlr_layer_shell_unstable_v1
#include <wlr/types/wlr_layer_shell_v1.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"

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

  // For resizing "wireframe layout"
  struct wlr_scene_rect *resize_preview_rect;

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

  // For drag or resize
  double grabbed_cursor_x;
  double grabbed_cursor_y;

  // Up to two grabbed mouse buttons - to start and cancel an interaction
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

  // Added for moving windows
  struct wlr_box initial_geom;

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

// For popup, to either xdg or layer
struct tinywl_popup {
  enum tinywl_surface_type type;
  struct wlr_xdg_popup *xdg_popup;
  struct wlr_scene_tree *scene_tree;
  struct wl_listener destroy;
};

struct tinywl_layer_surface {
  enum tinywl_surface_type type;
  struct wl_list link;
  struct tinywl_server *server;
  struct wlr_layer_surface_v1 *wlr_layer_surface;
  struct wlr_scene_layer_surface_v1 *scene_layer_surface;

  bool was_mapped_previously;

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener commit;
  struct wl_listener new_popup;
};

struct tinywl_incomplete_surface {
  struct wl_listener commit;
  struct tinywl_server *server;
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

void arrange_layers(struct tinywl_server *server) {
  if (wl_list_empty(&server->outputs)) {
    return; 
  }

  // Get our single laptop output
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
    struct wlr_layer_surface_v1_state *state = &wlr_surface->current;

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
    struct wlr_layer_surface_v1_state *state = &wlr_surface->current;

    // Fall back to requested sizes if anchors don't dictate stretching
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
    int x = usable_area.x;
    int y = usable_area.y;

    // full-screen or non-exclusive layer
    if (state->exclusive_zone <= 0) {
      x = full_area.x;
      y = full_area.y;
    }

    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
      x = (state->exclusive_zone > 0 ? usable_area.x + usable_area.width : full_area.width) - width;
    }
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
      y = (state->exclusive_zone > 0 ? usable_area.y + usable_area.height : full_area.height) - height;
    }

    // Apply margins
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) x += state->margin.left;
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) x -= state->margin.right;
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) y += state->margin.top;
    if (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) y -= state->margin.bottom;

    // Update the scene node position
    wlr_scene_node_set_position(&layer_surface->scene_layer_surface->tree->node, x, y);

    // Send the configure event to the client
    wlr_layer_surface_v1_configure(wlr_surface, width, height);

    // Update the scene-graph element
    wlr_scene_layer_surface_v1_configure(layer_surface->scene_layer_surface,
                                         &full_area, &usable_area);
  }
}







// For wlr_layer_shell_unstable_v1 
// Called once when a layer surface becomes visible
static void handle_layer_map(struct wl_listener *listener, void *data) {
  struct tinywl_layer_surface *surface =
      wl_container_of(listener, surface, map);
  struct tinywl_server *server = surface->server;
  wlr_scene_node_set_enabled(&surface->scene_layer_surface->tree->node, true);
  arrange_layers(server);
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
  wl_list_remove(&surface->commit.link);
  wl_list_remove(&surface->map.link);
  wl_list_remove(&surface->unmap.link);
  wl_list_remove(&surface->destroy.link);
  wl_list_remove(&surface->link);
  struct tinywl_server *server = surface->server;
  free(surface);
  arrange_layers(server);
}


static void server_popup_destroy(struct wl_listener *listener, void *data) {
  struct tinywl_popup *popup = wl_container_of(listener, popup, destroy);
  wl_list_remove(&popup->destroy.link);
  free(popup);
}


// For wlr_layer_shell_unstable_v1
static void handle_layer_commit(struct wl_listener *listener, void *data) {
  struct tinywl_layer_surface *layer_surface =
      wl_container_of(listener, layer_surface, commit);
  struct wlr_layer_surface_v1 *wlr_layer_surface = layer_surface->wlr_layer_surface;

  if (wlr_layer_surface->initial_commit) {
    arrange_layers(layer_surface->server);
    return;
  }

  // Skip if the surface isn't ready
  if (!wlr_layer_surface->surface->mapped) {
    return;
  }

  // Re-arrange the layers of properties have changed
  if (wlr_layer_surface->current.committed != 0) {
    arrange_layers(layer_surface->server);
  }
}

// Called when either a xdg or layer surface requests to open a popup
static void server_new_popup(struct tinywl_server *server,
                              struct wlr_xdg_surface *xdg_surface) {
  // calloc my custom wrapper
  struct wlr_xdg_popup *xdg_popup = xdg_surface->popup;
  struct tinywl_popup *popup = calloc(1, sizeof(*popup));
  if (popup == NULL) {
    wlr_log(WLR_ERROR, "Failed to allocate memory for popup tracking.");
    return;
  }
  
  popup->type = TINYWL_SURFACE_POPUP;
  popup->xdg_popup = xdg_popup;

  // Find the parent scene tree and attach the popup
  // The parent could be an layer surface, an xdg surface, or another popup
  struct wlr_scene_tree *parent_tree = NULL;
  struct wlr_surface *raw_parent = xdg_popup->parent;

  // Try to convert raw_parent to an XDG surface wrapper
  struct wlr_xdg_surface *parent_xdg =
      wlr_xdg_surface_try_from_wlr_surface(raw_parent);

  // Try to convert raw_parent to a layer surface wrapper
  struct wlr_layer_surface_v1 *parent_layer =
      wlr_layer_surface_v1_try_from_wlr_surface(raw_parent);

  // Attach the popup to the tree, based on its parent's type
  if (parent_xdg != NULL) {
    if (parent_xdg->data == NULL) {
      wlr_log(WLR_ERROR, "New popup's XDG parent has no data.");
      free(popup);
      return;
    }
    if (parent_xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
      struct tinywl_toplevel *parent_toplevel = parent_xdg->data;
      parent_tree = parent_toplevel->scene_tree;
    } else if (parent_xdg->role == WLR_XDG_SURFACE_ROLE_POPUP) {
      struct tinywl_popup *parent_popup = parent_xdg->data;
      parent_tree = parent_popup->scene_tree;
    }
  } else if (parent_layer != NULL) {
    if (parent_layer-> data == NULL) {
      wlr_log(WLR_ERROR, "New popup's layer parent has no data.");
      free(popup);
      return;
    }
    struct tinywl_layer_surface *layer = parent_layer->data;
    parent_tree = layer->scene_layer_surface->tree;
  } else {
    wlr_log(WLR_ERROR, "Unknown surface type tried to spawn a popup.");
    free(popup);
    return;
  }
   
  popup->scene_tree =
      wlr_scene_xdg_surface_create(parent_tree, xdg_surface);


  // Avoid going offscreen.
  struct wlr_box output_box;
  // Find the relevant monitor (though I haven't set up multi-monitors).
  struct wlr_output *output = wlr_output_layout_output_at(
      server->output_layout, 
      server->cursor->x,
      server->cursor->y
  );
  if (output) {
    wlr_output_layout_get_box(server->output_layout, output, &output_box);
    wlr_xdg_popup_unconstrain_from_box(xdg_popup, &output_box);
  }
  
  // Make compositor/wlr wrappers point to each other
  xdg_surface->data = popup;
  popup->scene_tree->node.data = popup;

  // Connect a listener
  popup->destroy.notify = server_popup_destroy;
  wl_signal_add(&xdg_surface->events.destroy, &popup->destroy);
}


static void layer_surface_handle_new_popup(struct wl_listener *listener, void *data) {
  struct tinywl_layer_surface *layer = 
      wl_container_of(listener, layer, new_popup);
  struct wlr_xdg_popup *xdg_popup = data;
  server_new_popup(layer->server, xdg_popup->base);
}


static void server_new_layer_surface(struct wl_listener *listener, void *data) {
  struct tinywl_server *server =
      wl_container_of(listener, server, new_layer_surface);
  struct wlr_layer_surface_v1 *wlr_layer_surface = data;

  // Allocate our tracker
  struct tinywl_layer_surface *layer_surface =
      calloc(1, sizeof(*layer_surface));
  layer_surface->server = server;
  layer_surface->wlr_layer_surface = wlr_layer_surface;

  // Backup default output
  if (wlr_layer_surface->output == NULL) {
    struct tinywl_output *output =
        wl_container_of(server->outputs.next, output, link);
    wlr_layer_surface->output = output->wlr_output;
  }

  // Add it to our custom tracker
  wl_list_insert(&server->layer_surfaces, &layer_surface->link);

  // Add it to the scene tree and make them point to each other
  // TO DO: pick the right layer, using wlr_layer_surface->pending.layer
  struct wlr_scene_tree *layer_tree = server->scene_top;
  layer_surface->scene_layer_surface =
      wlr_scene_layer_surface_v1_create(layer_tree, wlr_layer_surface);
  layer_surface->scene_layer_surface->tree->node.data = layer_surface;

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

  layer_surface->new_popup.notify = layer_surface_handle_new_popup;
  wl_signal_add(&wlr_layer_surface->events.new_popup,
                &layer_surface->new_popup);
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

// End an interaction and reset the cursor mode to passthrough.
static void end_interactive(struct tinywl_server *server) {
  if (server->cursor_mode == TINYWL_CURSOR_PASSTHROUGH) {
    return;
  }
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;

  // Put the window wherever the preview rectangle is
  if (server->resize_preview_rect == NULL) {
    wlr_log(WLR_ERROR, "Interactive: end interaction with no rectangle.");
    return;
  }

  // Get the geometry
  int x = server->resize_preview_rect->node.x;
  int y = server->resize_preview_rect->node.y;
  int width = server->resize_preview_rect->width;
  int height = server->resize_preview_rect->height;

  wlr_scene_node_destroy(&server->resize_preview_rect->node);
  server->resize_preview_rect = NULL;

  // Immediately reposition
  wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);

  // Request the app update the size (no need if it's a MOVE).
  if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
  }

  // Clear the server fields that track state
  server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;
  server->grabbed_toplevel = NULL;

  // Find what sits under the mouse pointer
  double sx, sy;
  struct wlr_surface *surface = NULL;
  struct tinywl_toplevel *hover_toplevel = desktop_toplevel_at(
      server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

  if (!hover_toplevel) {
    // Over empty space, restore default cursor arrow graphics
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
  }

  if (surface) {
    // Restore pointer focus
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

  // Make a preview rectangle
  server->resize_preview_rect =
      wlr_scene_rect_create(&server->scene->tree, 1, 1, 
                            (float[]){0.13f, 0.67f, 0.6f, 0.4f});
}

static void handle_cursor_motion(struct tinywl_server *server, uint32_t time) {

  struct wlr_seat *seat = server->seat;
  // Current cursor coordinates
  int cx = server->cursor->x; 
  int cy = server->cursor->y;
  enum tinywl_cursor_mode mode = server->cursor_mode;

  // If an active DnD is happening
  if (server->current_drag) {
    // position the icon
    if (server->current_drag->icon) {
      struct wlr_drag_icon *icon = seat->drag->icon;
      int dx = icon->surface->current.dx;
      int dy = icon->surface->current.dy;
      struct wlr_scene_node *icon_node = icon->data;
      if (icon_node) {
        wlr_scene_node_set_position(icon_node, cx+dx, cy+dy);
      }
    }

    // find the surface under the cursor
    double sx = 0;
    double sy = 0;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node, server->cursor->x, server->cursor->y,
        &sx, &sy);
    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
      struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
      struct wlr_scene_surface *scene_surface =
          wlr_scene_surface_try_from_buffer(scene_buffer);
      if (scene_surface) {
        surface = scene_surface->surface;
      }
    }

    // notify that surface, if there is one
    if (!surface) {
      wlr_seat_pointer_clear_focus(seat);
    } else {
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
      wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    }
    
    wlr_seat_pointer_notify_frame(seat);
    return;
  }

  // If moving the mouse normally
  if (mode == TINYWL_CURSOR_PASSTHROUGH) {
    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, cx, cy, &sx, &sy);
    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
      struct wlr_scene_buffer *scene_buffer =
          wlr_scene_buffer_from_node(node);
      struct wlr_scene_surface *scene_surface =
          wlr_scene_surface_try_from_buffer(scene_buffer);
      if (scene_surface) {
        surface = scene_surface->surface;
      }
    }
    if (!surface) {
      // Hovering over empty space
      wlr_seat_pointer_clear_focus(seat);
      wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
    } else {
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
      wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    }
    return;
  }

  // Otherwise, a move or resize interaction is happening.

  // Get the toplevel
  struct tinywl_toplevel *toplevel = server->grabbed_toplevel;

  if (toplevel == NULL) {
    wlr_log(WLR_ERROR, "Interacting with NULL toplevel");
    end_interactive(server);
    return;
  }

  // Get the original geometry from the start of the interaction
  int x = server->grabbed_toplevel->initial_geom.x;
  int y = server->grabbed_toplevel->initial_geom.y;
  int width = toplevel->initial_geom.width;
  int height = toplevel->initial_geom.height;

  // How much the cursor has moved
  int dx = cx - server->grabbed_cursor_x;
  int dy = cy - server->grabbed_cursor_y;

  switch (mode) {
    case TINYWL_CURSOR_MOVE:
      x += dx;
      y += dy;
      break;
    case TINYWL_CURSOR_RESIZE:
      if (server->resize_edges & WLR_EDGE_TOP) {
        height -= dy;
        y += dy;
      } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        height += dy;
      }
      if (server->resize_edges & WLR_EDGE_LEFT) {
        width -= dx;
        x += dx;
      } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        width += dx;
      }
      int min_width = toplevel->xdg_toplevel->current.min_width;
      int min_height = toplevel->xdg_toplevel->current.min_height;
      if (width < min_width) width = min_width;
      if (height < min_height) height = min_height;
      if (width < 50) width = 50;
      if (height < 50) height = 50;
      break;
    default:
      wlr_log(WLR_ERROR, "Unknown interaction mode.");
      return;
  }
  // Draw the preview rectangle at x,y with width,height 
  wlr_scene_node_set_position(&server->resize_preview_rect->node, x, y);
  wlr_scene_rect_set_size(server->resize_preview_rect, width, height);
}

// For ending a DnD
static void server_handle_destroy_drag(struct wl_listener *listener,
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
static void server_handle_start_drag(struct wl_listener *listener, void *data) {
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

  server->destroy_drag.notify = server_handle_destroy_drag;
  wl_signal_add(&drag->events.destroy, &server->destroy_drag);
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

// Focus: Unfocus old stuff, raise, and notify
static void focus_toplevel(struct tinywl_toplevel *toplevel) {
  if ((toplevel == NULL) ||
      (toplevel->xdg_toplevel == NULL) ||
      (toplevel->xdg_toplevel->base == NULL)) {
    wlr_log(WLR_ERROR, "Trying to focus an invalid toplevel.");
    return;
  }
  
  struct tinywl_server *server = toplevel->server;
  struct wlr_seat *seat = server->seat;
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
  struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
  struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

  if (prev_surface == surface) {
    return;
  }

  // Unfocus the old window
  unfocus_toplevel(server);

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

static bool get_wrapper_surface_and_type(struct wlr_scene_node *node, 
                                         void **out_wrapper,
                                         struct wlr_surface **out_surface,
                                         enum tinywl_surface_type *out_type) {
  if (node == NULL) {
    *out_wrapper = NULL;
    *out_surface = NULL;
    return false;
  }

  // Get the raw underlying surface ONLY if it's a buffer node
  *out_surface = NULL;
  if (node->type == WLR_SCENE_NODE_BUFFER) {
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (scene_surface != NULL) {
      *out_surface = scene_surface->surface;
    }
  }

  // Climb the parent trees to extract our custom tracker data
  struct wlr_scene_node *current = node;
  while (current != NULL) {
    if (current->data != NULL) {
      // Safe because 'type' is the first member of ALL our wrappers
      *out_type = *(enum tinywl_surface_type *)current->data;
      *out_wrapper = current->data;
      return true;
    }

    if (current->parent == NULL) {
      break;
    }
    current = &current->parent->node;
  }

  *out_wrapper = NULL;
  return false;
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
  struct wlr_scene_node *node = wlr_scene_node_at(
      &server->scene->tree.node, server->cursor->x, server->cursor->y, &sx, &sy);
  void *wrapper = NULL;
  enum tinywl_surface_type type;

  // Button releases where the corresponding press was grabbed

  // Release the button that started a mod+click interaction
  if (event->button == server->grabbed_active_button) {
    // it must be a button release, no need to check
    server->grabbed_active_button = 0;
    if (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH) {
      end_interactive(server);
    }
    return;
  }

  // Release the button that canceled a mod+click interaction
  if (event->button == server->grabbed_cancel_button) {
    server->grabbed_cancel_button = 0;
    return;
  }

  // a second press anywhere, that cancels a mod+click interaction
  // TO DO: maybe restore the pre-interaction geometry?
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) &&
      (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH)) {
    server->grabbed_cancel_button = event->button;
    end_interactive(server);
    return;
  }

  // Set wrapper, surface and type.
  bool on_empty_space =
      !get_wrapper_surface_and_type(node, &wrapper, &surface, &type);

  // grab LOGO+click on a toplevel
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) &&
      (modifiers & WLR_MODIFIER_LOGO) &&
      (type == TINYWL_SURFACE_TOPLEVEL)) {
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

  // In every other case, notify the seat
  wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                 event->button, event->state);

  // Click on empty space
  if ((event->state == WL_POINTER_BUTTON_STATE_PRESSED) && on_empty_space) {
    wlr_seat_keyboard_clear_focus(server->seat);
    wlr_seat_pointer_notify_clear_focus(server->seat);
    if (wl_list_empty(&server->toplevels)) {
      spawn("foot");
    }
    return;
  }

  // Button release
  if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    if (server->cursor_mode != TINYWL_CURSOR_PASSTHROUGH) {
      end_interactive(server);
    }
    return;
  }

  // Otherwise, check the type
  switch (type) {
    case TINYWL_SURFACE_TOPLEVEL: {
      struct tinywl_toplevel *toplevel = wrapper;
      focus_toplevel(toplevel);
      break;
    }
    case TINYWL_SURFACE_POPUP: {
      struct tinywl_popup *popup = wrapper;
      wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
      wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
      break;
    }
    case TINYWL_SURFACE_LAYER: {
      struct tinywl_layer_surface *layer = wrapper;
      wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
      // Route keyboard focus if the layer requested interactivity
      if (layer->wlr_layer_surface->current.keyboard_interactive !=
          ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        wlr_seat_keyboard_notify_enter(
            server->seat, 
            surface, 
            NULL, 0, NULL
        );
      }
      break;
    }
    default:
      wlr_log(WLR_ERROR, "Click on surface of unknown type.");
  }
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

  // Background color
  float background_color[4] = {0.02f, 0.20f, 0.20f, 1.0f};
  wlr_scene_rect_create(server->scene_background,
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

  // Position the window
  if (server->next_commit_at_cursor) {
    // Intercept placement after drag, added for DnD and tearoff tabs
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
      focus_toplevel(next_toplevel);
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
      focus_toplevel(next_toplevel);
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

// Called every time the client updates its surface buffer.
// Adjust the boundaries of the scene graph node so the frame can be drawn.
static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
  struct tinywl_toplevel *toplevel =
      wl_container_of(listener, toplevel, commit);
  struct tinywl_server *server = toplevel->server;
  struct wlr_xdg_surface *xdg_surface = toplevel->xdg_toplevel->base;

  // Do nothing on an initial commit
  if (xdg_surface->initial_commit) {
    return;
  }
}

// For wlr_xdg_decoration_manager_v1
static void handle_new_toplevel_decoration(struct wl_listener *listener, void *data) {
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

// Helper function
struct wlr_scene_tree *get_scene_tree_for_xdg_surface(struct wlr_xdg_surface *xdg_surface) {
    if (!xdg_surface) return NULL;

    if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        // If the parent is a main window, walk up to its view wrapper
        // Note: tinywl assigns xdg_surface->data = view in server_new_xdg_surface
        struct tinywl_toplevel *toplevel = xdg_surface->data;
        return toplevel ? toplevel->scene_tree : NULL;
    } 
    else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        // If the parent is another popup, we have to find its wrapper
        // You can look it up via the node tree data we saved earlier
        struct wlr_scene_tree *popup_scene = xdg_surface->data; // or find via custom tracking
        return popup_scene;
    }

    return NULL;
}

// A new toplevel is created, or finally assigned its role
static void server_new_toplevel(struct tinywl_server *server,
                                struct wlr_xdg_surface *xdg_surface) {
  struct tinywl_toplevel *toplevel = calloc(1, sizeof(struct tinywl_toplevel));
  if (toplevel == NULL) {
    wlr_log(WLR_ERROR, "Failed to allocate memory for toplevel tracking.");
    return;
  }

  toplevel->type = TINYWL_SURFACE_TOPLEVEL;
  toplevel->server = server;
  toplevel->xdg_toplevel = xdg_surface->toplevel;

  // Attach to the root scene tree
  toplevel->scene_tree = wlr_scene_xdg_surface_create(server->scene_normal,
                                                      xdg_surface);

  // Make compositor/wlr wrappers point to each other
  xdg_surface->data = toplevel;
  toplevel->scene_tree->node.data = toplevel;

  // Add to the our tracking list 
  wl_list_insert(&server->toplevels, &toplevel->link);

  // Connect listeners
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

static void server_handle_xdg_initial_commit(struct wl_listener *listener, void *data) {
  struct tinywl_incomplete_surface *incomplete = 
      wl_container_of(listener, incomplete, commit);
  struct wlr_xdg_surface *xdg_surface = data;

  // If the client committed but hasn't declared its role, keep waiting
  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_NONE) {
    wlr_log(WLR_DEBUG, "Role not yet declared.");
    return;
  }

  // The role is finally declared. Disconnect this temporary listener setup.
  wl_list_remove(&incomplete->commit.link);

  // The layout engines handle configuration
  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
    server_new_popup(incomplete->server, xdg_surface);
  } else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    server_new_toplevel(incomplete->server, xdg_surface);
  }

  free(incomplete); // Clean up the temporary tracker memory
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
  struct tinywl_server *server = wl_container_of(listener, server, new_xdg_surface);
  struct wlr_xdg_surface *xdg_surface = data;

  // If the client declares its role instantly, process right away
  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
    server_new_popup(server, xdg_surface);
    return;
  }
  if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    server_new_toplevel(server, xdg_surface);
    return;
  }

  // If not, bind a temporary listener that waits for the role commit
  struct tinywl_incomplete_surface *incomplete = calloc(1, sizeof(*incomplete));
  incomplete->server = server;
  incomplete->commit.notify = server_handle_xdg_initial_commit;
  wl_signal_add(&xdg_surface->surface->events.commit, &incomplete->commit);
}

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

  wlr_log(WLR_DEBUG, "wl_list_length %d.", wl_list_length(toplevels));
/*
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
  focus_toplevel(front);
*/
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

  // Create a scene graph.
  // wlr_scene_output_commit() uses this to render a frame.
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
  server.cursor_manager = wlr_xcursor_manager_create(NULL, 24);
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
  server.start_drag.notify = server_handle_start_drag;
  wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

  // PROTOCOLS

  // wlr_xdg_shell
  server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
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
  wlr_xcursor_manager_destroy(server.cursor_manager);
  wlr_output_layout_destroy(server.output_layout);
  wl_display_destroy(server.wl_display);
  return 0;
}
