/*
 *  Squeak: общее D3D11-устройство процесса для захвата, кодеков и рендера.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/gpu_context.h"

#if defined(WEBRTC_WIN)

#include <algorithm>
#include <cstring>
#include <utility>

#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"
#include "src/win/d3d11_frame_buffer.h"
#include "third_party/libyuv/include/libyuv/planar_functions.h"

namespace squeak {
namespace {

using Microsoft::WRL::ComPtr;

// Кадров в обороте на всё устройство: захват держит один-два, энкодер один,
// слои симулкаста по одному на слой, каждый принимаемый поток — два-три между
// декодером и рендером. Выше потолка пул отвечает nullptr — кадр пропускается,
// а не копится в видеопамяти.
constexpr size_t kMaxOutstanding = 24;

// Простаивающих текстур одного размера и всего. Разные размеры живут бок о бок
// (свой захват и чужие потоки), поэтому смена размера не выбрасывает чужие
// текстуры, а лишнее отсекается при возврате в пул.
constexpr size_t kMaxIdlePerKey = 8;
constexpr size_t kMaxIdleTotal = 32;

bool SameLuid(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

// Реестр контекстов по адаптеру. Слабых ссылок у webrtc::scoped_refptr нет,
// поэтому держим сильные: устройство живёт до конца процесса, это дёшево и
// избавляет от гонок пересоздания.
webrtc::Mutex& RegistryLock() {
  static webrtc::Mutex* lock = new webrtc::Mutex();
  return *lock;
}

std::vector<webrtc::scoped_refptr<GpuContext>>& Registry() {
  static auto* registry = new std::vector<webrtc::scoped_refptr<GpuContext>>();
  return *registry;
}

ComPtr<IDXGIAdapter1> PrimaryAdapter() {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

  // Адаптер с основным выходом: именно его дублирует захват экрана по
  // умолчанию, и именно на нём zero-copy имеет смысл.
  ComPtr<IDXGIAdapter1> first_hardware;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (first_hardware == nullptr) first_hardware = adapter;
    for (UINT j = 0;; ++j) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(j, &output) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_OUTPUT_DESC out_desc = {};
      if (SUCCEEDED(output->GetDesc(&out_desc)) && out_desc.AttachedToDesktop) {
        return adapter;
      }
    }
  }
  return first_hardware;
}

ComPtr<IDXGIAdapter1> AdapterByVendor(UINT vendor_id) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == vendor_id &&
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
      return adapter;
    }
  }
  return nullptr;
}

RECT FullRect(ID3D11Texture2D* texture) {
  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  return RECT{0, 0, static_cast<LONG>(desc.Width),
              static_cast<LONG>(desc.Height)};
}

// Вписывает source в target по меньшей стороне, центрируя. Стороны чётные:
// NV12 хранит цветность на пару пикселей.
RECT FitInside(const RECT& source, const RECT& target) {
  const double sw = static_cast<double>(source.right - source.left);
  const double sh = static_cast<double>(source.bottom - source.top);
  const double tw = static_cast<double>(target.right - target.left);
  const double th = static_cast<double>(target.bottom - target.top);
  if (sw <= 0 || sh <= 0 || tw <= 0 || th <= 0) return target;
  const double scale = std::min(tw / sw, th / sh);
  LONG w = static_cast<LONG>(sw * scale) & ~1L;
  LONG h = static_cast<LONG>(sh * scale) & ~1L;
  w = std::max<LONG>(2, w);
  h = std::max<LONG>(2, h);
  const LONG x = target.left + (static_cast<LONG>(tw) - w) / 2;
  const LONG y = target.top + (static_cast<LONG>(th) - h) / 2;
  return RECT{x, y, x + w, y + h};
}

bool IsYuv(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010 ||
         format == DXGI_FORMAT_420_OPAQUE;
}

}  // namespace

bool GpuContext::PoolKey::operator<(const PoolKey& other) const {
  if (width != other.width) return width < other.width;
  if (height != other.height) return height < other.height;
  return format < other.format;
}

bool GpuContext::ProcessorKey::operator<(const ProcessorKey& other) const {
  if (in_width != other.in_width) return in_width < other.in_width;
  if (in_height != other.in_height) return in_height < other.in_height;
  if (out_width != other.out_width) return out_width < other.out_width;
  return out_height < other.out_height;
}

GpuContext::GpuContext() = default;
GpuContext::~GpuContext() = default;

webrtc::scoped_refptr<GpuContext> GpuContext::Shared() {
  static webrtc::scoped_refptr<GpuContext>* shared = [] {
    auto* result = new webrtc::scoped_refptr<GpuContext>();
    ComPtr<IDXGIAdapter1> adapter = PrimaryAdapter();
    if (adapter != nullptr) *result = ForAdapter(adapter.Get());
    return result;
  }();
  return *shared;
}

webrtc::scoped_refptr<GpuContext> GpuContext::ForAdapter(
    IDXGIAdapter1* adapter) {
  if (adapter == nullptr) return nullptr;
  DXGI_ADAPTER_DESC1 desc = {};
  if (FAILED(adapter->GetDesc1(&desc))) return nullptr;

  webrtc::MutexLock lock(&RegistryLock());
  for (const auto& existing : Registry()) {
    if (SameLuid(existing->luid_, desc.AdapterLuid)) return existing;
  }
  auto context = webrtc::make_ref_counted<GpuContext>();
  if (!context->Init(adapter)) return nullptr;
  Registry().push_back(context);
  return context;
}

webrtc::scoped_refptr<GpuContext> GpuContext::ForVendor(UINT vendor_id) {
  ComPtr<IDXGIAdapter1> adapter = AdapterByVendor(vendor_id);
  return adapter != nullptr ? ForAdapter(adapter.Get()) : nullptr;
}

webrtc::scoped_refptr<GpuContext> GpuContext::ForDevice(ID3D11Device* device) {
  if (device == nullptr) return nullptr;
  webrtc::MutexLock lock(&RegistryLock());
  for (const auto& existing : Registry()) {
    if (existing->device_.Get() == device) return existing;
  }
  return nullptr;
}

bool GpuContext::SameAdapter(const GpuContext& other) const {
  return SameLuid(luid_, other.luid_);
}

bool GpuContext::Init(IDXGIAdapter1* adapter) {
  adapter_ = adapter;
  DXGI_ADAPTER_DESC1 desc = {};
  if (FAILED(adapter_->GetDesc1(&desc))) return false;
  luid_ = desc.AdapterLuid;
  vendor_id_ = desc.VendorId;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
               D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  HRESULT hr = ::D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                   nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device_, nullptr,
                                   &context_);
  if (hr == E_INVALIDARG) {
    // Старый рантайм не знает 11_1 — пробуем без него.
    hr = ::D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                             flags, levels + 1, ARRAYSIZE(levels) - 1,
                             D3D11_SDK_VERSION, &device_, nullptr, &context_);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "GPU: D3D11CreateDevice hr=" << hr;
    return false;
  }
  if (FAILED(device_.As(&video_device_)) ||
      FAILED(context_.As(&video_context_))) {
    RTC_LOG(LS_WARNING) << "GPU: устройство без видеоподдержки";
    return false;
  }

  // Immediate context дёргают поток захвата, кодеки через MF/NVENC/AMF и
  // рендеры из своих потоков — без защиты это гонка внутри рантайма.
  ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(device_.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }

  char name[128] = {};
  ::WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1,
                        nullptr, nullptr);
  const char* vendor = IsNvidia() ? "NVIDIA" : IsAmd() ? "AMD" : IsIntel() ? "Intel" : "?";
  RTC_LOG(LS_INFO) << "GPU: устройство " << name << " (" << vendor << ", vendor id "
                   << vendor_id_ << ")";
  return true;
}

ComPtr<ID3D11Texture2D> GpuContext::CreateTexture(UINT width, UINT height,
                                                  DXGI_FORMAT format,
                                                  UINT bind_flags,
                                                  D3D11_USAGE usage,
                                                  UINT cpu_access,
                                                  UINT misc_flags) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = usage;
  desc.BindFlags = bind_flags;
  desc.CPUAccessFlags = cpu_access;
  desc.MiscFlags = misc_flags;
  ComPtr<ID3D11Texture2D> texture;
  const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "GPU: CreateTexture2D " << width << "x" << height
                        << " fmt=" << format << " hr=" << hr;
    return nullptr;
  }
  return texture;
}

ComPtr<ID3D11Texture2D> GpuContext::TakeFromPool(const PoolKey& key) {
  // Под lock_.
  auto it = pool_.find(key);
  if (it != pool_.end() && !it->second.empty()) {
    ComPtr<ID3D11Texture2D> texture = it->second.back();
    it->second.pop_back();
    return texture;
  }
  if (pool_outstanding_ >= kMaxOutstanding) return nullptr;

  // Простаивающего слишком много — значит, размеры менялись и старые больше не
  // нужны. Освобождаем видеопамять разом; нужные пересоздадутся.
  size_t idle = 0;
  for (const auto& entry : pool_) idle += entry.second.size();
  if (idle > kMaxIdleTotal) pool_.clear();

  // Общий ресурс нужен движку Flutter: он открывает текстуру на своём
  // устройстве по хэндлу.
  return CreateTexture(key.width, key.height, key.format,
                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                       D3D11_USAGE_DEFAULT, 0, D3D11_RESOURCE_MISC_SHARED);
}

webrtc::scoped_refptr<D3D11FrameBuffer> GpuContext::CreateNV12Frame(
    int width, int height) {
  if (width <= 0 || height <= 0) return nullptr;
  const PoolKey key{static_cast<UINT>(width), static_cast<UINT>(height),
                    DXGI_FORMAT_NV12};
  ComPtr<ID3D11Texture2D> texture;
  {
    webrtc::MutexLock lock(&lock_);
    texture = TakeFromPool(key);
    if (texture == nullptr) return nullptr;
    ++pool_outstanding_;
  }
  return D3D11FrameBuffer::Create(
      webrtc::scoped_refptr<GpuContext>(this), texture, 0, width, height,
      /*pooled=*/true);
}

