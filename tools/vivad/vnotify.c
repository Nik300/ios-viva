/*
 * vnotify: post Darwin notifications from the link (iOS has no notifyutil).
 *   vnotify NAME [NAME..]     e.g. it.viva.display.dump (VivaDisplay: dump SpringBoard's views)
 */
#include <notify.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        uint32_t st = notify_post(argv[i]);
        printf("%s: %s\n", argv[i], st == NOTIFY_STATUS_OK ? "posted" : "failed");
        bad |= st != NOTIFY_STATUS_OK;
    }
    if (argc < 2)
        fprintf(stderr, "usage: vnotify NAME [NAME..]\n");
    return argc < 2 || bad;
}
