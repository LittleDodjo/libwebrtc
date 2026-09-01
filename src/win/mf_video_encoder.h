/*
 *  Squeak: аппаратный H.264-энкодер поверх Media Foundation (Windows).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef SRC_WIN_MF_VIDEO_ENCODER_H_
#define SRC_WIN_MF_VIDEO_ENCODER_H_

#include <memory>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"

namespace squeak {

// true, если в системе есть аппаратный H.264-энкодер, доступный через
// Media Foundation. Одна проверка закрывает AMD, Intel и NVIDIA: перечисление
// отдаёт то, что даёт установленный драйвер. Результат кэшируется.
bool MediaFoundationH264EncoderSupported();

std::unique_ptr<webrtc::VideoEncoder> CreateMediaFoundationH264Encoder(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format);

}  // namespace squeak

#endif  // SRC_WIN_MF_VIDEO_ENCODER_H_
