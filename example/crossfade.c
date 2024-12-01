/*
 * Crossfading music example.
 */

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#define sleep(sec)    Sleep((sec)*1000)
#else
#include <unistd.h>
#endif
#include "faun.h"

#define SOURCE_LIMIT    0
#define STREAM_LIMIT    2
#define MUSIC_SI        SOURCE_LIMIT

/*
 * Fade between the two streams MUSIC_SI & MUSIC_SI+1.
 *
 * If streamPath is NULL then simply fade out the current music.
 */
void music_crossfade(const char* streamPath)
{
    static int current = 0;
    faun_control(MUSIC_SI + current, 1, FC_FADE_OUT);
    current ^= 1;
    if (streamPath) {
        faun_playStream(MUSIC_SI + current, streamPath, 0, 0,
                        FAUN_PLAY_ONCE | FAUN_PLAY_FADE_IN);
    }
}

int main()
{
    static const char* trackList[] = {
        "data/gate_open.ogg",
        "data/townes.ogg",
        "data/gate_open.ogg",
        NULL
    };

    const char* err = faun_startup(0, SOURCE_LIMIT, STREAM_LIMIT, 0, "Faun Example");
    if (err) {
        printf("%s\n", err);
        return 1;
    }

    // Optionally override the default fade period on the two music streams.
    faun_setParameter(MUSIC_SI, 2, FAUN_FADE_PERIOD, 1.5f);

    // Play a list of tracks for a few seconds each.
    for (const char** track = trackList; *track; ++track) {
        music_crossfade(*track);
        sleep(4);
    }

    // Fade out the last track.
    music_crossfade(NULL);
    sleep(2);

    faun_shutdown();
    return 0;
}
