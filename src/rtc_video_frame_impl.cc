#include "rtc_video_frame_impl.h"

#include "api/video/i420_buffer.h"
#include "libyuv/convert_argb.h"
#include "libyuv/convert_from.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

#if defined(SQUEAK_USE_NVENC)
#include <d3d11.h>
#include <wrl/client.h>

#include "src/win/d3d11_frame_buffer.h"
#include "src/win/gpu_context.h"
#endif

namespace libwebrtc {

VideoFrameBufferImpl::VideoFrameBufferImpl(
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> frame_buffer)
    : buffer_(frame_buffer) {}

VideoFrameBufferImpl::VideoFrameBufferImpl(
    webrtc::scoped_refptr<webrtc::I420Buffer> frame_buffer)
    : buffer_(frame_buffer) {}

VideoFrameBufferImpl::~VideoFrameBufferImpl() {}

scoped_refptr<RTCVideoFrame> VideoFrameBufferImpl::Copy() {
  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(buffer_));
  return frame;
}

int VideoFrameBufferImpl::width() const { return buffer_->width(); }

int VideoFrameBufferImpl::height() const { return buffer_->height(); }

// Плоскости I420 по запросу. GetI420 отвечает только для буфера I420; кадры NV12
// и кадры в видеопамяти конвертируем один раз и держим рядом.
const webrtc::I420BufferInterface* VideoFrameBufferImpl::I420() const {
  if (buffer_->type() == webrtc::VideoFrameBuffer::Type::kI420) {
    return buffer_->GetI420();
  }
  if (!i420_) i420_ = buffer_->ToI420();
  return i420_.get();
}

const uint8_t* VideoFrameBufferImpl::DataY() const { return I420()->DataY(); }

const uint8_t* VideoFrameBufferImpl::DataU() const { return I420()->DataU(); }

const uint8_t* VideoFrameBufferImpl::DataV() const { return I420()->DataV(); }

int VideoFrameBufferImpl::StrideY() const { return I420()->StrideY(); }

int VideoFrameBufferImpl::StrideU() const { return I420()->StrideU(); }

int VideoFrameBufferImpl::StrideV() const { return I420()->StrideV(); }

int VideoFrameBufferImpl::ConvertToARGB(Type type, uint8_t* dst_buffer,
                                        int dst_stride, int dest_width,
                                        int dest_height) {
  // Путь через процессор — для софтового декодера и для рендеров без D3D11.
  // Лишних проходов нет: без поворота кадр не копируется, без изменения
  // размера не масштабируется. Раньше Rotate копировал кадр даже при нуле
  // градусов, а ScaleFrom гонял его ещё раз при равных размерах.
  webrtc::scoped_refptr<webrtc::I420BufferInterface> src = buffer_->ToI420();
  if (!src) return 0;
  if (rotation_ != webrtc::kVideoRotation_0) {
    src = webrtc::I420Buffer::Rotate(*src, rotation_);
  }
  if (src->width() != dest_width || src->height() != dest_height) {
    webrtc::scoped_refptr<webrtc::I420Buffer> scaled =
        webrtc::I420Buffer::Create(dest_width, dest_height);
    scaled->ScaleFrom(*src);
    src = scaled;
  }

  const int stride = dst_stride > 0 ? dst_stride : dest_width * 4;
  const int buf_size = stride * dest_height;
  switch (type) {
    case libwebrtc::RTCVideoFrame::Type::kARGB:
      libyuv::I420ToARGB(src->DataY(), src->StrideY(), src->DataU(),
                         src->StrideU(), src->DataV(), src->StrideV(),
                         dst_buffer, stride, dest_width, dest_height);
      break;
    case libwebrtc::RTCVideoFrame::Type::kBGRA:
      libyuv::I420ToBGRA(src->DataY(), src->StrideY(), src->DataU(),
                         src->StrideU(), src->DataV(), src->StrideV(),
                         dst_buffer, stride, dest_width, dest_height);
      break;
    case libwebrtc::RTCVideoFrame::Type::kABGR:
      libyuv::I420ToABGR(src->DataY(), src->StrideY(), src->DataU(),
                         src->StrideU(), src->DataV(), src->StrideV(),
                         dst_buffer, stride, dest_width, dest_height);
      break;
    case libwebrtc::RTCVideoFrame::Type::kRGBA:
      libyuv::I420ToRGBA(src->DataY(), src->StrideY(), src->DataU(),
                         src->StrideU(), src->DataV(), src->StrideV(),
                         dst_buffer, stride, dest_width, dest_height);
      break;
    default:
      break;
  }
  return buf_size;
}

