/*
 *  Squeak: захват экрана без обратного чтения BGRA (Windows).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef LIBWEBRTC_SRC_WIN_GPU_SCREEN_CAPTURER_H_
#define LIBWEBRTC_SRC_WIN_GPU_SCREEN_CAPTURER_H_

#if defined(WEBRTC_WIN)

#include <cstdint>
#include <memory>

#include "api/scoped_refptr.h"
#include "api/video/nv12_buffer.h"

namespace squeak {

// Штатный путь webrtc тянет кадр с видеокарты в BGRA (1440p — это 14.7 МБ на
// кадр) и конвертирует его в I420 процессором. Здесь конвертация и масштаб
// делаются на самой видеокарте через ID3D11VideoProcessor, а назад читается
// уже NV12 — 1.5 байта на пиксель вместо четырёх.
class GpuScreenCapturer {
 public:
  ~GpuScreenCapturer();

  // screen_index — порядковый номер выхода DXGI. nullptr, если Desktop
  // Duplication недоступна: вызывающий откатывается на путь webrtc.
  static std::unique_ptr<GpuScreenCapturer> Create(int64_t screen_index);

  // Габарит, в который вписывается кадр перед обратным чтением. Пропорции
  // сохраняются, вверх не растягиваем. Ноль — отдавать нативный размер
  // рабочего стола.
  //
  // Ради этого всё и затевалось: 1080p с 4K-экрана — это 186 МБ/с обратного
  // чтения вместо 746, и масштабирование на видеокарте вместо libyuv.
  void SetOutputSize(int width, int height);

  // nullptr до первого удачного кадра. Если новых кадров нет (экран не
  // менялся) — возвращает предыдущий, чтобы поток не выглядел оборванным.
  webrtc::scoped_refptr<webrtc::NV12Buffer> Capture();

 private:
  class Impl;

  explicit GpuScreenCapturer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace squeak

#endif  // WEBRTC_WIN

#endif  // LIBWEBRTC_SRC_WIN_GPU_SCREEN_CAPTURER_H_
