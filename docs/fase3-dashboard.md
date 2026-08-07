# DoveBox Dashboard LVGL (Fase 3, Pieza B) — guía técnica

Estado: **flasheado 2026-08-07** (firmware 2.4.2 custom + panel).

## Archivos

| Archivo | Rol |
|---|---|
| `main/boards/waveshare/esp32-s3-touch-amoled-2.16/dovebox_dashboard.cc` | Panel LVGL completo (clase `DoveboxDashboard`) |
| `main/boards/waveshare/esp32-s3-touch-amoled-2.16/esp32-s3-touch-amoled-2.16.cc` | Board; `SetupUI()` llama a `dovebox_dashboard_create(this, lv_screen_active())` |

El GLOB de `main/CMakeLists.txt` (`file(GLOB BOARD_SOURCES .../*.cc)`) compila el .cc automáticamente — no hace falta tocar CMake.

## Arquitectura

- **5 vistas** (contenedores full-screen 480×480, fondo `#000`): `face` (ojos animados con pupilas que siguen el touch), `clock` (hora 24h escalada ×2.4, fecha en español, día centrado, anillo lv_arc armónico), `home` (tarjetas clima: punto glow + temp + humedad con barra), `todo` (lista con toggle por tap → PATCH al agregador), `news` (round-robin de 5 fuentes con punto de color).
- **Paginación**: 5 puntos en flex row abajo; el activo se estira a 22px con gradiente cian + glow (misma regla que el preview).
- **Swipe**: polling del indev desde el timer del reloj (`UpdateGestures()`) — NO eventos LVGL (los callbacks en el screen no reciben PRESSED/RELEASED de hijos sin `LV_OBJ_FLAG_EVENT_BUBBLE`). Umbral 50px.
- **Datos**: `FetchDashboardAsync()` en task FreeRTOS (stack 8192) → `FetchDashboard()` parsea cJSON y copia a vectores estáticos bajo `DisplayLockGuard`; el timer del reloj marca `g_data_dirty` y re-renderiza la vista activa.
- **Chat de voz**: `UpdateStateVisibility()` oculta el dashboard cuando `DeviceState` es Listening/Speaking/Connecting/Upgrading/WifiConfiguring/AudioTesting y lo restaura en idle.

## Build (IMPORTANTE — el builder monta el código)

La imagen `xiaozhi/firmware-builder:idf61` copia el repo en build time (`COPY . .`), así que un `docker run` sin montar el código compila la versión VIEJA. Siempre:

```bash
cd /docker/xiaozhi-esp32
docker run --rm \
  -e FIRMWARE_SOURCE_DIR=/src \
  -e FIRMWARE_BOARD_DIR=waveshare/esp32-s3-touch-amoled-2.16 \
  -e FIRMWARE_BOARD_NAME=esp32-s3-touch-amoled-2.16 \
  -e FIRMWARE_LANGUAGE=es-ES -e FIRMWARE_WAKE_WORD=nihaoxiaozhi \
  -v $PWD:/src -v $PWD/output:/output \
  xiaozhi/firmware-builder:idf61 > /tmp/build.log 2>&1
```

- Log a archivo: `tail` bufferiza toda la salida hasta el final; con `>` al archivo se puede monitorizar con `grep`.
- Tarda ~2-3 min con ccache (config 125-130s + compile rápido); primera vez 10-30 min.
- Build correcto: `[N/10] Building ... dovebox_dashboard.cc.obj` sin errores, luego `Project build complete` y `merged-binary.bin`.

## Pitfalls

