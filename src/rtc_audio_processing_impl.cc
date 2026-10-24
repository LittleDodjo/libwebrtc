#include "rtc_audio_processing_impl.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"

namespace libwebrtc {

namespace {

// Частота обработки внутри APM. По умолчанию WebRTC режет её до 32 кГц, и
// постпроцессор получал 48 кГц, пересчитанные из 32, — полоса до 16 кГц.
// ApplyOptions движка начинает с GetConfig() и pipeline не трогает, поэтому
// значение переживает все последующие смены опций.
constexpr int kMaxInternalProcessingRateHz = 48000;

}  // namespace

RTCAudioProcessingImpl::RTCAudioProcessingImpl() {
  capture_post_processor_ = new CustomProcessingAdapter(
      CustomProcessingAdapter::Channels::kDownmixToMono);
  std::unique_ptr<webrtc::CustomProcessing> capture_post_processor(
      capture_post_processor_);

  render_pre_processor_ = new CustomProcessingAdapter(
      CustomProcessingAdapter::Channels::kFirstChannelOnly);
  std::unique_ptr<webrtc::CustomProcessing> render_pre_processor(
      render_pre_processor_);

  webrtc::AudioProcessing::Config config;
  config.pipeline.maximum_internal_processing_rate =
      kMaxInternalProcessingRateHz;
  apm_ = webrtc::BuiltinAudioProcessingBuilder(config)
             .SetCapturePostProcessing(std::move(capture_post_processor))
             .SetRenderPreProcessing(std::move(render_pre_processor))
             .Build(webrtc::CreateEnvironment());
}

RTCAudioProcessingImpl::~RTCAudioProcessingImpl() {}

void RTCAudioProcessingImpl::SetCapturePostProcessing(
    RTCAudioProcessing::CustomProcessing* processor) {
  capture_post_processor_->SetExternalAudioProcessing(processor);
}

void RTCAudioProcessingImpl::SetRenderPreProcessing(
    RTCAudioProcessing::CustomProcessing* processor) {
  render_pre_processor_->SetExternalAudioProcessing(processor);
}

string RTCAudioProcessingImpl::GetConfigSummary() {
  return string(apm_->GetConfig().ToString());
}

}  // namespace libwebrtc
