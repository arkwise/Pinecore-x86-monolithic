#include "core/key_input.h"
#include <string.h>

/* US-QWERTY scancode → ASCII tables. Indexed by Allegro KEY_* constants.
 * Entries that are 0 mean "no ASCII for this scancode" (modifiers, F-keys,
 * arrows, etc.). Non-zero entries are emitted on key-rising-edge. */
static const char unshifted[KEY_MAX] = {
    [KEY_A] = 'a', [KEY_B] = 'b', [KEY_C] = 'c', [KEY_D] = 'd',
    [KEY_E] = 'e', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h',
    [KEY_I] = 'i', [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l',
    [KEY_M] = 'm', [KEY_N] = 'n', [KEY_O] = 'o', [KEY_P] = 'p',
    [KEY_Q] = 'q', [KEY_R] = 'r', [KEY_S] = 's', [KEY_T] = 't',
    [KEY_U] = 'u', [KEY_V] = 'v', [KEY_W] = 'w', [KEY_X] = 'x',
    [KEY_Y] = 'y', [KEY_Z] = 'z',
    [KEY_0] = '0', [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3',
    [KEY_4] = '4', [KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7',
    [KEY_8] = '8', [KEY_9] = '9',
    [KEY_SPACE]      = ' ',
    [KEY_ENTER]      = '\n',
    [KEY_BACKSPACE]  = '\b',
    [KEY_TAB]        = '\t',
    [KEY_MINUS]      = '-',
    [KEY_EQUALS]     = '=',
    [KEY_OPENBRACE]  = '[',
    [KEY_CLOSEBRACE] = ']',
    [KEY_SEMICOLON]  = ';',
    [KEY_QUOTE]      = '\'',
    [KEY_BACKSLASH]  = '\\',
    [KEY_COMMA]      = ',',
    [KEY_STOP]       = '.',
    [KEY_SLASH]      = '/',
    [KEY_TILDE]      = '`',
};

static const char shifted[KEY_MAX] = {
    [KEY_A] = 'A', [KEY_B] = 'B', [KEY_C] = 'C', [KEY_D] = 'D',
    [KEY_E] = 'E', [KEY_F] = 'F', [KEY_G] = 'G', [KEY_H] = 'H',
    [KEY_I] = 'I', [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L',
    [KEY_M] = 'M', [KEY_N] = 'N', [KEY_O] = 'O', [KEY_P] = 'P',
    [KEY_Q] = 'Q', [KEY_R] = 'R', [KEY_S] = 'S', [KEY_T] = 'T',
    [KEY_U] = 'U', [KEY_V] = 'V', [KEY_W] = 'W', [KEY_X] = 'X',
    [KEY_Y] = 'Y', [KEY_Z] = 'Z',
    [KEY_0] = ')', [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#',
    [KEY_4] = '$', [KEY_5] = '%', [KEY_6] = '^', [KEY_7] = '&',
    [KEY_8] = '*', [KEY_9] = '(',
    [KEY_SPACE]      = ' ',
    [KEY_ENTER]      = '\n',
    [KEY_BACKSPACE]  = '\b',
    [KEY_TAB]        = '\t',
    [KEY_MINUS]      = '_',
    [KEY_EQUALS]     = '+',
    [KEY_OPENBRACE]  = '{',
    [KEY_CLOSEBRACE] = '}',
    [KEY_SEMICOLON]  = ':',
    [KEY_QUOTE]      = '"',
    [KEY_BACKSLASH]  = '|',
    [KEY_COMMA]      = '<',
    [KEY_STOP]       = '>',
    [KEY_SLASH]      = '?',
    [KEY_TILDE]      = '~',
};

void key_input_init(key_input_t *ks)
{
    memset(ks, 0, sizeof(*ks));
}

static void enqueue(key_input_t *ks, char c)
{
    int next = (ks->qhead + 1) % KEY_INPUT_QUEUE;
    if (next == ks->qtail) return;   /* drop on overflow */
    ks->queue[ks->qhead] = c;
    ks->qhead = next;
}

void key_input_poll(key_input_t *ks)
{
    int i;
    int shift = key[KEY_LSHIFT] || key[KEY_RSHIFT];

    /* Caps-lock toggle on rising edge */
    if (key[KEY_CAPSLOCK] && !ks->prev[KEY_CAPSLOCK]) {
        ks->caps_lock = !ks->caps_lock;
    }

    for (i = 0; i < KEY_MAX; i++) {
        int now = key[i] ? 1 : 0;
        if (now && !ks->prev[i]) {
            /* Rising edge — translate to ASCII */
            char c = shift ? shifted[i] : unshifted[i];
            if (c) {
                /* Caps lock affects letters only */
                if (ks->caps_lock && i >= KEY_A && i <= KEY_Z) {
                    c = shift ? unshifted[i] : shifted[i];
                }
                enqueue(ks, c);
            }
        }
        ks->prev[i] = (unsigned char)now;
    }
}

int key_input_take(key_input_t *ks)
{
    char c;
    if (ks->qhead == ks->qtail) return 0;
    c = ks->queue[ks->qtail];
    ks->qtail = (ks->qtail + 1) % KEY_INPUT_QUEUE;
    return (unsigned char)c;
}
