// cc -o write_test write_test.c -I. -L. -lfaun

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#define sleep(S)	Sleep((S)*1000)
#else
#include <unistd.h>
#endif
#include "faun.h"

int main(int argc, char** argv)
{
    const int sourceCount = 8;
    const int st0 = sourceCount;
    const char* error;
    const char* fname;
    const char* fname2 = NULL;
    uint32_t pid;
    int mode = FAUN_PLAY_ONCE;
    int rc = 0;

    /*
    if (argc < 2) {
        fprintf(stderr, "usage: %s [-f] <ogg-file>\n", argv[0]);
        return 64;
    }
    */

    if ((error = faun_startup(16, sourceCount, 3, 0, "Faun Test"))) {
        fprintf(stderr, "faun_startup: %s\n", error);
        return 1;
    }

    if (argc == 1) {
        fname  = "data/vintage_education.ogg";
        fname2 = "data/voice-lb.ogg";
        mode |= FAUN_SIGNAL_DONE;
        faun_setParameter(st0, 1, FAUN_VOLUME, 0.5);
    } else if (argc == 2) {
        fname = argv[1];
        mode |= FAUN_SIGNAL_DONE;
    } else {
        fname = argv[2];
    }

    pid = faun_playStream(st0, fname, 0, 0, mode);
    if (pid) {
        if (mode & FAUN_SIGNAL_DONE) {
            FaunSignal sig;

            if (fname2) {
                sleep(2);
                faun_playStream(st0+1, fname2, 0, 0, FAUN_PLAY_ONCE);
              //faun_playStreamPart(st0+1, 19.882448, 3.297234, FAUN_PLAY_ONCE);
            }

            faun_waitSignal(&sig);
        } else {
            // Fade out
            int loop = 0;
            while (faun_isPlaying(pid)) {
                sleep(1);
                ++loop;
                if (loop == 2)
                    faun_control(FAUN_PID_SOURCE(pid), 1, FC_FADE_OUT);
            }
        }
    } else {
        fprintf(stderr, "faun_playStream failed\n");
        rc = 1;
    }

    faun_shutdown();
    return rc;
}
