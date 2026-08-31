/*
 *  Squeak: фабрика видео-энкодеров с аппаратным H.264 на Windows.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef SRC_WIN_SQUEAK_VIDEO_ENCODER_FACTORY_H_
#define SRC_WIN_SQUEAK_VIDEO_ENCODER_FACTORY_H_

#include <memory>

#include "api/video_codecs/video_encoder_factory.h"

namespace squeak {

// H.264 отдаём NVENC, когда он есть в системе; всё остальное, и фоллбэк при
// отсутствии карты, — штатная софтовая фабрика WebRTC. Список форматов не
// меняем: согласование SDP остаётся ровно таким, каким было.
std::unique_ptr<webrtc::VideoEncoderFactory> CreateSqueakVideoEncoderFactory();

}  // namespace squeak

#endif  // SRC_WIN_SQUEAK_VIDEO_ENCODER_FACTORY_H_
