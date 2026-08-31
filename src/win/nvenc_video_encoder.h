/*
 *  Squeak: аппаратный H.264-энкодер NVIDIA (NVENC) для Windows.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef SRC_WIN_NVENC_VIDEO_ENCODER_H_
#define SRC_WIN_NVENC_VIDEO_ENCODER_H_

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"

namespace squeak {

// true, если в системе есть NVIDIA-адаптер с рабочей сессией NVENC и
// поддержкой H.264. Результат считается один раз и кэшируется: пробa
// открывает и закрывает реальную сессию энкодера.
bool NvencH264Supported();

std::unique_ptr<webrtc::VideoEncoder> CreateNvencH264Encoder(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format);

}  // namespace squeak

#endif  // SRC_WIN_NVENC_VIDEO_ENCODER_H_
