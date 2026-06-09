/* Minimal kernel — write to VGA text buffer to prove the toolchain works */

void kernel_main(void) {
    /* VGA text mode buffer at 0xB8000, 80x25, 2 bytes per char */
    /* Low byte = ASCII, high byte = attribute (0x0F = white on black) */
    volatile unsigned short *vga = (volatile unsigned short *)0xB8000;

    const char *msg = "DOS Desktop kernel — toolchain OK";
    int i;

    /* Clear screen */
    for (i = 0; i < 80 * 25; i++)
        vga[i] = 0x0F00 | ' ';

    /* Write message */
    for (i = 0; msg[i]; i++)
        vga[i] = 0x0F00 | (unsigned char)msg[i];
}
