/*
 * core/key_input.h — keyboard input bypassing Allegro's keypressed()
 *
 * Reads Allegro's key[] array directly per-scancode, detects rising
 * edges, and translates to ASCII with shift / caps lock state. Avoids
 * the s38 _stubinfo bug that keypressed()/readkey() currently trip.
 *
 * Usage per frame:
 *   key_input_poll(&state);  // updates internal edge state + caps
 *   while ((c = key_input_take(&state)) != 0) {
 *       // handle character c
 *   }
 */
#ifndef PINECONE_CORE_KEY_INPUT_H
#define PINECONE_CORE_KEY_INPUT_H

#include <allegro.h>

#define KEY_INPUT_QUEUE  32

typedef struct {
    unsigned char prev[KEY_MAX];   /* 1 if scancode was down last frame */
    int  caps_lock;                /* current toggle state */
    char queue[KEY_INPUT_QUEUE];
    int  qhead, qtail;
} key_input_t;

void key_input_init(key_input_t *ks);
void key_input_poll(key_input_t *ks);   /* once per frame */
int  key_input_take(key_input_t *ks);   /* returns 0 if no chars pending */

#endif
