# libwebrtc — форк C++-обёртки WebRTC для десктопа Squeak

Общие правила — в `~/.claude/CLAUDE.md`. Здесь только своё для этого репо.
Карточки **SQC**. `origin` — LittleDodjo/libwebrtc, `upstream` — webrtc-sdk/libwebrtc-wrapper.

## Цепочка

```
webrtc-sdk/webrtc@m144_release (ядро, build/.gclient)  ← не патчим, только patches/*.patch апстрима
  → этот репо, ветка squeak-nvenc → libwebrtc.dll / libwebrtc.so в релизе по тегу
    → flutter-webrtc squeak-nvenc-render: third_party/libwebrtc_version.ini (версия + sha256)
      → клиент Squeak (dependency_overrides на форк плагина)
```

Наш форк ядра `~/projects/webrtc` в сборке **не участвует**: CI клонирует `webrtc-sdk/webrtc`.

## Ветки и версия

- **`squeak-nvenc` — рабочая.** Цель — **m144** (`build/.gclient` → `m144_release`,
  `patches/custom_audio_source_m144.patch`). README говорит то же, он апстримный.
- `squeak-m144` — ранняя ступень, влита в `squeak-nvenc`. Не трогать.
- m150 есть только в апстриме (`upstream/main`, теги `libwebrtc.m150.*` — не наши). Переход на m150 —
  отдельная карточка вместе с flutter-webrtc и `livekit_client` в клиенте.

## Своё в форке

- `src/win/`: `nvenc_video_encoder`, `amf_video_encoder`, MF-декодер, `gpu_context` (общий D3D11),
  `gpu_screen_capturer` (Desktop Duplication + VideoProcessor → NV12), `squeak_video_encoder_factory`.
- Порядок энкодеров NVENC → AMF → MF → OpenH264 (`LibWebRTC::SetHardwareEncoderOrder`).
- Звук: AEC3 вместо DMO на Windows, APM 48 кГц, `rtc_audio_processing_sandbox_impl` для тестов.
- Публичный API — `include/`. Меняешь его → правка C++ в flutter-webrtc в той же карточке.
- Стиль — `.clang-format` (Google). Апстримные файлы без нужды не переформатировать.

## CI дорогой

- `.github/workflows/webrtc-builds.yml` стартует на **любой** push (кроме `**.md`, LICENSE, NOTICE,
  .gitignore) и на любой тег: gclient sync + сборка Windows и Linux x64/arm64, десятки минут.
- Коммитить локально пачкой, пушить готовое и проверенное, не по коммиту.
- Проверочный прогон без релиза — `workflow_dispatch` с `x64_only` (по умолчанию) и sccache; запускать
  только по просьбе или когда без CI не проверить.
- Локально `gclient sync` и сборку движка **не запускать** — нет места и времени. Проверка — CI.

## Релиз движка

1. Тег `libwebrtc.m144.7559.09-nvenc.<N+1>` (последний — `git tag --sort=-creatordate | head`).
2. Job `create-release` делает только `gh release upload` в релиз этого тега — релиз должен существовать.
3. Хеши — из `*.zip.shasum` релиза → в `flutter-webrtc/third_party/libwebrtc_version.ini`.
4. Тег ставить только когда изменения проверены и пользователь хочет новый движок в клиенте.

## Коммиты

Как в истории: `fix(win): …`, `feat(audio): …`, `feat(pc): …`, `ci: …` — описание по-русски, в конце `(SQC-N)`.
Апстримные коммиты не переписывать.
