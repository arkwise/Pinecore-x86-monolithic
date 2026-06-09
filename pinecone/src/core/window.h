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
} window_t;

#endif
