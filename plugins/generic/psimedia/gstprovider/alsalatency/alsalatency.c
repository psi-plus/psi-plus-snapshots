#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

// sets up a playback or capture handle to 44100/S16_LE/1
// on success, sets *psize to period_size and returns non-zero
static int setup_handle(snd_pcm_t *handle, unsigned int rate, snd_pcm_uframes_t *psize)
{
    int                  err;
    snd_pcm_hw_params_t *hw_params;

    if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
        fprintf(stderr, "cannot allocate hardware parameter structure (%s)\n", snd_strerror(err));
        return 0;
    }

    if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0) {
        fprintf(stderr, "cannot initialize hardware parameter structure (%s)\n", snd_strerror(err));
        return 0;
    }

    if ((err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "cannot set access type (%s)\n", snd_strerror(err));
        return 0;
    }

    if ((err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
        fprintf(stderr, "cannot set sample format (%s)\n", snd_strerror(err));
        return 0;
    }

    if ((err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, 0)) < 0) {
        fprintf(stderr, "cannot set sample rate (%s)\n", snd_strerror(err));
        return 0;
    }

    if ((err = snd_pcm_hw_params_set_channels(handle, hw_params, 1)) < 0) {
        fprintf(stderr, "cannot set channel count (%s)\n", snd_strerror(err));
        return 0;
    }

    if ((err = snd_pcm_hw_params(handle, hw_params)) < 0) {
        fprintf(stderr, "cannot set parameters (%s)\n", snd_strerror(err));
        return 0;
    }

    snd_pcm_hw_params_get_period_size(hw_params, psize, 0);
    snd_pcm_hw_params_free(hw_params);

    return 1;
}

static void usage()
{
    printf("usage: alsalatency rec  (capture_device)\n");
    printf("       alsalatency loop (play_device) (capture_device)\n");
    printf("\n");
    printf("note: if capture_device or play_device are omitted, 'default' is assumed.\n\n");
}

