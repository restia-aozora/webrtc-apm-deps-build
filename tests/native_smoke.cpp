#include "aozora_apm.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(EXIT_FAILURE);
  }
}

bool isAcceptedStatus(int status) {
  // WebRTC 1.3 kBadStreamParameterWarning is a successful, clamped result.
  return status == 0 || status == -13;
}

}  // namespace

int main() {
  errno = 0;
  require(aozora_apm_create(44100, 1, 0, 0) == nullptr,
          "reject unsupported sample rate");
  require(errno == EINVAL, "invalid configuration reports EINVAL");
  require(aozora_apm_create(16000, 0, 0, 0) == nullptr,
          "reject zero channels");
  require(aozora_apm_create(16000, 2, 0, 0) == nullptr,
          "reject stereo");
  require(aozora_apm_create(16000, 1, 2, 0) == nullptr,
          "reject non-boolean noise suppression");
  require(aozora_apm_create(16000, 1, 0, -1) == nullptr,
          "reject non-boolean gain control");

  std::array<int16_t, 160> silence{};
  require(aozora_apm_process_reverse(nullptr, silence.data(), silence.size()) ==
              AOZORA_APM_ERROR_INVALID_HANDLE,
          "reverse rejects null handle");
  require(aozora_apm_process_capture(nullptr, silence.data(), silence.size()) ==
              AOZORA_APM_ERROR_INVALID_HANDLE,
          "capture rejects null handle");
  require(aozora_apm_frame_samples(nullptr) == 0, "null handle has no frame");
  require(aozora_apm_get_voice_detected(nullptr) == -1,
          "null handle has unknown voice state");
  aozora_apm_set_stream_delay_ms(nullptr, 10);
  aozora_apm_destroy(nullptr);

  for (int sampleRate : {8000, 16000, 32000, 48000}) {
    for (int noiseSuppression : {0, 1}) {
      for (int gainControl : {0, 1}) {
        AozoraApm* processor =
            aozora_apm_create(sampleRate, 1, noiseSuppression, gainControl);
        require(processor != nullptr, "initialize each supported configuration");
        const size_t frameSamples = static_cast<size_t>(sampleRate / 100);
        require(aozora_apm_frame_samples(processor) == frameSamples,
                "10 ms mono frame size");
        if (sampleRate == 16000) {
          require(frameSamples == 160, "16 kHz frame contains 160 samples");
        }
        require(aozora_apm_get_voice_detected(processor) == -1,
                "voice state initially unknown");

        std::array<int16_t, 480> render{};
        std::array<int16_t, 480> capture{};
        require(aozora_apm_process_reverse(processor, nullptr, frameSamples) ==
                    AOZORA_APM_ERROR_INVALID_FRAME,
                "reverse rejects null samples");
        require(aozora_apm_process_capture(processor, nullptr, frameSamples) ==
                    AOZORA_APM_ERROR_INVALID_FRAME,
                "capture rejects null samples");
        require(aozora_apm_process_reverse(processor, render.data(), frameSamples - 1) ==
                    AOZORA_APM_ERROR_INVALID_FRAME,
                "reject short frame");
        require(aozora_apm_process_capture(processor, capture.data(), frameSamples + 1) ==
                    AOZORA_APM_ERROR_INVALID_FRAME,
                "reject long frame before reading memory");
        require(aozora_apm_process_capture(processor, capture.data(), 0) ==
                    AOZORA_APM_ERROR_INVALID_FRAME,
                "reject empty frame");

        // The upstream analysis-only reverse path may leave its destination
        // unwritten. A successful wrapper call must not replace PCM with zeros.
        for (size_t sampleIndex = 0; sampleIndex < frameSamples; ++sampleIndex) {
          render[sampleIndex] = static_cast<int16_t>(
              static_cast<int>(sampleIndex % 37) * 113 - 1800);
        }
        const auto originalRender = render;
        require(isAcceptedStatus(aozora_apm_process_reverse(
                    processor, render.data(), frameSamples)),
                "process nonzero analysis-only render frame");
        require(render == originalRender,
                "analysis-only reverse must preserve nonzero reference PCM");

        aozora_apm_set_stream_delay_ms(processor, -10);
        for (int frameIndex = 0; frameIndex < 30; ++frameIndex) {
          render.fill(0);
          capture.fill(0);
          require(isAcceptedStatus(aozora_apm_process_reverse(
                      processor, render.data(), frameSamples)),
                  "process silent render frame");
          require(isAcceptedStatus(aozora_apm_process_capture(
                      processor, capture.data(), frameSamples)),
                  "process silent capture frame");
        }
        aozora_apm_set_stream_delay_ms(processor, 10000);
        require(isAcceptedStatus(aozora_apm_process_capture(
                    processor, capture.data(), frameSamples)),
                "accept clamped stream-delay warning");
        const int voiceDetected = aozora_apm_get_voice_detected(processor);
        require(voiceDetected >= -1 && voiceDetected <= 1, "voice state domain");
        aozora_apm_destroy(processor);
        // Destruction invalidates the C pointer; never test use-after-free.
      }
    }
  }
  std::puts("PASS: native C API smoke (not an acoustic AEC quality test)");
  return EXIT_SUCCESS;
}
