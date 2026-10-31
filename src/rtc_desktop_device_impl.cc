#include "rtc_desktop_device_impl.h"

#include "rtc_base/thread.h"
#include "rtc_desktop_capturer.h"
#include "rtc_desktop_media_list.h"
#include "rtc_video_device_impl.h"

namespace libwebrtc {

RTCDesktopDeviceImpl::RTCDesktopDeviceImpl(webrtc::Thread* signaling_thread)
    : signaling_thread_(signaling_thread) {}

RTCDesktopDeviceImpl::~RTCDesktopDeviceImpl() {}

scoped_refptr<RTCDesktopCapturer> RTCDesktopDeviceImpl::CreateDesktopCapturer(
    scoped_refptr<MediaSource> source, bool showCursor) {
  MediaSourceImpl* source_impl = static_cast<MediaSourceImpl*>(source.get());
#ifdef WEBRTC_WIN
  // Превью экранов держат DXGI-дубликацию монитора — без этого GPU-захват
  // откатывается на путь webrtc со стрелкой вместо скрытого курсора (SQC-293).
  if (source_impl->type() == kScreen) {
    auto list = desktop_media_lists_.find(kScreen);
    if (list != desktop_media_lists_.end()) list->second->ReleaseCapturer();
  }
#endif
  return new RefCountedObject<RTCDesktopCapturerImpl>(
      source_impl->type(), source_impl->source_id(), signaling_thread_, source,
      showCursor);
}

scoped_refptr<RTCDesktopMediaList> RTCDesktopDeviceImpl::GetDesktopMediaList(
    DesktopType type) {
  if (desktop_media_lists_.find(type) == desktop_media_lists_.end()) {
    desktop_media_lists_[type] =
        new RefCountedObject<RTCDesktopMediaListImpl>(type, signaling_thread_);
  }
  return desktop_media_lists_[type];
}

}  // namespace libwebrtc
