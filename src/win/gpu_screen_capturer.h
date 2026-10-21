/*
 *  Squeak: захват экрана без обратного чтения (Windows, Desktop Duplication).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef LIBWEBRTC_SRC_WIN_GPU_SCREEN_CAPTURER_H_
#define LIBWEBRTC_SRC_WIN_GPU_SCREEN_CAPTURER_H_

#if defined(WEBRTC_WIN)

#include <cstdint>
#include <functional>
#include <memory>

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"

namespace squeak {

// Кадр рабочего стола остаётся в видеопамяти: Desktop Duplication отдаёт
// текстуру BGRA, ID3D11VideoProcessor переводит её в NV12 и вписывает в
// запрошенный габарит, а наружу уходит D3D11FrameBuffer — энкодер берёт его
// как есть, без единого байта через шину.
//
// Ритм задаёт собственный поток: дедлайн раз в 1/fps, между дедлайнами поток
// ждёт новый кадр у DWM (AcquireNextFrame), на дедлайне отдаёт самый свежий.
// Так каденция ровная и на 60-герцовом экране, и на 144-герцовом, а задержка
// не больше одного кадра. Прежний вариант «PostDelayedTask(16 мс) после
// захвата» на квантованном таймере Windows давал 30 к/с.
class GpuScreenCapturer {
 public:
  // Кадр и момент захвата (webrtc::TimeMicros). Зовётся из потока захвата.
  using FrameCallback = std::function<void(
      webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer,
      int64_t timestamp_us)>;

  ~GpuScreenCapturer();

  // screen_index — порядковый номер выхода DXGI. nullptr, если Desktop
  // Duplication недоступна: вызывающий откатывается на путь webrtc.
  static std::unique_ptr<GpuScreenCapturer> Create(int64_t screen_index,
                                                   bool show_cursor);

  // Габарит, в который вписывается кадр с сохранением пропорций; вверх не
  // растягиваем. Ноль — нативный размер рабочего стола. Можно менять на ходу.
  void SetOutputSize(int width, int height);

  // Запуск потока захвата. fps — целевая каденция (1..240).
  bool Start(uint32_t fps, FrameCallback callback);

  // Останавливает поток и ждёт его; после возврата callback больше не зовётся.
  void Stop();

  bool IsRunning() const;

 private:
  class Impl;

  explicit GpuScreenCapturer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace squeak

#endif  // WEBRTC_WIN

#endif  // LIBWEBRTC_SRC_WIN_GPU_SCREEN_CAPTURER_H_
