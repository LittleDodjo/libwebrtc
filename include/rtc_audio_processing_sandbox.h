#ifndef LIB_WEBRTC_RTC_AUDIO_PROCESSING_SANDBOX_HXX
#define LIB_WEBRTC_RTC_AUDIO_PROCESSING_SANDBOX_HXX

#include <cstdint>

#include "rtc_audio_processing.h"
#include "rtc_types.h"

namespace libwebrtc {

// Отдельный экземпляр APM, которому звук подают руками: проверка внешнего
// процессора на настоящей цепочке обработки без устройств и PeerConnection.
// Живой APM движка не трогает; потоков и ADM внутри нет.
class RTCAudioProcessingSandbox : public RefCountInterface {
 public:
  struct Config {
    bool echo_canceller = false;
    // Уровень kHigh — как ставит движок.
    bool noise_suppression = false;
    // AGC1 kAdaptiveAnalog — как у движка на Windows и Linux.
    bool gain_controller1 = false;
    bool gain_controller2 = false;
    bool high_pass_filter = true;
    // 32000 или 48000.
    int maximum_internal_processing_rate = 48000;
    bool multi_channel_capture = false;
  };

  LIB_WEBRTC_API static scoped_refptr<RTCAudioProcessingSandbox> Create(
      const Config& config);

  // nullptr отсоединяет процессор; прежнему вызывается Release(). Отсоединить
  // надо до того, как процессор уничтожен.
  virtual void SetCapturePostProcessing(
      RTCAudioProcessing::CustomProcessing* processor) = 0;

  virtual void SetRenderPreProcessing(
      RTCAudioProcessing::CustomProcessing* processor) = 0;

  // Кадр 10 мс, interleaved int16; in и out могут совпадать. Формат меняется
  // на лету, APM переинициализируется сам. Возвращает код AudioProcessing
  // (0 — успех).
  virtual int ProcessCapture(const int16_t* in, int sample_rate_hz,
                             int num_channels, int16_t* out) = 0;

  virtual int ProcessRender(const int16_t* in, int sample_rate_hz,
                            int num_channels) = 0;

  virtual int SetStreamDelayMs(int delay_ms) = 0;

  // 0..255. Движок этого не зовёт: аналоговая часть AGC1 у него без громкости.
  virtual void SetStreamAnalogLevel(int level) = 0;

  virtual int RecommendedStreamAnalogLevel() = 0;

  // Внутренняя частота обработки после последней инициализации.
  virtual int ProcSampleRateHz() = 0;

  virtual string GetConfigSummary() = 0;

 protected:
  virtual ~RTCAudioProcessingSandbox() {}
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_AUDIO_PROCESSING_SANDBOX_HXX
