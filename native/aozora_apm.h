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
  AOZORA_APM_ERROR_INTERNAL = -1003,
  AOZORA_APM_ERROR_INVALID_ARGUMENT = -1004
};

enum {
  AOZORA_APM_STATISTICS_VERSION = 1,
  AOZORA_APM_STATISTICS_VOICE = 1,
  AOZORA_APM_STATISTICS_ERL = 2,
  AOZORA_APM_STATISTICS_ERLE = 4,
  AOZORA_APM_STATISTICS_DELAY = 8,
  AOZORA_APM_STATISTICS_RESIDUAL = 16,
  AOZORA_APM_STATISTICS_RESIDUAL_MAX = 32
};

/* Additive v1 ABI: 88 bytes; counters at offset 16, doubles at offset 56.
 * Optional doubles are NaN when unavailable; voice/internal delay are -1.
 * Only a set valid_fields bit permits interpreting its optional metric.
 */
typedef struct AozoraApmStatistics {
  uint32_t struct_size;
  uint32_t version;
  uint32_t valid_fields;
  uint32_t sample_rate_hz;
  uint64_t capture_frames;
  uint64_t reverse_frames;
  int32_t stream_delay_ms;
  int32_t voice_detected;
  int32_t estimated_delay_ms;
  int32_t noise_suppression_enabled;
  int32_t gain_control_enabled;
  uint32_t reserved;
  double echo_return_loss_db;
  double echo_return_loss_enhancement_db;
  double residual_echo_likelihood;
  double residual_echo_likelihood_recent_max;
} AozoraApmStatistics;

AOZORA_APM_API const char* aozora_apm_get_build_revision(void);
/* Read-only frozen snapshot of the last accepted capture, including reverse
 * count and the delay actually submitted for that capture. Before capture,
 * counts and valid_fields are zero; rate/configuration are already known.
 * Does not consume upstream statistics. Returns 0 on success, negative on
 * NULL/short destination or NULL handle, without writing any destination byte.
 * Larger destinations are accepted but only sizeof(AozoraApmStatistics) bytes
 * are written. The existing live-handle/destruction rules still apply.
 */
AOZORA_APM_API int aozora_apm_get_statistics(AozoraApm* processor,
                                            AozoraApmStatistics* destination,
                                            size_t destination_size);

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
