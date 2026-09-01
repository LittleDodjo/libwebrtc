/*
 *  Squeak: захват экрана без обратного чтения BGRA (Windows).
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#include "src/win/gpu_screen_capturer.h"

#if defined(WEBRTC_WIN)

#include <cstring>
#include <utility>

#include "rtc_base/logging.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace squeak {
namespace {

using Microsoft::WRL::ComPtr;

// NV12 хранит цветность вдвое реже яркости, поэтому обе стороны обязаны быть
// чётными — иначе половина плоскости UV окажется за краем.
int MakeEven(UINT value) {
  return static_cast<int>(value & ~1u);
}

}  // namespace

class GpuScreenCapturer::Impl {
 public:
  bool Init(int64_t screen_index) {
    screen_index_ = screen_index < 0 ? 0 : screen_index;
    if (!CreateDevice()) return false;
    return OpenDuplication();
  }

  webrtc::scoped_refptr<webrtc::NV12Buffer> Capture() {
    if (duplication_ == nullptr && !OpenDuplication()) return last_;

    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return last_;
    if (FAILED(hr)) {
      // ACCESS_LOST прилетает на смене режима, UAC-затемнении и переключении
      // полноэкранного приложения — дубликацию надо просто пересоздать.
      RTC_LOG(LS_WARNING) << "GPU-capture: AcquireNextFrame hr=" << hr;
      duplication_.Reset();
      return last_;
    }

    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource.As(&texture))) Convert(texture);
    duplication_->ReleaseFrame();
    return last_;
  }

 private:
  bool CreateDevice() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    // Устройство обязано жить на том же адаптере, что и захватываемый выход,
    // иначе DuplicateOutput вернёт E_INVALIDARG.
    UINT adapter_index = 0;
    ComPtr<IDXGIAdapter1> adapter;
    while (factory->EnumAdapters1(adapter_index, &adapter) != DXGI_ERROR_NOT_FOUND) {
      UINT output_index = 0;
      ComPtr<IDXGIOutput> output;
      while (adapter->EnumOutputs(output_index, &output) != DXGI_ERROR_NOT_FOUND) {
        if (static_cast<int64_t>(seen_outputs_) == screen_index_) {
          adapter_ = adapter;
          output_ = output;
        }
        ++seen_outputs_;
        ++output_index;
        output.Reset();
      }
      ++adapter_index;
      adapter.Reset();
    }
    if (adapter_ == nullptr || output_ == nullptr) return false;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1};
    const HRESULT hr = D3D11CreateDevice(
        adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr,
        &context_);
    if (FAILED(hr)) return false;

    return SUCCEEDED(device_.As(&video_device_)) &&
           SUCCEEDED(context_.As(&video_context_));
  }

  bool OpenDuplication() {
    if (output_ == nullptr || device_ == nullptr) return false;
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output_.As(&output1))) return false;
    const HRESULT hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
      duplication_.Reset();
      return false;
    }
    return true;
  }

  // Пересобираем конвейер, когда сменилось разрешение экрана.
  bool EnsureProcessor(UINT width, UINT height) {
    const int even_width = MakeEven(width);
    const int even_height = MakeEven(height);
    if (even_width <= 0 || even_height <= 0) return false;
    if (processor_ != nullptr && even_width == width_ && even_height == height_) {
      return true;
    }

    width_ = even_width;
    height_ = even_height;
    processor_.Reset();
    enumerator_.Reset();
    output_view_.Reset();
    source_.Reset();
    nv12_.Reset();
    staging_.Reset();
    last_ = nullptr;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputWidth = static_cast<UINT>(width_);
    content.OutputHeight = static_cast<UINT>(height_);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content,
                                                             &enumerator_))) {
      return false;
    }
    if (FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0,
                                                   &processor_))) {
      return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &source_))) return false;

    desc = {};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &nv12_))) return false;

    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view = {};
    view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    return SUCCEEDED(video_device_->CreateVideoProcessorOutputView(
        nv12_.Get(), enumerator_.Get(), &view, &output_view_));
  }

  void Convert(const ComPtr<ID3D11Texture2D>& texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (!EnsureProcessor(desc.Width, desc.Height)) return;

    // Текстуру, пришедшую из Desktop Duplication, часть драйверов отказывается
    // принимать во входное представление видеопроцессора — она создана не с
    // теми bind-флагами. Копия внутри видеопамяти стоит копейки.
    context_->CopyResource(source_.Get(), texture.Get());

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view = {};
    view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            source_.Get(), enumerator_.Get(), &view, &input_view))) {
      return;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    // Один блит делает и переход BGRA -> NV12, и масштаб, если разрешение
    // выхода отличается от входа.
    if (FAILED(video_context_->VideoProcessorBlt(processor_.Get(),
                                                 output_view_.Get(), 0, 1,
                                                 &stream))) {
      return;
    }

    context_->CopyResource(staging_.Get(), nv12_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return;
    }

    auto buffer = webrtc::NV12Buffer::Create(width_, height_);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst_y = buffer->MutableDataY();
    for (int row = 0; row < height_; ++row) {
      std::memcpy(dst_y + row * buffer->StrideY(), src + row * mapped.RowPitch,
                  width_);
    }
    // Плоскость UV лежит сразу за яркостной, её высота вдвое меньше, а ширина
    // в байтах та же: пара U и V на два пикселя по горизонтали. Отступ
    // считается по высоте staging-текстуры (height_), а НЕ источника: они
    // расходятся, когда сторона экрана нечётная.
    const uint8_t* src_uv = src + mapped.RowPitch * height_;
    uint8_t* dst_uv = buffer->MutableDataUV();
    for (int row = 0; row < height_ / 2; ++row) {
      std::memcpy(dst_uv + row * buffer->StrideUV(),
                  src_uv + row * mapped.RowPitch, width_);
    }
    context_->Unmap(staging_.Get(), 0);

    last_ = buffer;
  }

  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<IDXGIOutput> output_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  ComPtr<ID3D11VideoProcessorOutputView> output_view_;
  ComPtr<ID3D11Texture2D> source_;
  ComPtr<ID3D11Texture2D> nv12_;
  ComPtr<ID3D11Texture2D> staging_;

  webrtc::scoped_refptr<webrtc::NV12Buffer> last_;

  int64_t screen_index_ = 0;
  UINT seen_outputs_ = 0;
  int width_ = 0;
  int height_ = 0;
};

GpuScreenCapturer::GpuScreenCapturer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GpuScreenCapturer::~GpuScreenCapturer() = default;

std::unique_ptr<GpuScreenCapturer> GpuScreenCapturer::Create(
    int64_t screen_index) {
  auto impl = std::make_unique<Impl>();
  if (!impl->Init(screen_index)) {
    RTC_LOG(LS_INFO) << "GPU-capture: недоступен, откат на путь webrtc";
    return nullptr;
  }
  RTC_LOG(LS_INFO) << "GPU-capture: Desktop Duplication + NV12 на выходе";
  return std::unique_ptr<GpuScreenCapturer>(
      new GpuScreenCapturer(std::move(impl)));
}

webrtc::scoped_refptr<webrtc::NV12Buffer> GpuScreenCapturer::Capture() {
  return impl_->Capture();
}

}  // namespace squeak

#endif  // WEBRTC_WIN
