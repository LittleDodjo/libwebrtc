#ifndef LIB_WEBRTC_RTC_CUSTOM_PROCESSING_ADAPTER_HXX
#define LIB_WEBRTC_RTC_CUSTOM_PROCESSING_ADAPTER_HXX

#include <string>

#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_audio_processing.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/thread_annotations.h"

namespace libwebrtc {

// Мост между хуком webrtc::CustomProcessing и внешним процессором клиента.
// По одному экземпляру на хук (капчур, рендер) у каждого APM.
class CustomProcessingAdapter : public webrtc::CustomProcessing {
 public:
  enum class Channels {
    // Каналы сводятся в первый, он обрабатывается и копируется в остальные:
    // у внешнего процессора одно рекуррентное состояние на поток, по каналам
    // его не вызвать.
    kDownmixToMono,
    // Обрабатывается только первый канал, остальные не трогаются.
    kFirstChannelOnly,
  };

  explicit CustomProcessingAdapter(Channels channels);
  ~CustomProcessingAdapter() override;

  // nullptr отсоединяет; прежнему процессору вызывается Release().
  void SetExternalAudioProcessing(
      RTCAudioProcessing::CustomProcessing* processor);

  void SetBypassFlag(bool bypass);

 private:
  void Initialize(int sample_rate_hz, int num_channels) override;
  void Process(webrtc::AudioBuffer* audio) override;
  std::string ToString() const override;
  void SetRuntimeSetting(
      webrtc::AudioProcessing::RuntimeSetting setting) override;

  const Channels channels_;
  mutable webrtc::Mutex mutex_;
  RTCAudioProcessing::CustomProcessing* custom_processor_
      RTC_GUARDED_BY(mutex_) = nullptr;
  bool bypass_flag_ RTC_GUARDED_BY(mutex_) = false;
  bool initialized_ RTC_GUARDED_BY(mutex_) = false;
  int sample_rate_hz_ RTC_GUARDED_BY(mutex_) = 0;
  int num_channels_ RTC_GUARDED_BY(mutex_) = 0;
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_CUSTOM_PROCESSING_ADAPTER_HXX