void* VideoFrameBufferImpl::d3d11_texture() const {
#if defined(SQUEAK_USE_NVENC)
  if (buffer_ && buffer_->type() == webrtc::VideoFrameBuffer::Type::kNative) {
    return static_cast<squeak::D3D11FrameBuffer*>(buffer_.get())->texture();
  }
#endif
  return nullptr;
}

bool VideoFrameBufferImpl::RenderToD3D11Texture(void* texture, int x, int y,
                                                int width, int height) {
#if defined(SQUEAK_USE_NVENC)
  auto* dst = static_cast<ID3D11Texture2D*>(texture);
  if (dst == nullptr || !buffer_) return false;

  Microsoft::WRL::ComPtr<ID3D11Device> device;
  dst->GetDevice(&device);
  webrtc::scoped_refptr<squeak::GpuContext> gpu =
      squeak::GpuContext::ForDevice(device.Get());
  if (gpu == nullptr) return false;

  webrtc::scoped_refptr<squeak::D3D11FrameBuffer> frame =
      squeak::ToD3D11Frame(buffer_, gpu.get());
  if (frame == nullptr) return false;

  if (width <= 0 || height <= 0) {
    return gpu->Blit(frame->texture(), frame->subresource(), nullptr, dst, 0,
                     nullptr, /*letterbox=*/true);
  }
  const RECT rect{x, y, x + width, y + height};
  return gpu->Blit(frame->texture(), frame->subresource(), nullptr, dst, 0,
                   &rect, /*letterbox=*/true);
#else
  (void)texture;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  return false;
#endif
}

libwebrtc::RTCVideoFrame::VideoRotation VideoFrameBufferImpl::rotation() {
  switch (rotation_) {
    case webrtc::kVideoRotation_0:
      return RTCVideoFrame::kVideoRotation_0;
    case webrtc::kVideoRotation_90:
      return RTCVideoFrame::kVideoRotation_90;
    case webrtc::kVideoRotation_180:
      return RTCVideoFrame::kVideoRotation_180;
    case webrtc::kVideoRotation_270:
      return RTCVideoFrame::kVideoRotation_270;
    default:
      break;
  }
  return RTCVideoFrame::kVideoRotation_0;
}

scoped_refptr<RTCVideoFrame> RTCVideoFrame::Create(int width, int height,
                                                   const uint8_t* buffer,
                                                   int length) {
  int stride_y = width;
  int stride_uv = (width + 1) / 2;

  int size_y = stride_y * height;
  int size_u = stride_uv * height / 2;
  // int size_v = size_u;

  RTC_DCHECK(length == (width * height * 3) / 2);

  const uint8_t* data_y = buffer;
  const uint8_t* data_u = buffer + size_y;
  const uint8_t* data_v = buffer + size_y + size_u;

  webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Copy(
      width, height, data_y, stride_y, data_u, stride_uv, data_v, stride_uv);

  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(i420_buffer));
  return frame;
}

scoped_refptr<RTCVideoFrame> RTCVideoFrame::Create(
    int width, int height, const uint8_t* data_y, int stride_y,
    const uint8_t* data_u, int stride_u, const uint8_t* data_v, int stride_v) {
  webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Copy(
      width, height, data_y, stride_y, data_u, stride_u, data_v, stride_v);

  scoped_refptr<VideoFrameBufferImpl> frame =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(i420_buffer));
  return frame;
}

}  // namespace libwebrtc
