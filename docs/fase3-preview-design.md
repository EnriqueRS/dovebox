# DoveBox Panel — Design System & Preview Spec (Fase 3)

Fuente de verdad visual: `http://192.168.100.73:18100/preview` (simulador 480×480).
Archivo: `/docker/dovebox-aggregator/web/preview.html` (CSS + JS todo inline).

## Design tokens (aprobados por Quique, 2026-08-06)

- **Fondo pantalla**: negro puro `#000000` — AMOLED a tope (píxeles apagados no emiten nada, ahorra batería).
- **Acento primario (cian hielo)**: `#4dd7ff` — botones, puntos activos, glow.
- **Acento secundario (azul)**: `#2f6bff` — gradientes con el cian (`linear-gradient(90deg, #2f6bff, #4dd7ff)`).
- **Paneles/cards**: `#0e1116` → `#131722` (gradiente vertical), borde `#181d27`.
- **Texto**: `#e8ecf1` (principal), `#7c8798` (dim), `#4a5260` (faint).
- **Estados**: mint `#4ade80` (OK/done), ámbar `#fbbf24`, rojo `#f87171` (error/calor), sky `#38bdf8` (humedad).
- **Tipografía**: Inter (UI), **JetBrains Mono** para datos (temperaturas, hora, batería) — mono para números es clave del estilo.
- **Radius**: 18px cards, 36px pantalla, 56px marco.
- **Fuente de noticias → color del punto**: marca `#ff5252`, besoccer `#ff9f43`, mundodeportivo `#5b8cff`, elespanol `#ffd166`, xataka `#9b5cff`.

## Reglas de diseño de Quique (NO violar)

1. **Nada de emoticonos/emojis** en la UI. Reemplazos:
   - Paginación → **puntos** (`width:6px` círculo, activo se estira a `22px` barrita con gradiente cian + glow).
   - Icono de clima → punto con glow que cambia de color: `hot` (naranja→rojo) / `cold` (azul) / default (cian).
   - Empty states → punto pulsante (animation pulse).
2. **Swipe para cambiar de pantalla** (touch + ratón drag, umbral 50px). Los puntos son indicadores, no el mecanismo principal.
3. **Fondo negro puro** siempre que sea pantalla de dispositivo.
4. **Noticias en round-robin** — intercalar 1 item de cada fuente en orden fijo (`marca, besoccer, mundodeportivo, elespanol, xataka`) para que nunca haya 2 titulares del mismo medio seguidos. NO agrupar por proveedor, NO shuffle aleatorio puro.

## Las 5 vistas (orden de swipe)

| # | id | Contenido |
|---|---|---|
| 1 | `face` | Dos ojos animados (`.eye` + `.pupil`): parpadeo aleatorio 2.5-6s, pupilas siguen puntero/touch (rango ±12px), mirada ociosa deambula, click → expresión feliz 700ms. Etiqueta "DoveBox · escuchando…" + hint swipe. |
| 2 | `clock` | Hora **74px mono en formato 24h** (`15:41`, medianoche = `00:31` — `String(getHours()).padStart(2,"0")`, **SIN AM/PM y SIN segundos** — correcciones de Quique; el AM/PM inline cian se eliminó por completo). Anillo SVG r=158 (dasharray 992.7) con **ciclo armónico de 2 min**: minuto **impar = llenar** (`strokeDashoffset = CIRC*(1-frac)`, sin transform), minuto **par = vaciar** (`strokeDashoffset = CIRC*frac` POSITIVO + `transform="rotate(frac*360 165 165)"`) — SIEMPRE en sentido horario, sin saltos al cambiar de minuto. ⚠️ **Pitfall paridad**: `minuto % 2 === 0` para llenar es el bug que Quique detectó — la condición correcta es `% 2 === 1` (impar llena, par vacía). ⚠️ **Pitfall centrado día**: `.clock-sub` es `display:flex` y necesita `justify-content: center` (solo `align-items` no centra horizontalmente; el día quedaba pegado a la izquierda). Fecha "6 **agosto** 2026" (mes en bold), **día de la semana centrado debajo SOLO** (sin etiqueta "DoveBox"). Refresca cada 100ms. |
| 3 | `home` | Tarjetas clima: icono punto + nombre + temperatura (mono 21px) + humedad + barra de humedad (`hum-bar`, gradiente cian, color DENTRO de la barra). |
| 4 | `todo` | Input + lista; check cuadrado → done (mint + tachado); hover revela botón ✕ (usar ✕ texto, no emoji). |
| 5 | `news` | Items con etiqueta fuente (`src` con punto de color antes del nombre) + titular. Round-robin. |

