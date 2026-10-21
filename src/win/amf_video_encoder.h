/*
 *  Squeak: аппаратный H.264-энкодер AMD (AMF, Advanced Media Framework).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef LIBWEBRTC_SRC_WIN_AMF_VIDEO_ENCODER_H_
#define LIBWEBRTC_SRC_WIN_AMF_VIDEO_ENCODER_H_

#include <memory>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"

namespace squeak {

// Есть ли в системе рантайм AMF (amfrt64.dll из драйвера AMD) и адаптер с
// блоком VCE/VCN, умеющим H.264. Проба выполняется один раз.
bool AmfH264Supported();

// nullptr, если AMF недоступен — фабрика идёт дальше по цепочке.
std::unique_ptr<webrtc::VideoEncoder> CreateAmfH264Encoder(
    const webrtc::Environment& env, const webrtc::SdpVideoFormat& format);

}  // namespace squeak

#endif  // LIBWEBRTC_SRC_WIN_AMF_VIDEO_ENCODER_H_
