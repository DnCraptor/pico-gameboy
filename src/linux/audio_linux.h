#ifndef ORANGE_GB_AUDIO_LINUX_H
#define ORANGE_GB_AUDIO_LINUX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ALSA playback, the Linux counterpart of the RP2350 I2S output.
 *
 * On the RP2350 i2s_dma_write() waits until the previous Game Boy frame of
 * samples has been played, so the sound clock paces the emulation.  Here
 * alsa_write() queues one frame of samples and waits while more than about
 * two frames are still queued; a playback thread writes the queue to the PCM
 * device (silence when it runs dry: menu, file browser).  The device is
 * $AUDIODEV, else the ALSA card "HDMI" (plughw:CARD=HDMI,DEV=0), else
 * "default".  (The names avoid minigb_apu's audio_init() and friends.)
 */

/* Start playback at rate Hz; false (sound disabled) if no device opens. */
bool alsa_open(unsigned rate);

/* Queue count interleaved stereo S16 frames (L, R), waiting for room as
 * described above.  False if there is no sound output: then nothing waits
 * and the caller keeps the frame rate itself. */
bool alsa_write(const int16_t *frames, unsigned count);

/* Stop the playback thread and close the PCM device. */
void alsa_close(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGE_GB_AUDIO_LINUX_H */
