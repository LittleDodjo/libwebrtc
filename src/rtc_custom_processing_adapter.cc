#include "rtc_custom_processing_adapter.h"

#include <algorithm>

#include "modules/audio_processing/audio_buffer.h"
#include "modules/audio_processing/ns/ns_common.h"

namespace libwebrtc {

CustomProcessingAdapter::CustomProcessingAdapter(Channels channels)
    : channels_(channels) {}

CustomProcessingAdapter::~CustomProcessingAdapter() = default;

void CustomProcessingAdapter::SetExternalAudioProcessing(
    RTCAudioProcessing::CustomProcessing* processor) {
  RTCAudioProcessing::CustomProcessing* previous = nullptr;
  {
    webrtc::MutexLock lock(&mutex_);
    if (custom_processor_ == processor) return;
    previous = custom_processor_;
    custom_processor_ = processor;
    if (processor != nullptr && initialized_) {
      processor->Initialize(sample_rate_hz_, num_channels_);
    }
  }
  // Process держит мьютекс на весь вызов: после подмены прежний процессор
  // больше не зовётся, и его Release безопасен вне замка.
  if (previous != nullptr) previous->Release();
}

void CustomProcessingAdapter::SetBypassFlag(bool bypass) {
  webrtc::MutexLock lock(&mutex_);
  bypass_flag_ = bypass;
}

void CustomProcessingAdapter::Initialize(int sample_rate_hz, int num_channels) {
  webrtc::MutexLock lock(&mutex_);
  sample_rate_hz_ = sample_rate_hz;
  num_channels_ = num_channels;
  initialized_ = true;
  if (custom_processor_ != nullptr) {
    custom_processor_->Initialize(sample_rate_hz, num_channels);
  }
}

void CustomProcessingAdapter::Process(webrtc::AudioBuffer* audio) {
  webrtc::MutexLock lock(&mutex_);
  if (custom_processor_ == nullptr || bypass_flag_ || !initialized_) return;

  const size_t num_frames = audio->num_frames();
  const size_t num_bands = audio->num_bands();
  const size_t num_channels = audio->num_channels();
  if (num_frames == 0 || num_channels == 0) return;

  // Буфер — 10 мс, отсюда частота. APM может сменить формат и без Initialize.
  const int rate = static_cast<int>(num_frames) * 100;
  if (rate != sample_rate_hz_) {
    custom_processor_->Reset(rate);
    sample_rate_hz_ = rate;
  }

  float* const* channels = audio->channels();
  if (channels_ == Channels::kDownmixToMono && num_channels > 1) {
    const float scale = 1.0f / static_cast<float>(num_channels);
    for (size_t i = 0; i < num_frames; ++i) {
      float sum = 0.0f;
      for (size_t c = 0; c < num_channels; ++c) sum += channels[c][i];
      channels[0][i] = sum * scale;
    }
  }

  custom_processor_->Process(static_cast<int>(num_bands),
                             static_cast<int>(num_frames),
                             static_cast<int>(webrtc::kNsFrameSize * num_bands),
                             channels[0]);

  if (channels_ == Channels::kDownmixToMono) {
    for (size_t c = 1; c < num_channels; ++c) {
      std::copy_n(channels[0], num_frames, channels[c]);
    }
  }
}

std::string CustomProcessingAdapter::ToString() const {
  return "ExternalAudioProcessor";
}

void CustomProcessingAdapter::SetRuntimeSetting(
    webrtc::AudioProcessing::RuntimeSetting setting) {}

}  // namespace libwebrtc
