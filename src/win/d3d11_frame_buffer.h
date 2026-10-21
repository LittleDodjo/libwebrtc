/*
 *  Squeak: кадр webrtc, живущий в видеопамяти как текстура D3D11 (NV12).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef LIBWEBRTC_SRC_WIN_D3D11_FRAME_BUFFER_H_
#define LIBWEBRTC_SRC_WIN_D3D11_FRAME_BUFFER_H_

#if defined(WEBRTC_WIN)

#include <string>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include "api/array_view.h"
#include "api/scoped_refptr.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "rtc_base/synchronization/mutex.h"
#include "src/win/gpu_context.h"

namespace squeak {

// Тип kNative: webrtc не трогает содержимое и отдаёт кадр энкодеру как есть,
// если тот объявил supports_native_handle. Всё, что хочет пиксели в системной
// памяти (софтовый кодек, превью), получает их через ToI420 — это
// единственное место, где кадр пересекает шину.
class D3D11FrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  // texture — NV12 на устройстве gpu. pooled — вернуть текстуру в пул
  // контекста, когда кадр умрёт.
  static webrtc::scoped_refptr<D3D11FrameBuffer> Create(
      webrtc::scoped_refptr<GpuContext> gpu,
      Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, UINT subresource,
      int width, int height, bool pooled);

  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;

  // Энкодер, который предпочитает NV12 в памяти, получает прямое чтение без
  // круга через I420.
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GetMappedFrameBuffer(
      webrtc::ArrayView<webrtc::VideoFrameBuffer::Type> types) override;

  // Масштаб на видеокарте. Этим пользуются scaleResolutionDownBy, слои
  // симулкаста и адаптация разрешения — все они раньше звали ScaleFrom на
  // процессоре.
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(
      int offset_x, int offset_y, int crop_width, int crop_height,
      int scaled_width, int scaled_height) override;

  std::string storage_representation() const override { return "D3D11 NV12"; }

  // NV12 в системной памяти. Читается один раз на кадр и кэшируется — звать
  // могут несколько потребителей (откат кодека, превью, другое устройство).
  webrtc::scoped_refptr<webrtc::NV12Buffer> ReadbackNV12();

  GpuContext* gpu() const { return gpu_.get(); }
  ID3D11Texture2D* texture() const { return texture_.Get(); }
  UINT subresource() const { return subresource_; }

  // Хэндл для устройств вне нашего контекста (движок Flutter). Текстуры пула
  // создаются с D3D11_RESOURCE_MISC_SHARED, поэтому хэндл не требует закрытия
  // и живёт столько же, сколько текстура.
  HANDLE shared_handle();

 protected:
  D3D11FrameBuffer(webrtc::scoped_refptr<GpuContext> gpu,
                   Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                   UINT subresource, int width, int height, bool pooled);
  ~D3D11FrameBuffer() override;

 private:
  const webrtc::scoped_refptr<GpuContext> gpu_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  const UINT subresource_;
  const int width_;
  const int height_;
  const bool pooled_;

  // Чтение дорогое, а звать ToI420 могут несколько потребителей одного кадра.
  webrtc::Mutex readback_lock_;
  webrtc::scoped_refptr<webrtc::NV12Buffer> readback_;
  HANDLE shared_handle_ = nullptr;
};

// Кадр webrtc в любой форме -> текстура NV12 на устройстве gpu. Для нативного
// кадра на том же устройстве — он сам, без копий; для NV12/I420 из памяти —
// заливка. nullptr, если не вышло.
webrtc::scoped_refptr<D3D11FrameBuffer> ToD3D11Frame(
    const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer,
    GpuContext* gpu);

}  // namespace squeak

#endif  // WEBRTC_WIN

#endif  // LIBWEBRTC_SRC_WIN_D3D11_FRAME_BUFFER_H_
