#include "aozora_apm.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr int kSampleRate = 16000;
constexpr size_t kFrameSamples = 160;
constexpr size_t kAdaptationSamples = 20 * kSampleRate;
constexpr size_t kEvaluationSamples = 8 * kSampleRate;
constexpr size_t kTotalSamples = kAdaptationSamples + kEvaluationSamples;
constexpr size_t kEchoDelaySamples = 640;
constexpr double kPi = 3.14159265358979323846;

enum class ReferenceMode { Matched, Disconnected, Wrong };
enum class CaptureMode { EchoOnly, NearOnly, DoubleTalk };
enum class FaultMode { None, DisconnectReference, ZeroOutput };

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(EXIT_FAILURE);
  }
}

bool isAccepted(int status) {
  return status == 0 || status == -13;
}

int16_t quantize(double sample) {
  require(std::isfinite(sample) && std::abs(sample) < 30000.0,
          "synthetic input must be finite and unclipped");
  return static_cast<int16_t>(std::lround(sample));
}

std::vector<double> generateSignal(uint32_t seed, double modulationFrequency) {
  std::vector<double> signal(kTotalSamples);
  double previousLowPass = 0.0;
  double previousHighPass = 0.0;
  double previousInput = 0.0;
  for (size_t sampleIndex = 0; sampleIndex < signal.size(); ++sampleIndex) {
    seed = seed * 1664525u + 1013904223u;
    const double whiteNoise = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
    previousLowPass = 0.65 * previousLowPass + 0.35 * whiteNoise;
    const double highPass = previousLowPass - previousInput + 0.97 * previousHighPass;
    previousInput = previousLowPass;
    previousHighPass = highPass;
    const double timeSeconds = static_cast<double>(sampleIndex) / kSampleRate;
    const double envelope = 0.65 + 0.35 * std::sin(2.0 * kPi * modulationFrequency * timeSeconds);
    signal[sampleIndex] = quantize(18000.0 * envelope * highPass);
  }
  return signal;
}

double delayedSample(const std::vector<double>& signal, size_t sampleIndex, size_t delay) {
  return sampleIndex >= delay ? signal[sampleIndex - delay] : 0.0;
}

std::vector<double> generateEcho(const std::vector<double>& render) {
  std::vector<double> echo(render.size());
  for (size_t sampleIndex = 0; sampleIndex < echo.size(); ++sampleIndex) {
    echo[sampleIndex] = quantize(
        0.55 * delayedSample(render, sampleIndex, kEchoDelaySamples) +
        0.20 * delayedSample(render, sampleIndex, kEchoDelaySamples + 37) -
        0.12 * delayedSample(render, sampleIndex, kEchoDelaySamples + 83));
  }
  return echo;
}

std::vector<double> runProcessor(const std::vector<double>& render,
                                 const std::vector<double>& wrongReference,
                                 const std::vector<double>& echo,
                                 const std::vector<double>& nearSignal,
                                 ReferenceMode referenceMode, CaptureMode captureMode,
                                 FaultMode faultMode, bool invalidConfiguration) {
  std::unique_ptr<AozoraApm, decltype(&aozora_apm_destroy)> processor(
      aozora_apm_create(invalidConfiguration ? 44100 : kSampleRate, 1, 0, 0), aozora_apm_destroy);
  require(processor != nullptr, "create actual AEC3 with NS=0 and AGC=0");
  aozora_apm_set_stream_delay_ms(processor.get(), 40);
  AozoraApmStatistics configuration{};
  require(aozora_apm_get_statistics(processor.get(), &configuration, sizeof(configuration)) == 0 &&
              configuration.noise_suppression_enabled == 0 &&
              configuration.gain_control_enabled == 0,
          "effect test configuration must disable NS and AGC");
  std::vector<double> output(kTotalSamples);
  for (size_t frameStart = 0; frameStart < kTotalSamples; frameStart += kFrameSamples) {
    std::array<int16_t, kFrameSamples> reverseFrame{};
    std::array<int16_t, kFrameSamples> captureFrame{};
    for (size_t frameOffset = 0; frameOffset < kFrameSamples; ++frameOffset) {
      const size_t sampleIndex = frameStart + frameOffset;
      const bool nearActive = sampleIndex >= kAdaptationSamples;
      const double nearSample = nearActive ? nearSignal[sampleIndex] : 0.0;
      const double echoSample = captureMode == CaptureMode::NearOnly ? 0.0 : echo[sampleIndex];
      captureFrame[frameOffset] = quantize(echoSample +
          (captureMode == CaptureMode::EchoOnly ? 0.0 : nearSample));
      reverseFrame[frameOffset] = quantize(referenceMode == ReferenceMode::Wrong
          ? wrongReference[sampleIndex] : render[sampleIndex]);
    }
    // Faults exist only here, never in the wrapper or upstream implementation.
    const bool connectReference = captureMode != CaptureMode::NearOnly &&
        referenceMode != ReferenceMode::Disconnected &&
        faultMode != FaultMode::DisconnectReference;
    if (connectReference) {
      require(isAccepted(aozora_apm_process_reverse(
                  processor.get(), reverseFrame.data(), reverseFrame.size())),
              "process synthetic reference with actual WebRTC");
    }
    require(isAccepted(aozora_apm_process_capture(
                processor.get(), captureFrame.data(), captureFrame.size())),
            "process synthetic capture with actual WebRTC");
    if (faultMode == FaultMode::ZeroOutput) captureFrame.fill(0);
    for (size_t frameOffset = 0; frameOffset < kFrameSamples; ++frameOffset) {
      output[frameStart + frameOffset] = captureFrame[frameOffset];
    }
  }
  return output;
}

