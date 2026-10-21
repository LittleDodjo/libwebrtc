/*
 *  Squeak: аппаратный H.264-энкодер AMD (AMF, Advanced Media Framework).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/amf_video_encoder.h"

#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/encoded_image.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/video_codec.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/include/bitrate_adjuster.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

#if defined(WEBRTC_WIN)

#include <d3d11.h>
#include <windows.h>

// Вендоренные заголовки AMD собираются как наши, с полным набором
// предупреждений-как-ошибок Chromium; чужой код под ними не писался.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "src/win/amf/components/VideoConverter.h"
#include "src/win/amf/components/VideoEncoderVCE.h"
#include "src/win/amf/core/Factory.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "src/win/d3d11_frame_buffer.h"
#include "src/win/gpu_context.h"

#endif  // WEBRTC_WIN

namespace squeak {
namespace {

#if defined(WEBRTC_WIN)

constexpr UINT kAmdVendorId = 0x1002;
constexpr int kLowQp = 24;
constexpr int kHighQp = 37;

// Сколько ждём кодирования одного кадра. Блок VCN отдаёт 4K за единицы
// миллисекунд; выше — что-то встало, кадр отдадим на следующем вызове.
constexpr int64_t kOutputWaitMs = 40;

// Кадров в полёте внутри AMF. Больше — энкодер не отдаёт результат, дальше
// копить нельзя: кадры в видеопамяти, а их пул общий с захватом.
constexpr size_t kMaxPending = 6;

struct AmfRuntime {
  HMODULE library = nullptr;
  amf::AMFFactory* factory = nullptr;
  amf_uint64 version = 0;
  bool loaded = false;
};

const AmfRuntime& GetAmf() {
  static const AmfRuntime* runtime = [] {
    auto* result = new AmfRuntime();
    result->library = ::LoadLibraryA(AMF_DLL_NAMEA);
    if (result->library == nullptr) {
      RTC_LOG(LS_INFO) << "AMF: " << AMF_DLL_NAMEA << " не найдена";
      return result;
    }
    auto query = reinterpret_cast<AMFQueryVersion_Fn>(
        ::GetProcAddress(result->library, AMF_QUERY_VERSION_FUNCTION_NAME));
    auto init = reinterpret_cast<AMFInit_Fn>(
        ::GetProcAddress(result->library, AMF_INIT_FUNCTION_NAME));
    if (query == nullptr || init == nullptr) {
      RTC_LOG(LS_WARNING) << "AMF: в рантайме нет AMFInit/AMFQueryVersion";
      return result;
    }
    if (query(&result->version) != AMF_OK) return result;
    // Просим не версию заголовка, а минимально нужную: всё, что мы трогаем,
    // есть с 1.4.30, а драйверы с рантаймом старше заголовка встречаются.
    if (init(AMF_MAKE_FULL_VERSION(1, 4, 30, 0), &result->factory) != AMF_OK ||
        result->factory == nullptr) {
      RTC_LOG(LS_WARNING) << "AMF: AMFInit не удался, рантайм "
                          << (result->version >> 48) << "."
                          << ((result->version >> 32) & 0xFFFF) << "."
                          << ((result->version >> 16) & 0xFFFF);
      return result;
    }
    result->loaded = true;
    RTC_LOG(LS_INFO) << "AMF: рантайм " << (result->version >> 48) << "."
                     << ((result->version >> 32) & 0xFFFF) << "."
                     << ((result->version >> 16) & 0xFFFF);
    return result;
  }();
  return *runtime;
}

// Устройство для AMF: общее устройство процесса, если оно на AMD — тогда
// кадр захвата уходит в энкодер без копий. Иначе первый адаптер AMD.
webrtc::scoped_refptr<GpuContext> AmfGpu() {
  webrtc::scoped_refptr<GpuContext> shared = GpuContext::Shared();
  if (shared != nullptr && shared->IsAmd()) return shared;
  return GpuContext::ForVendor(kAmdVendorId);
}

bool StartsWithSps(const uint8_t* data, size_t length) {
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

class AmfSession {
 public:
  ~AmfSession() { Destroy(); }

  bool Open(webrtc::scoped_refptr<GpuContext> gpu) {
    const AmfRuntime& amf = GetAmf();
    if (!amf.loaded || gpu == nullptr) return false;
    gpu_ = std::move(gpu);
    if (amf.factory->CreateContext(&context_) != AMF_OK) return false;
    if (context_->InitDX11(gpu_->device(), amf::AMF_DX11_0) != AMF_OK) {
      RTC_LOG(LS_WARNING) << "AMF: InitDX11 не удался";
      Destroy();
      return false;
    }
    if (amf.factory->CreateComponent(context_, AMFVideoEncoderVCE_AVC,
                                     &encoder_) != AMF_OK) {
      RTC_LOG(LS_WARNING) << "AMF: нет компонента AVC-энкодера";
      Destroy();
      return false;
    }
    return true;
  }

  // Пропускная способность блока в макроблоках в секунду. 0 — неизвестно.
  int64_t MaxThroughput() {
    if (encoder_ == nullptr) return 0;
    amf::AMFCapsPtr caps;
    if (encoder_->GetCaps(&caps) != AMF_OK || caps == nullptr) return 0;
    amf_int64 value = 0;
    caps->GetProperty(AMF_VIDEO_ENCODER_CAP_MAX_THROUGHPUT, &value);
    return value;
  }

  bool SupportsQueryTimeout() {
    if (encoder_ == nullptr) return false;
    amf::AMFCapsPtr caps;
    if (encoder_->GetCaps(&caps) != AMF_OK || caps == nullptr) return false;
    amf_bool value = false;
    caps->GetProperty(AMF_VIDEO_ENCODER_CAP_QUERY_TIMEOUT_SUPPORT, &value);
    return value;
  }

  void Destroy() {
    if (encoder_ != nullptr) {
      encoder_->Terminate();
      encoder_ = nullptr;
    }
    if (context_ != nullptr) {
      context_->Terminate();
      context_ = nullptr;
    }
    gpu_ = nullptr;
  }

  amf::AMFContext* context() const { return context_; }
  amf::AMFComponent* encoder() const { return encoder_; }
  GpuContext* gpu() const { return gpu_.get(); }

 private:
  webrtc::scoped_refptr<GpuContext> gpu_;
  amf::AMFContextPtr context_;
  amf::AMFComponentPtr encoder_;
};

bool ProbeAmfH264() {
  AmfSession session;
  if (!session.Open(AmfGpu())) {
    RTC_LOG(LS_INFO) << "AMF: недоступен — идём дальше по цепочке";
    return false;
  }
  RTC_LOG(LS_INFO) << "AMF: H.264 доступен, пропускная способность "
                   << session.MaxThroughput() << " МБ/с, zero-copy="
                   << (session.gpu() == GpuContext::Shared().get() ? "да" : "нет");
  return true;
}

class AmfH264Encoder : public webrtc::VideoEncoder {
 public:
  AmfH264Encoder() : bitrate_adjuster_(0.5, 0.95) {}

  ~AmfH264Encoder() override { Release(); }

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

    width_ = codec_settings->width & ~1;
    height_ = codec_settings->height & ~1;
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
    pending_.clear();
    sequence_header_.clear();
    session_.Destroy();
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override {
    if (session_.encoder() == nullptr) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    if (callback_ == nullptr) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

    bool key_frame = false;
    if (frame_types != nullptr && !frame_types->empty()) {
      if ((*frame_types)[0] == webrtc::VideoFrameType::kEmptyFrame) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
      key_frame = (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey;
    }

    if (reconfigure_needed_) ApplyRates();

    // Сначала забираем всё, что энкодер уже отдал: освобождаем очередь и
    // текстуры.
    DrainOutput(/*wait_ms=*/0);
    if (pending_.size() >= kMaxPending) {
      RTC_LOG(LS_WARNING) << "AMF: энкодер не отдаёт кадры, пропускаем";
      return WEBRTC_VIDEO_CODEC_OK;
    }

    // Кадр — текстура на устройстве сессии; размер приводим к настроенному,
    // чтобы scaleResolutionDownBy работал масштабом, а не пересозданием.
    webrtc::scoped_refptr<D3D11FrameBuffer> input =
        ToD3D11Frame(frame.video_frame_buffer(), session_.gpu());
    if (input == nullptr) return WEBRTC_VIDEO_CODEC_ERROR;
    if (input->width() != width_ || input->height() != height_) {
      input = ToD3D11Frame(input->Scale(width_, height_), session_.gpu());
      if (input == nullptr) return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // В энкодер приходят только текстуры пула — не массивы, нулевой подресурс.
    RTC_DCHECK_EQ(input->subresource(), 0u);
    amf::AMFSurfacePtr surface;
    if (session_.context()->CreateSurfaceFromDX11Native(
            input->texture(), &surface, nullptr) != AMF_OK ||
        surface == nullptr) {
      RTC_LOG(LS_ERROR) << "AMF: CreateSurfaceFromDX11Native не удался";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    const amf_pts pts = static_cast<amf_pts>(frame_index_++) * 10000000LL /
                        (framerate_ > 0 ? framerate_ : 30);
    surface->SetPts(pts);
    if (key_frame) {
      surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                           static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
      surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
      surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
    }

    AMF_RESULT result = session_.encoder()->SubmitInput(surface);
    if (result == AMF_INPUT_FULL) {
      DrainOutput(kOutputWaitMs);
      result = session_.encoder()->SubmitInput(surface);
    }
    if (result != AMF_OK) {
      RTC_LOG(LS_ERROR) << "AMF: SubmitInput result=" << result;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    Pending pending;
    pending.frame = input;
    pending.rtp_timestamp = frame.rtp_timestamp();
    pending.ntp_time_ms = frame.ntp_time_ms();
    pending.render_time_ms = frame.render_time_ms();
    pending.rotation = frame.rotation();
    pending_.push_back(std::move(pending));

    // В режиме низкой задержки результат есть уже на этом кадре; ждём его
    // здесь, чтобы задержка была ровно один кадр.
    DrainOutput(kOutputWaitMs);
    return WEBRTC_VIDEO_CODEC_OK;
  }

  void SetRates(const RateControlParameters& parameters) override {
    if (parameters.bitrate.get_sum_bps() == 0) return;
    const int target = static_cast<int>(parameters.bitrate.get_sum_bps());
    const int framerate = parameters.framerate_fps > 1.0
                              ? static_cast<int>(parameters.framerate_fps + 0.5)
                              : framerate_;
    if (target == target_bitrate_bps_ && framerate == framerate_) return;
    target_bitrate_bps_ = target;
    framerate_ = framerate;
    bitrate_adjuster_.SetTargetBitrateBps(target);
    reconfigure_needed_ = true;
  }

  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override {
    webrtc::VideoEncoder::EncoderInfo info;
    info.implementation_name = "AMF H264";
    info.is_hardware_accelerated = true;
    info.supports_native_handle = true;
    info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kNV12};
    info.has_trusted_rate_controller = false;
    info.requested_resolution_alignment = 2;
    info.apply_alignment_to_all_simulcast_layers = true;
    info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(kLowQp, kHighQp);
    return info;
  }

 private:
  struct Pending {
    webrtc::scoped_refptr<D3D11FrameBuffer> frame;
    uint32_t rtp_timestamp = 0;
    int64_t ntp_time_ms = 0;
    int64_t render_time_ms = 0;
    webrtc::VideoRotation rotation = webrtc::kVideoRotation_0;
  };

  template <typename T>
  void Set(const wchar_t* name, const T& value) {
    const AMF_RESULT result = session_.encoder()->SetProperty(name, value);
    if (result != AMF_OK) {
      // Не каждый блок VCN знает каждое свойство: это штатно, не ошибка.
      RTC_LOG(LS_INFO) << "AMF: свойство не принято, result=" << result;
    }
  }

  int OpenEncoder() {
    if (!session_.Open(AmfGpu())) return WEBRTC_VIDEO_CODEC_ERROR;
    amf::AMFComponent* encoder = session_.encoder();

    // Профиль применения задаёт разумные умолчания под стриминг (без B-кадров,
    // короткий VBV); всё, что важно для нас, перекрываем ниже явно.
    Set(AMF_VIDEO_ENCODER_USAGE,
        static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY));
    Set(AMF_VIDEO_ENCODER_PROFILE,
        static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_HIGH));
    Set(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(width_, height_));
    Set(AMF_VIDEO_ENCODER_FRAMERATE,
        AMFConstructRate(static_cast<amf_uint32>(framerate_), 1));

    // Качество: QUALITY, пока хватает пропускной способности блока (с запасом
    // в полтора раза), иначе BALANCED. 4K60 — это ~1.95 млн макроблоков/с.
    const int64_t needed = static_cast<int64_t>((width_ + 15) / 16) *
                           ((height_ + 15) / 16) * framerate_;
    const int64_t throughput = session_.MaxThroughput();
    const bool quality = throughput == 0 || throughput >= needed * 3 / 2;
    Set(AMF_VIDEO_ENCODER_QUALITY_PRESET,
        static_cast<amf_int64>(quality ? AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY
                                       : AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED));

    Set(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
    Set(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
    Set(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(0));
    Set(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, static_cast<amf_int64>(0));
    Set(AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER, true);
    Set(AMF_VIDEO_ENCODER_CABAC_ENABLE,
        static_cast<amf_int64>(AMF_VIDEO_ENCODER_CABAC));
    // Пропуск кадров рейт-контролем для WebRTC — дыра в потоке, которую
    // зритель видит как рывок. Лучше кадр покрупнее.
    Set(AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE, false);
    Set(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
    Set(AMF_VIDEO_ENCODER_ENFORCE_HRD, true);
    // Адаптивное квантование по вариативности и подпор на движении — то, что
    // делает текст чистым, а быстрые сцены без каши.
    Set(AMF_VIDEO_ENCODER_ENABLE_VBAQ, true);
    Set(AMF_VIDEO_ENCODER_PREENCODE_ENABLE,
        static_cast<amf_int64>(quality ? AMF_VIDEO_ENCODER_PREENCODE_ENABLED
                                       : AMF_VIDEO_ENCODER_PREENCODE_DISABLED));
    Set(AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE, true);
    Set(AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, false);
    Set(AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE,
        static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
    Set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE,
        static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
    Set(AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
        static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709));
    Set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES,
        static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709));
    ApplyRateControl();

    query_timeout_ = session_.SupportsQueryTimeout();
    if (query_timeout_) {
      Set(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, static_cast<amf_int64>(kOutputWaitMs));
    }

    const AMF_RESULT result =
        encoder->Init(amf::AMF_SURFACE_NV12, width_, height_);
    if (result != AMF_OK) {
      RTC_LOG(LS_ERROR) << "AMF: Init result=" << result;
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    CacheSequenceHeader();

    reconfigure_needed_ = false;
    RTC_LOG(LS_INFO) << "AMF: инициализирован " << width_ << "x" << height_
                     << "@" << framerate_ << " " << target_bitrate_bps_
                     << " bit/s, " << (quality ? "QUALITY" : "BALANCED")
                     << ", High/CABAC, VBAQ, throughput=" << throughput;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  void ApplyRateControl() {
    const int64_t average = bitrate_adjuster_.GetAdjustedBitrateBps() > 0
                                ? bitrate_adjuster_.GetAdjustedBitrateBps()
                                : target_bitrate_bps_;
    const int fps = framerate_ > 0 ? framerate_ : 30;
    Set(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
        static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
    Set(AMF_VIDEO_ENCODER_TARGET_BITRATE, average);
    Set(AMF_VIDEO_ENCODER_PEAK_BITRATE, average);
    // VBV в два кадра: ровный поток без крупных всплесков, но ключевому кадру
    // и смене сцены есть куда расти.
    Set(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, average * 2 / fps);
    Set(AMF_VIDEO_ENCODER_INITIAL_VBV_BUFFER_FULLNESS, static_cast<amf_int64>(64));
  }

  void ApplyRates() {
    reconfigure_needed_ = false;
    Set(AMF_VIDEO_ENCODER_FRAMERATE,
        AMFConstructRate(static_cast<amf_uint32>(framerate_), 1));
    ApplyRateControl();
  }

  void CacheSequenceHeader() {
    sequence_header_.clear();
    amf::AMFVariant var;
    if (session_.encoder()->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &var) !=
        AMF_OK) {
      return;
    }
    if (var.type != amf::AMF_VARIANT_INTERFACE) return;
    amf::AMFBufferPtr buffer(var.pInterface);
    if (buffer == nullptr || buffer->GetSize() == 0) return;
    const auto* data = static_cast<const uint8_t*>(buffer->GetNative());
    sequence_header_.assign(data, data + buffer->GetSize());
  }

  // Забирает готовые кадры. wait_ms > 0 — ждём первый результат до этого
  // срока (через QueryTimeout энкодера или коротким опросом).
  void DrainOutput(int64_t wait_ms) {
    const int64_t deadline = webrtc::TimeMillis() + wait_ms;
    while (true) {
      amf::AMFDataPtr data;
      const AMF_RESULT result = session_.encoder()->QueryOutput(&data);
      if (result == AMF_OK && data != nullptr) {
        Deliver(data);
        if (pending_.empty()) return;
        continue;
      }
      if (result != AMF_REPEAT && result != AMF_OK) {
        if (result != AMF_EOF) RTC_LOG(LS_WARNING) << "AMF: QueryOutput result=" << result;
        return;
      }
      if (webrtc::TimeMillis() >= deadline) return;
      if (!query_timeout_) ::Sleep(1);
    }
  }

  void Deliver(const amf::AMFDataPtr& data) {
    amf::AMFBufferPtr buffer(data);
    if (buffer == nullptr || pending_.empty()) return;

    amf_int64 type = AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_P;
    buffer->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
    const bool key_frame = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR ||
                           type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I;

    const auto* payload = static_cast<const uint8_t*>(buffer->GetNative());
    const size_t length = buffer->GetSize();

    webrtc::EncodedImage image;
    if (key_frame && !sequence_header_.empty() &&
        !StartsWithSps(payload, length)) {
      std::vector<uint8_t> joined;
      joined.reserve(sequence_header_.size() + length);
      joined.insert(joined.end(), sequence_header_.begin(),
                    sequence_header_.end());
      joined.insert(joined.end(), payload, payload + length);
      image.SetEncodedData(
          webrtc::EncodedImageBuffer::Create(joined.data(), joined.size()));
    } else {
      image.SetEncodedData(webrtc::EncodedImageBuffer::Create(payload, length));
    }

    const Pending meta = std::move(pending_.front());
    pending_.pop_front();

    image._encodedWidth = width_;
    image._encodedHeight = height_;
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
    codec_specific.codecSpecific.H264.idr_frame = key_frame;

    const webrtc::EncodedImageCallback::Result result =
        callback_->OnEncodedImage(image, &codec_specific);
    if (result.error != webrtc::EncodedImageCallback::Result::OK) {
      RTC_LOG(LS_WARNING) << "AMF: OnEncodedImage error " << result.error;
    }
    bitrate_adjuster_.Update(image.size());
  }

  AmfSession session_;
  std::deque<Pending> pending_;
  std::vector<uint8_t> sequence_header_;

  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::H264BitstreamParser bitstream_parser_;
  webrtc::BitrateAdjuster bitrate_adjuster_;

  int width_ = 0;
  int height_ = 0;
  int framerate_ = 30;
  int target_bitrate_bps_ = 0;
  int64_t frame_index_ = 0;
  bool query_timeout_ = false;
  bool reconfigure_needed_ = false;
  webrtc::VideoCodecMode mode_ = webrtc::VideoCodecMode::kRealtimeVideo;
};

#endif  // WEBRTC_WIN

}  // namespace

bool AmfH264Supported() {
#if defined(WEBRTC_WIN)
  static const bool supported = ProbeAmfH264();
  return supported;
#else
  return false;
#endif
}

std::unique_ptr<webrtc::VideoEncoder> CreateAmfH264Encoder(
    const webrtc::Environment& /* env */,
    const webrtc::SdpVideoFormat& /* format */) {
#if defined(WEBRTC_WIN)
  if (!AmfH264Supported()) return nullptr;
  return std::make_unique<AmfH264Encoder>();
#else
  return nullptr;
#endif
}

}  // namespace squeak
