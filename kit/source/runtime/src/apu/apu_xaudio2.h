/**
 * MA2 audio-output contract: XAudio2 on Windows, SDL2 on POSIX.
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Initialize the output device. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down the output device and release all resources. */
void xa2_shutdown(void);

/* Returns 1 while the output backend is initialized. */
int xa2_is_active(void);

/* Submit num_samples stereo frames of interleaved 16-bit PCM.
 * Returns 1 if accepted, otherwise the caller may wait and retry. */
int xa2_submit_samples(const int16_t *samples, int num_samples);
/* Returns 1 when space is available, 0 on timeout, or -1 when the caller
 * must drop the block (device failure or a bounded stall). */
int xa2_wait_for_space(int milliseconds);

/* Get the preferred buffer size in stereo frames. */
int xa2_get_buffer_size(void);

#endif /* APU_XAUDIO2_H */
