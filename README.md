# DoveBox

Asistente de escritorio DIY: panel táctil (LVGL) + terminal de voz para [Hermes Agent](https://hermes-agent.nousresearch.com). Todo self-hosted en la NUC, sin depender del cloud chino de Xiaozhi.

Hardware: **Waveshare ESP32-S3-Touch-AMOLED-2.16** (480×480 AMOLED, 8MB PSRAM, 16MB flash, doble micro ES7210 + altavoz ES8311, PMIC AXP2101).

## Arquitectura

```
┌─────────────┐   HTTP :18100   ┌──────────────────┐   WS :18000   ┌──────────────┐
│  ESP32-S3   │ ──────────────▶ │  Aggregator      │ ◀──────────── │ xiaozhi-server│
│  (LVGL 9)   │                 │  (Python stdlib) │               │  (LLM/ASR/TTS)│
│  5 vistas   │                 │  RSS+HA+ToDo     │               │  LiteLLM+Vosk │
└─────────────┘                 └──────────────────┘               └──────────────┘
```

- **Firmware** (`firmware/`): dashboard LVGL de 5 vistas (face, clock, home, todo, news) integrado en xiaozhi-esp32 v2.4.2 para el board Waveshare. Swipe para navegar, auto-cycle de vistas, polling al agregador cada 60s, se oculta durante el chat de voz.
- **Aggregator** (`aggregator/`): servidor HTTP Python (sin dependencias) que consolida noticias RSS (Marca, BeSoccer, Mundo Deportivo, El Español, Xataka), sensores de Home Assistant y una lista ToDo propia. Incluye simulador web del dispositivo (`/preview`).
- **Scripts** (`scripts/`): generador de logos LVGL y lector de consola serie.

## Vistas del panel

| Vista | Contenido |
|---|---|
| Face | Ojos animados que parpadean y siguen al dedo + hora |
| Clock | Hora 24h + fecha + anillo de progreso armónico (2 min) |
| Home | Clima por habitación + gráfica de temperatura 24h |
| Todo | Lista de tareas (CRUD por toque) |
| News | Titulares round-robin con logos de los proveedores |

## Repos del firmware base

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — firmware base (board: `waveshare/esp32-s3-touch-amoled-2.16`)
- [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) — servidor de voz

## Despliegue rápido

### 1. Agregador

```bash
docker compose -f aggregator/compose.yaml up -d
curl http://<nuc>:18100/health   # → {"status":"ok"}
```

### 2. Firmware

Compilar con el builder IDF (v6.1) y flashear por USB:

```bash
cd xiaozhi-esp32
docker run --rm \
  -e FIRMWARE_SOURCE_DIR=/src \
  -e FIRMWARE_BOARD_DIR=waveshare/esp32-s3-touch-amoled-2.16 \
  -e FIRMWARE_BOARD_NAME=esp32-s3-touch-amoled-2.16 \
  -e FIRMWARE_LANGUAGE=es-ES -e FIRMWARE_WAKE_WORD=nihaoxiaozhi \
  -v $PWD:/src -v $PWD/output:/output xiaozhi/firmware-builder:idf61

# flash (el firmware siembra la WiFi por defecto si el NVS está vacío)
python -m esptool --chip esp32s3 -b 460800 write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 output/merged-binary.bin
```

> La WiFi por defecto se configura en `firmware/dovebox_dashboard.cc` → `wifi_board.cc` (`CONFIG_DOVEBOX_DEFAULT_WIFI_SSID/PASSWORD`). Si el NVS no tiene credenciales, el dispositivo las siembra automáticamente al arrancar.

## Licencia

MIT (proyecto personal de Enrique Ríos).
