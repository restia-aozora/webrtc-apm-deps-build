#include "aozora_apm.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

#include "modules/audio_processing/include/audio_processing.h"

static_assert(sizeof(AozoraApmStatistics) == 88, "Statistics v1 ABI size");
static_assert(offsetof(AozoraApmStatistics, capture_frames) == 16, "Statistics counter offset");
static_assert(offsetof(AozoraApmStatistics, echo_return_loss_db) == 56, "Statistics double offset");

namespace {

bool isAcceptedStatus(int status) {
  return status == webrtc::AudioProcessing::kNoError ||
         status == webrtc::AudioProcessing::kBadStreamParameterWarning;
}

void copyOptionalMetric(const absl::optional<double>& source, uint32_t valid_bit,
                        double& destination, uint32_t& valid_fields) {
  if (source.has_value() && std::isfinite(*source)) {
    destination = *source;
    valid_fields |= valid_bit;
  }
}

}  // namespace

struct AozoraApm {
  explicit AozoraApm(int sampleRate, bool noiseSuppression, bool gainControl)
      : sample_rate(sampleRate), frame_samples(static_cast<size_t>(sampleRate / 100)) {
    cached_statistics.struct_size = sizeof(AozoraApmStatistics);
    cached_statistics.version = AOZORA_APM_STATISTICS_VERSION;
    cached_statistics.sample_rate_hz = static_cast<uint32_t>(sampleRate);
    cached_statistics.noise_suppression_enabled = noiseSuppression ? 1 : 0;
    cached_statistics.gain_control_enabled = gainControl ? 1 : 0;
    clearOptionalStatistics(cached_statistics);
    webrtc::AudioProcessingBuilder builder;
    processor = rtc::scoped_refptr<webrtc::AudioProcessing>(builder.Create());
    if (!processor) {
      return;
    }
    webrtc::AudioProcessing::Config configuration;
    configuration.pipeline.maximum_internal_processing_rate = sampleRate >= 32000 ? 48000 : 32000;
    configuration.pipeline.multi_channel_render = false;
    configuration.pipeline.multi_channel_capture = false;
    configuration.echo_canceller.enabled = true;
    configuration.echo_canceller.mobile_mode = false;
    configuration.noise_suppression.enabled = noiseSuppression;
    configuration.gain_controller2.enabled = gainControl;
    configuration.voice_detection.enabled = true;
    processor->ApplyConfig(configuration);
    if (processor->Initialize(sampleRate, sampleRate, sampleRate,
                              webrtc::AudioProcessing::kMono,
                              webrtc::AudioProcessing::kMono,
                              webrtc::AudioProcessing::kMono) != webrtc::AudioProcessing::kNoError) {
      processor = nullptr;
    }
  }

  static void clearOptionalStatistics(AozoraApmStatistics& snapshot) {
    snapshot.valid_fields = 0;
    snapshot.voice_detected = -1;
    snapshot.estimated_delay_ms = -1;
    snapshot.echo_return_loss_db = std::numeric_limits<double>::quiet_NaN();
    snapshot.echo_return_loss_enhancement_db = std::numeric_limits<double>::quiet_NaN();
    snapshot.residual_echo_likelihood = std::numeric_limits<double>::quiet_NaN();
    snapshot.residual_echo_likelihood_recent_max = std::numeric_limits<double>::quiet_NaN();
  }

  void cacheCaptureStatistics(const webrtc::AudioProcessingStats& statistics,
                              int submitted_delay) {
    auto snapshot = cached_statistics;
    clearOptionalStatistics(snapshot);
    snapshot.capture_frames += 1;
    snapshot.reverse_frames = accepted_reverse_frames;
    snapshot.stream_delay_ms = submitted_delay;
    if (statistics.voice_detected.has_value()) {
      snapshot.voice_detected = *statistics.voice_detected ? 1 : 0;
      snapshot.valid_fields |= AOZORA_APM_STATISTICS_VOICE;
    }
    if (statistics.delay_ms.has_value() && *statistics.delay_ms >= 0) {
      snapshot.estimated_delay_ms = *statistics.delay_ms;
      snapshot.valid_fields |= AOZORA_APM_STATISTICS_DELAY;
    }
    copyOptionalMetric(statistics.echo_return_loss, AOZORA_APM_STATISTICS_ERL,
                       snapshot.echo_return_loss_db, snapshot.valid_fields);
    copyOptionalMetric(statistics.echo_return_loss_enhancement, AOZORA_APM_STATISTICS_ERLE,
                       snapshot.echo_return_loss_enhancement_db, snapshot.valid_fields);
    copyOptionalMetric(statistics.residual_echo_likelihood, AOZORA_APM_STATISTICS_RESIDUAL,
                       snapshot.residual_echo_likelihood, snapshot.valid_fields);
    copyOptionalMetric(statistics.residual_echo_likelihood_recent_max,
                       AOZORA_APM_STATISTICS_RESIDUAL_MAX,
                       snapshot.residual_echo_likelihood_recent_max, snapshot.valid_fields);
    cached_statistics = snapshot;
    voice_detected.store(snapshot.voice_detected, std::memory_order_relaxed);
  }

