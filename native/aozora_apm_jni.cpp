#include "aozora_apm.h"

#include <jni.h>
#include <array>
#include <cstdint>

namespace {

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
  return static_cast<jlong>(reinterpret_cast<intptr_t>(processor));
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
  aozora_apm_destroy(fromHandle(handle));
}
