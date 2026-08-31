/*
 *  Squeak: аппаратный H.264-декодер поверх Media Foundation (Windows).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef SRC_WIN_MF_VIDEO_DECODER_H_
#define SRC_WIN_MF_VIDEO_DECODER_H_

#include <memory>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"

namespace squeak {

// В отличие от энкодера декодер вендор-независимый: системный H.264-MFT
// использует то железо, которое есть в машине, — NVIDIA, AMD или Intel.
bool MediaFoundationH264Supported();

std::unique_ptr<webrtc::VideoDecoder> CreateMediaFoundationH264Decoder();

}  // namespace squeak

#endif  // SRC_WIN_MF_VIDEO_DECODER_H_
