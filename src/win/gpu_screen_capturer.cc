/*
 *  Squeak: захват экрана без обратного чтения (Windows, Desktop Duplication).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/gpu_screen_capturer.h"

#if defined(WEBRTC_WIN)

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "src/win/d3d11_frame_buffer.h"
#include "src/win/gpu_context.h"

#include <windows.h>

#include <avrt.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <timeapi.h>
#include <wrl/client.h>

namespace squeak {
namespace {

using Microsoft::WRL::ComPtr;

int MakeEven(UINT value) {
  return static_cast<int>(value & ~1u);
}

// Курсор рисуется на копии рабочего стола поверх кадра крошечным проходом:
// квадрат в нормализованных координатах из константного буфера и выборка из
// текстуры формы с альфа-смешиванием.
constexpr char kCursorShader[] = R"(
cbuffer Placement : register(b0) {
  float4 rect;  // x, y, w, h in NDC
};
Texture2D shape : register(t0);
SamplerState linear_sampler : register(s0);
struct Vertex { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
  float2 corner = float2(id & 1, (id >> 1) & 1);
  Vertex v;
  v.pos = float4(rect.x + corner.x * rect.z, rect.y - corner.y * rect.w, 0, 1);
  v.uv = corner;
  return v;
}
float4 PS(Vertex v) : SV_TARGET { return shape.Sample(linear_sampler, v.uv); }
)";

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
                                     const D3D_SHADER_MACRO*, ID3DInclude*,
                                     LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                     ID3DBlob**);

D3DCompileFn LoadD3DCompile() {
  static D3DCompileFn fn = [] {
    HMODULE library = ::LoadLibraryW(L"d3dcompiler_47.dll");
    if (library == nullptr) return static_cast<D3DCompileFn>(nullptr);
    return reinterpret_cast<D3DCompileFn>(
        ::GetProcAddress(library, "D3DCompile"));
  }();
  return fn;
}

}  // namespace

class GpuScreenCapturer::Impl {
 public:
  explicit Impl(bool show_cursor) : show_cursor_(show_cursor) {}

  ~Impl() { Stop(); }

  bool Init(int64_t screen_index) {
    screen_index_ = screen_index < 0 ? 0 : screen_index;
    if (!FindOutput()) return false;
    gpu_ = GpuContext::ForAdapter(adapter_.Get());
    if (gpu_ == nullptr) return false;
    return OpenDuplication();
  }

  void SetOutputSize(int width, int height) {
    target_width_.store(width > 0 ? width : 0);
    target_height_.store(height > 0 ? height : 0);
  }

  bool Start(uint32_t fps, FrameCallback callback) {
    if (running_.load()) return true;
    if (fps == 0 || callback == nullptr) return false;
    period_us_ = 1000000 / std::min<uint32_t>(std::max<uint32_t>(fps, 1), 240);
    callback_ = std::move(callback);
    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    return true;
  }

  void Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
  }

  bool IsRunning() const { return running_.load(); }

 private:
  bool FindOutput() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    // Устройство обязано жить на том же адаптере, что и захватываемый выход,
    // иначе DuplicateOutput вернёт E_INVALIDARG.
    UINT seen = 0;
    for (UINT a = 0;; ++a) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      for (UINT o = 0;; ++o) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
        if (static_cast<int64_t>(seen) == screen_index_) {
          adapter_ = adapter;
          output_ = output;
          return true;
        }
        ++seen;
      }
    }
    return false;
  }

  bool OpenDuplication() {
    duplication_.Reset();
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output_.As(&output1))) return false;
    const HRESULT hr = output1->DuplicateOutput(gpu_->device(), &duplication_);
    if (FAILED(hr)) {
      // E_ACCESSDENIED — другой процесс держит дубликацию или идёт переход в
      // полноэкранный режим; через паузу обычно отпускает.
      RTC_LOG(LS_WARNING) << "GPU-capture: DuplicateOutput hr=" << hr;
      duplication_.Reset();
      return false;
    }
    return true;
  }

  void Run() {
    // Квант таймера 1 мс: иначе таймауты AcquireNextFrame округляются до
    // 15.6 мс и каденция разваливается. MMCSS поднимает приоритет потока до
    // уровня мультимедийных задач — захват не теснится UI и сетью.
    ::timeBeginPeriod(1);
    DWORD task_index = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Capture", &task_index);

    int64_t deadline_us = webrtc::TimeMicros() + period_us_;
    while (running_.load()) {
      if (duplication_ == nullptr) {
        if (!OpenDuplication()) {
          ::Sleep(100);
          deadline_us = webrtc::TimeMicros() + period_us_;
          continue;
        }
      }

      const int64_t now = webrtc::TimeMicros();
      const int64_t remaining = deadline_us - now;
      // Таймаут в целых миллисекундах вниз: лучше вернуться на долю
      // миллисекунды раньше и добрать опросом, чем проспать дедлайн.
      const UINT timeout_ms =
          remaining > 1000 ? static_cast<UINT>(std::min<int64_t>(remaining / 1000, 100))
                           : 0;
      Acquire(timeout_ms);

      if (webrtc::TimeMicros() < deadline_us) continue;

      Emit();
      deadline_us += period_us_;
      // Отставание больше периода (сон машины, долгий блит) — не навёрстываем
      // пачкой, а начинаем отсчёт заново.
      if (webrtc::TimeMicros() - deadline_us > period_us_) {
        deadline_us = webrtc::TimeMicros() + period_us_;
      }
    }

    if (mmcss != nullptr) ::AvRevertMmThreadCharacteristics(mmcss);
    ::timeEndPeriod(1);
  }

  // Забирает у DWM следующий кадр, если он есть, и переносит его в свою копию.
  void Acquire(UINT timeout_ms) {
    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr =
        duplication_->AcquireNextFrame(timeout_ms, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
    if (FAILED(hr)) {
      // ACCESS_LOST прилетает на смене режима, UAC-затемнении и переключении
      // полноэкранного приложения — дубликацию надо просто пересоздать.
      RTC_LOG(LS_WARNING) << "GPU-capture: AcquireNextFrame hr=" << hr;
      duplication_.Reset();
      return;
    }

    if (show_cursor_ && info.LastMouseUpdateTime.QuadPart != 0) {
      // Курсор DWM рисует аппаратным оверлеем, в кадр дубликации он не входит:
      // движение мыши по неподвижному столу — тоже повод собрать новый кадр.
      cursor_visible_ = info.PointerPosition.Visible != FALSE;
      cursor_x_ = info.PointerPosition.Position.x;
      cursor_y_ = info.PointerPosition.Position.y;
      if (info.PointerShapeBufferSize > 0) ReadCursorShape(info);
      dirty_ = true;
    }

    // Нулевое LastPresentTime — кадр без новой картинки, только метаданные
    // мыши. Копировать его незачем, но ReleaseFrame обязателен.
    if (info.LastPresentTime.QuadPart != 0) {
      ComPtr<ID3D11Texture2D> texture;
      if (SUCCEEDED(resource.As(&texture))) {
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        if (EnsureSource(desc.Width, desc.Height)) {
          webrtc::MutexLock lock(&gpu_->lock());
          gpu_->context()->CopyResource(source_.Get(), texture.Get());
          dirty_ = true;
        }
      }
    }
    duplication_->ReleaseFrame();
  }

  bool EnsureSource(UINT width, UINT height) {
    if (source_ != nullptr && width == input_width_ && height == input_height_) {
      return true;
    }
    // Текстуру дубликации часть драйверов отказывается принимать во входное
    // представление видеопроцессора — она создана не с теми bind-флагами, и
    // кадр нужно отпустить обратно DWM как можно раньше. Своя копия решает оба
    // вопроса и стоит копейки внутри видеопамяти.
    source_ = gpu_->CreateTexture(
        width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (source_ == nullptr) return false;
    composed_.Reset();
    composed_rtv_.Reset();
    if (show_cursor_) {
      // Стол держим нетронутым, курсор кладём на копию: иначе при движении мыши
      // без нового кадра старый курсор остался бы нарисованным.
      composed_ = gpu_->CreateTexture(
          width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    }
    input_width_ = width;
    input_height_ = height;
    RTC_LOG(LS_INFO) << "GPU-capture: рабочий стол " << width << "x" << height;
    return true;
  }

  void OutputSizeFor(int* out_width, int* out_height) const {
    const int target_width = target_width_.load();
    const int target_height = target_height_.load();
    if (target_width <= 0 || target_height <= 0) {
      *out_width = MakeEven(input_width_);
      *out_height = MakeEven(input_height_);
      return;
    }
    const double scale = std::min(
        static_cast<double>(target_width) / static_cast<double>(input_width_),
        static_cast<double>(target_height) /
            static_cast<double>(input_height_));
    if (scale >= 1.0) {
      *out_width = MakeEven(input_width_);
      *out_height = MakeEven(input_height_);
      return;
    }
    *out_width = MakeEven(static_cast<UINT>(input_width_ * scale + 0.5));
    *out_height = MakeEven(static_cast<UINT>(input_height_ * scale + 0.5));
  }

  // На дедлайне: новый кадр — блит в NV12 нужного габарита, иначе повтор
  // предыдущего, чтобы поток не выглядел оборванным.
  void Emit() {
    if (dirty_ && source_ != nullptr) {
      int out_width = 0;
      int out_height = 0;
      OutputSizeFor(&out_width, &out_height);
      if (out_width >= 2 && out_height >= 2) {
        ID3D11Texture2D* input = source_.Get();
        if (show_cursor_ && composed_ != nullptr) {
          {
            webrtc::MutexLock lock(&gpu_->lock());
            gpu_->context()->CopyResource(composed_.Get(), source_.Get());
          }
          if (cursor_visible_) DrawCursor();
          input = composed_.Get();
        }
        auto frame = gpu_->CreateNV12Frame(out_width, out_height);
        if (frame != nullptr &&
            gpu_->Blit(input, 0, nullptr, frame->texture(), 0, nullptr,
                       /*letterbox=*/false)) {
          if (last_ == nullptr || last_->width() != out_width ||
              last_->height() != out_height) {
            RTC_LOG(LS_INFO) << "GPU-capture: " << input_width_ << "x"
                             << input_height_ << " -> " << out_width << "x"
                             << out_height;
          }
          last_ = frame;
        }
        // frame == nullptr: все текстуры пула заняты энкодером — кадр
        // пропускаем, повторим предыдущий.
      }
      dirty_ = false;
    }
    if (last_ != nullptr) callback_(last_, webrtc::TimeMicros());
  }

  void ReadCursorShape(const DXGI_OUTDUPL_FRAME_INFO& info) {
    std::vector<uint8_t> raw(info.PointerShapeBufferSize);
    UINT required = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape = {};
    if (FAILED(duplication_->GetFramePointerShape(
            static_cast<UINT>(raw.size()), raw.data(), &required, &shape))) {
      return;
    }

    // Форма приводится к BGRA с альфой. Инверсию экрана (XOR) честно не
    // нарисовать без чтения кадра, поэтому инвертирующие пиксели считаем
    // белыми — так делают и другие захватчики на видеокарте.
    UINT width = shape.Width;
    UINT height = shape.Height;
    std::vector<uint32_t> pixels;
    switch (shape.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        pixels.resize(static_cast<size_t>(width) * height);
        for (UINT y = 0; y < height; ++y) {
          std::memcpy(&pixels[static_cast<size_t>(y) * width],
                      raw.data() + static_cast<size_t>(y) * shape.Pitch,
                      static_cast<size_t>(width) * 4);
        }
        break;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
        height /= 2;
        pixels.assign(static_cast<size_t>(width) * height, 0);
        for (UINT y = 0; y < height; ++y) {
          const uint8_t* and_row = raw.data() + static_cast<size_t>(y) * shape.Pitch;
          const uint8_t* xor_row =
              raw.data() + static_cast<size_t>(y + height) * shape.Pitch;
          for (UINT x = 0; x < width; ++x) {
            const uint8_t bit = 0x80 >> (x % 8);
            const bool and_bit = (and_row[x / 8] & bit) != 0;
            const bool xor_bit = (xor_row[x / 8] & bit) != 0;
            uint32_t color = 0;
            if (!and_bit) {
              color = xor_bit ? 0xFFFFFFFFu : 0xFF000000u;
            } else if (xor_bit) {
              color = 0xFFFFFFFFu;
            }
            pixels[static_cast<size_t>(y) * width + x] = color;
          }
        }
        break;
      }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        pixels.resize(static_cast<size_t>(width) * height);
        for (UINT y = 0; y < height; ++y) {
          const auto* row = reinterpret_cast<const uint32_t*>(
              raw.data() + static_cast<size_t>(y) * shape.Pitch);
          for (UINT x = 0; x < width; ++x) {
            const uint32_t value = row[x];
            // Старший байт 0xFF — пиксель инвертирует экран; остальные
            // непрозрачны.
            pixels[static_cast<size_t>(y) * width + x] =
                (value >> 24) == 0xFF ? 0xFFFFFFFFu : (value | 0xFF000000u);
          }
        }
        break;
      default:
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = pixels.data();
    data.SysMemPitch = width * 4;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(gpu_->device()->CreateTexture2D(&desc, &data, &texture))) return;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(gpu_->device()->CreateShaderResourceView(texture.Get(), nullptr,
                                                        &view))) {
      return;
    }
    cursor_view_ = view;
    cursor_width_ = static_cast<int>(width);
    cursor_height_ = static_cast<int>(height);
  }

  bool EnsureCursorPipeline() {
    if (cursor_ps_ != nullptr && cursor_sampler_ != nullptr &&
        cursor_blend_ != nullptr) {
      return true;
    }
    if (cursor_pipeline_failed_) return false;
    D3DCompileFn compile = LoadD3DCompile();
    if (compile == nullptr) {
      cursor_pipeline_failed_ = true;
      RTC_LOG(LS_WARNING) << "GPU-capture: нет d3dcompiler_47 — курсор без отрисовки";
      return false;
    }
    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> ps;
    ComPtr<ID3DBlob> errors;
    if (FAILED(compile(kCursorShader, sizeof(kCursorShader) - 1, nullptr,
                       nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vs, &errors)) ||
        FAILED(compile(kCursorShader, sizeof(kCursorShader) - 1, nullptr,
                       nullptr, nullptr, "PS", "ps_4_0", 0, 0, &ps, &errors))) {
      cursor_pipeline_failed_ = true;
      RTC_LOG(LS_WARNING) << "GPU-capture: шейдер курсора не собрался";
      return false;
    }
    ID3D11Device* device = gpu_->device();
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(),
                                          vs->GetBufferSize(), nullptr,
                                          &cursor_vs_)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(),
                                         ps->GetBufferSize(), nullptr,
                                         &cursor_ps_))) {
      cursor_pipeline_failed_ = true;
      return false;
    }

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &cursor_cb_))) {
      cursor_pipeline_failed_ = true;
      return false;
    }

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sampler, &cursor_sampler_);

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &cursor_blend_);
    if (cursor_sampler_ == nullptr || cursor_blend_ == nullptr) {
      cursor_pipeline_failed_ = true;
      return false;
    }
    return true;
  }

  void DrawCursor() {
    if (cursor_view_ == nullptr || !EnsureCursorPipeline()) return;
    if (composed_rtv_ == nullptr &&
        FAILED(gpu_->device()->CreateRenderTargetView(composed_.Get(), nullptr,
                                                      &composed_rtv_))) {
      return;
    }

    // Position из метаданных — уже левый верхний угол формы, hot spot при рисовании
    // не учитывается (так же делает сэмпл DesktopDuplication).
    const float left = static_cast<float>(cursor_x_);
    const float top = static_cast<float>(cursor_y_);
    const float w = static_cast<float>(input_width_);
    const float h = static_cast<float>(input_height_);
    const float rect[4] = {left / w * 2.0f - 1.0f, 1.0f - top / h * 2.0f,
                           cursor_width_ / w * 2.0f, cursor_height_ / h * 2.0f};

    webrtc::MutexLock lock(&gpu_->lock());
    ID3D11DeviceContext* ctx = gpu_->context();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(cursor_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                        &mapped))) {
      return;
    }
    std::memcpy(mapped.pData, rect, sizeof(rect));
    ctx->Unmap(cursor_cb_.Get(), 0);

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, w, h, 0.0f, 1.0f};
    ID3D11RenderTargetView* rtv = composed_rtv_.Get();
    ID3D11ShaderResourceView* srv = cursor_view_.Get();
    ID3D11SamplerState* sampler = cursor_sampler_.Get();
    ID3D11Buffer* cb = cursor_cb_.Get();
    const float blend_factor[4] = {0, 0, 0, 0};

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(cursor_vs_.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShader(cursor_ps_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->OMSetBlendState(cursor_blend_.Get(), blend_factor, 0xFFFFFFFF);
    ctx->Draw(4, 0);

    // Не оставляем состояние: видеопроцессор и чужие проходы делят контекст.
    ID3D11RenderTargetView* no_rtv = nullptr;
    ID3D11ShaderResourceView* no_srv = nullptr;
    ctx->OMSetRenderTargets(1, &no_rtv, nullptr);
    ctx->PSSetShaderResources(0, 1, &no_srv);
    ctx->OMSetBlendState(nullptr, blend_factor, 0xFFFFFFFF);
  }

  const bool show_cursor_;
  int64_t screen_index_ = 0;
  int64_t period_us_ = 16667;

  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<IDXGIOutput> output_;
  webrtc::scoped_refptr<GpuContext> gpu_;
  ComPtr<IDXGIOutputDuplication> duplication_;

  // Копия рабочего стола в BGRA и её вариант с нарисованным курсором (вход
  // видеопроцессора).
  ComPtr<ID3D11Texture2D> source_;
  ComPtr<ID3D11Texture2D> composed_;
  ComPtr<ID3D11RenderTargetView> composed_rtv_;
  UINT input_width_ = 0;
  UINT input_height_ = 0;
  // Есть что собирать к дедлайну: новая картинка стола или движение курсора.
  bool dirty_ = false;
  webrtc::scoped_refptr<D3D11FrameBuffer> last_;

  std::atomic<int> target_width_{0};
  std::atomic<int> target_height_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
  FrameCallback callback_;

  // Курсор: положение из метаданных кадра, форма — текстура с альфой.
  bool cursor_visible_ = false;
  int cursor_x_ = 0;
  int cursor_y_ = 0;
  int cursor_width_ = 0;
  int cursor_height_ = 0;
  ComPtr<ID3D11ShaderResourceView> cursor_view_;
  ComPtr<ID3D11VertexShader> cursor_vs_;
  ComPtr<ID3D11PixelShader> cursor_ps_;
  ComPtr<ID3D11Buffer> cursor_cb_;
  ComPtr<ID3D11SamplerState> cursor_sampler_;
  ComPtr<ID3D11BlendState> cursor_blend_;
  bool cursor_pipeline_failed_ = false;
};

GpuScreenCapturer::GpuScreenCapturer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GpuScreenCapturer::~GpuScreenCapturer() = default;

std::unique_ptr<GpuScreenCapturer> GpuScreenCapturer::Create(
    int64_t screen_index, bool show_cursor) {
  auto impl = std::make_unique<Impl>(show_cursor);
  if (!impl->Init(screen_index)) {
    RTC_LOG(LS_INFO) << "GPU-capture: недоступен, откат на путь webrtc";
    return nullptr;
  }
  RTC_LOG(LS_INFO) << "GPU-capture: Desktop Duplication, кадр остаётся в "
                      "видеопамяти";
  return std::unique_ptr<GpuScreenCapturer>(
      new GpuScreenCapturer(std::move(impl)));
}

void GpuScreenCapturer::SetOutputSize(int width, int height) {
  impl_->SetOutputSize(width, height);
}

bool GpuScreenCapturer::Start(uint32_t fps, FrameCallback callback) {
  return impl_->Start(fps, std::move(callback));
}

void GpuScreenCapturer::Stop() {
  impl_->Stop();
}

bool GpuScreenCapturer::IsRunning() const {
  return impl_->IsRunning();
}

}  // namespace squeak

#endif  // WEBRTC_WIN