double measureEnergy(const std::vector<double>& signal, size_t start, size_t end) {
  double energy = 0.0;
  for (size_t sampleIndex = start; sampleIndex < end; ++sampleIndex) {
    energy += signal[sampleIndex] * signal[sampleIndex];
  }
  return energy;
}

double measureAttenuation(const std::vector<double>& echo, const std::vector<double>& output) {
  const double inputEnergy = measureEnergy(echo, kAdaptationSamples, kTotalSamples);
  const double outputEnergy = measureEnergy(output, kAdaptationSamples, kTotalSamples);
  require(inputEnergy > 1.0e9, "echo fixture must contain substantial nonzero energy");
  return 10.0 * std::log10(inputEnergy / std::max(outputEnergy, 1.0));
}

struct NearProjection {
  double gain = 0.0;
  double correlation = 0.0;
  size_t alignment_samples = 0;
};

NearProjection measureNearProjection(const std::vector<double>& nearSignal,
                                      const std::vector<double>& output) {
  NearProjection best;
  // Search the bounded processing latency, not the acoustic echo delay. Use
  // the same interior window at every lag, excluding near-onset transients.
  const size_t start = kAdaptationSamples + kSampleRate;
  const size_t end = kTotalSamples - 480;
  for (size_t alignment = 0; alignment <= 480; ++alignment) {
    double nearEnergy = 0.0;
    double outputEnergy = 0.0;
    double crossEnergy = 0.0;
    for (size_t sampleIndex = start; sampleIndex < end; ++sampleIndex) {
      const double nearSample = nearSignal[sampleIndex - alignment];
      const double outputSample = output[sampleIndex];
      nearEnergy += nearSample * nearSample;
      outputEnergy += outputSample * outputSample;
      crossEnergy += nearSample * outputSample;
    }
    require(nearEnergy > 1.0e9, "independent near fixture must be nonzero");
    const double correlation = crossEnergy /
        std::sqrt(std::max(nearEnergy * outputEnergy, 1.0));
    if (correlation > best.correlation) {
      best.gain = crossEnergy / nearEnergy;
      best.correlation = correlation;
      best.alignment_samples = alignment;
    }
  }
  return best;
}

}  // namespace

