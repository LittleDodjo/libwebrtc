#ifndef LIB_WEBRTC_RTC_VIDEO_FRAME_HXX
#define LIB_WEBRTC_RTC_VIDEO_FRAME_HXX

#include "rtc_types.h"

namespace libwebrtc {

class RTCVideoFrame : public RefCountInterface {
 public:
  enum class Type { kARGB, kBGRA, kABGR, kRGBA };

  enum VideoRotation {
    kVideoRotation_0 = 0,
    kVideoRotation_90 = 90,
    kVideoRotation_180 = 180,
    kVideoRotation_270 = 270
  };

 public:
  LIB_WEBRTC_API static scoped_refptr<RTCVideoFrame> Create(
      int width, int height, const uint8_t* buffer, int length);

  LIB_WEBRTC_API static scoped_refptr<RTCVideoFrame> Create(
      int width, int height, const uint8_t* data_y, int stride_y,
      const uint8_t* data_u, int stride_u, const uint8_t* data_v, int stride_v);

  virtual scoped_refptr<RTCVideoFrame> Copy() = 0;

  // The resolution of the frame in pixels. For formats where some planes are
  // subsampled, this is the highest-resolution plane.
  virtual int width() const = 0;
  virtual int height() const = 0;

  virtual VideoRotation rotation() = 0;

  // Returns pointer to the pixel data for a given plane. The memory is owned by
  // the VideoFrameBuffer object and must not be freed by the caller.
  virtual const uint8_t* DataY() const = 0;
  virtual const uint8_t* DataU() const = 0;
  virtual const uint8_t* DataV() const = 0;

  // Returns the number of bytes between successive rows for a given plane.
  virtual int StrideY() const = 0;
  virtual int StrideU() const = 0;
  virtual int StrideV() const = 0;

  virtual int ConvertToARGB(Type type, uint8_t* dst_argb, int dst_stride_argb,
                            int dest_width, int dest_height) = 0;

  /**
   * Windows: кадр живёт в видеопамяти. ID3D11Texture2D* (NV12, без AddRef) на
   * устройстве LibWebRTC::GetD3D11Device(); nullptr для кадра в системной
   * памяти и вне Windows. Текстура живёт, пока жив кадр.
   */
  virtual void* d3d11_texture() const { return nullptr; }

  /**
   * Рисует кадр в текстуру D3D11 (BGRA/RGBA render target на устройстве
   * LibWebRTC::GetD3D11Device(), например back buffer swap chain или общая
   * текстура для Flutter). Перевод цвета и масштаб делает видеокарта; кадр
   * вписывается в прямоугольник (x, y, width, height) с сохранением пропорций,
   * поля внутри него чёрные; width/height <= 0 — вся текстура. Поворот
   * кадра не применяется. Кадр из системной памяти предварительно
   * заливается. false — не смогли (чужое устройство, нет D3D11): вызывающий
   * рисует через ConvertToARGB.
   */
  virtual bool RenderToD3D11Texture(void* texture, int x, int y, int width,
                                    int height) {
    return false;
  }

 protected:
  virtual ~RTCVideoFrame() {}
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_VIDEO_FRAME_HXX
