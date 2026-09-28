#include "aozora_apm.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "modules/audio_processing/include/audio_processing.h"

struct AozoraApm {
  explicit AozoraApm(int sampleRate, bool noiseSuppression, bool gainControl)
      : sample_rate(sampleRate), frame_samples(static_cast<size_t>(sampleRate / 100)) {
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

  int process(int16_t* samples, size_t count, bool reverse) {
    if (!samples || count != frame_samples) {
      return AOZORA_APM_ERROR_INVALID_FRAME;
    }
    std::lock_guard<std::mutex> lock(process_mutex);
    std::array<int16_t, 480> processed{};
    const webrtc::StreamConfig stream(sample_rate, 1);
    int status;
    if (reverse) {
      status = processor->ProcessReverseStream(samples, stream, stream, processed.data());
    } else {
      const int delay_status = processor->set_stream_delay_ms(stream_delay.load(std::memory_order_relaxed));
      if (delay_status < 0) {
        return delay_status;
      }
      status = processor->ProcessStream(samples, stream, stream, processed.data());
      if (status == webrtc::AudioProcessing::kNoError ||
          status == webrtc::AudioProcessing::kBadStreamParameterWarning) {
        const auto statistics = processor->GetStatistics();
        voice_detected.store(statistics.voice_detected.has_value()
                                 ? (*statistics.voice_detected ? 1 : 0) : -1,
                             std::memory_order_relaxed);
      }
    }
    if (status == webrtc::AudioProcessing::kNoError ||
        status == webrtc::AudioProcessing::kBadStreamParameterWarning) {
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
};

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
