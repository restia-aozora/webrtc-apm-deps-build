#ifndef AOZORA_APM_H
#define AOZORA_APM_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define AOZORA_APM_API __attribute__((visibility("default")))
#else
#define AOZORA_APM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AozoraApm AozoraApm;

enum {
  AOZORA_APM_ERROR_INVALID_HANDLE = -1000,
  AOZORA_APM_ERROR_INVALID_FRAME = -1001,
  AOZORA_APM_ERROR_RELEASED = -1002,
  AOZORA_APM_ERROR_INTERNAL = -1003
};

/* Mono signed PCM16, 8/16/32/48 kHz, exactly 10 ms per frame.
 * Boolean options must be 0 or 1. AEC3 is always enabled.
 * Returns NULL on failure; errno is EINVAL, ENOMEM or EIO.
 */
AOZORA_APM_API AozoraApm* aozora_apm_create(int sample_rate, int channels,
                                          int enable_ns, int enable_agc);
/* Calls on one live instance are serialized. Samples are updated in place.
 * Zero and WebRTC kBadStreamParameterWarning (-13) are accepted results.
 */
AOZORA_APM_API int aozora_apm_process_reverse(AozoraApm* processor,
                                             int16_t* samples, size_t count);
AOZORA_APM_API int aozora_apm_process_capture(AozoraApm* processor,
                                             int16_t* samples, size_t count);
AOZORA_APM_API void aozora_apm_set_stream_delay_ms(AozoraApm* processor, int delay);
/* -1 = unknown, 0 = not detected, 1 = detected. */
AOZORA_APM_API int aozora_apm_get_voice_detected(AozoraApm* processor);
AOZORA_APM_API size_t aozora_apm_frame_samples(AozoraApm* processor);
/* The caller MUST serialize destruction against all operations and never
 * reuse a destroyed handle. NULL is safe; a stale pointer is not.
 */
AOZORA_APM_API void aozora_apm_destroy(AozoraApm* processor);

#ifdef __cplusplus
}
#endif
#endif