## API del agregador (contrato para el firmware)

- `GET /api/dashboard` → `{todos: [{text, done, created}], home: {rooms: [{name, temperature:{value,unit}, humidity:{value,unit}}], updated}, news: {feeds: {marca: [{title, link}], ...}, updated}, updated}`
- `GET /api/todos`, `POST /api/todos {"text"}`, `PATCH /api/todos/{idx} {"done":bool}`, `DELETE /api/todos/{idx}`
- Cache 5 min en el servidor; el preview refresca cada 60s.

## Notas para la Pieza B (firmware LVGL)

- El firmware ya usa LVGL + pantalla táctil 480×480; el patrón para UI custom es override de `SetupUI()` (referencia: `main/boards/spotpear/sp-esp32-s3-1.28-box/`, `movecall/moji-esp32s3/`).
- En el ESP32 no hay Google Fonts ni CSS: los tokens de color y la estructura de vistas son lo que se traslada; tipografía = fuentes LVGL embebidas (mono para números si se embebe JetBrains Mono subset).
- Los ojos animados en LVGL: lv_anim + lv_canvas o imágenes; el parpadeo se puede hacer con lv_anim scaleY. Las pupilas "siguen" al touch con lv_indev_get_point y transformando la posición dentro del ojo.
- Reloj: lv_timer para refrescar; anillo = lv_arc con `set_value` para el progreso del minuto.
- **Técnica del ciclo llenar/vaciar armónico (IMPORTANTE — versión corregida 2026-08-06)**: llenado = `offset = CIRC*(1-frac)` (el arco crece en horario desde las 12h); **vaciado = `offset = CIRC*frac` POSITIVO + `transform="rotate(frac*360 165 165)"`** — la rotación desplaza el hueco para que crezca en horario desde las 12h sin usar jamás un dashoffset negativo. ⚠️ **NO usar `-CIRC*frac`**: con `stroke-linecap: round` los dashoffset negativos se renderizan mal en Chrome/WebKit (el anillo salta/"se reinicia"). ⚠️ **NO usar `transition: stroke-dashoffset` en el CSS**: al cambiar de fase el offset salta de −992 a +992 (ambos = anillo vacío visualmente) y la transición anima el NÚMERO pasando por 0 = anillo lleno en medio segundo = artefacto de "reinicio". El JS ya interpola cada 100ms, la transición CSS sobra. En LVGL lv_arc no hay dashoffset — implementar el vaciado como `set_range` invertido o con dos arcos (uno que se acorta por el extremo opuesto).
- **Deep-links del preview**: `/preview#face|clock|home|todo|news` — el preview escucha `hashchange` y hace `switchView`; útil para capturas headless de una vista concreta (Chrome headless + `--virtual-time-budget`).
- **Verificación por DOM cuando la visión auxiliar da 401**: en lugar de screenshot + vision model, abrir el preview en browser_navigate y evaluar con browser_console: `getBoundingClientRect()` del elemento vs. su contenedor para comprobar centrado (diff == 0 ⇒ centrado), `getComputedStyle` para display/justify-content, y `ring.style.strokeDashoffset` en vivo para confirmar la fase del anillo (**llenando = offset positivo SIN transform; vaciando = offset positivo CON `transform="rotate(...)"`** — ya no hay negativos). Para validar el ciclo llenar/vaciar: esperar al cambio de minuto (`sleep 45-55` en terminal) y releer — el offset debe seguir siendo positivo pero la rotación debe aparecer/desaparecer según la paridad. También comprobar `getComputedStyle(ring).transitionDuration === '0s'` (sin transición CSS).
- **Pitfall caché del preview**: `BaseHTTPRequestHandler` no envía cabeceras de caché → el navegador de Quique sirve la versión vieja del HTML aunque el archivo ya esté corregido (¡el "se sigue reiniciando" persistía por eso!). Fix en `_serve_preview()`: añadir `Cache-Control: no-store, no-cache, must-revalidate` + `Pragma: no-cache` y reiniciar el contenedor. Tras un fix, pedir a Quique **Ctrl+Shift+R** (recarga forzada).
