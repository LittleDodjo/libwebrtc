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

#include "absl/strings/match.h"
#include "api/environment/environment.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "rtc_base/logging.h"
#include "src/win/mf_video_encoder.h"
#include "src/win/nvenc_video_encoder.h"

namespace squeak {
namespace {

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
      // NVENC первым: у него тоньше настройка задержки и GOP. Media Foundation
      // следом — одна реализация закрывает AMD, Intel и NVIDIA без карты
      // конкретного вендора. Софт остаётся последним рубежом.
      if (NvencH264Supported()) {
        std::unique_ptr<webrtc::VideoEncoder> encoder =
            CreateNvencH264Encoder(env, format);
        if (encoder != nullptr) {
          RTC_LOG(LS_INFO) << "Squeak: H.264 идёт через NVENC";
          return encoder;
        }
      }
      if (MediaFoundationH264EncoderSupported()) {
        std::unique_ptr<webrtc::VideoEncoder> encoder =
            CreateMediaFoundationH264Encoder(env, format);
        if (encoder != nullptr) {
          RTC_LOG(LS_INFO) << "Squeak: H.264 идёт через Media Foundation";
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

}  // namespace squeak