### Runtime (crash + reboot loop)
- **NO hacer fetch HTTP en el constructor del dashboard**. `SetupUI()` corre antes de que lwIP/WiFi esté listo → `assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)` → reboot.
- Fix: el constructor solo crea `poll_timer_` con periodo **3000ms**; `FetchDashboard()` comprueba `app.GetDeviceState() != kDeviceStateIdle → return` (idle ⇒ red lista); tras el primer fetch OK hace `lv_timer_set_period(poll_timer_, 60000)`.
- **Fuente NULL → `InstrFetchProhibited` PC=0x0** (panic al renderizar, justo tras Got IP/activating): si `lv_obj_set_style_text_font(label, NULL, 0)` recibe NULL (el screen aún no tiene la fuente del tema en SetupUI), LVGL crashea en `lv_font_get_glyph_width` → `f->get_glyph_dsc` en dirección 0. Fix: helper `GetThemeFont()` que lee `display_->GetTheme()` (cast a `LvglTheme*` → `text_font()->font()`) con fallback a `LV_FONT_DEFAULT`; usarlo en TODOS los labels (BuildUI + renders), nunca pasar NULL.
- **Decodificar panic**: las direcciones del `Backtrace:` del boot log se resuelven con addr2line xtensa dentro del contenedor IDF (ver SKILL.md) — el ELF está en `build/xiaozhi.elf`.

### Compilación C++
1. **No declarar Y definir el mismo callback inline en la clase** — C++ lo rechaza ("cannot be overloaded"). Declarar dentro (`static void ClockTickCb(lv_timer_t* t);`), definir fuera (`void DoveboxDashboard::ClockTickCb(...)`).
2. `lv_obj_get_style_text_font(obj, 0)` → el 2º arg es `lv_part_t`, no int: usar `LV_PART_MAIN`.
3. `lv_event_get_target(e)` devuelve `void*` → `static_cast<lv_obj_t*>(...)`.
4. `lv_timer_t` es incomplete type desde `lvgl.h` → usar `lv_timer_get_user_data(t)` en vez de `t->user_data`.
5. Structs auxiliares pasados a tasks FreeRTOS (`PatchArgs`) → declarar como miembros de la clase, no locales a la función.
6. `lv_obj_set_user_data(row, (void*)(uintptr_t)i)` para guardar índices; recuperar con `lv_obj_get_user_data`.

### LVGL 9 API usada
- `lv_timer_create(cb, period_ms, user_data)` / `lv_timer_set_period`
- `lv_arc_set_bg_angles`, `lv_arc_set_value`, `lv_arc_set_range`, `lv_arc_set_rotation(270)` (start 12h)
- `lv_obj_set_style_transform_scale_x/y` + `transform_pivot` para agrandar labels (hora ×2.4, temp ×1.4)
- `lv_obj_set_style_translate_x/y` para pupilas
- `lv_indev_active()`, `lv_indev_get_point`, `lv_indev_get_state`
- Anillo armónico 2 min: minuto impar llena (`value = frac*100`), par vacía (`bg_angles(frac*360, 360)` + `value=100`) — siempre horario, sin saltos.

## HTTP (agregador)

Patrón del proyecto (sin esp_http_client directo):
```cpp
auto network = Board::GetInstance().GetNetwork();
auto http = network->CreateHttp(3);          // timeout 3s
http->SetHeader("Accept", "application/json");
http->Open("GET", "http://192.168.100.73:18100/api/dashboard");
http->GetStatusCode(); http->ReadAll(); http->Close();
```
- Todos: PATCH `http://...:18100/api/todos/{idx}` body `{"done":true|false}`.
- JSON con cJSON (`cJSON_Parse`, `cJSON_GetObjectItem`, `cJSON_ArrayForEach`).
- Round-robin de noticias implementado en firmware: recolecta por fuente y entrelaza `marca→besoccer→mundodeportivo→elespanol→xataka`.

## Verificación

- Boot log por serie (el puerto reinicia al abrir): `App version: 2.4.2`, `Got IP`, `State: ... -> idle`, sin asserts.
- El dashboard visible en la AMOLED: face (ojos) en idle, swipe a clock/home/todo/news.
- Los datos cargan cuando la red conecta (≤3s tras idle) — si el agregador está caído, las vistas muestran "sin datos".