void GpuContext::RecycleTexture(ComPtr<ID3D11Texture2D> texture) {
  if (texture == nullptr) return;
  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  webrtc::MutexLock lock(&lock_);
  if (pool_outstanding_ > 0) --pool_outstanding_;
  auto& idle = pool_[PoolKey{desc.Width, desc.Height, desc.Format}];
  if (idle.size() < kMaxIdlePerKey) idle.push_back(std::move(texture));
}

GpuContext::Processor* GpuContext::ProcessorFor(const ProcessorKey& key) {
  // Под lock_.
  auto it = processors_.find(key);
  if (it != processors_.end()) return it->second.get();

  // Редкие размеры не должны копиться: при смене разрешения потока старые
  // процессоры уходят.
  if (processors_.size() >= 6) processors_.clear();

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputWidth = key.in_width;
  content.InputHeight = key.in_height;
  content.OutputWidth = key.out_width;
  content.OutputHeight = key.out_height;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  auto processor = std::make_unique<Processor>();
  if (FAILED(video_device_->CreateVideoProcessorEnumerator(
          &content, &processor->enumerator))) {
    return nullptr;
  }
  if (FAILED(video_device_->CreateVideoProcessor(processor->enumerator.Get(),
                                                 0, &processor->processor))) {
    return nullptr;
  }

  // «Улучшайзеры» драйвера (шумодав, оттенки кожи, динамический контраст)
  // искажают картинку рабочего стола — отключаем.
  video_context_->VideoProcessorSetStreamAutoProcessingMode(
      processor->processor.Get(), 0, FALSE);
  video_context_->VideoProcessorSetStreamFrameFormat(
      processor->processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_context_->VideoProcessorSetStreamOutputRate(
      processor->processor.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL,
      TRUE, nullptr);

  Processor* result = processor.get();
  processors_[key] = std::move(processor);
  return result;
}

ID3D11VideoProcessorInputView* GpuContext::InputView(Processor* processor,
                                                     ID3D11Texture2D* texture,
                                                     UINT subresource) {
  // Под lock_. Кэш только для текстур с нулевым подресурсом — у массивов
  // декодера срез меняется от кадра к кадру, их не кэшируем.
  const bool cacheable = subresource == 0;
  if (cacheable) {
    auto it = processor->input_views.find(texture);
    if (it != processor->input_views.end()) return it->second.Get();
  }
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {};
  desc.FourCC = 0;
  desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipSlice = 0;
  desc.Texture2D.ArraySlice = subresource;
  ComPtr<ID3D11VideoProcessorInputView> view;
  if (FAILED(video_device_->CreateVideoProcessorInputView(
          texture, processor->enumerator.Get(), &desc, &view))) {
    return nullptr;
  }
  if (cacheable) {
    processor->input_views[texture] = view;
    processor->keepalive.emplace_back(texture);
  } else {
    // Живёт до следующего вызова: один слот под некэшируемые.
    processor->input_views[nullptr] = view;
  }
  return view.Get();
}

ID3D11VideoProcessorOutputView* GpuContext::OutputView(Processor* processor,
                                                       ID3D11Texture2D* texture,
                                                       UINT subresource) {
  // Под lock_. Выходы бывают чужие — back buffer swap chain, который
  // пересоздаётся при ресайзе и требует отпустить все ссылки. Кэшируем только
  // свои текстуры из пула (они помечены MISC_SHARED и живут в пуле).
  D3D11_TEXTURE2D_DESC tex = {};
  texture->GetDesc(&tex);
  const bool cacheable =
      subresource == 0 && (tex.MiscFlags & D3D11_RESOURCE_MISC_SHARED) != 0 &&
      IsYuv(tex.Format);
  if (cacheable) {
    auto it = processor->output_views.find(texture);
    if (it != processor->output_views.end()) return it->second.Get();
  }
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc = {};
  if (tex.ArraySize > 1) {
    desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipSlice = 0;
    desc.Texture2DArray.FirstArraySlice = subresource;
    desc.Texture2DArray.ArraySize = 1;
  } else {
    desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
  }
  ComPtr<ID3D11VideoProcessorOutputView> view;
  if (FAILED(video_device_->CreateVideoProcessorOutputView(
          texture, processor->enumerator.Get(), &desc, &view))) {
    return nullptr;
  }
  if (cacheable) {
    processor->output_views[texture] = view;
    processor->keepalive.emplace_back(texture);
  } else {
    processor->output_views[nullptr] = view;
  }
  return view.Get();
}

bool GpuContext::Blit(ID3D11Texture2D* src, UINT src_subresource,
                      const RECT* src_rect, ID3D11Texture2D* dst,
                      UINT dst_subresource, const RECT* dst_rect,
                      bool letterbox) {
  if (src == nullptr || dst == nullptr) return false;
  D3D11_TEXTURE2D_DESC src_desc = {};
  D3D11_TEXTURE2D_DESC dst_desc = {};
  src->GetDesc(&src_desc);
  dst->GetDesc(&dst_desc);

  const RECT source = src_rect != nullptr ? *src_rect : FullRect(src);
  const RECT target = dst_rect != nullptr ? *dst_rect : FullRect(dst);
  const RECT dest = letterbox ? FitInside(source, target) : target;

  webrtc::MutexLock lock(&lock_);
  Processor* processor = ProcessorFor(ProcessorKey{
      static_cast<UINT>(source.right - source.left),
      static_cast<UINT>(source.bottom - source.top),
      static_cast<UINT>(target.right - target.left),
      static_cast<UINT>(target.bottom - target.top)});
  if (processor == nullptr) return false;

  ID3D11VideoProcessorInputView* input =
      InputView(processor, src, src_subresource);
  ID3D11VideoProcessorOutputView* output =
      OutputView(processor, dst, dst_subresource);
  if (input == nullptr || output == nullptr) return false;

  ID3D11VideoProcessor* vp = processor->processor.Get();

  // Цвет: BT.709, YCbCr в студийном диапазоне 16-235, RGB в полном. Без явного
  // указания драйвер берёт BT.601, и оттенки на приёме уезжают.
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_space = {};
  in_space.Usage = 0;
  in_space.RGB_Range = 0;
  in_space.YCbCr_Matrix = 1;
  in_space.Nominal_Range = IsYuv(src_desc.Format)
                               ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
                               : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_space = in_space;
  out_space.Nominal_Range = IsYuv(dst_desc.Format)
                                ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
                                : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
  video_context_->VideoProcessorSetStreamColorSpace(vp, 0, &in_space);
  video_context_->VideoProcessorSetOutputColorSpace(vp, &out_space);

  video_context_->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &source);
  video_context_->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dest);
  video_context_->VideoProcessorSetOutputTargetRect(vp, TRUE, &target);

  // Поля letterbox внутри target — чёрные.
  D3D11_VIDEO_COLOR black = {};
  if (IsYuv(dst_desc.Format)) {
    black.YCbCr.Y = 16.0f / 255.0f;
    black.YCbCr.Cb = 0.5f;
    black.YCbCr.Cr = 0.5f;
    black.YCbCr.A = 1.0f;
    video_context_->VideoProcessorSetOutputBackgroundColor(vp, TRUE, &black);
  } else {
    black.RGBA.A = 1.0f;
    video_context_->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &black);
  }

  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.OutputIndex = 0;
  stream.InputFrameOrField = 0;
  stream.pInputSurface = input;
  const HRESULT hr =
      video_context_->VideoProcessorBlt(vp, output, 0, 1, &stream);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "GPU: VideoProcessorBlt hr=" << hr;
    return false;
  }
  return true;
}

