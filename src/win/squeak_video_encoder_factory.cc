/*
 *  Squeak: фабрика видео-энкодеров с аппаратным H.264 на Windows.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/squeak_video_encoder_factory.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "api/environment/environment.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"
#include "src/win/amf_video_encoder.h"
#include "src/win/mf_video_encoder.h"
#include "src/win/nvenc_video_encoder.h"

namespace squeak {
namespace {

// Порядок проб. По умолчанию — прямые SDK вендоров, затем Media Foundation
// (Intel и запасной путь для остальных), и уже потом софт. Клиент может
// переставить или выключить звенья без пересборки: это операционный
// предохранитель на случай сломанного драйвера.
webrtc::Mutex& OrderLock() {
  static webrtc::Mutex* lock = new webrtc::Mutex();
  return *lock;
}

std::vector<std::string>& Order() {
  static auto* order = new std::vector<std::string>{"nvenc", "amf", "mf"};
  return *order;
}

std::vector<std::string> CurrentOrder() {
  webrtc::MutexLock lock(&OrderLock());
  return Order();
}

bool HardwareSupported(const std::string& name) {
  if (name == "nvenc") return NvencH264Supported();
  if (name == "amf") return AmfH264Supported();
  if (name == "mf") return MediaFoundationH264EncoderSupported();
  return false;
}

bool AnyHardware() {
  for (const std::string& name : CurrentOrder()) {
    if (HardwareSupported(name)) return true;
  }
  return false;
}

// Энкодер одного слоя: первое живое звено цепочки. nullptr — железа нет, и
// адаптер возьмёт софт из запасной фабрики.
class HardwareH264Factory : public webrtc::VideoEncoderFactory {
 public:
  explicit HardwareH264Factory(const webrtc::VideoEncoderFactory& builtin)
      : builtin_(builtin) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return builtin_.GetSupportedFormats();
  }

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    for (const std::string& name : CurrentOrder()) {
      if (!HardwareSupported(name)) continue;
      std::unique_ptr<webrtc::VideoEncoder> encoder;
      if (name == "nvenc") {
        encoder = CreateNvencH264Encoder(env, format);
      } else if (name == "amf") {
        encoder = CreateAmfH264Encoder(env, format);
      } else if (name == "mf") {
        encoder = CreateMediaFoundationH264Encoder(env, format);
      }
      if (encoder != nullptr) {
        RTC_LOG(LS_INFO) << "Squeak: H.264 идёт через " << name;
        return encoder;
      }
    }
    return nullptr;
  }

 private:
  const webrtc::VideoEncoderFactory& builtin_;
};

class SqueakVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  SqueakVideoEncoderFactory()
      : builtin_(webrtc::CreateBuiltinVideoEncoderFactory()),
        hardware_(*builtin_) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return builtin_->GetSupportedFormats();
  }

  // Наши энкодеры кодируют один поток: настройки симулкаста они молча
  // принимали и выдавали верхний слой в полном разрешении под меткой нижнего,
  // с битрейтом всех слоёв сразу. Слои раскладывает штатный адаптер — по
  // аппаратному энкодеру на слой, кадр в видеопамяти каждый энкодер ужимает
  // сам. При одном слое адаптер просто пропускает всё насквозь. Запасная
  // фабрика — софт: слой, которому не хватило аппаратной сессии, уходит туда,
  // а не ломает всю демонстрацию.
  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    if (absl::EqualsIgnoreCase(format.name, "H264")) {
      if (AnyHardware()) {
        return std::make_unique<webrtc::SimulcastEncoderAdapter>(
            env, &hardware_, builtin_.get(), format);
      }
      RTC_LOG(LS_INFO) << "Squeak: H.264 идёт софтом";
    }
    return builtin_->Create(env, format);
  }

  webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      std::optional<std::string> scalability_mode) const override {
    return builtin_->QueryCodecSupport(format, scalability_mode);
  }

 private:
  const std::unique_ptr<webrtc::VideoEncoderFactory> builtin_;
  HardwareH264Factory hardware_;
};

}  // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> CreateSqueakVideoEncoderFactory() {
  return std::make_unique<SqueakVideoEncoderFactory>();
}

void SetHardwareEncoderOrder(const std::string& csv) {
  std::vector<std::string> order;
  for (absl::string_view part : absl::StrSplit(csv, ',', absl::SkipEmpty())) {
    std::string name = std::string(absl::StripAsciiWhitespace(part));
    absl::AsciiStrToLower(&name);
    if (name == "nvenc" || name == "amf" || name == "mf") order.push_back(name);
  }
  webrtc::MutexLock lock(&OrderLock());
  Order() = order;
  RTC_LOG(LS_INFO) << "Squeak: порядок аппаратных энкодеров: "
                   << (order.empty() ? std::string("только софт") : csv);
}

}  // namespace squeak
