/*
 *  Squeak: аппаратный H.264-энкодер NVIDIA (NVENC) для Windows.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/nvenc_video_encoder.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
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
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv/planar_functions.h"

#if defined(WEBRTC_WIN)

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include "src/win/d3d11_frame_buffer.h"
#include "src/win/gpu_context.h"
#include "src/win/nvenc/nvEncodeAPI.h"

#endif  // WEBRTC_WIN

namespace squeak {
namespace {

#if defined(WEBRTC_WIN)

using Microsoft::WRL::ComPtr;

constexpr UINT kNvidiaVendorId = 0x10DE;

// Сколько текстур держим зарегистрированными в NVENC. Пул контекста отдаёт
// одни и те же текстуры по кругу, поэтому регистрация — разовая операция на
// текстуру, а не на кадр. Выше потолка — старые снимаем.
constexpr size_t kMaxRegistrations = 12;

// Кадров в работе у NVENC одновременно. Под игрой видеокарта отдаёт кадр за
// 25–30 мс при бюджете 16,7: синхронный энкодер ждал каждый, и кадры, пришедшие
// за это время, WebRTC выбрасывал — 20–30 к/с (SQC-293). С конвейером выход
// забирается на следующих вызовах, задержка та же, частота держится, пока
// видеокарта укладывается в kInFlight кадров.
constexpr size_t kInFlight = 3;

// Раз в столько кадров — строка в лог: сколько ждали выход и глубина конвейера.
constexpr int kStatsEvery = 600;

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

// Устройство для сессии: общее устройство процесса, если оно на NVIDIA —
// тогда текстуры захвата регистрируются в NVENC без копий. Иначе первый
// NVIDIA-адаптер (ноутбук с Optimus: экран на встроенной графике, кодирование
// на дискретной — кадр едет через системную память, как и раньше).
webrtc::scoped_refptr<GpuContext> NvencGpu() {
  webrtc::scoped_refptr<GpuContext> shared = GpuContext::Shared();
  if (shared != nullptr && shared->IsNvidia()) return shared;
  return GpuContext::ForVendor(kNvidiaVendorId);
}

class NvencSession {
 public:
  ~NvencSession() { Destroy(); }

  bool Open(webrtc::scoped_refptr<GpuContext> gpu) {
    const NvencApi& api = GetNvencApi();
    if (!api.loaded || gpu == nullptr) return false;
    gpu_ = std::move(gpu);

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = gpu_->device();
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
    if (encoder_ == nullptr) return false;
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

  int Cap(NV_ENC_CAPS cap) {
    const NvencApi& api = GetNvencApi();
    if (encoder_ == nullptr) return 0;
    NV_ENC_CAPS_PARAM param = {};
    param.version = NV_ENC_CAPS_PARAM_VER;
    param.capsToQuery = cap;
    int value = 0;
    if (api.fn.nvEncGetEncodeCaps(encoder_, NV_ENC_CODEC_H264_GUID, &param,
                                  &value) != NV_ENC_SUCCESS) {
      return 0;
    }
    return value;
  }

  void Destroy() {
    if (encoder_ != nullptr) {
      GetNvencApi().fn.nvEncDestroyEncoder(encoder_);
      encoder_ = nullptr;
    }
    gpu_ = nullptr;
  }

  void* encoder() const { return encoder_; }
  GpuContext* gpu() const { return gpu_.get(); }

 private:
  webrtc::scoped_refptr<GpuContext> gpu_;
  void* encoder_ = nullptr;
};

bool ProbeNvencH264() {
  NvencSession session;
  if (!session.Open(NvencGpu())) {
    RTC_LOG(LS_INFO) << "NVENC: сессия не открылась — уходим дальше по цепочке";
    return false;
  }
  const bool ok = session.SupportsH264();
  RTC_LOG(LS_INFO) << "NVENC: H.264 " << (ok ? "доступен" : "не поддержан")
                   << ", zero-copy="
                   << (session.gpu() == GpuContext::Shared().get() ? "да" : "нет");
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
    const NvencApi& api = GetNvencApi();
    if (session_.encoder() != nullptr) {
      // Кадры в работе не отдаём — энкодер закрывают, — но ждём, пока NVENC
      // их дочитает: снимать отображение текстуры раньше нельзя.
      for (Pending& pending : pending_) {
        if (async_) ::WaitForSingleObject(slots_[pending.slot].event, 200);
        if (pending.mapped != nullptr) {
          api.fn.nvEncUnmapInputResource(session_.encoder(), pending.mapped);
        }
      }
      for (auto& registration : registrations_) {
        api.fn.nvEncUnregisterResource(session_.encoder(),
                                       registration.handle);
      }
      if (input_buffer_ != nullptr) {
        api.fn.nvEncDestroyInputBuffer(session_.encoder(), input_buffer_);
      }
      for (Slot& slot : slots_) {
        if (slot.event != nullptr) {
          NV_ENC_EVENT_PARAMS event = {};
          event.version = NV_ENC_EVENT_PARAMS_VER;
          event.completionEvent = slot.event;
          api.fn.nvEncUnregisterAsyncEvent(session_.encoder(), &event);
        }
        if (slot.output != nullptr) {
          api.fn.nvEncDestroyBitstreamBuffer(session_.encoder(), slot.output);
        }
      }
    }
    for (Slot& slot : slots_) {
      if (slot.event != nullptr) ::CloseHandle(slot.event);
    }
    pending_.clear();
    slots_.clear();
    input_buffer_ = nullptr;
    registrations_.clear();
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

    if (reconfigure_needed_) {
      // Перенастройку с кадрами в работе NVENC не обещает — сначала отдаём их.
      DeliverPending(/*block=*/true);
      if (!Reconfigure()) return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // Кадр — текстура на устройстве сессии. Нативный кадр с того же
    // устройства остаётся собой; всё остальное (NV12 или I420 из памяти,
    // текстура другого адаптера) заливается один раз. Размер приводим к
    // настроенному на видеокарте: так scaleResolutionDownBy работает, а не
    // пересоздаёт энкодер под размер входа.
    webrtc::scoped_refptr<D3D11FrameBuffer> input =
        ToD3D11Frame(frame.video_frame_buffer(), session_.gpu());
    if (input == nullptr) {
      RTC_LOG(LS_ERROR) << "NVENC: кадр не лёг на устройство";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (input->width() != width_ || input->height() != height_) {
      input = ToD3D11Frame(input->Scale(width_, height_), session_.gpu());
      if (input == nullptr) return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // В энкодер приходят только текстуры пула — не массивы, нулевой подресурс.
    RTC_DCHECK_EQ(input->subresource(), 0u);
    const NvencApi& api = GetNvencApi();

    // Готовое отдаём по порядку; если все выходные буферы заняты — ждём
    // старший кадр, иначе новому некуда писать.
    DeliverReady();
    if (pending_.size() >= slots_.size()) {
      const int64_t wait_start = webrtc::TimeMicros();
      DeliverFront();
      stats_wait_us_ += webrtc::TimeMicros() - wait_start;
    }

    // Текстура напрямую; если драйвер её не регистрирует — прежний путь через
    // входной буфер в системной памяти. Чёрный экран хуже лишней копии.
    NV_ENC_MAP_INPUT_RESOURCE map = {};
    NV_ENC_INPUT_PTR picture_input = nullptr;
    NV_ENC_BUFFER_FORMAT picture_format = NV_ENC_BUFFER_FORMAT_NV12;
    if (!host_input_) {
      NV_ENC_REGISTERED_PTR registered = Register(input->texture());
      if (registered != nullptr) {
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registered;
        if (api.fn.nvEncMapInputResource(session_.encoder(), &map) ==
            NV_ENC_SUCCESS) {
          picture_input = map.mappedResource;
          picture_format = map.mappedBufferFmt;
        } else {
          map.mappedResource = nullptr;
        }
      }
      if (picture_input == nullptr) {
        RTC_LOG(LS_WARNING) << "NVENC: текстуры не регистрируются, кодируем из "
                               "системной памяти";
        host_input_ = true;
      }
    }
    if (host_input_) {
      if (!FillHostInput(*input)) return WEBRTC_VIDEO_CODEC_ERROR;
      picture_input = input_buffer_;
    }

    // Свободный буфер есть всегда: выше старший кадр отдан, если все заняты.
    size_t slot = 0;
    while (std::any_of(pending_.begin(), pending_.end(),
                       [slot](const Pending& p) { return p.slot == slot; })) {
      ++slot;
    }
    RTC_DCHECK_LT(slot, slots_.size());
    if (async_) ::ResetEvent(slots_[slot].event);

    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = picture_input;
    pic.bufferFmt = picture_format;
    pic.outputBitstream = slots_[slot].output;
    pic.completionEvent = async_ ? slots_[slot].event : nullptr;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputWidth = width_;
    pic.inputHeight = height_;
    pic.inputPitch = 0;
    pic.inputTimeStamp = frame.rtp_timestamp();
    if (key_frame) {
      pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                           NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    const int64_t submit_start = webrtc::TimeMicros();
    const NVENCSTATUS status =
        api.fn.nvEncEncodePicture(session_.encoder(), &pic);
    stats_submit_us_ += webrtc::TimeMicros() - submit_start;
    if (status != NV_ENC_SUCCESS) {
      RTC_LOG(LS_ERROR) << "NVENC: nvEncEncodePicture status=" << status;
      if (map.mappedResource != nullptr) {
        api.fn.nvEncUnmapInputResource(session_.encoder(), map.mappedResource);
      }
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // Текстура остаётся отображённой и живой, пока выход не прочитан: NVENC
    // может ещё читать из неё.
    Pending pending;
    pending.slot = slot;
    pending.mapped = map.mappedResource;
    pending.input = input;
    pending.rtp_timestamp = frame.rtp_timestamp();
    pending.ntp_time_ms = frame.ntp_time_ms();
    pending.capture_time_ms = frame.render_time_ms();
    pending.rotation = frame.rotation();
    pending.color_space = frame.color_space();
    pending_.push_back(std::move(pending));
    stats_max_pending_ = std::max(stats_max_pending_, pending_.size());

    // Без асинхронного режима и на запасном входе (один буфер в системной
    // памяти) — по-старому: ждём кадр сразу.
    if (!async_ || host_input_) {
      DeliverPending(/*block=*/true);
    } else {
      DeliverReady();
    }
    LogStats();
    return WEBRTC_VIDEO_CODEC_OK;
  }

  // Запасной вход: буфер NVENC в системной памяти, кадр читается с видеокарты
  // один раз (ReadbackNV12 кэшируется в кадре) и копируется плоскостями.
  bool FillHostInput(D3D11FrameBuffer& input) {
    const NvencApi& api = GetNvencApi();
    if (input_buffer_ == nullptr) {
      NV_ENC_CREATE_INPUT_BUFFER create = {};
      create.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
      create.width = width_;
      create.height = height_;
      create.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
      if (api.fn.nvEncCreateInputBuffer(session_.encoder(), &create) !=
          NV_ENC_SUCCESS) {
        RTC_LOG(LS_ERROR) << "NVENC: nvEncCreateInputBuffer failed";
        return false;
      }
      input_buffer_ = create.inputBuffer;
    }
    webrtc::scoped_refptr<webrtc::NV12Buffer> host = input.ReadbackNV12();
    if (host == nullptr) return false;

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
    libyuv::CopyPlane(host->DataY(), host->StrideY(), dst_y,
                      static_cast<int>(lock.pitch), width_, height_);
    libyuv::CopyPlane(host->DataUV(), host->StrideUV(), dst_uv,
                      static_cast<int>(lock.pitch), width_, (height_ + 1) / 2);
    api.fn.nvEncUnlockInputBuffer(session_.encoder(), input_buffer_);
    return true;
  }

  void SetRates(const RateControlParameters& parameters) override {
    // Слой поставили на паузу: новых Encode не будет, и кадры в работе
    // отдаём сейчас, а не при возобновлении устаревшими.
    if (parameters.bitrate.get_sum_bps() == 0) {
      DeliverPending(/*block=*/true);
      return;
    }
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
    info.implementation_name = "NVENC H264";
    info.is_hardware_accelerated = true;
    // Кадры в видеопамяти принимаем как есть; из памяти предпочитаем NV12 —
    // ровно то, что отдаёт захват, без лишнего перекладывания плоскостей.
    info.supports_native_handle = true;
    info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kNV12};
    // CBR с VBV в один кадр держит битрейт сам. Недоверенному энкодеру
    // WebRTC включает свой выброс кадров (MediaOptimization), и для H.264 без
    // временных слоёв он не выключается даже на демонстрации: после каждого
    // крупного опорного кадра выкидывались следующие — картинка дёргалась
    // (SQC-258).
    info.has_trusted_rate_controller = true;
    info.requested_resolution_alignment = 2;
    info.apply_alignment_to_all_simulcast_layers = true;
    info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(kLowQp, kHighQp);
    return info;
  }

 private:
  static constexpr int kLowQp = 24;
  static constexpr int kHighQp = 37;

  struct Registration {
    ComPtr<ID3D11Texture2D> texture;
    NV_ENC_REGISTERED_PTR handle = nullptr;
  };

  struct Slot {
    NV_ENC_OUTPUT_PTR output = nullptr;
    HANDLE event = nullptr;
  };

  // Кадр, отданный NVENC: выход ещё не прочитан, текстура отображена.
  struct Pending {
    size_t slot = 0;
    NV_ENC_INPUT_PTR mapped = nullptr;
    webrtc::scoped_refptr<D3D11FrameBuffer> input;
    uint32_t rtp_timestamp = 0;
    int64_t ntp_time_ms = 0;
    int64_t capture_time_ms = 0;
    webrtc::VideoRotation rotation = webrtc::kVideoRotation_0;
    std::optional<webrtc::ColorSpace> color_space;
  };

  int OpenEncoder() {
    if (!session_.Open(NvencGpu())) return WEBRTC_VIDEO_CODEC_ERROR;
    const NvencApi& api = GetNvencApi();

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    if (api.fn.nvEncGetEncodePresetConfigEx(
            session_.encoder(), NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
            NV_ENC_TUNING_INFO_LOW_LATENCY, &preset) != NV_ENC_SUCCESS) {
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    config_ = preset.presetCfg;
    // High только там, где железо умеет CABAC; иначе остаёмся на профиле пресета.
    const bool cabac = session_.Cap(NV_ENC_CAPS_SUPPORT_CABAC) != 0;
    const bool transform_8x8 =
        session_.Cap(NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM) != 0;
    if (cabac) config_.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    // Бесконечный GOP: ключевые кадры шлём только по запросу WebRTC, он сам
    // просит их при потерях и при подключении нового зрителя.
    config_.gopLength = NVENC_INFINITE_GOPLENGTH;
    config_.frameIntervalP = 1;
    ApplyRateControl(&config_);

    NV_ENC_CONFIG_H264& h264 = config_.encodeCodecConfig.h264Config;
    h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    h264.repeatSPSPPS = 1;
    h264.sliceMode = 0;
    h264.sliceModeData = 0;
    h264.level = NV_ENC_LEVEL_AUTOSELECT;
    h264.chromaFormatIDC = 1;
    // High profile: CABAC и адаптивное преобразование 8x8 дают 10-15% к
    // качеству на том же битрейте. Декодеры приёмной стороны (MediaFoundation,
    // VideoToolbox) профиль из SDP не сверяют.
    if (cabac) h264.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    if (transform_8x8) {
      h264.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_ENABLE;
    }
    h264.outputBufferingPeriodSEI = 0;
    h264.outputPictureTimingSEI = 0;
    h264.enableFillerDataInsertion = 0;
    // Цвет объявляем явно: BT.709, студийный диапазон. Декодер и рендер на
    // другой стороне берут матрицу отсюда, а не угадывают по разрешению.
    h264.h264VUIParameters.videoSignalTypePresentFlag = 1;
    h264.h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    h264.h264VUIParameters.videoFullRangeFlag = 0;
    h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
    h264.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    h264.h264VUIParameters.transferCharacteristics =
        NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    h264.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    // bitstream_restriction в VUI: декодеру сказано, что переупорядочивания
    // нет (B-кадров нет), и держать кадры под худший случай DPB незачем.
    // Без этого декодер Microsoft на приёме копил кадры (29.10, SQC-258).
    h264.h264VUIParameters.bitstreamRestrictionFlag = 1;

    init_params_ = {};
    init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params_.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init_params_.presetGUID = NV_ENC_PRESET_P4_GUID;
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
    async_ = session_.Cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT) != 0;
    init_params_.enableEncodeAsync = async_ ? 1 : 0;

    NVENCSTATUS status =
        api.fn.nvEncInitializeEncoder(session_.encoder(), &init_params_);
    if (status != NV_ENC_SUCCESS) {
      RTC_LOG(LS_ERROR) << "NVENC: nvEncInitializeEncoder status=" << status;
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    const size_t slot_count = async_ ? kInFlight : 1;
    for (size_t i = 0; i < slot_count; ++i) {
      NV_ENC_CREATE_BITSTREAM_BUFFER output = {};
      output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      status = api.fn.nvEncCreateBitstreamBuffer(session_.encoder(), &output);
      if (status != NV_ENC_SUCCESS) {
        Release();
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
      slots_.push_back(Slot{output.bitstreamBuffer, nullptr});
      if (!async_) continue;
      // Ручной сброс: проверка готовности без ожидания не должна гасить
      // событие до того, как кадр заберут. Сбрасываем перед отправкой.
      HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      NV_ENC_EVENT_PARAMS params = {};
      params.version = NV_ENC_EVENT_PARAMS_VER;
      params.completionEvent = event;
      if (event == nullptr ||
          api.fn.nvEncRegisterAsyncEvent(session_.encoder(), &params) !=
              NV_ENC_SUCCESS) {
        RTC_LOG(LS_ERROR) << "NVENC: событие завершения не зарегистрировано";
        if (event != nullptr) ::CloseHandle(event);
        Release();
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
      slots_.back().event = event;
    }

    reconfigure_needed_ = false;
    RTC_LOG(LS_INFO) << "NVENC: инициализирован " << width_ << "x" << height_
                     << "@" << framerate_ << " " << target_bitrate_bps_
                     << " bit/s, P4 low-latency, two-pass, cabac=" << cabac
                     << " 8x8=" << transform_8x8 << ", "
                     << (async_ ? "конвейер на " : "синхронно, ")
                     << slot_count << " кадр.";
    return WEBRTC_VIDEO_CODEC_OK;
  }

  void ApplyRateControl(NV_ENC_CONFIG* config) {
    const uint32_t average = bitrate_adjuster_.GetAdjustedBitrateBps() > 0
                                 ? bitrate_adjuster_.GetAdjustedBitrateBps()
                                 : target_bitrate_bps_;
    const uint32_t fps = framerate_ > 0 ? framerate_ : 30;
    config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config->rcParams.averageBitRate = average;
    config->rcParams.maxBitRate = average;
    // Два прохода (первый в четверть разрешения) — распределение битов по
    // кадру заметно ровнее, особенно на тексте и резких границах.
    config->rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
    config->rcParams.enableAQ = 1;
    config->rcParams.enableTemporalAQ = 0;
    config->rcParams.enableLookahead = 0;
    config->rcParams.disableBadapt = 1;
    config->rcParams.zeroReorderDelay = 1;
    config->rcParams.enableNonRefP = 0;
    // VBV в один кадр — это и есть low latency: энкодеру нечем «занять»
    // будущие кадры, поэтому он не копит задержку. На демонстрации ключевой
    // кадр не крупнее обычного: он грубее (кубики на полсекунды, следующие
    // кадры дорисовывают), зато нет всплеска, после которого кадры
    // задерживаются в пейсере, — частота держится (SQC-258). У камеры —
    // втрое крупнее: там мыльный IDR заметнее короткого всплеска.
    config->rcParams.vbvBufferSize = average / fps;
    config->rcParams.vbvInitialDelay = config->rcParams.vbvBufferSize;
    config->rcParams.lowDelayKeyFrameScale =
        mode_ == webrtc::VideoCodecMode::kScreensharing ? 1 : 3;
  }

  bool Reconfigure() {
    const NvencApi& api = GetNvencApi();
    ApplyRateControl(&config_);
    init_params_.frameRateNum = framerate_;
    init_params_.encodeConfig = &config_;

    NV_ENC_RECONFIGURE_PARAMS params = {};
    params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    params.reInitEncodeParams = init_params_;
    params.resetEncoder = 0;
    params.forceIDR = 0;
    if (api.fn.nvEncReconfigureEncoder(session_.encoder(), &params) !=
        NV_ENC_SUCCESS) {
      RTC_LOG(LS_WARNING) << "NVENC: nvEncReconfigureEncoder failed";
      return false;
    }
    reconfigure_needed_ = false;
    return true;
  }

  NV_ENC_REGISTERED_PTR Register(ID3D11Texture2D* texture) {
    for (const auto& registration : registrations_) {
      if (registration.texture.Get() == texture) return registration.handle;
    }
    const NvencApi& api = GetNvencApi();
    if (registrations_.size() >= kMaxRegistrations) {
      // Текстуру кадра в работе снимать нельзя: NVENC ещё читает её.
      const auto victim = std::find_if(
          registrations_.begin(), registrations_.end(),
          [this](const Registration& registration) {
            return std::none_of(
                pending_.begin(), pending_.end(), [&](const Pending& pending) {
                  return pending.input->texture() == registration.texture.Get();
                });
          });
      if (victim != registrations_.end()) {
        api.fn.nvEncUnregisterResource(session_.encoder(), victim->handle);
        registrations_.erase(victim);
      }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = texture;
    reg.width = desc.Width;
    reg.height = desc.Height;
    reg.pitch = 0;
    reg.subResourceIndex = 0;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    const NVENCSTATUS status =
        api.fn.nvEncRegisterResource(session_.encoder(), &reg);
    if (status != NV_ENC_SUCCESS) {
      RTC_LOG(LS_ERROR) << "NVENC: nvEncRegisterResource status=" << status;
      return nullptr;
    }
    Registration registration;
    registration.texture = texture;
    registration.handle = reg.registeredResource;
    registrations_.push_back(registration);
    return registration.handle;
  }

  bool FrontReady() const {
    return !pending_.empty() && async_ &&
           ::WaitForSingleObject(slots_[pending_.front().slot].event, 0) ==
               WAIT_OBJECT_0;
  }

  // Всё, что NVENC уже закончил, — по порядку и без ожидания.
  void DeliverReady() {
    while (FrontReady()) DeliverFront();
  }

  // Все кадры в работе; block — ждать незаконченные.
  void DeliverPending(bool block) {
    while (!pending_.empty() && (block || FrontReady())) DeliverFront();
  }

  void DeliverFront() {
    Pending pending = std::move(pending_.front());
    pending_.pop_front();
    const NvencApi& api = GetNvencApi();
    // Событие ручного сброса: проверка готовности его не гасит, и ожидание
    // здесь не повиснет. Таймаут — страховка от зависшего драйвера.
    if (async_ &&
        ::WaitForSingleObject(slots_[pending.slot].event, 1000) != WAIT_OBJECT_0) {
      RTC_LOG(LS_ERROR) << "NVENC: кадр не закончился за секунду";
    }
    DeliverBitstream(pending);
    if (pending.mapped != nullptr) {
      api.fn.nvEncUnmapInputResource(session_.encoder(), pending.mapped);
    }
  }

  void LogStats() {
    if (++stats_frames_ < kStatsEvery) return;
    RTC_LOG(LS_INFO) << "NVENC " << width_ << "x" << height_ << ": "
                     << (async_ ? "конвейер" : "синхронно")
                     << ", в работе до " << stats_max_pending_
                     << ", ожидание выхода "
                     << stats_wait_us_ / stats_frames_ / 1000.0
                     << " мс/кадр, отправка "
                     << stats_submit_us_ / stats_frames_ / 1000.0
                     << " мс/кадр";
    stats_frames_ = 0;
    stats_wait_us_ = 0;
    stats_submit_us_ = 0;
    stats_max_pending_ = 0;
  }

  void DeliverBitstream(const Pending& pending) {
    const NvencApi& api = GetNvencApi();
    const NV_ENC_OUTPUT_PTR output = slots_[pending.slot].output;
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = output;
    lock.doNotWait = 0;
    if (api.fn.nvEncLockBitstream(session_.encoder(), &lock) !=
        NV_ENC_SUCCESS) {
      RTC_LOG(LS_ERROR) << "NVENC: nvEncLockBitstream failed";
      return;
    }

    webrtc::EncodedImage image;
    image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
        static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
        lock.bitstreamSizeInBytes));
    const uint32_t size = lock.bitstreamSizeInBytes;
    const bool key_frame = lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                           lock.pictureType == NV_ENC_PIC_TYPE_I;
    const int avg_qp = static_cast<int>(lock.frameAvgQP);
    api.fn.nvEncUnlockBitstream(session_.encoder(), output);

    image._encodedWidth = width_;
    image._encodedHeight = height_;
    image.SetRtpTimestamp(pending.rtp_timestamp);
    image.ntp_time_ms_ = pending.ntp_time_ms;
    image.capture_time_ms_ = pending.capture_time_ms;
    image.rotation_ = pending.rotation;
    image.SetColorSpace(pending.color_space);
    image.content_type_ = mode_ == webrtc::VideoCodecMode::kScreensharing
                              ? webrtc::VideoContentType::SCREENSHARE
                              : webrtc::VideoContentType::UNSPECIFIED;
    image.timing_.flags = webrtc::VideoSendTiming::kInvalid;
    image._frameType = key_frame ? webrtc::VideoFrameType::kVideoFrameKey
                                 : webrtc::VideoFrameType::kVideoFrameDelta;

    if (avg_qp > 0 && avg_qp <= 51) {
      image.qp_ = avg_qp;
    } else {
      bitstream_parser_.ParseBitstream(image);
      image.qp_ = bitstream_parser_.GetLastSliceQp().value_or(-1);
    }

    webrtc::CodecSpecificInfo codec_specific;
    codec_specific.codecType = webrtc::kVideoCodecH264;
    codec_specific.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;
    codec_specific.codecSpecific.H264.idr_frame = key_frame;

    const webrtc::EncodedImageCallback::Result result =
        callback_->OnEncodedImage(image, &codec_specific);
    if (result.error != webrtc::EncodedImageCallback::Result::OK) {
      RTC_LOG(LS_WARNING) << "NVENC: OnEncodedImage error " << result.error;
    }
    bitrate_adjuster_.Update(size);
  }

  NvencSession session_;
  NV_ENC_INITIALIZE_PARAMS init_params_ = {};
  NV_ENC_CONFIG config_ = {};
  NV_ENC_INPUT_PTR input_buffer_ = nullptr;
  bool host_input_ = false;
  std::deque<Registration> registrations_;

  // Выходной буфер и событие завершения на каждый кадр в работе.
  std::vector<Slot> slots_;
  std::deque<Pending> pending_;
  bool async_ = false;

  int stats_frames_ = 0;
  int64_t stats_wait_us_ = 0;
  int64_t stats_submit_us_ = 0;
  size_t stats_max_pending_ = 0;

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
  if (!NvencH264Supported()) return nullptr;
  return std::make_unique<NvencH264Encoder>();
#else
  return nullptr;
#endif
}

}  // namespace squeak
