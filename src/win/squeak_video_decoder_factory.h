/*
 *  Squeak: фабрика видео-декодеров с аппаратным H.264 на Windows.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef SRC_WIN_SQUEAK_VIDEO_DECODER_FACTORY_H_
#define SRC_WIN_SQUEAK_VIDEO_DECODER_FACTORY_H_

#include <memory>

#include "api/video_codecs/video_decoder_factory.h"

namespace squeak {

// H.264 отдаём системному MFT, если в машине есть аппаратный декодер; всё
// остальное и фоллбэк — штатная софтовая фабрика WebRTC. Список форматов не
// меняем: согласование остаётся прежним.
std::unique_ptr<webrtc::VideoDecoderFactory> CreateSqueakVideoDecoderFactory();

}  // namespace squeak

#endif  // SRC_WIN_SQUEAK_VIDEO_DECODER_FACTORY_H_
