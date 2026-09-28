#include "aozora_apm.h"

#include <napi/native_api.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace {

using Processor = std::unique_ptr<AozoraApm, decltype(&aozora_apm_destroy)>;

struct Handle {
  explicit Handle(AozoraApm* instance) : processor(instance, aozora_apm_destroy) {}
  Processor processor;
  std::mutex mutex;
};

// Validate module ownership before dereferencing a pointer from napi_unwrap.
std::mutex registry_mutex;
std::unordered_map<void*, std::shared_ptr<Handle>> registry;

void requireStatus(napi_status status) {
  if (status != napi_ok) throw std::runtime_error("N-API operation failed");
}

template <typename Callback>
napi_value protect(napi_env environment, Callback callback) {
  try {
    return callback();
  } catch (const std::exception& error) {
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    if (!pending) napi_throw_error(environment, nullptr, error.what());
  } catch (...) {
    napi_throw_error(environment, nullptr, "Native APM operation failed");
  }
  return nullptr;
}

template <size_t ArgumentCount>
std::array<napi_value, ArgumentCount> arguments(napi_env environment, napi_callback_info information) {
  std::array<napi_value, ArgumentCount> values{};
  size_t count = values.size();
  requireStatus(napi_get_cb_info(environment, information, &count, values.data(), nullptr, nullptr));
  if (count != values.size()) throw std::invalid_argument("Incorrect argument count");
  return values;
}

int integer(napi_env environment, napi_value value) {
  double number;
  requireStatus(napi_get_value_double(environment, value, &number));
  if (!std::isfinite(number) || std::trunc(number) != number ||
      number < std::numeric_limits<int32_t>::min() || number > std::numeric_limits<int32_t>::max()) {
    throw std::invalid_argument("Expected a 32-bit integer");
  }
  return static_cast<int>(number);
}

napi_value integerResult(napi_env environment, int number) {
  napi_value value;
  requireStatus(napi_create_int32(environment, number, &value));
  return value;
}

napi_value undefined(napi_env environment) {
  napi_value value;
  requireStatus(napi_get_undefined(environment, &value));
  return value;
}

std::shared_ptr<Handle> getHandle(napi_env environment, napi_value value) {
  void* pointer = nullptr;
  requireStatus(napi_unwrap(environment, value, &pointer));
  std::lock_guard<std::mutex> lock(registry_mutex);
  const auto entry = registry.find(pointer);
  if (entry == registry.end()) throw std::invalid_argument("Invalid APM handle");
  return entry->second;
}

void finalize(napi_env, void* pointer, void*) {
  // No exception may cross the VM's finalizer boundary.
  try {
    std::lock_guard<std::mutex> lock(registry_mutex);
    registry.erase(pointer);
  } catch (...) {
  }
}

napi_value create(napi_env environment, napi_callback_info information) {
  return protect(environment, [&]() {
    const auto values = arguments<4>(environment, information);
    bool noise_suppression, gain_control;
    requireStatus(napi_get_value_bool(environment, values[2], &noise_suppression));
    requireStatus(napi_get_value_bool(environment, values[3], &gain_control));
    Processor processor(aozora_apm_create(integer(environment, values[0]), integer(environment, values[1]),
                                         noise_suppression, gain_control), aozora_apm_destroy);
    if (!processor) throw std::runtime_error("APM initialization failed; check rate and mono channel count");
    auto handle = std::make_shared<Handle>(nullptr);
    handle->processor = std::move(processor);
    napi_value object, kind;
    requireStatus(napi_create_object(environment, &object));
    requireStatus(napi_create_string_utf8(environment, "AozoraApm", NAPI_AUTO_LENGTH, &kind));
    requireStatus(napi_set_named_property(environment, object, "kind", kind));
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      registry.emplace(handle.get(), handle);
    }
    const auto status = napi_wrap(environment, object, handle.get(), finalize, nullptr, nullptr);
    if (status != napi_ok) {
      std::lock_guard<std::mutex> lock(registry_mutex);
      registry.erase(handle.get());
      requireStatus(status);
    }
    return object;
  });
}

napi_value process(napi_env environment, napi_callback_info information, bool reverse) {
  return protect(environment, [&]() {
    const auto values = arguments<2>(environment, information);
    const auto handle = getHandle(environment, values[0]);
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->processor) return integerResult(environment, AOZORA_APM_ERROR_RELEASED);
    napi_typedarray_type type;
    size_t count, offset;
    void* data;
    napi_value buffer;
    requireStatus(napi_get_typedarray_info(environment, values[1], &type, &count, &data, &buffer, &offset));
    if (type != napi_int16_array || !data || count != aozora_apm_frame_samples(handle->processor.get())) {
      return integerResult(environment, AOZORA_APM_ERROR_INVALID_FRAME);
    }
    const int status = reverse
        ? aozora_apm_process_reverse(handle->processor.get(), static_cast<int16_t*>(data), count)
        : aozora_apm_process_capture(handle->processor.get(), static_cast<int16_t*>(data), count);
    return integerResult(environment, status);
  });
}

napi_value processReverse(napi_env environment, napi_callback_info information) {
  return process(environment, information, true);
}

napi_value processCapture(napi_env environment, napi_callback_info information) {
  return process(environment, information, false);
}

napi_value setStreamDelay(napi_env environment, napi_callback_info information) {
  return protect(environment, [&]() {
    const auto values = arguments<2>(environment, information);
    const auto handle = getHandle(environment, values[0]);
    const int delay = integer(environment, values[1]);
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (!handle->processor) throw std::invalid_argument("APM handle was released");
    aozora_apm_set_stream_delay_ms(handle->processor.get(), delay);
    return undefined(environment);
  });
}

napi_value getVoiceDetected(napi_env environment, napi_callback_info information) {
  return protect(environment, [&]() {
    const auto handle = getHandle(environment, arguments<1>(environment, information)[0]);
    std::lock_guard<std::mutex> lock(handle->mutex);
    return integerResult(environment, aozora_apm_get_voice_detected(handle->processor.get()));
  });
}

napi_value release(napi_env environment, napi_callback_info information) {
  return protect(environment, [&]() {
    const auto handle = getHandle(environment, arguments<1>(environment, information)[0]);
    std::lock_guard<std::mutex> lock(handle->mutex);
    handle->processor.reset();
    return undefined(environment);
  });
}

napi_value initialize(napi_env environment, napi_value exports) {
  return protect(environment, [&]() {
    const napi_property_descriptor properties[] = {
        {"create", nullptr, create, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"processReverse", nullptr, processReverse, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"processCapture", nullptr, processCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStreamDelay", nullptr, setStreamDelay, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getVoiceDetected", nullptr, getVoiceDetected, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"release", nullptr, release, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    requireStatus(napi_define_properties(environment, exports, sizeof(properties) / sizeof(properties[0]), properties));
    return exports;
  });
}

napi_module module = {1, 0, nullptr, initialize, "aozora_apm", nullptr, {nullptr}};

__attribute__((constructor)) void registerModule() {
  napi_module_register(&module);
}

}  // namespace
