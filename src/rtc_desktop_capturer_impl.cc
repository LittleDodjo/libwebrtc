/*
 * Copyright 2022 LiveKit
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rtc_desktop_capturer_impl.h"

#include <algorithm>
#include <utility>

#include "api/sequence_checker.h"
#include "rtc_base/checks.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv.h"
#ifdef WEBRTC_WIN
#include <timeapi.h>

#include "modules/desktop_capture/win/window_capture_utils.h"
#endif

namespace libwebrtc {

enum { kCaptureDelay = 33, kCaptureMessageId = 1000 };

RTCDesktopCapturerImpl::RTCDesktopCapturerImpl(
    DesktopType type, webrtc::DesktopCapturer::SourceId source_id,
    webrtc::Thread* signaling_thread, scoped_refptr<MediaSource> source,
    bool showCursor)
    : thread_(webrtc::Thread::Create()),
      source_id_(source_id),
      signaling_thread_(signaling_thread),
      source_(source) {
  RTC_DCHECK(thread_);
  type_ = type;
  thread_->Start();
  options_ = webrtc::DesktopCaptureOptions::CreateDefault();
  options_.set_detect_updated_region(true);
#ifdef WEBRTC_WIN
  options_.set_allow_directx_capturer(true);
#endif
#ifdef WEBRTC_LINUX
  if (type == kScreen) {
    options_.set_allow_pipewire(true);
  }
#endif
#ifdef WEBRTC_WIN
  if (type == kScreen) {
    gpu_capturer_ = squeak::GpuScreenCapturer::Create(source_id, showCursor);
  }
  if (gpu_capturer_ != nullptr) return;
#endif
  thread_->BlockingCall([this, type, showCursor] {
    if (type == kScreen) {
      if (showCursor) {
        capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
            webrtc::DesktopCapturer::CreateScreenCapturer(options_), options_);
      } else {
        capturer_ = webrtc::DesktopAndCursorComposer::CreateWithoutMouseCursorMonitor(
                webrtc::DesktopCapturer::CreateScreenCapturer(options_));
      }
    } else {
      capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
          webrtc::DesktopCapturer::CreateWindowCapturer(options_), options_);
    }
  });
}

RTCDesktopCapturerImpl::~RTCDesktopCapturerImpl() {
#ifdef WEBRTC_WIN
  // Сначала останавливаем поток захвата: его callback зовёт OnFrame этого объекта.
  if (gpu_capturer_ != nullptr) gpu_capturer_->Stop();
  if (timer_period_raised_) {
    ::timeEndPeriod(1);
    timer_period_raised_ = false;
  }
#endif
  thread_->Stop();
  capturer_.reset();
#ifdef WEBRTC_WIN
  gpu_capturer_.reset();
#endif
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  if (!w_ || !h) {
    x_ = 0;
    y_ = 0;
  }
  return Start(fps);
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps) {
  if (capture_state_ == CS_RUNNING) {
    return capture_state_;
  }

  if (fps == 0) {
    capture_state_ = CS_FAILED;
    return capture_state_;
  }

  if (fps >= 60) {
    capture_delay_ = uint32_t(1000.0 / 60.0);
  } else {
    capture_delay_ = uint32_t(1000.0 / fps);
  }

#ifdef WEBRTC_WIN
  if (gpu_capturer_ != nullptr) {
    // Ритм задаёт сам захватчик в своём потоке; сюда приходят готовые кадры
    // в видеопамяти, никакой работы с пикселями на этом пути нет.
    const bool started = gpu_capturer_->Start(
        fps >= 60 ? 60 : fps,
        [this](webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer,
               int64_t timestamp_us) {
          if (capture_state_ != CS_RUNNING) return;
          OnFrame(webrtc::VideoFrame::Builder()
                      .set_video_frame_buffer(std::move(buffer))
                      .set_timestamp_us(timestamp_us)
                      .set_rotation(webrtc::kVideoRotation_0)
                      .build());
        });
    if (!started) {
      capture_state_ = CS_FAILED;
      return capture_state_;
    }
    capture_state_ = CS_RUNNING;
    if (observer_) {
      signaling_thread_->BlockingCall([&, this]() { observer_->OnStart(this); });
    }
    return capture_state_;
  }
#endif

  if (source_id_ != -1) {
    if (!capturer_->SelectSource(source_id_)) {
      capture_state_ = CS_FAILED;
      return capture_state_;
    }
    if (type_ == kWindow) {
      if (!capturer_->FocusOnSelectedSource()) {
        capture_state_ = CS_FAILED;
        return capture_state_;
      }
    }
  }

  thread_->BlockingCall([this] { capturer_->Start(this); });
  capture_state_ = CS_RUNNING;
#ifdef WEBRTC_WIN
  // Запасной путь живёт на отложенных задачах потока webrtc, а их ожидание
  // квантуется системным таймером: без кванта в 1 мс дедлайн 16 мс превращается
  // в 31.
  if (!timer_period_raised_) {
    ::timeBeginPeriod(1);
    timer_period_raised_ = true;
  }
#endif
  next_capture_us_ = 0;
  thread_->PostTask([this] { CaptureFrame(); });
  if (observer_) {
    signaling_thread_->BlockingCall([&, this]() { observer_->OnStart(this); });
  }
  return capture_state_;
}

void RTCDesktopCapturerImpl::SetOutputSize(int width, int height) {
  target_width_ = width > 0 ? width : 0;
  target_height_ = height > 0 ? height : 0;
#ifdef WEBRTC_WIN
  if (gpu_capturer_ != nullptr) gpu_capturer_->SetOutputSize(width, height);
#endif
}

void RTCDesktopCapturerImpl::Stop() {
  if (observer_) {
    if (!signaling_thread_->IsCurrent()) {
      signaling_thread_->BlockingCall([&, this]() { observer_->OnStop(this); });
    } else {
      observer_->OnStop(this);
    }
  }
  capture_state_ = CS_STOPPED;
#ifdef WEBRTC_WIN
  if (gpu_capturer_ != nullptr) gpu_capturer_->Stop();
  if (timer_period_raised_) {
    ::timeEndPeriod(1);
    timer_period_raised_ = false;
  }
#endif
}

bool RTCDesktopCapturerImpl::IsRunning() {
  return capture_state_ == CS_RUNNING;
}

#ifdef WEBRTC_WIN
int filterException(int code, PEXCEPTION_POINTERS ex) {
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void RTCDesktopCapturerImpl::OnCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) {
  if (result != result_) {
    if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnError(this); });
      }
      capture_state_ = CS_FAILED;
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnPaused(this); });
      }
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::SUCCESS) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnStart(this); });
      }
    }
  }

  if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
    return;
  }

  int width = frame->size().width();
  int height = frame->size().height();
#ifdef WEBRTC_WIN
  webrtc::DesktopRect rect_ = webrtc::DesktopRect::MakeWH(width, height);

  if (type_ != kScreen) {
    webrtc::GetWindowRect(reinterpret_cast<HWND>(source_id_), &rect_);
  }

  __try
#endif
  {
    width = w_ > 0 ? w_ : width;
    height = h_ > 0 ? h_ : height;
    if (!i420_buffer_ || !i420_buffer_.get() ||
        i420_buffer_->width() * i420_buffer_->height() != width * height) {
      i420_buffer_ = webrtc::I420Buffer::Create(width, height);
    }

    libyuv::ConvertToI420(frame->data(), 0, i420_buffer_->MutableDataY(),
                          i420_buffer_->StrideY(), i420_buffer_->MutableDataU(),
                          i420_buffer_->StrideU(), i420_buffer_->MutableDataV(),
                          i420_buffer_->StrideV(), x_, y_,
#ifdef WEBRTC_WIN
                          rect_.width(), rect_.height(),
#else
                          width, height,
#endif
                          width, height, libyuv::kRotate0, libyuv::FOURCC_ARGB);

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> out = i420_buffer_;
    if (target_width_ > 0 && target_height_ > 0 &&
        (width > target_width_ || height > target_height_)) {
      // Вписываем с сохранением пропорций; чётные стороны — требование I420.
      const double scale =
          std::min(static_cast<double>(target_width_) / width,
                   static_cast<double>(target_height_) / height);
      const int out_width = std::max(2, static_cast<int>(width * scale) & ~1);
      const int out_height =
          std::max(2, static_cast<int>(height * scale) & ~1);
      webrtc::scoped_refptr<webrtc::I420Buffer> scaled =
          scaled_pool_.CreateI420Buffer(out_width, out_height);
      if (!scaled) return;
      scaled->ScaleFrom(*i420_buffer_);
      out = scaled;
    }

    OnFrame(webrtc::VideoFrame(out, 0, webrtc::TimeMillis(),
                               webrtc::kVideoRotation_0));
  }
#ifdef WEBRTC_WIN
  __except (filterException(GetExceptionCode(), GetExceptionInformation())) {
  }
#endif
}

void RTCDesktopCapturerImpl::CaptureFrame() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (capture_state_ != CS_RUNNING) return;

  // Запасной путь webrtc (окна, не-Windows). Дедлайн держим сами: «поставить
  // следующий тик через период после текущего» на квантованном таймере Windows
  // давало период в два кванта — 30 к/с при запрошенных 60.
  const int64_t now_us = webrtc::TimeMicros();
  if (next_capture_us_ == 0) next_capture_us_ = now_us;
  capturer_->CaptureFrame();
  next_capture_us_ += static_cast<int64_t>(capture_delay_) * 1000;
  if (next_capture_us_ < now_us) next_capture_us_ = now_us;
  const int64_t wait_us = next_capture_us_ - webrtc::TimeMicros();
  thread_->PostDelayedHighPrecisionTask(
      [this]() { CaptureFrame(); },
      webrtc::TimeDelta::Micros(wait_us > 0 ? wait_us : 0));
}

}  // namespace libwebrtc