int main(int argumentCount, char** arguments) {
  FaultMode faultMode = FaultMode::None;
  bool disableMutation = false;
  bool invalidConfiguration = false;
  for (int argumentIndex = 1; argumentIndex < argumentCount; ++argumentIndex) {
    if (std::strcmp(arguments[argumentIndex], "--disconnect-reference") == 0) {
      require(faultMode == FaultMode::None, "one test-only mutation at a time");
      faultMode = FaultMode::DisconnectReference;
    } else if (std::strcmp(arguments[argumentIndex], "--zero-output") == 0) {
      require(faultMode == FaultMode::None, "one test-only mutation at a time");
      faultMode = FaultMode::ZeroOutput;
    } else if (std::strcmp(arguments[argumentIndex], "--disable-mutation") == 0) {
      disableMutation = true;
    } else if (std::strcmp(arguments[argumentIndex], "--invalid-configuration") == 0) {
      invalidConfiguration = true;
    } else {
      require(false, "recognized test-only fault argument");
    }
  }
  require(faultMode != FaultMode::None || (!disableMutation && !invalidConfiguration),
          "rejection controls require a named mutation");
  const auto render = generateSignal(0x13579bdfu, 1.7);
  const auto wrongReference = generateSignal(0x8642ace0u, 2.3);
  const auto nearSignal = generateSignal(0xc0ffee11u, 3.1);
  const auto echo = generateEcho(render);
  // Mutate only the paths needed to exercise the named predicate. Unrelated
  // controls remain normal and must pass before a mutation can be recognized.
  const auto appliedFault = disableMutation ? FaultMode::None : faultMode;
  const auto echoFault = appliedFault == FaultMode::DisconnectReference ? appliedFault : FaultMode::None;
  const auto nearFault = appliedFault == FaultMode::ZeroOutput ? appliedFault : FaultMode::None;
  const auto matchedOutput = runProcessor(render, wrongReference, echo, nearSignal,
      ReferenceMode::Matched, CaptureMode::EchoOnly, echoFault, invalidConfiguration);
  const auto disconnectedOutput = runProcessor(render, wrongReference, echo, nearSignal,
      ReferenceMode::Disconnected, CaptureMode::EchoOnly, FaultMode::None, invalidConfiguration);
  const auto wrongOutput = runProcessor(render, wrongReference, echo, nearSignal,
      ReferenceMode::Wrong, CaptureMode::EchoOnly, FaultMode::None, invalidConfiguration);
  const auto nearOnlyOutput = runProcessor(render, wrongReference, echo, nearSignal,
      ReferenceMode::Disconnected, CaptureMode::NearOnly, nearFault, invalidConfiguration);
  const auto doubleTalkOutput = runProcessor(render, wrongReference, echo, nearSignal,
      ReferenceMode::Matched, CaptureMode::DoubleTalk, nearFault, invalidConfiguration);
  const double matchedAttenuation = measureAttenuation(echo, matchedOutput);
  const double disconnectedAttenuation = measureAttenuation(echo, disconnectedOutput);
  const double wrongAttenuation = measureAttenuation(echo, wrongOutput);
  const auto nearOnly = measureNearProjection(nearSignal, nearOnlyOutput);
  const auto doubleTalk = measureNearProjection(nearSignal, doubleTalkOutput);
  std::printf("Echo attenuation dB: matched=%.3f disconnected=%.3f wrong=%.3f\n",
              matchedAttenuation, disconnectedAttenuation, wrongAttenuation);
  std::printf("Near-only: gain=%.3f correlation=%.3f alignment=%zu samples\n",
              nearOnly.gain, nearOnly.correlation, nearOnly.alignment_samples);
  std::printf("Double-talk after pure-echo adaptation: gain=%.3f correlation=%.3f alignment=%zu samples\n",
              doubleTalk.gain, doubleTalk.correlation, doubleTalk.alignment_samples);
  std::fflush(stdout);
  const bool nearOnlyPreserved = nearOnly.gain >= 0.55 && nearOnly.gain <= 1.50 &&
      nearOnly.correlation >= 0.70;
  const bool doubleTalkPreserved = doubleTalk.gain >= 0.40 && doubleTalk.gain <= 1.50 &&
      doubleTalk.gain >= nearOnly.gain * 0.60 && doubleTalk.correlation >= 0.45 &&
      doubleTalk.correlation >= nearOnly.correlation * 0.55;
  const bool matchedEchoCancelled = matchedAttenuation >= 15.0;
  require(std::isfinite(matchedAttenuation) && std::isfinite(disconnectedAttenuation) &&
              std::isfinite(wrongAttenuation), "finite echo measurements");
  require(disconnectedAttenuation < 5.0, "without reverse attenuation must remain below 5 dB");
  require(wrongAttenuation < 5.0, "independent wrong reference attenuation must remain below 5 dB");
  if (faultMode == FaultMode::ZeroOutput) {
    require(matchedEchoCancelled, "unmodified matched reverse must cancel at least 15 dB");
    require(!nearOnlyPreserved && !doubleTalkPreserved,
            "zero-output mutation was not detected by both near preservation predicates");
    std::puts("PASS: detected mutation: near-preservation");
    std::puts("Detected failed near-only and double-talk projection predicates; unmodified echo controls passed");
    return EXIT_SUCCESS;
  }
  require(nearOnlyPreserved, "near-only aligned projection must preserve independent near signal");
  require(doubleTalkPreserved,
          "double-talk aligned near projection must survive relative to near-only control");
  if (faultMode == FaultMode::DisconnectReference) {
    require(!matchedEchoCancelled,
            "disconnect-reference mutation was not detected by echo attenuation predicate");
    std::puts("PASS: detected mutation: echo-attenuation");
    std::puts("Detected matched attenuation below 15 dB; unmodified near and echo controls passed");
    return EXIT_SUCCESS;
  }
  require(matchedEchoCancelled, "matched reverse must cancel at least 15 dB");
  std::puts("PASS: synthetic independent-near proxy only; not human/device acoustic acceptance");
  return EXIT_SUCCESS;
}
