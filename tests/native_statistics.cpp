#include "aozora_apm.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(EXIT_FAILURE);
  }
}

bool isAccepted(int status) {
  return status == 0 || status == -13;
}

AozoraApmStatistics readStatistics(AozoraApm* processor) {
  AozoraApmStatistics statistics{};
  require(aozora_apm_get_statistics(processor, &statistics, sizeof(statistics)) == 0,
          "read frozen statistics");
  return statistics;
}

void requireSameSnapshot(const AozoraApmStatistics& first,
                         const AozoraApmStatistics& second) {
  require(std::memcmp(&first, &second, sizeof(first)) == 0,
          "getter and rejected calls must not change snapshot bytes");
}

void requireOptionalValues(const AozoraApmStatistics& statistics) {
  require((statistics.valid_fields & ~uint32_t{63}) == 0, "v1 valid bits only");
  require((statistics.valid_fields & 1)
              ? statistics.voice_detected == 0 || statistics.voice_detected == 1
              : statistics.voice_detected == -1,
          "voice unknown is distinct from valid false");
  require((statistics.valid_fields & 8)
              ? statistics.estimated_delay_ms >= 0
              : statistics.estimated_delay_ms == -1,
          "unknown internal delay is not a fabricated zero");
  const std::array<double, 4> values{{statistics.echo_return_loss_db,
      statistics.echo_return_loss_enhancement_db,
      statistics.residual_echo_likelihood,
      statistics.residual_echo_likelihood_recent_max}};
  const std::array<uint32_t, 4> bits{{2, 4, 16, 32}};
  for (size_t metricIndex = 0; metricIndex < values.size(); ++metricIndex) {
    require((statistics.valid_fields & bits[metricIndex])
                ? std::isfinite(values[metricIndex]) : std::isnan(values[metricIndex]),
            "optional double is finite when valid and NaN when absent");
  }
}

}  // namespace

int main() {
  static_assert(std::is_standard_layout<AozoraApmStatistics>::value, "C layout");
  static_assert(sizeof(AozoraApmStatistics) == 88, "v1 fixed width layout");
  static_assert(offsetof(AozoraApmStatistics, capture_frames) == 16, "counter offset");
  static_assert(offsetof(AozoraApmStatistics, echo_return_loss_db) == 56, "double offset");
  require(std::strcmp(aozora_apm_get_build_revision(), "native-apm-v2-stats1") == 0,
          "additive evidence revision");
  for (int sampleRate : {8000, 16000, 32000, 48000}) {
    for (int noiseSuppression : {0, 1}) {
      for (int gainControl : {0, 1}) {
        AozoraApm* processor = aozora_apm_create(sampleRate, 1, noiseSuppression, gainControl);
        require(processor != nullptr, "create real configured processor");
        const size_t frameSamples = static_cast<size_t>(sampleRate / 100);
        std::array<int16_t, 480> samples{};
        auto initial = readStatistics(processor);
        require(initial.struct_size == 88 && initial.version == 1, "v1 schema header");
        require(initial.sample_rate_hz == static_cast<uint32_t>(sampleRate), "sample rate");
        require(initial.noise_suppression_enabled == noiseSuppression &&
                    initial.gain_control_enabled == gainControl, "configuration flags");
        require(initial.capture_frames == 0 && initial.reverse_frames == 0 &&
                    initial.valid_fields == 0 && initial.reserved == 0, "empty initial snapshot");
        requireOptionalValues(initial);
        requireSameSnapshot(initial, readStatistics(processor));

        alignas(AozoraApmStatistics)
            std::array<unsigned char, sizeof(AozoraApmStatistics) + 16> guarded;
        guarded.fill(0xa5);
        const auto sentinel = guarded;
        require(aozora_apm_get_statistics(processor,
                    reinterpret_cast<AozoraApmStatistics*>(guarded.data()), 87) < 0,
                "reject short buffer");
        require(guarded == sentinel, "short buffer writes neither payload nor guard");
        require(aozora_apm_get_statistics(nullptr,
                    reinterpret_cast<AozoraApmStatistics*>(guarded.data()), 88) < 0,
                "reject null handle");
        require(guarded == sentinel, "invalid handle leaves destination untouched");
        require(aozora_apm_get_statistics(processor, nullptr, 88) < 0,
                "reject null destination");
        require(aozora_apm_process_capture(processor, nullptr, frameSamples) < 0 &&
                    aozora_apm_process_reverse(processor, samples.data(), frameSamples - 1) < 0,
                "reject invalid processing calls");
        requireSameSnapshot(initial, readStatistics(processor));

        require(isAccepted(aozora_apm_process_reverse(processor, samples.data(), frameSamples)),
                "accept first reverse");
        aozora_apm_set_stream_delay_ms(processor, 37);
        requireSameSnapshot(initial, readStatistics(processor));
        require(isAccepted(aozora_apm_process_capture(processor, samples.data(), frameSamples)),
                "accept first capture");
        auto captured = readStatistics(processor);
        require(captured.capture_frames == 1 && captured.reverse_frames == 1,
                "successful calls advance captured counts");
        require(captured.stream_delay_ms == 37, "capture records actual submitted delay");
        requireOptionalValues(captured);
        requireSameSnapshot(captured, readStatistics(processor));
        aozora_apm_set_stream_delay_ms(processor, 83);
        require(isAccepted(aozora_apm_process_reverse(processor, samples.data(), frameSamples)),
                "accept reverse after capture");
        require(aozora_apm_process_capture(processor, samples.data(), frameSamples + 1) < 0,
                "reject capture without publishing pending reverse or delay");
        requireSameSnapshot(captured, readStatistics(processor));

        bool observedValidFalseVoice = false;
        for (int frameIndex = 0; frameIndex < 40; ++frameIndex) {
          samples.fill(0);
          require(isAccepted(aozora_apm_process_capture(processor, samples.data(), frameSamples)),
                  "accept silent capture");
          const auto current = readStatistics(processor);
          require(current.capture_frames == static_cast<uint64_t>(frameIndex + 2) &&
                      current.reverse_frames == 2 && current.stream_delay_ms == 83,
                  "no counter advances on getters or rejected calls");
          requireOptionalValues(current);
          observedValidFalseVoice |= (current.valid_fields & 1) && current.voice_detected == 0;
          requireSameSnapshot(current, readStatistics(processor));
        }
        require(observedValidFalseVoice, "real VAD reports valid false, not unknown");
        const auto beforeOversizedRead = readStatistics(processor);
        require(aozora_apm_get_statistics(processor,
                    reinterpret_cast<AozoraApmStatistics*>(guarded.data()), guarded.size()) == 0,
                "accept larger destination for forward compatibility");
        require(std::memcmp(guarded.data(), &beforeOversizedRead, 88) == 0,
                "large destination contains current snapshot");
        for (size_t guardIndex = 88; guardIndex < guarded.size(); ++guardIndex) {
          require(guarded[guardIndex] == 0xa5, "write only v1 size, preserve tail guard");
        }
        aozora_apm_destroy(processor);
      }
    }
  }
  std::puts("PASS: real-library frozen statistics v1 contract");
  return EXIT_SUCCESS;
}
