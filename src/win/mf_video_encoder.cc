/*
 *  Squeak: аппаратный H.264-энкодер поверх Media Foundation (Windows).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/mf_video_encoder.h"

#include <cstring>
#include <deque>
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
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"

#if defined(WEBRTC_WIN)

// Порядок значим, по алфавиту сортировать нельзя: windows.h идёт первым, а сам
// интерфейс ICodecAPI объявлен в MIDL-заголовке icodecapi.h — в codecapi.h
// лежат только CODECAPI_*-гуиды, поэтому одного его мало.
#include <windows.h>

#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#endif  // WEBRTC_WIN

namespace squeak {
namespace {

#if defined(WEBRTC_WIN)

using Microsoft::WRL::ComPtr;

constexpr int kLowQp = 24;
constexpr int kHighQp = 37;

// Сколько ждём события от асинхронного MFT, прежде чем считать кадр потерянным.
// Энкодер отвечает за единицы миллисекунд; секунда — это уже «что-то встало».
constexpr DWORD kEventTimeoutMs = 1000;

bool EnsureMediaFoundation() {
  static const bool started = [] {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return false;
    return SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }();
  return started;
}

// Берём первый аппаратный MFT, который умеет NV12 -> H.264. Вендор не важен:
// перечисление отдаёт то, что поставил драйвер, поэтому одна реализация
// закрывает и AMD, и Intel, и NVIDIA.
ComPtr<IMFTransform> CreateHardwareH264Encoder(std::string* name) {
  if (!EnsureMediaFoundation()) return nullptr;

  MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video, MFVideoFormat_H264};

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const HRESULT hr = ::MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output,
      &activates, &count);
  if (FAILED(hr) || count == 0) {
    if (activates != nullptr) ::CoTaskMemFree(activates);
    return nullptr;
  }

  ComPtr<IMFTransform> transform;
  for (UINT32 i = 0; i < count; ++i) {
    if (transform == nullptr &&
        SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform)))) {
      if (name != nullptr) {
        LPWSTR friendly = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(
                MFT_FRIENDLY_NAME_Attribute, &friendly, &length))) {
          const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, friendly, -1,
                                                  nullptr, 0, nullptr, nullptr);
          if (bytes > 1) {
            name->resize(static_cast<size_t>(bytes) - 1);
            ::WideCharToMultiByte(CP_UTF8, 0, friendly, -1, name->data(), bytes,
                                  nullptr, nullptr);
          }
          ::CoTaskMemFree(friendly);
        }
      }
    }
    activates[i]->Release();
  }
  ::CoTaskMemFree(activates);
  return transform;
}

// Асинхронные MFT до разблокировки притворяются, что их нет. Разблокировка —
// штатная процедура, а не обход защиты: флаг введён ровно для приложений,
// которые сами крутят событийный цикл.
bool UnlockAsync(IMFTransform* transform, bool* is_async) {
  ComPtr<IMFAttributes> attributes;
  if (FAILED(transform->GetAttributes(&attributes))) return false;

  UINT32 async = 0;
  attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
  *is_async = async != 0;
  if (async == 0) return true;
  return SUCCEEDED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
}

class MediaFoundationH264Encoder : public webrtc::VideoEncoder {
 public:
  MediaFoundationH264Encoder() : bitrate_adjuster_(0.5, 0.95) {}

  ~MediaFoundationH264Encoder() override { Release(); }

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
    if (transform_ != nullptr) {
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
      transform_.Reset();
    }
    events_.Reset();
    codec_api_.Reset();
    pending_.clear();
    sequence_header_.clear();
    need_input_ = 0;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override {
    if (transform_ == nullptr) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    if (callback_ == nullptr) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

    bool key_frame = false;
    if (frame_types != nullptr && !frame_types->empty()) {
      if ((*frame_types)[0] == webrtc::VideoFrameType::kEmptyFrame) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
      key_frame = (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey;
    }

    // Смена разрешения на лету: переинициализируем и обязательно с ключевого.
    if (frame.width() != width_ || frame.height() != height_) {
      width_ = frame.width();
      height_ = frame.height();
      Release();
      const int result = OpenEncoder();
      if (result != WEBRTC_VIDEO_CODEC_OK) return result;
      key_frame = true;
    }

    if (reconfigure_needed_) ApplyBitrate();
    if (key_frame) ForceKeyFrame();

    ComPtr<IMFSample> sample;
    if (!WrapFrame(frame, &sample)) return WEBRTC_VIDEO_CODEC_ERROR;

    if (!WaitForInputSlot()) return WEBRTC_VIDEO_CODEC_ERROR;

    const HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MF-enc: ProcessInput hr=" << hr;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (need_input_ > 0) need_input_--;

    pending_.push_back({frame.rtp_timestamp(), frame.ntp_time_ms(),
                        frame.render_time_ms(), frame.rotation(), width_,
                        height_});

    PumpEvents(/*block=*/false);
    return WEBRTC_VIDEO_CODEC_OK;
  }

  void SetRates(const RateControlParameters& parameters) override {
    if (parameters.bitrate.get_sum_bps() == 0) return;
    const int target = static_cast<int>(parameters.bitrate.get_sum_bps());
    const int framerate = parameters.framerate_fps > 1.0
                              ? static_cast<int>(parameters.framerate_fps)
                              : framerate_;
    if (target == target_bitrate_bps_ && framerate == framerate_) return;
    target_bitrate_bps_ = target;
    framerate_ = framerate;
    bitrate_adjuster_.SetTargetBitrateBps(target);
    reconfigure_needed_ = true;
  }

  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override {
    webrtc::VideoEncoder::EncoderInfo info;
    info.implementation_name = implementation_name_;
    info.is_hardware_accelerated = true;
    info.supports_native_handle = false;
    info.has_trusted_rate_controller = false;
    info.scaling_settings =
        webrtc::VideoEncoder::ScalingSettings(kLowQp, kHighQp);
    return info;
  }

 private:
  struct FrameMeta {
    uint32_t rtp_timestamp;
    int64_t ntp_time_ms;
    int64_t render_time_ms;
    webrtc::VideoRotation rotation;
    int width;
    int height;
  };

  int OpenEncoder() {
    std::string name;
    transform_ = CreateHardwareH264Encoder(&name);
    if (transform_ == nullptr) return WEBRTC_VIDEO_CODEC_ERROR;
    implementation_name_ =
        name.empty() ? "MediaFoundation H264" : ("MF " + name);

    bool is_async = false;
    if (!UnlockAsync(transform_.Get(), &is_async)) {
      RTC_LOG(LS_WARNING) << "MF-enc: не удалось разблокировать async MFT";
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    is_async_ = is_async;
    if (is_async_ && FAILED(transform_.As(&events_))) {
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // Порядок важен: у энкодера сначала выходной тип, потом входной.
    if (!SetOutputType() || !SetInputType()) {
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    transform_.As(&codec_api_);
    ConfigureRateControl();

    transform_->GetOutputStreamInfo(0, &output_info_);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    RTC_LOG(LS_INFO) << "MF-enc: " << implementation_name_ << " " << width_
                     << "x" << height_ << "@" << framerate_ << " "
                     << target_bitrate_bps_ / 1000 << "kbps async=" << is_async_;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  bool SetOutputType() {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(&type))) return false;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    type->SetUINT32(MF_MT_AVG_BITRATE,
                    static_cast<UINT32>(target_bitrate_bps_));
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_ConstrainedBase);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, framerate_, 1);
    ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    const HRESULT hr = transform_->SetOutputType(0, type.Get(), 0);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MF-enc: SetOutputType hr=" << hr;
      return false;
    }

    // SPS/PPS многие аппаратные MFT отдают только здесь, а не в потоке.
    // Запоминаем, чтобы приклеить к каждому ключевому кадру.
    CacheSequenceHeader();
    return true;
  }

  void CacheSequenceHeader() {
    sequence_header_.clear();
    ComPtr<IMFMediaType> current;
    if (FAILED(transform_->GetOutputCurrentType(0, &current))) return;
    UINT32 size = 0;
    if (FAILED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) ||
        size == 0) {
      return;
    }
    sequence_header_.resize(size);
    if (FAILED(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                sequence_header_.data(), size, &size))) {
      sequence_header_.clear();
    }
  }

  bool SetInputType() {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(&type))) return false;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, framerate_, 1);
    ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    const HRESULT hr = transform_->SetInputType(0, type.Get(), 0);
    if (FAILED(hr)) RTC_LOG(LS_WARNING) << "MF-enc: SetInputType hr=" << hr;
    return SUCCEEDED(hr);
  }

  void SetCodecApiUint32(const GUID& api, UINT32 value) {
    if (codec_api_ == nullptr) return;
    VARIANT var = {};
    var.vt = VT_UI4;
    var.ulVal = value;
    codec_api_->SetValue(&api, &var);
  }

  void ConfigureRateControl() {
    if (codec_api_ == nullptr) return;
    // CBR + низкая задержка: у демонстрации важнее ровный поток, чем экономия
    // битрейта на статичных кадрах.
    SetCodecApiUint32(CODECAPI_AVEncCommonRateControlMode,
                      eAVEncCommonRateControlMode_CBR);
    SetCodecApiUint32(CODECAPI_AVEncCommonMeanBitRate,
                      static_cast<UINT32>(target_bitrate_bps_));
    // Бесконечный GOP: ключевые кадры шлём только по запросу WebRTC, он сам
    // знает, когда зрителю нужен опорный.
    SetCodecApiUint32(CODECAPI_AVEncMPVGOPSize, 0);
    SetCodecApiUint32(CODECAPI_AVEncVideoOutputFrameRate,
                      static_cast<UINT32>(framerate_));

    VARIANT low_latency = {};
    low_latency.vt = VT_BOOL;
    low_latency.boolVal = VARIANT_TRUE;
    codec_api_->SetValue(&CODECAPI_AVLowLatencyMode, &low_latency);
  }

  void ApplyBitrate() {
    reconfigure_needed_ = false;
    SetCodecApiUint32(CODECAPI_AVEncCommonMeanBitRate,
                      static_cast<UINT32>(bitrate_adjuster_.GetAdjustedBitrateBps()));
    SetCodecApiUint32(CODECAPI_AVEncVideoOutputFrameRate,
                      static_cast<UINT32>(framerate_));
  }

  void ForceKeyFrame() {
    if (codec_api_ == nullptr) return;
    VARIANT var = {};
    var.vt = VT_UI4;
    var.ulVal = 1;
    codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
  }

  // WebRTC отдаёт I420, аппаратный энкодер хочет NV12. Перекладка плоскостей
  // libyuv — единственное, что тут остаётся на CPU, и стоит она копейки на фоне
  // самого кодирования.
  bool WrapFrame(const webrtc::VideoFrame& frame, ComPtr<IMFSample>* out) {
    const webrtc::scoped_refptr<const webrtc::I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (i420 == nullptr) return false;

    const int y_stride = width_;
    const int uv_stride = width_ % 2 == 0 ? width_ : width_ + 1;
    const size_t y_size = static_cast<size_t>(y_stride) * height_;
    const size_t uv_size =
        static_cast<size_t>(uv_stride) * ((height_ + 1) / 2);
    const size_t total = y_size + uv_size;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(total), &buffer))) {
      return false;
    }

    BYTE* data = nullptr;
    if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return false;
    const int converted = libyuv::I420ToNV12(
        i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
        i420->DataV(), i420->StrideV(), data, y_stride, data + y_size,
        uv_stride, width_, height_);
    buffer->Unlock();
    if (converted != 0) return false;
    buffer->SetCurrentLength(static_cast<DWORD>(total));

    ComPtr<IMFSample> sample;
    if (FAILED(::MFCreateSample(&sample))) return false;
    sample->AddBuffer(buffer.Get());

    const LONGLONG duration = framerate_ > 0 ? 10000000LL / framerate_ : 333333;
    sample->SetSampleTime(frame_index_++ * duration);
    sample->SetSampleDuration(duration);

    *out = sample;
    return true;
  }

  // Асинхронный MFT сам просит кадры: пока не пришёл METransformNeedInput,
  // ProcessInput отдаст MF_E_NOTACCEPTING.
  bool WaitForInputSlot() {
    if (!is_async_) return true;
    if (need_input_ > 0) return true;
    const int64_t deadline = webrtc::TimeMillis() + kEventTimeoutMs;
    while (need_input_ == 0) {
      if (!PumpEvents(/*block=*/true)) return false;
      if (webrtc::TimeMillis() > deadline) {
        RTC_LOG(LS_WARNING) << "MF-enc: энкодер не просит кадры";
        return false;
      }
    }
    return true;
  }

  // Возвращает false только на настоящей ошибке: «событий больше нет» — это
  // нормальный выход из неблокирующего опроса.
  bool PumpEvents(bool block) {
    if (!is_async_) {
      if (!block) DrainOutput();
      return true;
    }
    while (true) {
      ComPtr<IMFMediaEvent> event;
      const HRESULT hr =
          events_->GetEvent(block ? 0 : MF_EVENT_FLAG_NO_WAIT, &event);
      if (hr == MF_E_NO_EVENTS_AVAILABLE) return true;
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING) << "MF-enc: GetEvent hr=" << hr;
        return false;
      }

      MediaEventType type = MEUnknown;
      if (FAILED(event->GetType(&type))) continue;
      if (type == METransformNeedInput) {
        need_input_++;
        if (block) return true;
      } else if (type == METransformHaveOutput) {
        DrainOutput();
      }
      if (!block) continue;
      return true;
    }
  }

  void DrainOutput() {
    while (true) {
      MFT_OUTPUT_DATA_BUFFER output = {};
      DWORD status = 0;

      // Аппаратные MFT выделяют выходной сэмпл сами; софтовые ждут наш.
      ComPtr<IMFSample> owned;
      if ((output_info_.dwFlags &
           MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        ComPtr<IMFMediaBuffer> buffer;
        const DWORD size = output_info_.cbSize > 0
                               ? output_info_.cbSize
                               : static_cast<DWORD>(width_ * height_ * 2);
        if (FAILED(::MFCreateMemoryBuffer(size, &buffer))) return;
        if (FAILED(::MFCreateSample(&owned))) return;
        owned->AddBuffer(buffer.Get());
        output.pSample = owned.Get();
      }

      const HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Энкодер пересогласовал выход — перечитываем SPS/PPS.
        CacheSequenceHeader();
        continue;
      }
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING) << "MF-enc: ProcessOutput hr=" << hr;
        return;
      }

      if (output.pSample != nullptr) {
        DeliverSample(output.pSample);
        if (output.pSample != owned.Get()) output.pSample->Release();
      }
      if (output.pEvents != nullptr) output.pEvents->Release();
      if (!is_async_) return;
    }
  }

  void DeliverSample(IMFSample* sample) {
    if (pending_.empty()) return;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;

    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return;

    UINT32 clean_point = 0;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);
    const bool key_frame = clean_point != 0;

    // Часть энкодеров держит SPS/PPS только в типе, а в поток кладёт голые
    // слайсы. Без заголовка поздно подключившийся зритель не декодирует ничего,
    // поэтому на ключевом кадре приклеиваем его сами — но только если энкодер
    // не положил его туда сам.
    std::vector<uint8_t> payload;
    const bool needs_header =
        key_frame && !sequence_header_.empty() && !StartsWithSps(data, length);
    if (needs_header) {
      payload.reserve(sequence_header_.size() + length);
      payload.insert(payload.end(), sequence_header_.begin(),
                     sequence_header_.end());
      payload.insert(payload.end(), data, data + length);
    }

    webrtc::EncodedImage image;
    image.SetEncodedData(
        needs_header
            ? webrtc::EncodedImageBuffer::Create(payload.data(), payload.size())
            : webrtc::EncodedImageBuffer::Create(data, length));
    const size_t size = needs_header ? payload.size() : length;
    buffer->Unlock();

    const FrameMeta meta = pending_.front();
    pending_.pop_front();

    image._encodedWidth = meta.width;
    image._encodedHeight = meta.height;
    image.SetRtpTimestamp(meta.rtp_timestamp);
    image.ntp_time_ms_ = meta.ntp_time_ms;
    image.capture_time_ms_ = meta.render_time_ms;
    image.rotation_ = meta.rotation;
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
      RTC_LOG(LS_WARNING) << "MF-enc: OnEncodedImage error " << result.error;
    }
    bitrate_adjuster_.Update(size);
  }

  // Ищем стартовый код и тип NAL 7 (SPS) в начале потока.
  static bool StartsWithSps(const uint8_t* data, size_t length) {
    if (length < 5) return false;
    size_t offset = 0;
    if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
      offset = 3;
    } else if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
      offset = 4;
    } else {
      return false;
    }
    return (data[offset] & 0x1F) == 7;
  }

  MFT_OUTPUT_STREAM_INFO output_info_ = {};

  ComPtr<IMFTransform> transform_;
  ComPtr<IMFMediaEventGenerator> events_;
  ComPtr<ICodecAPI> codec_api_;

  std::deque<FrameMeta> pending_;
  std::vector<uint8_t> sequence_header_;
  std::vector<uint8_t> nv12_;
  std::string implementation_name_ = "MediaFoundation H264";

  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::H264BitstreamParser bitstream_parser_;
  webrtc::BitrateAdjuster bitrate_adjuster_;

  int width_ = 0;
  int height_ = 0;
  int framerate_ = 30;
  int target_bitrate_bps_ = 0;
  int need_input_ = 0;
  int64_t frame_index_ = 0;
  bool is_async_ = false;
  bool reconfigure_needed_ = false;
  webrtc::VideoCodecMode mode_ = webrtc::VideoCodecMode::kRealtimeVideo;
};

#endif  // WEBRTC_WIN

}  // namespace

bool MediaFoundationH264EncoderSupported() {
#if defined(WEBRTC_WIN)
  static const bool supported = [] {
    std::string name;
    const ComPtr<IMFTransform> transform = CreateHardwareH264Encoder(&name);
    const bool ok = transform != nullptr;
    RTC_LOG(LS_INFO) << "MF-enc: аппаратный H.264 "
                     << (ok ? ("доступен: " + name) : std::string("недоступен"));
    return ok;
  }();
  return supported;
#else
  return false;
#endif
}

std::unique_ptr<webrtc::VideoEncoder> CreateMediaFoundationH264Encoder(
    const webrtc::Environment& /* env */,
    const webrtc::SdpVideoFormat& /* format */) {
#if defined(WEBRTC_WIN)
  if (!MediaFoundationH264EncoderSupported()) return nullptr;
  return std::make_unique<MediaFoundationH264Encoder>();
#else
  return nullptr;
#endif
}

}  // namespace squeak