  int process(int16_t* samples, size_t count, bool reverse) {
    if (!samples || count != frame_samples) {
      return AOZORA_APM_ERROR_INVALID_FRAME;
    }
    std::lock_guard<std::mutex> lock(process_mutex);
    std::array<int16_t, 480> processed{};
    const webrtc::StreamConfig stream(sample_rate, 1);
    int status;
    if (reverse) {
      // Analysis-only reverse processing may leave its destination unwritten.
      std::memcpy(processed.data(), samples, count * sizeof(int16_t));
      status = processor->ProcessReverseStream(samples, stream, stream, processed.data());
    } else {
      const int submitted_delay = stream_delay.load(std::memory_order_relaxed);
      const int delay_status = processor->set_stream_delay_ms(submitted_delay);
      if (delay_status < 0) {
        return delay_status;
      }
      status = processor->ProcessStream(samples, stream, stream, processed.data());
      if (isAcceptedStatus(status)) {
        const auto statistics = processor->GetStatistics();
        cacheCaptureStatistics(statistics, submitted_delay);
      }
    }
    if (isAcceptedStatus(status)) {
      if (reverse) {
        accepted_reverse_frames += 1;
      }
      std::memcpy(samples, processed.data(), count * sizeof(int16_t));
    }
    return status;
  }

  const int sample_rate;
  const size_t frame_samples;
  rtc::scoped_refptr<webrtc::AudioProcessing> processor;
  std::mutex process_mutex;
  std::atomic<int> stream_delay{0};
  std::atomic<int> voice_detected{-1};
  uint64_t accepted_reverse_frames{0};
  AozoraApmStatistics cached_statistics{};
};

extern "C" const char* aozora_apm_get_build_revision(void) {
  return "native-apm-v2-stats1";
}

extern "C" int aozora_apm_get_statistics(AozoraApm* processor,
                                          AozoraApmStatistics* destination,
                                          size_t destination_size) {
  if (!processor) return AOZORA_APM_ERROR_INVALID_HANDLE;
  if (!destination || destination_size < sizeof(AozoraApmStatistics)) {
    return AOZORA_APM_ERROR_INVALID_ARGUMENT;
  }
  try {
    std::lock_guard<std::mutex> lock(processor->process_mutex);
    std::memcpy(destination, &processor->cached_statistics, sizeof(AozoraApmStatistics));
    return 0;
  } catch (...) {
    return AOZORA_APM_ERROR_INTERNAL;
  }
}

extern "C" AozoraApm* aozora_apm_create(int sample_rate, int channels, int enable_ns, int enable_agc) {
  if ((sample_rate != 8000 && sample_rate != 16000 && sample_rate != 32000 && sample_rate != 48000) ||
      channels != 1 || (enable_ns != 0 && enable_ns != 1) || (enable_agc != 0 && enable_agc != 1)) {
    errno = EINVAL;
    return nullptr;
  }
  try {
    auto instance = std::make_unique<AozoraApm>(sample_rate, enable_ns != 0, enable_agc != 0);
    if (!instance->processor) {
      errno = EIO;
      return nullptr;
    }
    return instance.release();
  } catch (const std::bad_alloc&) {
    errno = ENOMEM;
  } catch (...) {
    errno = EIO;
  }
  return nullptr;
}

extern "C" int aozora_apm_process_reverse(AozoraApm* processor, int16_t* samples, size_t count) {
  if (!processor) return AOZORA_APM_ERROR_INVALID_HANDLE;
  try {
    return processor->process(samples, count, true);
  } catch (...) {
    return AOZORA_APM_ERROR_INTERNAL;
  }
}

extern "C" int aozora_apm_process_capture(AozoraApm* processor, int16_t* samples, size_t count) {
  if (!processor) return AOZORA_APM_ERROR_INVALID_HANDLE;
  try {
    return processor->process(samples, count, false);
  } catch (...) {
    return AOZORA_APM_ERROR_INTERNAL;
  }
}

extern "C" void aozora_apm_set_stream_delay_ms(AozoraApm* processor, int delay) {
  if (processor) {
    // WebRTC accepts 0..500 ms. Keep oversized inputs from blocking capture.
    const int bounded_delay = delay < 0 ? 0 : (delay > 500 ? 500 : delay);
    processor->stream_delay.store(bounded_delay, std::memory_order_relaxed);
  }
}

extern "C" int aozora_apm_get_voice_detected(AozoraApm* processor) {
  return processor ? processor->voice_detected.load(std::memory_order_relaxed) : -1;
}

extern "C" size_t aozora_apm_frame_samples(AozoraApm* processor) {
  return processor ? processor->frame_samples : 0;
}

extern "C" void aozora_apm_destroy(AozoraApm* processor) {
  delete processor;
}
