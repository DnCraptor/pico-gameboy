/*
 * ALSA audio output (see audio_linux.h).
 */
#include "audio_linux.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AUDIO_PERIOD      256        /* frames per write, ~5.8 ms at 44.1 kHz */
#define AUDIO_LATENCY_US  40000u
#define QUEUE_FRAMES      4096u      /* power of two, ~93 ms */
#define QUEUE_LIMIT       2048u      /* alsa_write() waits above this, ~46 ms */

static snd_pcm_t *pcm;
static pthread_t audio_thread;
static atomic_bool audio_running;
static atomic_bool audio_started;

/* Single-producer (emulation thread) / single-consumer (playback thread)
 * queue of interleaved L/R frames. */
static uint32_t queue[QUEUE_FRAMES];
static atomic_uint queue_w;
static atomic_uint queue_r;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_drained = PTHREAD_COND_INITIALIZER;

bool alsa_write(const int16_t *frames, unsigned count)
{
	if (!atomic_load(&audio_started))
		return false;
	if (count > QUEUE_LIMIT)
		count = QUEUE_LIMIT;
	unsigned w = atomic_load_explicit(&queue_w, memory_order_relaxed);

	pthread_mutex_lock(&queue_mutex);
	while (atomic_load(&audio_running) &&
	       w - atomic_load_explicit(&queue_r, memory_order_acquire) + count > QUEUE_LIMIT) {
		/* the timeout only guards against a playback thread that died */
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 100000000L;
		if (until.tv_nsec >= 1000000000L) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&queue_drained, &queue_mutex, &until);
	}
	pthread_mutex_unlock(&queue_mutex);
	if (!atomic_load(&audio_running))
		return false;

	for (unsigned i = 0; i < count; ++i, ++w)
		queue[w & (QUEUE_FRAMES - 1)] = (uint32_t)(uint16_t)frames[i * 2] |
						((uint32_t)(uint16_t)frames[i * 2 + 1] << 16);
	atomic_store_explicit(&queue_w, w, memory_order_release);
	return true;
}

static void *audio_main(void *arg)
{
	(void)arg;
	int16_t buf[AUDIO_PERIOD * 2];
	while (atomic_load(&audio_running)) {
		unsigned r = atomic_load_explicit(&queue_r, memory_order_relaxed);
		const unsigned w = atomic_load_explicit(&queue_w, memory_order_acquire);
		for (int i = 0; i < AUDIO_PERIOD; ++i) {
			/* an empty queue (menu, file browser, a late frame): silence */
			const uint32_t v = r != w ? queue[r++ & (QUEUE_FRAMES - 1)] : 0;
			buf[i * 2 + 0] = (int16_t)(v & 0xFFFF);
			buf[i * 2 + 1] = (int16_t)(v >> 16);
		}
		pthread_mutex_lock(&queue_mutex);
		atomic_store_explicit(&queue_r, r, memory_order_release);
		pthread_cond_signal(&queue_drained);
		pthread_mutex_unlock(&queue_mutex);

		snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, AUDIO_PERIOD);
		if (n < 0) {
			n = snd_pcm_recover(pcm, (int)n, 1);
			if (n < 0) {
				fprintf(stderr, "audio: %s\n", snd_strerror((int)n));
				break;
			}
		}
	}
	/* wake a waiting alsa_write(): it returns false from now on */
	pthread_mutex_lock(&queue_mutex);
	atomic_store(&audio_running, false);
	pthread_cond_signal(&queue_drained);
	pthread_mutex_unlock(&queue_mutex);
	return NULL;
}

/*
 * Sound output device, first match:
 *   AUDIODEV in the environment (as SDL programs use it),
 *   the HDMI card, which carries the picture as well,
 *   ALSA "default".
 */
static const char *audio_device_name(void)
{
	const char *dev = getenv("AUDIODEV");
	if (dev && *dev)
		return dev;
	if (snd_card_get_index("HDMI") >= 0)
		return "plughw:CARD=HDMI,DEV=0";
	return "default";
}

bool alsa_open(unsigned rate)
{
	if (atomic_load(&audio_started))
		return true;
	const char *dev = audio_device_name();
	int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "audio: cannot open %s: %s (sound disabled)\n",
			dev, snd_strerror(err));
		pcm = NULL;
		return false;
	}
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
				 2, rate, 1, AUDIO_LATENCY_US);
	if (err < 0) {
		fprintf(stderr, "audio: %s: %s (sound disabled)\n", dev, snd_strerror(err));
		snd_pcm_close(pcm);
		pcm = NULL;
		return false;
	}
	atomic_store(&audio_running, true);
	err = pthread_create(&audio_thread, NULL, audio_main, NULL);
	if (err) {
		fprintf(stderr, "audio: cannot start thread: %s\n", strerror(err));
		atomic_store(&audio_running, false);
		snd_pcm_close(pcm);
		pcm = NULL;
		return false;
	}
	atomic_store(&audio_started, true);
	printf("audio: %s, %u Hz stereo\n", dev, rate);
	return true;
}

void alsa_close(void)
{
	if (!atomic_load(&audio_started))
		return;
	atomic_store(&audio_started, false);
	pthread_mutex_lock(&queue_mutex);
	atomic_store(&audio_running, false);
	pthread_cond_signal(&queue_drained);
	pthread_mutex_unlock(&queue_mutex);
	pthread_join(audio_thread, NULL);
	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
	pcm = NULL;
}
