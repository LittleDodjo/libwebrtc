/*
 *  Squeak: фабрика видео-энкодеров с аппаратным H.264 на Windows.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef SRC_WIN_SQUEAK_VIDEO_ENCODER_FACTORY_H_
#define SRC_WIN_SQUEAK_VIDEO_ENCODER_FACTORY_H_

#include <memory>
#include <string>

#include "api/video_codecs/video_encoder_factory.h"

namespace squeak {

// H.264 отдаём железу по цепочке NVENC -> AMF -> Media Foundation; всё
// остальное и откат при отсутствии железа — штатная софтовая фабрика WebRTC.
// Список форматов не меняем: согласование SDP остаётся ровно таким, каким было.
std::unique_ptr<webrtc::VideoEncoderFactory> CreateSqueakVideoEncoderFactory();

// Порядок и состав аппаратных звеньев: имена через запятую из "nvenc", "amf",
// "mf". Пустая строка — только софт. Действует на энкодеры, создаваемые после
// вызова.
void SetHardwareEncoderOrder(const std::string& csv);

}  // namespace squeak

#endif  // SRC_WIN_SQUEAK_VIDEO_ENCODER_FACTORY_H_
