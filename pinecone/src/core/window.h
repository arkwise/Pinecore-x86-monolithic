/*
 * core/window.h — common window_t typedef shared between the shell
 * (main.c) and individual app modules under apps/.
 *
 * The window manager itself (draw_window_frame, z-order helpers,
 * win_is_active) lives in main.c for now and is reached via unity
 * include — prompt.c sees those as same-TU static functions.
 */
#ifndef PINECONE_CORE_WINDOW_H
#define PINECONE_CORE_WINDOW_H

#include <allegro.h>

typedef struct {
    int x, y, w, h;
    int minimized;
    int closed;
    int maximized;
    int drag_active;
    int drag_off_x, drag_off_y;
    int restore_x, restore_y, restore_w, restore_h;
    /* Resize support — Win95-style sizing grip in the bottom-right.
     * resize_active: set while the user is dragging the grip.
     * resize_off_w/h: window dimensions captured at drag-start so the
     *   new size = captured + (mouse delta). Keeps the resize stable
     *   even if the cursor briefly leaves the grip's pixel area.
     * min_w/min_h: 0 = use defaults (160×96). Apps can raise these
     *   so e.g. FreeCom v86 can't shrink below readable 80×25. */
    int resize_active;
    int resize_off_w, resize_off_h;
    int resize_mouse_x, resize_mouse_y;
    int min_w, min_h;
} window_t;

#endif