ComPtr<ID3D11Texture2D> GpuContext::StagingFor(UINT width, UINT height,
                                               DXGI_FORMAT format) {
  // Под lock_.
  const PoolKey key{width, height, format};
  if (staging_ != nullptr && !(staging_key_ < key) && !(key < staging_key_)) {
    return staging_;
  }
  staging_ = CreateTexture(width, height, format, 0, D3D11_USAGE_STAGING,
                           D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE);
  staging_key_ = key;
  return staging_;
}

webrtc::scoped_refptr<webrtc::NV12Buffer> GpuContext::ReadbackNV12(
    ID3D11Texture2D* texture, UINT subresource, int width, int height) {
  if (texture == nullptr || width <= 0 || height <= 0) return nullptr;
  webrtc::MutexLock lock(&lock_);
  ComPtr<ID3D11Texture2D> staging = StagingFor(
      static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_NV12);
  if (staging == nullptr) return nullptr;

  D3D11_BOX box = {0, 0, 0, static_cast<UINT>(width),
                   static_cast<UINT>(height), 1};
  context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture,
                                  subresource, &box);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT hr =
      context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "GPU: Map(staging) hr=" << hr;
    return nullptr;
  }

  auto buffer = webrtc::NV12Buffer::Create(width, height);
  const auto* src = static_cast<const uint8_t*>(mapped.pData);
  libyuv::CopyPlane(src, static_cast<int>(mapped.RowPitch),
                    buffer->MutableDataY(), buffer->StrideY(), width, height);
  // Плоскость UV начинается после height строк Y с тем же шагом.
  libyuv::CopyPlane(src + static_cast<size_t>(mapped.RowPitch) * height,
                    static_cast<int>(mapped.RowPitch), buffer->MutableDataUV(),
                    buffer->StrideUV(), width, (height + 1) / 2);
  context_->Unmap(staging.Get(), 0);
  return buffer;
}

