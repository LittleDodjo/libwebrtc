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

class SqueakVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  SqueakVideoEncoderFactory()
      : builtin_(webrtc::CreateBuiltinVideoEncoderFactory()) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return builtin_->GetSupportedFormats();
  }

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    if (absl::EqualsIgnoreCase(format.name, "H264")) {
      for (const std::string& name : CurrentOrder()) {
        std::unique_ptr<webrtc::VideoEncoder> encoder;
        if (name == "nvenc" && NvencH264Supported()) {
          encoder = CreateNvencH264Encoder(env, format);
        } else if (name == "amf" && AmfH264Supported()) {
          encoder = CreateAmfH264Encoder(env, format);
        } else if (name == "mf" && MediaFoundationH264EncoderSupported()) {
          encoder = CreateMediaFoundationH264Encoder(env, format);
        }
        if (encoder != nullptr) {
          RTC_LOG(LS_INFO) << "Squeak: H.264 идёт через " << name;
          return encoder;
        }
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
