#include "libwebrtc.h"

#include "api/scoped_refptr.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "rtc_peerconnection_factory_impl.h"
#if defined(SQUEAK_USE_NVENC)
#include "src/win/gpu_context.h"
#include "src/win/squeak_video_encoder_factory.h"
#endif

namespace libwebrtc {

// Initialize static variable g_is_initialized to false.
static bool g_is_initialized = false;

// Initializes SSL, if not initialized.
bool LibWebRTC::Initialize() {
  if (!g_is_initialized) {
    webrtc::InitializeSSL();
    g_is_initialized = true;
  }
  return g_is_initialized;
}

// Stops and cleans up the threads and SSL.
void LibWebRTC::Terminate() {
  webrtc::ThreadManager::Instance()->SetCurrentThread(NULL);
  webrtc::CleanupSSL();

  // Resets the static variable g_is_initialized to false.
  g_is_initialized = false;
}

// Creates and returns an instance of RTCPeerConnectionFactory.
scoped_refptr<RTCPeerConnectionFactory>
LibWebRTC::CreateRTCPeerConnectionFactory() {
  scoped_refptr<RTCPeerConnectionFactory> rtc_peerconnection_factory =
      scoped_refptr<RTCPeerConnectionFactory>(
          new RefCountedObject<RTCPeerConnectionFactoryImpl>());
  return rtc_peerconnection_factory;
}

void LibWebRTC::SetHardwareEncoderOrder(const string& csv) {
#if defined(SQUEAK_USE_NVENC)
  squeak::SetHardwareEncoderOrder(to_std_string(csv));
#else
  (void)csv;
#endif
}

void* LibWebRTC::GetD3D11Device() {
#if defined(SQUEAK_USE_NVENC)
  auto gpu = squeak::GpuContext::Shared();
  return gpu != nullptr ? gpu->device() : nullptr;
#else
  return nullptr;
#endif
}

}  // namespace libwebrtc