webrtc::scoped_refptr<D3D11FrameBuffer> GpuContext::UploadNV12(
    const uint8_t* data_y, int stride_y, const uint8_t* data_uv, int stride_uv,
    int width, int height) {
  auto frame = CreateNV12Frame(width, height);
  if (frame == nullptr) return nullptr;

  // Кадр отпускаем только вне lock_: его деструктор возвращает текстуру в пул
  // под тем же нерекурсивным мьютексом.
  bool ok = false;
  {
    webrtc::MutexLock lock(&lock_);
    ComPtr<ID3D11Texture2D> staging = StagingFor(
        static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_NV12);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (staging != nullptr &&
        SUCCEEDED(context_->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0,
                                &mapped))) {
      auto* dst = static_cast<uint8_t*>(mapped.pData);
      libyuv::CopyPlane(data_y, stride_y, dst,
                        static_cast<int>(mapped.RowPitch), width, height);
      libyuv::CopyPlane(data_uv, stride_uv,
                        dst + static_cast<size_t>(mapped.RowPitch) * height,
                        static_cast<int>(mapped.RowPitch), width,
                        (height + 1) / 2);
      context_->Unmap(staging.Get(), 0);
      context_->CopyResource(frame->texture(), staging.Get());
      ok = true;
    }
  }
  return ok ? frame : nullptr;
}

}  // namespace squeak

#endif  // WEBRTC_WIN
