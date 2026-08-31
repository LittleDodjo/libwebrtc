/*
 *  Squeak: аппаратный H.264-декодер поверх Media Foundation (Windows).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/mf_video_decoder.h"

#include <memory>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/render_resolution.h"
#include "api/video/video_frame.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv/convert.h"

#if defined(WEBRTC_WIN)

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <windows.h>
#include <wrl/client.h>

#endif  // WEBRTC_WIN

namespace squeak {
namespace {

#if defined(WEBRTC_WIN)

using Microsoft::WRL::ComPtr;

// Сессия Media Foundation одна на процесс. COM инициализируем тут же: нас
// зовут из потоков WebRTC, где его никто не поднимал.
class MediaFoundationRuntime {
 public:
  static bool Ensure() {
    static const bool started = [] {
      const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        RTC_LOG(LS_WARNING) << "MF: CoInitializeEx hr=" << com;
        return false;
      }
      const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING) << "MF: MFStartup hr=" << hr;
        return false;
      }
      return true;
    }();
    return started;
  }
};

// Берём именно системный декодер Microsoft, а не то, что отдаёт MFTEnumEx с
// флагом HARDWARE: вендорские аппаратные MFT асинхронные, их нельзя гонять
// синхронной парой ProcessInput/ProcessOutput. Системный синхронный, а
// аппаратное ускорение получает через переданный ему D3D-менеджер (DXVA).
ComPtr<IMFTransform> CreateH264Transform() {
  if (!MediaFoundationRuntime::Ensure()) return nullptr;

  ComPtr<IMFTransform> transform;
  const HRESULT hr = ::CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&transform));
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MF: CoCreateInstance(H264 decoder) hr=" << hr;
    return nullptr;
  }
  return transform;
}

bool ProbeMediaFoundationH264() {
  const ComPtr<IMFTransform> transform = CreateH264Transform();
  const bool ok = transform != nullptr;
  RTC_LOG(LS_INFO) << "MF: H.264-декодер " << (ok ? "доступен" : "недоступен");
  return ok;
}

class MediaFoundationH264Decoder : public webrtc::VideoDecoder {
 public:
  ~MediaFoundationH264Decoder() override { Release(); }

  bool Configure(const Settings& settings) override {
    Release();

    transform_ = CreateH264Transform();
    if (transform_ == nullptr) return false;

    if (!CreateDevice() || !BindDeviceManager()) {
      Release();
      return false;
    }

    const webrtc::RenderResolution resolution = settings.max_render_resolution();
    width_ = resolution.Valid() ? resolution.Width() : 1920;
    height_ = resolution.Valid() ? resolution.Height() : 1080;

    if (!SetInputType() || !SetOutputType()) {
      Release();
      return false;
    }

    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    RTC_LOG(LS_INFO) << "MF: H.264-декодер поднят " << width_ << "x" << height_;
    return true;
  }

  int32_t Decode(const webrtc::EncodedImage& input,
                 int64_t /* render_time_ms */) override {
    if (transform_ == nullptr) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    if (callback_ == nullptr) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    if (input.size() == 0) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

    ComPtr<IMFSample> sample;
    if (!WrapEncodedImage(input, &sample)) return WEBRTC_VIDEO_CODEC_ERROR;

    const HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MF: ProcessInput hr=" << hr;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    DrainOutput(input);
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    if (transform_ != nullptr) {
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
      transform_.Reset();
    }
    device_manager_.Reset();
    device_.Reset();
    return WEBRTC_VIDEO_CODEC_OK;
  }

  DecoderInfo GetDecoderInfo() const override {
    DecoderInfo info;
    info.implementation_name = "MediaFoundation H264";
    info.is_hardware_accelerated = true;
    return info;
  }

 private:
  bool CreateDevice() {
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                       D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level = {};
    const HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, &device_, &level, nullptr);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MF: D3D11CreateDevice hr=" << hr;
      return false;
    }

    // MFT дёргает устройство из своих потоков — без этого флага возможны гонки.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread))) {
      multithread->SetMultithreadProtected(TRUE);
    }
    return true;
  }

  bool BindDeviceManager() {
    UINT token = 0;
    if (FAILED(::MFCreateDXGIDeviceManager(&token, &device_manager_))) {
      return false;
    }
    if (FAILED(device_manager_->ResetDevice(device_.Get(), token))) {
      return false;
    }
    const HRESULT hr = transform_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MF: SET_D3D_MANAGER hr=" << hr;
      return false;
    }
    return true;
  }

  bool SetInputType() {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(&type))) return false;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    const HRESULT hr = transform_->SetInputType(0, type.Get(), 0);
    if (FAILED(hr)) RTC_LOG(LS_WARNING) << "MF: SetInputType hr=" << hr;
    return SUCCEEDED(hr);
  }

  // Перебираем предложенные MFT форматы и берём NV12 — единственный, который
  // отдают все вендоры и который дёшево превращается в I420.
  bool SetOutputType() {
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      const HRESULT available = transform_->GetOutputAvailableType(0, i, &type);
      if (available == MF_E_NO_MORE_TYPES) break;
      if (FAILED(available)) break;

      GUID subtype = {};
      if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
      if (subtype != MFVideoFormat_NV12) continue;

      if (SUCCEEDED(transform_->SetOutputType(0, type.Get(), 0))) return true;
    }
    RTC_LOG(LS_WARNING) << "MF: NV12 на выходе не предложен";
    return false;
  }

  bool WrapEncodedImage(const webrtc::EncodedImage& input,
                        ComPtr<IMFSample>* out) {
    ComPtr<IMFSample> sample;
    if (FAILED(::MFCreateSample(&sample))) return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(input.size()),
                                      &buffer))) {
      return false;
    }

    BYTE* data = nullptr;
    DWORD max_length = 0;
    if (FAILED(buffer->Lock(&data, &max_length, nullptr))) return false;
    memcpy(data, input.data(), input.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(input.size()));

    sample->AddBuffer(buffer.Get());
    // MF считает время в 100-наносекундных единицах.
    sample->SetSampleTime(static_cast<LONGLONG>(input.RtpTimestamp()) * 10);
    if (input.FrameType() == webrtc::VideoFrameType::kVideoFrameKey) {
      sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }
    *out = sample;
    return true;
  }

  void DrainOutput(const webrtc::EncodedImage& input) {
    while (true) {
      MFT_OUTPUT_DATA_BUFFER output = {};
      DWORD status = 0;
      const HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);

      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (!SetOutputType()) return;
        continue;
      }
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING) << "MF: ProcessOutput hr=" << hr;
        return;
      }

      if (output.pSample != nullptr) {
        EmitFrame(output.pSample, input);
        output.pSample->Release();
      }
      if (output.pEvents != nullptr) output.pEvents->Release();
    }
  }

  void EmitFrame(IMFSample* sample, const webrtc::EncodedImage& input) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;

    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return;

    // MFT отдаёт NV12 одним куском: плоскость Y, следом чередующаяся UV.
    const int stride = width_;
    const uint8_t* y = data;
    const uint8_t* uv = data + static_cast<size_t>(stride) * height_;

    webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
        webrtc::I420Buffer::Create(width_, height_);
    const int converted = libyuv::NV12ToI420(
        y, stride, uv, stride, i420->MutableDataY(), i420->StrideY(),
        i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(),
        i420->StrideV(), width_, height_);
    buffer->Unlock();

    if (converted != 0) return;

    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                   .set_video_frame_buffer(i420)
                                   .set_rtp_timestamp(input.RtpTimestamp())
                                   .set_timestamp_us(webrtc::TimeMicros())
                                   .build();
    callback_->Decoded(frame);
  }

  ComPtr<IMFTransform> transform_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  ComPtr<ID3D11Device> device_;

  webrtc::DecodedImageCallback* callback_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

#endif  // WEBRTC_WIN

}  // namespace

bool MediaFoundationH264Supported() {
#if defined(WEBRTC_WIN)
  static const bool supported = ProbeMediaFoundationH264();
  return supported;
#else
  return false;
#endif
}

std::unique_ptr<webrtc::VideoDecoder> CreateMediaFoundationH264Decoder() {
#if defined(WEBRTC_WIN)
  if (!MediaFoundationH264Supported()) return nullptr;
  return std::make_unique<MediaFoundationH264Decoder>();
#else
  return nullptr;
#endif
}

}  // namespace squeak
