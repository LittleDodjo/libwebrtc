#ifndef LIB_WEBRTC_RTC_AUDIO_PROCESSING_SANDBOX_IMPL_HXX
#define LIB_WEBRTC_RTC_AUDIO_PROCESSING_SANDBOX_IMPL_HXX

#include <vector>

#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_audio_processing_sandbox.h"
#include "rtc_custom_processing_adapter.h"

namespace libwebrtc {

class RTCAudioProcessingSandboxImpl : public RTCAudioProcessingSandbox {
 public:
  explicit RTCAudioProcessingSandboxImpl(const Config& config);
  ~RTCAudioProcessingSandboxImpl() override;

  void SetCapturePostProcessing(
      RTCAudioProcessing::CustomProcessing* processor) override;
  void SetRenderPreProcessing(
      RTCAudioProcessing::CustomProcessing* processor) override;
  int ProcessCapture(const int16_t* in, int sample_rate_hz, int num_channels,
                     int16_t* out) override;
  int ProcessRender(const int16_t* in, int sample_rate_hz,
                    int num_channels) override;
  int SetStreamDelayMs(int delay_ms) override;
  void SetStreamAnalogLevel(int level) override;
  int RecommendedStreamAnalogLevel() override;
  int ProcSampleRateHz() override;
  string GetConfigSummary() override;

 private:
  CustomProcessingAdapter* capture_post_processor_;
  CustomProcessingAdapter* render_pre_processor_;
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
  std::vector<int16_t> render_scratch_;
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_AUDIO_PROCESSING_SANDBOX_IMPL_HXX
