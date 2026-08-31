/*
 *  Squeak: фабрика видео-декодеров с аппаратным H.264 на Windows.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/squeak_video_decoder_factory.h"

#include <memory>
#include <vector>

#include "absl/strings/match.h"
#include "api/environment/environment.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "rtc_base/logging.h"
#include "src/win/mf_video_decoder.h"

namespace squeak {
namespace {

class SqueakVideoDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  SqueakVideoDecoderFactory()
      : builtin_(webrtc::CreateBuiltinVideoDecoderFactory()) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return builtin_->GetSupportedFormats();
  }

  std::unique_ptr<webrtc::VideoDecoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    if (absl::EqualsIgnoreCase(format.name, "H264") &&
        MediaFoundationH264Supported()) {
      std::unique_ptr<webrtc::VideoDecoder> decoder =
          CreateMediaFoundationH264Decoder();
      if (decoder != nullptr) {
        RTC_LOG(LS_INFO) << "Squeak: H.264 декодируется MediaFoundation";
        return decoder;
      }
    }
    return builtin_->Create(env, format);
  }

 private:
  const std::unique_ptr<webrtc::VideoDecoderFactory> builtin_;
};

}  // namespace

std::unique_ptr<webrtc::VideoDecoderFactory> CreateSqueakVideoDecoderFactory() {
  return std::make_unique<SqueakVideoDecoderFactory>();
}

}  // namespace squeak
