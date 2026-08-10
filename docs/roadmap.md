# DoveBox Roadmap — v2 (2026-08-10)

Roadmap de las ideas de evolución de DoveBox, priorizadas por impacto en el día a día.
Basado en el listado de ideas discutido el 2026-08-09. El **top 3** es la prioridad actual.

## Top 3 — prioridad actual (orden de ejecución)

| # | Idea | Esfuerzo | Impacto | Estado |
|---|------|----------|---------|--------|
| 1 | **Puente MCP → Hermes API por voz** (Fase 4 original) | Medio | ⭐⭐⭐ | 🔨 EN CURSO |
| 2 | **Wake word en español** ("Oye Dove") | Alto | ⭐⭐⭐ | ⏳ Pendiente |
| 3 | **Briefing matutino** hablado | Bajo | ⭐⭐ | ⏳ Pendiente |

---

### 1. Puente MCP → Hermes API por voz 🎯 (EN CURSO)

**Objetivo**: que DoveBox pueda *hacer cosas* vía Hermes por voz: crear crons, consultar datos reales, controlar Home Assistant, lanzar tareas de fondo.

**Ejemplos de uso**:
- "DoveBox, pon una alarma a las 8" → crea cron en Hermes
- "DoveBox, ¿qué tiempo hace mañana?" → responde con datos reales
- "DoveBox, apaga las luces del salón" → control HA

**Mecanismo**: el xiaozhi-server ya soporta **server-side MCP** (`data/.mcp_server_settings.json`). Se monta un **MCP server propio** (Python, stdio) que expone tools que llaman a:
- Hermes CLI (`hermes chat -q`, `hermes cron`) para tareas de agente
- Home Assistant API directamente (mismo patrón que el agregador)
- Endpoints del agregador (`/api/dashboard`, `/api/todos`, `/api/home`)

**Definición de done**: decir "DoveBox, apaga las luces del salón" y que se apaguen; "crea un recordatorio para mañana" y que aparezca en cron de Hermes.

**Paso 1** (hecho): verificado que el server soporta MCP client (stdio y HTTP/SSE) vía `ServerMCPClient` → config en `data/.mcp_server_settings.json` con `mcpServers`.
**Paso 2**: construir `hermes_mcp_bridge.py` (MCP server stdio con tools `ha_call`, `hermes_query`, `hermes_cron`, `aggregator_todo`, `aggregator_dashboard`).
**Paso 3**: registrar en `.mcp_server_settings.json` + reiniciar server + verificar tools en el log.
**Paso 4**: probar por voz con el dispositivo.

---

### 2. Wake word en español ("Oye Dove") 🎙️

**Problema**: el wake word actual es `你好小智` (mandarín) — con acento español casi nunca salta. Es la mejora de UX más grande posible: hoy hay que pulsar BOOT/KEY siempre.

**Mecanismo**: entrenar un modelo ESP-SR (esp_sr) con wake word "Oye Dove" (o "Dove"). Requiere:
1. Dataset de muestras de audio (grabar con el propio mic del device o TTS propio).
2. Entrenar con `esp_sr` toolkit (o usar un modelo de wake word disponible).
3. Integrar en el build: `--wake-word` custom + modelo en el firmware.

**Alternativa más barata (fase previa)**: mapear el wake word del builder a un nombre ya soportado o usar `disabled` + despertar por tap/KEY, mientras se entrena el modelo propio.

**Definición de done**: decir "Oye Dove" desde ~1-2m y que el device entre en modo escucha.

---

### 3. Briefing matutino hablado ☀️

**Objetivo**: al despertar (o a hora fija), DoveBox lee en voz alta: clima + noticias + todos del día + recordatorios. Es lo más fácil de montar: **todos los datos ya están en el agregador** (`/api/dashboard` → home/clima, news, todos).

**Mecanismo**:
1. Nuevo endpoint en el agregador: `GET /api/briefing` que arma un texto corto (clima hoy, 3-5 noticias top, todos pendientes).
2. Disparo: cron en el agregador (a hora configurable, p.ej. 7:45) o al detectar el primer uso del día.
3. El server lee el briefing con EdgeTTS (ya configurado) — o un endpoint que el server llame para inyectar el mensaje hablado.

**Definición de done**: el device dice "Buenos días, hoy hace X grados... tus tareas pendientes son..." sin que Quique tenga que pedirlo.

---

## Resto de ideas (backlog, sin orden)

### Voz y conversación
- **4. Lip sync real (fase 2)**: amplitud del audio ES8311 moviendo la boca en vez de la onda pseudo-aleatoria. El mecanismo ya está preparado.
- **5. Continuidad de conversación**: ventana ~10s tras responder para repreguntar sin wake word.
- **6. Captions en vivo**: STT parcial mostrado mientras se habla.
- **7. Barge-in**: interrumpir al asistente hablando por encima (⚠️ difícil sin AEC).
- **8. GroqASR**: Whisper de Groq en vez de Vosk — mejor transcripción, sobre todo con voces infantiles.

### Cara y pantalla
- **9. Timer por voz con anillo de progreso**: "pon 10 minutos" → anillo circular que se consume + sonido al terminar.
- **10. Modo focus visible**: countdown en pantalla con arco + cara concentrada.
- **11. QR en pantalla**: cuando una tarea de fondo genere un documento/enlace.
- **12. Estados de ánimo de fondo**: cara idle que varía según hora y última interacción.
- **13. Zonas táctiles en la cara**: tocar un ojo → parpadeo/sorpresa, insistir → enfado con escalada.
- **14. Modo noche**: brillo mínimo + notificaciones silenciadas en rango horario.

### Datos y notificaciones
- **15. Telemetría proactiva**: "estás al 15%, enchúfame" cuando la batería baja.
- **16. DoveBox como hub de notificaciones**: avisos por voz de lo que ya vigila Hermes (Price Alert, seguridad NUC, mensajes importantes).
- **17. Control de HA por voz** (parte del puente Hermes, #1).

### Sistema
- **18. Console web de sesiones**: página en el agregador con transcripciones/tool calls/errores para debuggear sin cable serie.
- **19. Botón KEY (GPIO18) — Fase 2 original**: acceso directo al asistente con animación custom.
- **20. Multi-dispositivo**: segundo DoveBox con memoria/recordatorios compartidos.

---

## Notas de arquitectura para el puente

- El xiaozhi-server lee `data/.mcp_server_settings.json` con formato:
  ```json
  {
    "mcpServers": {
      "nombre": {
        "command": "/usr/bin/python3",
        "args": ["/path/to/bridge.py"],
        "env": {}
      }
    }
  }
  ```
  Soporta `command` (stdio) y `url` (SSE/streamable-http). Los tools se exponen al LLM automáticamente.
- El LLM actual es `nan/deepseek-v4-flash` vía LiteLLM (soporta tool calling).
- Los tools se refresh en cada conexión; si falla la init, el server loguea y sigue (no crashea).