int main(int argc, char **argv)
{
    char *            playback_device;
    char *            capture_device;
    snd_pcm_t *       playback_handle;
    snd_pcm_t *       capture_handle;
    snd_pcm_uframes_t playback_psize;
    snd_pcm_uframes_t capture_psize;
    int               mode;
    unsigned int      rate = 44100;
    short *           pbuf, *cbuf;
    int               count;
    int               err;
    int               at, at_play;
    FILE *            fin, *fout;

    if (argc < 2) {
        usage();
        return 1;
    }

    mode = -1;
    if (!strcmp(argv[1], "rec"))
        mode = 0;
    else if (!strcmp(argv[1], "loop"))
        mode = 1;
    else {
        usage();
        return 1;
    }

    playback_device = "default";
    capture_device  = playback_device;

    if (mode == 0) {
        if (argc >= 3)
            capture_device = argv[2];
    } else // 1
    {
        if (argc >= 3) {
            playback_device = argv[2];

            if (argc >= 4)
                capture_device = argv[3];
        }
    }

    if (mode == 0) {
        if ((err = snd_pcm_open(&capture_handle, capture_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
            fprintf(stderr, "cannot open audio device %s (%s)\n", capture_device, snd_strerror(err));
            return 1;
        }

        if (!setup_handle(capture_handle, rate, &capture_psize))
            return 1;

        fout = fopen("play.raw", "wb");
        if (!fout) {
            fprintf(stderr, "Error opening play.raw for writing.\n");
            return 1;
        }

        cbuf = (short *)malloc(capture_psize * 2);

        printf("Recording 5-second audio clip to play.raw\n");

        if ((err = snd_pcm_prepare(capture_handle)) < 0) {
            fprintf(stderr, "cannot prepare audio interface for use (%s)\n", snd_strerror(err));
            fclose(fout);
            free(cbuf);
            return 1;
        }

        for (at = 0; at < (int)rate * 5;) {
            if (at + capture_psize < rate * 5)
                count = capture_psize;
            else
                count = (rate * 5) - at;

            if ((err = snd_pcm_readi(capture_handle, cbuf, count)) != count) {
                fprintf(stderr, "read from audio interface failed (%s)\n", snd_strerror(err));
                fclose(fout);
                free(cbuf);
                return 1;
            }

            fwrite(cbuf, count * 2, 1, fout);
            at += count;
        }

        snd_pcm_close(capture_handle);
        free(cbuf);
        fclose(fout);
    } else // 1
    {
        if ((err = snd_pcm_open(&playback_handle, playback_device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
            fprintf(stderr, "cannot open audio device %s (%s)\n", playback_device, snd_strerror(err));
            return 1;
        }

        if ((err = snd_pcm_open(&capture_handle, capture_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
            fprintf(stderr, "cannot open audio device %s (%s)\n", capture_device, snd_strerror(err));
            return 1;
        }

        if (!setup_handle(playback_handle, rate, &playback_psize))
            return 1;

        if (!setup_handle(capture_handle, rate, &capture_psize))
            return 1;

        fin = fopen("play.raw", "rb");
        if (!fin) {
            fprintf(stderr, "Error opening play.raw for reading.\n");
            return 1;
        }

        fout = fopen("loop.raw", "wb");
        if (!fout) {
            fprintf(stderr, "Error opening loop.raw for writing.\n");
            fclose(fin);
            return 1;
        }

        pbuf = (short *)malloc(playback_psize * 2);
        cbuf = (short *)malloc(capture_psize * 2);

        printf("Playing play.raw while recording simultaneously to loop.raw\n");

        if ((err = snd_pcm_prepare(playback_handle)) < 0) {
            fprintf(stderr, "cannot prepare audio interface for use (%s)\n", snd_strerror(err));
            fclose(fin);
            fclose(fout);
            free(pbuf);
            free(cbuf);
            return 1;
        }

        at_play = 0;
        at      = -1;
        while (at < at_play) {
            if (fin) {
                if (!feof(fin)) {
                    count = fread(pbuf, 2, playback_psize, fin);
                    if (count <= 0) {
                        snd_pcm_close(playback_handle);
                        free(pbuf);
                        pbuf = 0;
                        fclose(fin);
                        fin = 0;
                        continue;
                    }

                    if ((err = snd_pcm_writei(playback_handle, pbuf, count)) != count) {
                        fprintf(stderr, "write to audio interface failed (%s)\n", snd_strerror(err));
                        snd_pcm_prepare(playback_handle);
                    }

                    at_play += count;

                    // make sure play buffer has enough
                    //   data to survive while capturing
                    if (at_play < (at == -1 ? 0 : at) + (4 * (int)capture_psize))
                        continue;
                } else {
                    snd_pcm_close(playback_handle);
                    free(pbuf);
                    pbuf = 0;
                    fclose(fin);
                    fin = 0;
                }
            }

            if (at == -1) {
                if ((err = snd_pcm_prepare(capture_handle)) < 0) {
                    fprintf(stderr, "cannot prepare audio interface for use (%s)\n", snd_strerror(err));
                    if (fin)
                        fclose(fin);
                    fclose(fout);
                    free(pbuf);
                    free(cbuf);
                    return 1;
                }

                at = 0;
            }

            if (at + (int)capture_psize < at_play)
                count = capture_psize;
            else
                count = at_play - at;

            if ((err = snd_pcm_readi(capture_handle, cbuf, count)) != count) {
                fprintf(stderr, "read from audio interface failed (%s)\n", snd_strerror(err));
                if (fin)
                    fclose(fin);
                fclose(fout);
                free(pbuf);
                free(cbuf);
                return 1;
            }

            fwrite(cbuf, count * 2, 1, fout);
            at += count;
        }

        snd_pcm_close(capture_handle);
        free(cbuf);
        fclose(fout);
    }

    printf("done\n");
    return 0;
}
