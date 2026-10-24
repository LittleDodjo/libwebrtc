#include "rtc_audio_processing_sandbox_impl.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"

namespace libwebrtc {

scoped_refptr<RTCAudioProcessingSandbox> RTCAudioProcessingSandbox::Create(
    const Config& config) {
  return scoped_refptr<RTCAudioProcessingSandbox>(
      new RefCountedObject<RTCAudioProcessingSandboxImpl>(config));
}

RTCAudioProcessingSandboxImpl::RTCAudioProcessingSandboxImpl(
    const Config& config) {
  capture_post_processor_ = new CustomProcessingAdapter(
      CustomProcessingAdapter::Channels::kDownmixToMono);
  std::unique_ptr<webrtc::CustomProcessing> capture_post_processor(
      capture_post_processor_);

  render_pre_processor_ = new CustomProcessingAdapter(
      CustomProcessingAdapter::Channels::kFirstChannelOnly);
  std::unique_ptr<webrtc::CustomProcessing> render_pre_processor(
      render_pre_processor_);

  // Те же значения, что движок ставит в ApplyOptions: NS kHigh, AGC1
  // kAdaptiveAnalog. Иначе тест проверял бы не ту цепочку, что в проде.
  webrtc::AudioProcessing::Config apm_config;
  apm_config.pipeline.maximum_internal_processing_rate =
      config.maximum_internal_processing_rate;
  apm_config.pipeline.multi_channel_capture = config.multi_channel_capture;
  apm_config.echo_canceller.enabled = config.echo_canceller;
  apm_config.noise_suppression.enabled = config.noise_suppression;
  apm_config.noise_suppression.level =
      webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
  apm_config.gain_controller1.enabled = config.gain_controller1;
  apm_config.gain_controller1.mode =
      webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;
  apm_config.gain_controller2.enabled = config.gain_controller2;
  apm_config.high_pass_filter.enabled = config.high_pass_filter;

  apm_ = webrtc::BuiltinAudioProcessingBuilder(apm_config)
             .SetCapturePostProcessing(std::move(capture_post_processor))
             .SetRenderPreProcessing(std::move(render_pre_processor))
             .Build(webrtc::CreateEnvironment());
}

RTCAudioProcessingSandboxImpl::~RTCAudioProcessingSandboxImpl() {}

void RTCAudioProcessingSandboxImpl::SetCapturePostProcessing(
    RTCAudioProcessing::CustomProcessing* processor) {
  capture_post_processor_->SetExternalAudioProcessing(processor);
}

void RTCAudioProcessingSandboxImpl::SetRenderPreProcessing(
    RTCAudioProcessing::CustomProcessing* processor) {
  render_pre_processor_->SetExternalAudioProcessing(processor);
}

int RTCAudioProcessingSandboxImpl::ProcessCapture(const int16_t* in,
                                                  int sample_rate_hz,
                                                  int num_channels,
                                                  int16_t* out) {
  const webrtc::StreamConfig config(sample_rate_hz,
                                    static_cast<size_t>(num_channels));
  return apm_->ProcessStream(in, config, config, out);
}

int RTCAudioProcessingSandboxImpl::ProcessRender(const int16_t* in,
                                                 int sample_rate_hz,
                                                 int num_channels) {
  const webrtc::StreamConfig config(sample_rate_hz,
                                    static_cast<size_t>(num_channels));
  render_scratch_.resize(config.num_samples());
  return apm_->ProcessReverseStream(in, config, config,
                                    render_scratch_.data());
}

int RTCAudioProcessingSandboxImpl::SetStreamDelayMs(int delay_ms) {
  return apm_->set_stream_delay_ms(delay_ms);
}

void RTCAudioProcessingSandboxImpl::SetStreamAnalogLevel(int level) {
  apm_->set_stream_analog_level(level);
}

int RTCAudioProcessingSandboxImpl::RecommendedStreamAnalogLevel() {
  return apm_->recommended_stream_analog_level();
}

int RTCAudioProcessingSandboxImpl::ProcSampleRateHz() {
  return apm_->proc_sample_rate_hz();
}

string RTCAudioProcessingSandboxImpl::GetConfigSummary() {
  return string(apm_->GetConfig().ToString());
}

}  // namespace libwebrtc
