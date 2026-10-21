/*
 *  Squeak: общее D3D11-устройство процесса для захвата, кодеков и рендера.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source tree.
 */

#ifndef LIBWEBRTC_SRC_WIN_GPU_CONTEXT_H_
#define LIBWEBRTC_SRC_WIN_GPU_CONTEXT_H_

#if defined(WEBRTC_WIN)

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include "api/ref_count.h"
#include "api/scoped_refptr.h"
#include "api/video/nv12_buffer.h"
#include "rtc_base/synchronization/mutex.h"

namespace squeak {

class D3D11FrameBuffer;

// Одно устройство на адаптер. Пока захват, энкодер и декодер жили каждый на
// своём ID3D11Device, кадр между ними ходил через системную память: staging,
// memcpy, обратная заливка. На общем устройстве кадр остаётся текстурой, и
// каждый этап получает её по указателю.
//
// Immediate context защищён ID3D10Multithread, а последовательности команд
// (настроить видеопроцессор -> Blt) — мьютексом lock().
class GpuContext : public webrtc::RefCountInterface {
 public:
  // Устройство процесса по умолчанию: адаптер, к которому подключён основной
  // выход. На одной видеокарте это и есть «всё». nullptr без D3D11.
  static webrtc::scoped_refptr<GpuContext> Shared();

  // Устройство на конкретном адаптере. Для адаптера Shared() возвращает его же.
  static webrtc::scoped_refptr<GpuContext> ForAdapter(IDXGIAdapter1* adapter);

  // Первый адаптер указанного вендора (0x10DE NVIDIA, 0x1002 AMD, 0x8086
  // Intel). nullptr, если такого нет.
  static webrtc::scoped_refptr<GpuContext> ForVendor(UINT vendor_id);

  // Контекст, которому принадлежит устройство. nullptr для чужого устройства.
  static webrtc::scoped_refptr<GpuContext> ForDevice(ID3D11Device* device);

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }
  ID3D11VideoDevice* video_device() const { return video_device_.Get(); }
  ID3D11VideoContext* video_context() const { return video_context_.Get(); }
  IDXGIAdapter1* adapter() const { return adapter_.Get(); }
  const LUID& luid() const { return luid_; }
  UINT vendor_id() const { return vendor_id_; }
  bool IsNvidia() const { return vendor_id_ == 0x10DE; }
  bool IsAmd() const { return vendor_id_ == 0x1002; }
  bool IsIntel() const { return vendor_id_ == 0x8086; }
  bool SameAdapter(const GpuContext& other) const;

  // Сериализует работу с immediate context между потоками захвата, кодека и
  // рендера.
  webrtc::Mutex& lock() { return lock_; }

  // Кадр NV12 из пула текстур (RENDER_TARGET | SHADER_RESOURCE, MISC_SHARED).
  // nullptr, если видеопамяти не хватило.
  webrtc::scoped_refptr<D3D11FrameBuffer> CreateNV12Frame(int width,
                                                          int height);

  // Текстура общего назначения вне пула: BGRA для копии рабочего стола,
  // staging для чтения и так далее.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateTexture(
      UINT width, UINT height, DXGI_FORMAT format, UINT bind_flags,
      D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu_access = 0,
      UINT misc_flags = 0);

  // Перевод цвета и масштаб на видеокарте через ID3D11VideoProcessor:
  // BGRA -> NV12 (захват), NV12 -> NV12 (слои и адаптация), NV12 -> BGRA
  // (рендер). nullptr вместо прямоугольника — вся текстура. letterbox —
  // вписать с сохранением пропорций, поля внутри dst_rect залить чёрным;
  // иначе растянуть ровно в dst_rect. Снаружи dst_rect ничего не трогается.
  bool Blit(ID3D11Texture2D* src, UINT src_subresource,
            const RECT* src_rect, ID3D11Texture2D* dst,
            UINT dst_subresource, const RECT* dst_rect,
            bool letterbox = false);

  // Чтение NV12-текстуры в системную память. Единственный путь через шину
  // — только для откатов (софтовый кодек, превью).
  webrtc::scoped_refptr<webrtc::NV12Buffer> ReadbackNV12(
      ID3D11Texture2D* texture, UINT subresource, int width, int height);

  // Заливка NV12 из системной памяти в текстуру из пула — для рендера кадров,
  // пришедших от софтового декодера.
  webrtc::scoped_refptr<D3D11FrameBuffer> UploadNV12(
      const uint8_t* data_y, int stride_y, const uint8_t* data_uv,
      int stride_uv, int width, int height);

  // Вызывается пулом: текстура освободилась и может быть выдана снова.
  void RecycleTexture(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture);

 protected:
  GpuContext();
  ~GpuContext() override;

 private:
  struct PoolKey {
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    bool operator<(const PoolKey& other) const;
  };

  struct ProcessorKey {
    UINT in_width;
    UINT in_height;
    UINT out_width;
    UINT out_height;
    bool operator<(const ProcessorKey& other) const;
  };

  struct Processor {
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
    std::map<ID3D11Texture2D*,
             Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>>
        input_views;
    std::map<ID3D11Texture2D*,
             Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>>
        output_views;
    // Представления держат текстуры живыми, иначе адрес в ключе мог бы
    // достаться новой текстуре.
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> keepalive;
  };

  bool Init(IDXGIAdapter1* adapter);
  Processor* ProcessorFor(const ProcessorKey& key);
  ID3D11VideoProcessorInputView* InputView(Processor* processor,
                                           ID3D11Texture2D* texture,
                                           UINT subresource);
  ID3D11VideoProcessorOutputView* OutputView(Processor* processor,
                                             ID3D11Texture2D* texture,
                                             UINT subresource);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> StagingFor(UINT width, UINT height,
                                                     DXGI_FORMAT format);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> TakeFromPool(const PoolKey& key);

  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  LUID luid_ = {};
  UINT vendor_id_ = 0;

  webrtc::Mutex lock_;
  // Под lock_: процессоры, пул и staging.
  std::map<ProcessorKey, std::unique_ptr<Processor>> processors_;
  std::map<PoolKey, std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>>>
      pool_;
  size_t pool_outstanding_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  PoolKey staging_key_ = {};
};

}  // namespace squeak

#endif  // WEBRTC_WIN

#endif  // LIBWEBRTC_SRC_WIN_GPU_CONTEXT_H_
