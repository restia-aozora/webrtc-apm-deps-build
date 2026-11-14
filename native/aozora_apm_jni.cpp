#include "aozora_apm.h"

#include <jni.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace {

// Only the additive diagnostics use this registry; audio processing keeps its
// existing path. Hold the registry lock through reads to exclude JNI release.
std::mutex live_handles_mutex;
std::unordered_set<AozoraApm*> live_handles;

AozoraApm* fromHandle(jlong handle) {
  return reinterpret_cast<AozoraApm*>(static_cast<intptr_t>(handle));
}

jint processFrame(JNIEnv* environment, jlong handle, jshortArray samples, bool reverse) {
  AozoraApm* processor = fromHandle(handle);
  if (!processor) return AOZORA_APM_ERROR_INVALID_HANDLE;
  if (!samples) return AOZORA_APM_ERROR_INVALID_FRAME;
  const jsize count = environment->GetArrayLength(samples);
  if (count < 0 || static_cast<size_t>(count) != aozora_apm_frame_samples(processor)) {
    return AOZORA_APM_ERROR_INVALID_FRAME;
  }
  std::array<int16_t, 480> buffer{};
  environment->GetShortArrayRegion(samples, 0, count, reinterpret_cast<jshort*>(buffer.data()));
  if (environment->ExceptionCheck()) return AOZORA_APM_ERROR_INVALID_FRAME;
  const int status = reverse
      ? aozora_apm_process_reverse(processor, buffer.data(), static_cast<size_t>(count))
      : aozora_apm_process_capture(processor, buffer.data(), static_cast<size_t>(count));
  if (status == 0 || status == -13) {
    environment->SetShortArrayRegion(samples, 0, count, reinterpret_cast<const jshort*>(buffer.data()));
    if (environment->ExceptionCheck()) return AOZORA_APM_ERROR_INVALID_FRAME;
  }
  return status;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_aozora_audio_AozoraWebRtcApm_nativeCreate(
    JNIEnv*, jclass, jint sampleRate, jint channels, jboolean noiseSuppression, jboolean gainControl) {
  AozoraApm* processor = aozora_apm_create(sampleRate, channels,
                                         noiseSuppression == JNI_TRUE, gainControl == JNI_TRUE);
  if (!processor) return 0;
  try {
    std::lock_guard<std::mutex> lock(live_handles_mutex);
    live_handles.insert(processor);
  } catch (...) {
    // Optional diagnostics must not invalidate a successfully created APM.
    // An unregistered instance remains usable; only statistics are unavailable.
  }
  return static_cast<jlong>(reinterpret_cast<intptr_t>(processor));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_aozora_audio_AozoraWebRtcApm_nativeGetBuildRevision(JNIEnv* environment, jclass) {
  try {
    return environment->NewStringUTF(aozora_apm_get_build_revision());
  } catch (...) {
    return nullptr;
  }
}

extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_aozora_audio_AozoraWebRtcApm_nativeGetStatistics(
    JNIEnv* environment, jclass, jlong handle) {
  try {
    AozoraApmStatistics statistics{};
    {
      std::lock_guard<std::mutex> lock(live_handles_mutex);
      AozoraApm* processor = fromHandle(handle);
      if (live_handles.find(processor) == live_handles.end() ||
          aozora_apm_get_statistics(processor, &statistics, sizeof(statistics)) != 0) {
        return nullptr;
      }
    }
    const std::array<jdouble, 14> values{{
        static_cast<jdouble>(statistics.version),
        static_cast<jdouble>(statistics.valid_fields),
        static_cast<jdouble>(statistics.capture_frames),
        static_cast<jdouble>(statistics.reverse_frames),
        static_cast<jdouble>(statistics.stream_delay_ms),
        static_cast<jdouble>(statistics.voice_detected),
        static_cast<jdouble>(statistics.sample_rate_hz),
        static_cast<jdouble>(statistics.estimated_delay_ms),
        statistics.echo_return_loss_db,
        statistics.echo_return_loss_enhancement_db,
        statistics.residual_echo_likelihood,
        statistics.residual_echo_likelihood_recent_max,
        static_cast<jdouble>(statistics.noise_suppression_enabled),
        static_cast<jdouble>(statistics.gain_control_enabled)}};
    jdoubleArray result = environment->NewDoubleArray(static_cast<jsize>(values.size()));
    if (!result) return nullptr;
    environment->SetDoubleArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    if (environment->ExceptionCheck()) {
      environment->DeleteLocalRef(result);
      return nullptr;
    }
    return result;
  } catch (...) {
    return nullptr;
  }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_aozora_audio_AozoraWebRtcApm_nativeProcessReverse(
    JNIEnv* environment, jclass, jlong handle, jshortArray samples) {
  return processFrame(environment, handle, samples, true);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_aozora_audio_AozoraWebRtcApm_nativeProcessCapture(
    JNIEnv* environment, jclass, jlong handle, jshortArray samples) {
  return processFrame(environment, handle, samples, false);
}

extern "C" JNIEXPORT void JNICALL
Java_com_aozora_audio_AozoraWebRtcApm_nativeSetStreamDelay(JNIEnv*, jclass, jlong handle, jint delay) {
  aozora_apm_set_stream_delay_ms(fromHandle(handle), delay);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_aozora_audio_AozoraWebRtcApm_nativeGetVoiceDetected(JNIEnv*, jclass, jlong handle) {
  return aozora_apm_get_voice_detected(fromHandle(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_com_aozora_audio_AozoraWebRtcApm_nativeRelease(JNIEnv*, jclass, jlong handle) {
  try {
    std::lock_guard<std::mutex> lock(live_handles_mutex);
    AozoraApm* processor = fromHandle(handle);
    live_handles.erase(processor);
    aozora_apm_destroy(processor);
  } catch (...) {
    // No C++ exception may escape the JNI boundary.
  }
}
