/*
 *  Squeak: кадр webrtc, живущий в видеопамяти как текстура D3D11 (NV12).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/d3d11_frame_buffer.h"

#if defined(WEBRTC_WIN)

#include <dxgi.h>

#include <utility>

#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/logging.h"

namespace squeak {

using Microsoft::WRL::ComPtr;

webrtc::scoped_refptr<D3D11FrameBuffer> D3D11FrameBuffer::Create(
    webrtc::scoped_refptr<GpuContext> gpu, ComPtr<ID3D11Texture2D> texture,
    UINT subresource, int width, int height, bool pooled) {
  if (gpu == nullptr || texture == nullptr || width <= 0 || height <= 0) {
    return nullptr;
  }
  return webrtc::make_ref_counted<D3D11FrameBuffer>(
      std::move(gpu), std::move(texture), subresource, width, height, pooled);
}

D3D11FrameBuffer::D3D11FrameBuffer(webrtc::scoped_refptr<GpuContext> gpu,
                                   ComPtr<ID3D11Texture2D> texture,
                                   UINT subresource, int width, int height,
                                   bool pooled)
    : gpu_(std::move(gpu)),
      texture_(std::move(texture)),
      subresource_(subresource),
      width_(width),
      height_(height),
      pooled_(pooled) {}

D3D11FrameBuffer::~D3D11FrameBuffer() {
  if (pooled_) gpu_->RecycleTexture(std::move(texture_));
}

webrtc::scoped_refptr<webrtc::NV12Buffer> D3D11FrameBuffer::ReadbackNV12() {
  webrtc::MutexLock lock(&readback_lock_);
  if (readback_ == nullptr) {
    readback_ = gpu_->ReadbackNV12(texture_.Get(), subresource_, width_,
                                   height_);
  }
  return readback_;
}

webrtc::scoped_refptr<webrtc::I420BufferInterface>
D3D11FrameBuffer::ToI420() {
  auto nv12 = ReadbackNV12();
  if (nv12 == nullptr) {
    // Лучше чёрный кадр, чем падение в софтовом кодеке на nullptr.
    auto black = webrtc::I420Buffer::Create(width_, height_);
    webrtc::I420Buffer::SetBlack(black.get());
    return black;
  }
  return nv12->ToI420();
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer>
D3D11FrameBuffer::GetMappedFrameBuffer(
    webrtc::ArrayView<webrtc::VideoFrameBuffer::Type> types) {
  for (const auto type : types) {
    if (type == Type::kNV12) return ReadbackNV12();
    if (type == Type::kI420) return ToI420();
  }
  return nullptr;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> D3D11FrameBuffer::CropAndScale(
    int offset_x, int offset_y, int crop_width, int crop_height,
    int scaled_width, int scaled_height) {
  if (scaled_width <= 0 || scaled_height <= 0) return nullptr;
  if (offset_x == 0 && offset_y == 0 && crop_width == width_ &&
      crop_height == height_ && scaled_width == width_ &&
      scaled_height == height_) {
    return webrtc::scoped_refptr<VideoFrameBuffer>(this);
  }

  // Чётные стороны и смещения: иначе плоскость цветности поедет на полпикселя.
  const int out_width = (scaled_width + 1) & ~1;
  const int out_height = (scaled_height + 1) & ~1;
  auto target = gpu_->CreateNV12Frame(out_width, out_height);
  if (target != nullptr) {
    const RECT source{offset_x & ~1, offset_y & ~1,
                      (offset_x & ~1) + (crop_width & ~1),
                      (offset_y & ~1) + (crop_height & ~1)};
    if (gpu_->Blit(texture_.Get(), subresource_, &source, target->texture(),
                   0, nullptr, /*letterbox=*/false)) {
      return target;
    }
  }
  RTC_LOG(LS_WARNING) << "D3D11Frame: масштаб на видеокарте не удался, "
                         "считаем процессором";
  return VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                        crop_height, scaled_width,
                                        scaled_height);
}

HANDLE D3D11FrameBuffer::shared_handle() {
  if (shared_handle_ != nullptr) return shared_handle_;
  ComPtr<IDXGIResource> resource;
  if (FAILED(texture_.As(&resource))) return nullptr;
  HANDLE handle = nullptr;
  if (FAILED(resource->GetSharedHandle(&handle))) return nullptr;
  shared_handle_ = handle;
  return shared_handle_;
}

webrtc::scoped_refptr<D3D11FrameBuffer> ToD3D11Frame(
    const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer,
    GpuContext* gpu) {
  if (buffer == nullptr || gpu == nullptr) return nullptr;

  if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
    // Единственный нативный тип в этой сборке — наш. Чужое устройство
    // (другой адаптер) нативно не переложить: читаем и заливаем.
    auto* native = static_cast<D3D11FrameBuffer*>(buffer.get());
    if (native->gpu() == gpu) {
      return webrtc::scoped_refptr<D3D11FrameBuffer>(native);
    }
    auto host = native->ReadbackNV12();
    if (host == nullptr) return nullptr;
    return gpu->UploadNV12(host->DataY(), host->StrideY(), host->DataUV(),
                           host->StrideUV(), host->width(), host->height());
  }

  if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
    const auto* nv12 = static_cast<const webrtc::NV12BufferInterface*>(
        buffer.get());
    return gpu->UploadNV12(nv12->DataY(), nv12->StrideY(), nv12->DataUV(),
                           nv12->StrideUV(), nv12->width(), nv12->height());
  }

  auto i420 = buffer->ToI420();
  if (i420 == nullptr) return nullptr;
  auto host = webrtc::NV12Buffer::Copy(*i420);
  return gpu->UploadNV12(host->DataY(), host->StrideY(), host->DataUV(),
                         host->StrideUV(), host->width(), host->height());
}

}  // namespace squeak

#endif  // WEBRTC_WIN
