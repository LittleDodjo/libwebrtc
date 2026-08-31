/*
 *  Squeak: аппаратный H.264-энкодер NVIDIA (NVENC) для Windows.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/nvenc_video_encoder.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/video_codec.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/include/bitrate_adjuster.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert.h"

#if defined(WEBRTC_WIN)

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include "src/win/nvenc/nvEncodeAPI.h"

#endif  // WEBRTC_WIN

namespace squeak {
namespace {

#if defined(WEBRTC_WIN)

using Microsoft::WRL::ComPtr;

constexpr UINT kNvidiaVendorId = 0x10DE;

// Отношение к таргетному битрейту, при котором просим NVENC потолок. WebRTC
// уже прислал нам target — потолок нужен только чтобы всплеск на резкой
// смене кадров не выбивал буфер.
constexpr double kMaxBitrateFactor = 1.5;

struct NvencApi {
  HMODULE library = nullptr;
  NV_ENCODE_API_FUNCTION_LIST fn = {};
  bool loaded = false;
};

const NvencApi& GetNvencApi() {
  static const NvencApi* api = [] {
    auto* result = new NvencApi();
    result->library = ::LoadLibraryW(L"nvEncodeAPI64.dll");
    if (result->library == nullptr) {
      RTC_LOG(LS_INFO) << "NVENC: nvEncodeAPI64.dll не найдена";
      return result;
    }
    using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto create = reinterpret_cast<CreateInstanceFn>(
        ::GetProcAddress(result->library, "NvEncodeAPICreateInstance"));
    if (create == nullptr) {
      RTC_LOG(LS_WARNING) << "NVENC: нет NvEncodeAPICreateInstance";
      return result;
    }
    result->fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    const NVENCSTATUS status = create(&result->fn);
    if (status != NV_ENC_SUCCESS) {
      // NV_ENC_ERR_INVALID_VERSION (15) означает, что драйвер старее заголовка,
      // с которым мы собраны, — самая частая причина отказа на живых машинах.
      RTC_LOG(LS_WARNING) << "NVENC: NvEncodeAPICreateInstance status=" << status;
      return result;
    }
    result->loaded = true;
    return result;
  }();
  return *api;
}

// Явно выбираем NVIDIA-адаптер: на ноутбуках с Optimus дефолтный адаптер —
// встроенная графика, и сессия NVENC на ней не откроется.
ComPtr<ID3D11Device> CreateNvidiaD3D11Device() {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory))) {
    return nullptr;
  }

  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != kNvidiaVendorId) {
      continue;
    }

    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = ::D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, nullptr);
    if (SUCCEEDED(hr)) {
      return device;
    }
  }
  return nullptr;
}

class NvencSession {
 public:
  ~NvencSession() { Destroy(); }

  bool Open() {
    const NvencApi& api = GetNvencApi();
    if (!api.loaded) {
      return false;
    }
    device_ = CreateNvidiaD3D11Device();
    if (device_ == nullptr) {
      return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = device_.Get();
    params.apiVersion = NVENCAPI_VERSION;
    const NVENCSTATUS status =
        api.fn.nvEncOpenEncodeSessionEx(&params, &encoder_);
    if (status != NV_ENC_SUCCESS) {
      RTC_LOG(LS_WARNING) << "NVENC: nvEncOpenEncodeSessionEx status=" << status;
      encoder_ = nullptr;
      return false;
    }
    return true;
  }

  bool SupportsH264() {
    const NvencApi& api = GetNvencApi();
    if (encoder_ == nullptr) {
      return false;
    }
    uint32_t count = 0;
    if (api.fn.nvEncGetEncodeGUIDCount(encoder_, &count) != NV_ENC_SUCCESS ||
        count == 0) {
      return false;
    }
    std::vector<GUID> guids(count);
    uint32_t filled = 0;
    if (api.fn.nvEncGetEncodeGUIDs(encoder_, guids.data(), count, &filled) !=
        NV_ENC_SUCCESS) {
      return false;
    }
    for (uint32_t i = 0; i < filled; ++i) {
      if (std::memcmp(&guids[i], &NV_ENC_CODEC_H264_GUID, sizeof(GUID)) == 0) {
        return true;
      }
    }
    return false;
  }

  void Destroy() {
    if (encoder_ != nullptr) {
      GetNvencApi().fn.nvEncDestroyEncoder(encoder_);
      encoder_ = nullptr;
    }
    device_.Reset();
  }

  void* encoder() const { return encoder_; }

 private:
  ComPtr<ID3D11Device> device_;
  void* encoder_ = nullptr;
};

bool ProbeNvencH264() {
  NvencSession session;
  if (!session.Open()) {
    RTC_LOG(LS_INFO) << "NVENC: сессия не открылась — уходим на софт";
    return false;
  }
  const bool ok = session.SupportsH264();
  RTC_LOG(LS_INFO) << "NVENC: H.264 " << (ok ? "доступен" : "не поддержан");
  return ok;
}

class NvencH264Encoder : public webrtc::VideoEncoder {
 public:
  NvencH264Encoder() : bitrate_adjuster_(0.5, 0.95) {}

  ~NvencH264Encoder() override { Release(); }

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const webrtc::VideoEncoder::Settings& /* settings */) override {
    if (codec_settings == nullptr ||
        codec_settings->codecType != webrtc::kVideoCodecH264) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (codec_settings->width == 0 || codec_settings->height == 0) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }

    Release();

    width_ = codec_settings->width;
    height_ = codec_settings->height;
    framerate_ = codec_settings->maxFramerate > 0
                     ? static_cast<int>(codec_settings->maxFramerate)
                     : 30;
    target_bitrate_bps_ = codec_settings->startBitrate * 1000;
    if (target_bitrate_bps_ <= 0) {
      target_bitrate_bps_ = codec_settings->maxBitrate * 1000;
    }
    mode_ = codec_settings->mode;
    bitrate_adjuster_.SetTargetBitrateBps(target_bitrate_bps_);

    return OpenEncoder();
  }

  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    const NvencApi& api = GetNvencApi();
    if (session_.encoder() != nullptr) {
      if (input_buffer_ != nullptr) {
        api.fn.nvEncDestroyInputBuffer(session_.encoder(), input_buffer_);
        input_buffer_ = nullptr;
      }
      if (output_buffer_ != nullptr) {
        api.fn.nvEncDestroyBitstreamBuffer(session_.encoder(), output_buffer_);
        output_buffer_ = nullptr;
      }
    }
    session_.Destroy();
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override {
    if (session_.encoder() == nullptr) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    if (callback_ == nullptr) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }

    bool key_frame = false;
    if (frame_types != nullptr && !frame_types->empty()) {
      if ((*frame_types)[0] == webrtc::VideoFrameType::kEmptyFrame) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
      key_frame = (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey;
    }

    if (frame.width() != width_ || frame.height() != height_) {
      width_ = frame.width();
      height_ = frame.height();
      Release();
      const int result = OpenEncoder();
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
      key_frame = true;
    }

    if (reconfigure_needed_ && !Reconfigure()) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    if (!FillInputBuffer(frame)) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    const NvencApi& api = GetNvencApi();
    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = input_buffer_;
    pic.outputBitstream = output_buffer_;
    pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputWidth = width_;
    pic.inputHeight = height_;
    pic.inputTimeStamp = frame.rtp_timestamp();
    if (key_frame) {
      pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                           NV_ENC_PIC_FLAG_FORCEINTRA |
                           NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    if (api.fn.nvEncEncodePicture(session_.encoder(), &pic) != NV_ENC_SUCCESS) {
      RTC_LOG(LS_ERROR) << "NVENC: nvEncEncodePicture failed";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    return DeliverBitstream(frame);
  }

  void SetRates(const RateControlParameters& parameters) override {
    if (parameters.bitrate.get_sum_bps() == 0) {
      return;
    }
    const int target = static_cast<int>(parameters.bitrate.get_sum_bps());
    const int framerate = parameters.framerate_fps > 1.0
                              ? static_cast<int>(parameters.framerate_fps)
                              : framerate_;
    if (target == target_bitrate_bps_ && framerate == framerate_) {
      return;
    }
    target_bitrate_bps_ = target;
    framerate_ = framerate;
    bitrate_adjuster_.SetTargetBitrateBps(target);
    reconfigure_needed_ = true;
  }

  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override {
    webrtc::VideoEncoder::EncoderInfo info;
    info.implementation_name = "NVENC H264";
    info.is_hardware_accelerated = true;
    info.supports_native_handle = false;
    info.has_trusted_rate_controller = false;
    info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(kLowQp, kHighQp);
    return info;
  }

 private:
  static constexpr int kLowQp = 24;
  static constexpr int kHighQp = 37;

  int OpenEncoder() {
    if (!session_.Open()) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    const NvencApi& api = GetNvencApi();

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    if (api.fn.nvEncGetEncodePresetConfigEx(
            session_.encoder(), NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P3_GUID,
            NV_ENC_TUNING_INFO_LOW_LATENCY, &preset) != NV_ENC_SUCCESS) {
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    config_ = preset.presetCfg;
    ApplyRateControl(&config_);
    // Бесконечный GOP: ключевые кадры шлём только по запросу WebRTC, он сам
    // просит их при потерях и при подключении нового зрителя.
    config_.gopLength = NVENC_INFINITE_GOPLENGTH;
    config_.frameIntervalP = 1;
    config_.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    config_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    config_.encodeCodecConfig.h264Config.sliceMode = 0;
    config_.encodeCodecConfig.h264Config.sliceModeData = 0;

    init_params_ = {};
    init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params_.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init_params_.presetGUID = NV_ENC_PRESET_P3_GUID;
    init_params_.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init_params_.encodeWidth = width_;
    init_params_.encodeHeight = height_;
    init_params_.darWidth = width_;
    init_params_.darHeight = height_;
    init_params_.maxEncodeWidth = width_;
    init_params_.maxEncodeHeight = height_;
    init_params_.frameRateNum = framerate_;
    init_params_.frameRateDen = 1;
    init_params_.enablePTD = 1;
    init_params_.encodeConfig = &config_;

    if (api.fn.nvEncInitializeEncoder(session_.encoder(), &init_params_) !=
        NV_ENC_SUCCESS) {
      RTC_LOG(LS_ERROR) << "NVENC: nvEncInitializeEncoder failed";
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    NV_ENC_CREATE_INPUT_BUFFER input = {};
    input.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    input.width = width_;
    input.height = height_;
    input.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    if (api.fn.nvEncCreateInputBuffer(session_.encoder(), &input) !=
        NV_ENC_SUCCESS) {
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    input_buffer_ = input.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER output = {};
    output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (api.fn.nvEncCreateBitstreamBuffer(session_.encoder(), &output) !=
        NV_ENC_SUCCESS) {
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    output_buffer_ = output.bitstreamBuffer;

    reconfigure_needed_ = false;
    RTC_LOG(LS_INFO) << "NVENC: инициализирован " << width_ << "x" << height_
                     << "@" << framerate_ << " " << target_bitrate_bps_
                     << " bit/s";
    return WEBRTC_VIDEO_CODEC_OK;
  }

  void ApplyRateControl(NV_ENC_CONFIG* config) {
    const uint32_t average = bitrate_adjuster_.GetAdjustedBitrateBps() > 0
                                 ? bitrate_adjuster_.GetAdjustedBitrateBps()
                                 : target_bitrate_bps_;
    config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config->rcParams.averageBitRate = average;
    config->rcParams.maxBitRate =
        static_cast<uint32_t>(average * kMaxBitrateFactor);
    config->rcParams.enableAQ = 1;
    config->rcParams.disableBadapt = 1;
    // Буфер размером в один кадр — это и есть low latency: энкодеру нечем
    // «занять» будущие кадры, поэтому он не копит задержку.
    config->rcParams.vbvBufferSize = average / (framerate_ > 0 ? framerate_ : 30);
    config->rcParams.vbvInitialDelay = config->rcParams.vbvBufferSize;
  }

  bool Reconfigure() {
    const NvencApi& api = GetNvencApi();
    ApplyRateControl(&config_);
    init_params_.frameRateNum = framerate_;
    init_params_.encodeConfig = &config_;

    NV_ENC_RECONFIGURE_PARAMS params = {};
    params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    params.reInitEncodeParams = init_params_;
    if (api.fn.nvEncReconfigureEncoder(session_.encoder(), &params) !=
        NV_ENC_SUCCESS) {
      RTC_LOG(LS_WARNING) << "NVENC: nvEncReconfigureEncoder failed";
      return false;
    }
    reconfigure_needed_ = false;
    return true;
  }

  bool FillInputBuffer(const webrtc::VideoFrame& frame) {
    const NvencApi& api = GetNvencApi();
    NV_ENC_LOCK_INPUT_BUFFER lock = {};
    lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lock.inputBuffer = input_buffer_;
    if (api.fn.nvEncLockInputBuffer(session_.encoder(), &lock) !=
        NV_ENC_SUCCESS) {
      RTC_LOG(LS_ERROR) << "NVENC: nvEncLockInputBuffer failed";
      return false;
    }

    auto* dst_y = static_cast<uint8_t*>(lock.bufferDataPtr);
    uint8_t* dst_uv = dst_y + static_cast<size_t>(height_) * lock.pitch;

    webrtc::scoped_refptr<const webrtc::I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    const int result = libyuv::I420ToNV12(
        i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
        i420->DataV(), i420->StrideV(), dst_y, lock.pitch, dst_uv, lock.pitch,
        width_, height_);

    api.fn.nvEncUnlockInputBuffer(session_.encoder(), input_buffer_);
    return result == 0;
  }

  int32_t DeliverBitstream(const webrtc::VideoFrame& frame) {
    const NvencApi& api = GetNvencApi();
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = output_buffer_;
    lock.doNotWait = 0;
    if (api.fn.nvEncLockBitstream(session_.encoder(), &lock) !=
        NV_ENC_SUCCESS) {
      RTC_LOG(LS_ERROR) << "NVENC: nvEncLockBitstream failed";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    webrtc::EncodedImage image;
    image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
        static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
        lock.bitstreamSizeInBytes));
    const uint32_t size = lock.bitstreamSizeInBytes;
    const bool key_frame = lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                           lock.pictureType == NV_ENC_PIC_TYPE_I;
    api.fn.nvEncUnlockBitstream(session_.encoder(), output_buffer_);

    image._encodedWidth = width_;
    image._encodedHeight = height_;
    image.SetRtpTimestamp(frame.rtp_timestamp());
    image.ntp_time_ms_ = frame.ntp_time_ms();
    image.capture_time_ms_ = frame.render_time_ms();
    image.rotation_ = frame.rotation();
    image.SetColorSpace(frame.color_space());
    image.content_type_ = mode_ == webrtc::VideoCodecMode::kScreensharing
                              ? webrtc::VideoContentType::SCREENSHARE
                              : webrtc::VideoContentType::UNSPECIFIED;
    image.timing_.flags = webrtc::VideoSendTiming::kInvalid;
    image._frameType = key_frame ? webrtc::VideoFrameType::kVideoFrameKey
                                 : webrtc::VideoFrameType::kVideoFrameDelta;

    bitstream_parser_.ParseBitstream(image);
    image.qp_ = bitstream_parser_.GetLastSliceQp().value_or(-1);

    webrtc::CodecSpecificInfo codec_specific;
    codec_specific.codecType = webrtc::kVideoCodecH264;
    codec_specific.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;

    const webrtc::EncodedImageCallback::Result result =
        callback_->OnEncodedImage(image, &codec_specific);
    if (result.error != webrtc::EncodedImageCallback::Result::OK) {
      RTC_LOG(LS_WARNING) << "NVENC: OnEncodedImage error " << result.error;
    }
    bitrate_adjuster_.Update(size);
    return WEBRTC_VIDEO_CODEC_OK;
  }

  NvencSession session_;
  NV_ENC_INITIALIZE_PARAMS init_params_ = {};
  NV_ENC_CONFIG config_ = {};
  NV_ENC_INPUT_PTR input_buffer_ = nullptr;
  NV_ENC_OUTPUT_PTR output_buffer_ = nullptr;

  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::H264BitstreamParser bitstream_parser_;
  webrtc::BitrateAdjuster bitrate_adjuster_;

  int width_ = 0;
  int height_ = 0;
  int framerate_ = 30;
  int target_bitrate_bps_ = 0;
  bool reconfigure_needed_ = false;
  webrtc::VideoCodecMode mode_ = webrtc::VideoCodecMode::kRealtimeVideo;
};

#endif  // WEBRTC_WIN

}  // namespace

bool NvencH264Supported() {
#if defined(WEBRTC_WIN)
  static const bool supported = ProbeNvencH264();
  return supported;
#else
  return false;
#endif
}

std::unique_ptr<webrtc::VideoEncoder> CreateNvencH264Encoder(
    const webrtc::Environment& /* env */,
    const webrtc::SdpVideoFormat& /* format */) {
#if defined(WEBRTC_WIN)
  if (!NvencH264Supported()) {
    return nullptr;
  }
  return std::make_unique<NvencH264Encoder>();
#else
  return nullptr;
#endif
}

}  // namespace squeak
