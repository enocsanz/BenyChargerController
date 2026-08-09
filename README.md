# CargadorBenyV2 — Control Inteligente de Carga EV

Sistema de control de carga para vehículos eléctricos basado en **M5StickC Plus (ESP32)**, diseñado para gestionar un cargador **Beny** en combinación con un inversor solar **Huawei** y datos de precios eléctricos del mercado español (PVPC/ESIOS).

El objetivo principal es maximizar el autoconsumo solar, proteger la instalación eléctrica y permitir el control remoto total vía **Telegram**.

## Mejoras de Estabilidad (Última versión)

- **Pausa automática eliminada**: el cargador ignoraba la orden de STOP (modo *Plug and Charge*) y rearrancaba a plena potencia, justo lo contrario de lo buscado. El control es ahora exclusivamente por amperaje, con suelo en 6A. Ver [El mínimo de 6A es el suelo del sistema](#el-mínimo-de-6a-es-el-suelo-del-sistema).
- **Notificaciones de Telegram sin bloqueo**: se encolan y se entregan desde la tarea de Telegram, en lugar de disparar una petición HTTPS bloqueante desde `setup()` o desde la lógica de control (riesgo de reinicio por watchdog).
- **Fin del bucle de reinicio por caída de WiFi**: si el router no está disponible al arrancar, el equipo ya **no se reinicia**; continúa el arranque y reintenta desde el bucle principal. Ver [Reconexión WiFi](#reconexión-wifi).
- **Arranque tolerante a fallos de NTP**: El sistema puede arrancar y funcionar (sin hora) si el servidor NTP no responde en 10 segundos, evitando bloqueos infinitos y reinicios por Watchdog.
- **Modbus Huawei Asíncrono Robusto**: Múltiples peticiones de lectura TCP a los contadores Grid y Solar manejan sus propios buffers, impidiendo solapamientos y falsas lecturas si el inversor retrasa respuestas. 
- **Verificación de tramas UDP Beny**: Descarte automático de mensajes corruptos o incompletos para proteger al sistema de picos irreales de potencia o cambios de estado erróneos.
- **Throttling Automático**: Adaptación de las peticiones Modbus TCP si se detecta alta latencia por parte del inversor Huawei (para prevenir cuelgues del dispositivo puente EW11).

## Arquitectura del Sistema

```
┌─────────────┐     Modbus TCP     ┌──────────────────┐
│  Huawei     │◄──────────────────►│                  │
│  Inversor   │  (Grid + PV data)  │                  │
└─────────────┘                    │   M5StickC Plus  │
                                   │     (ESP32)      │
┌─────────────┐     UDP/TCP        │                  │
│  Beny       │◄──────────────────►│  - DLB Logic     │
│  Cargador   │  (Control carga)   │  - LCD Display   │
└─────────────┘                    │  - Telegram Bot  │
                                   │  - Google Sheets │
┌─────────────┐     HTTPS          │  - ESIOS/PVPC   │
│  Telegram   │◄──────────────────►│                  │
│  Bot API    │  (Comandos/Notif)  └──────────────────┘
└─────────────┘
```

## Modos de Carga

| Modo | ID | Objetivo de red | Descripción |
|------|----|-----------------|-------------|
| ☀️ **Solar** | 0 | `SOLAR_GRID_TARGET` (0W) | Carga con excedentes, ajustando el amperaje para no importar de red. |
| ⚖️ **Balanceo** | 1 | `/set_limit` (4600W) | Carga dinámica aprovechando hasta el límite de red configurado. |

**Modo por defecto:** Solar (ID 0).

En ambos modos el control es el mismo: **solo se modula el amperaje**, entre `BENY_MIN_AMPS` (6A) y `BENY_MAX_AMPS` (32A), a razón de ±1A/s. Lo único que cambia entre modos es el objetivo de potencia de red al que apunta el DLB.

## El mínimo de 6A es el suelo del sistema

**No existe pausa automática.** El cargador funciona en modo *Plug and Charge* e **ignora la orden de STOP**: el coche lo rearranca por su cuenta, y en ese rearranque el Beny usa **su propio máximo (32A)** en vez del amperaje que le habíamos fijado. Además, el STOP viaja por UDP sin confirmación. El resultado era que la "pausa" terminaba cargando a plena potencia — exactamente lo contrario de su propósito — así que el mecanismo se ha eliminado por completo.

Consecuencias que hay que tener presentes:

- El consumo mínimo del cargador mientras hay un coche enchufado es **6A (~1.4kW)**. Si no hay sol, ese consumo se toma de la red incluso en modo Solar.
- **Configura `/set_limit` contando con ese consumo residual.** El límite debe dejar margen para los 6A del cargador por debajo de tu potencia contratada; el DLB no puede bajar de ahí.
- Cuando el sistema está en el suelo de 6A lo señala explícitamente: en amarillo en la pantalla (línea `Amp:`) y con un aviso en `/status`.
- Si quieres un corte real de la carga, hay que **desactivar Plug and Charge en el propio cargador**; desde el M5Stick no es posible garantizarlo.

## Control Dinámico por Pasos e Histéresis (DLB)

Para evitar oscilaciones bruscas del amperaje y proteger los contactores del cargador, el algoritmo DLB ajusta la corriente de carga a razón de **±1 Amperio por segundo**. 

Además, implementa un margen de **histéresis (zona muerta) de 200W** en torno al objetivo. Si la lectura de la red varía menos de 200W respecto a la meta, el cargador asimila ese pequeño margen temporal sin emitir órdenes continuas de ajuste.

## Reconexión WiFi

`WiFi.setAutoReconnect(true)` por sí solo no siempre recupera al ESP32 cuando el punto de acceso desaparece un rato, así que la reconexión es **escalonada y no bloqueante**:

| Momento | Acción |
|---------|--------|
| Cada 15s sin enlace | `WiFi.reconnect()` sobre la configuración existente. |
| Cada 4º reintento (~1 min) | `WiFi.disconnect(true)` + `WiFi.begin()`: reasociación completa. |
| A los 15 min | Reinicio, como último recurso. |

Puntos clave del diseño:

- **El arranque nunca reinicia por falta de WiFi.** Antes, un fallo de conexión en `setup()` provocaba `ESP.restart()` a los 30s, y con el router caído eso era un **bucle de reinicio infinito**: 30s de puntos en pantalla, reinicio, otros 30s de puntos… El equipo nunca llegaba al bucle principal. Ese es el síntoma de "pantalla negra llenándose de puntos".
- **El umbral de reinicio es largo (15 min) a propósito.** Reiniciar no arregla un router que sigue caído; solo tira la hora de funcionamiento y esconde el problema.
- **Las tareas de red se saltan sin enlace.** Telegram, ESIOS, Google Sheets, Huawei y Beny no se ejecutan mientras no hay WiFi: intentar conexiones imposibles consume el ciclo (un handshake TLS puede tardar segundos) y dejaba sin tiempo a la propia lógica de reconexión y a los botones.
- **El DLB también se detiene sin enlace**, porque las lecturas de Beny y Huawei estarían obsoletas y ninguna orden llegaría al cargador.
- La pantalla muestra `SIN WIFI - reintent.` en rojo mientras dura la caída, para no confundir una desconexión con un equipo colgado mostrando datos viejos.

## Gestión de Pantalla (Salvapantallas)

- La pantalla se apaga completamente tras **2 minutos** de inactividad (comando ST7789 DISPOFF) para proteger el panel.
- Se despierta inmediatamente al:
  - Pulsar el **Botón B** (lateral) — Acción dedicada: solo despierta la pantalla.
  - Pulsar el **Botón A** (frontal) — Si está dormida, despierta; si ya está encendida, cicla el modo de carga.
  - Cambiar el modo remotamente vía Telegram.

## Comandos de Telegram

### Información
| Comando | Descripción |
|---------|-------------|
| `/start` | Muestra el mensaje de bienvenida con todos los comandos. |
| `/help` | Lista todos los comandos disponibles. |
| `/status` | Estado completo: red, solar, cargador, amperaje objetivo/real y modo activo. |

### Modos de Carga
| Comando | Descripción |
|---------|-------------|
| `/solar` | Activa el modo Solar (objetivo de red 0W, mínimo 6A). |
| `/balanceo` | Activa el modo Balanceo Dinámico (objetivo `/set_limit`). |

### Configuración
| Comando | Descripción | Rango | Defecto |
|---------|-------------|-------|---------|
| `/set_limit <watts>` | Objetivo de red del modo Balanceo (W). | 1000 – 10000 | 4600 |
| `/set_price <valor>` | Umbral de precio eléctrico (solo informativo). | > 0 | 0.05 |

### Comandos retirados
`/set_pausa`, `/set_reinicio`, `/set_margen` (pausa automática eliminada), `/off`, `/stop` y `/turbo`. Siguen reconociéndose para responder con una explicación en vez de fallar en silencio.

### Notificaciones Automáticas
El sistema envía mensajes proactivos a Telegram cuando:
- Se inicia el sistema (indicando el modo activo).
- Se cambia de modo mediante el botón físico del M5Stick.

Las notificaciones **no se envían desde el punto donde se generan**: se encolan (hasta 4) y `loopTelegram()` entrega una por ciclo de polling. `bot.sendMessage()` es una petición HTTPS bloqueante de varios segundos, y llamarla desde `setup()` o desde la lógica de control podía agotar el watchdog de 30s y reiniciar el equipo.

## Estructura del Proyecto

```
CargadorBenyV2/
├── include/
│   ├── BenyTask.h          # Interfaz del cargador Beny (struct BenyData + comandos)
│   ├── EsiosTask.h         # Interfaz de precios PVPC (struct PriceState)
│   ├── GoogleSheetsTask.h  # Interfaz del datalogger
│   ├── HuaweiTask.h        # Interfaz del inversor Huawei (grid + PV power)
│   └── TelegramTask.h      # Interfaz del bot de Telegram
├── src/
│   ├── main.cpp            # Setup, loop, DLB logic, UI, botones, salvapantallas
│   ├── BenyTask.cpp        # Comunicación UDP con el cargador Beny
│   ├── HuaweiTask.cpp      # Lectura Modbus TCP del inversor Huawei
│   ├── TelegramTask.cpp    # Bot de Telegram (comandos + notificaciones)
│   ├── EsiosTask.cpp       # Consulta de precios PVPC vía API ESIOS
│   ├── GoogleSheetsTask.cpp# Envío horario de datos a Google Sheets
│   ├── config.h            # Credenciales y constantes de configuración
└── platformio.ini          # Configuración de PlatformIO
```

## Frecuencias de Polling

| Tarea | Intervalo | Descripción |
|-------|----------|-------------|
| Huawei (Modbus) | 1s | Lectura de potencia de red y solar. |
| Beny (UDP) | 2s | Lectura de estado del cargador. |
| Lógica DLB | 1s | Cálculo y ajuste de amperaje continuo (±1A). |
| Telegram | 2s | Polling de mensajes entrantes. |
| Pantalla LCD | 500ms | Refresco de la interfaz visual. |
| Google Sheets | 10s (check) / 1h (envío) | Envío de datos cada hora en punto. |
| Precios ESIOS | Variable | Consulta diaria de precios PVPC. |

## Google Sheets — Parámetros Enviados

Cada hora en punto, el sistema envía un `GET` al Google Apps Script con los siguientes parámetros:

| Parámetro | Tipo | Descripción |
|-----------|------|-------------|
| `date` | String | Fecha (`dd/mm/yyyy`) |
| `time` | String | Hora (`HH:MM:SS`) |
| `grid` | int | Potencia de red (W). Positivo = importando. |
| `solar` | int | Producción solar (W). |
| `price` | float | Precio PVPC actual (€/kWh). |
| `mode` | int | Modo activo (0=Solar, 1=Balanceo). |
| `beny_w` | int | Potencia de carga del Beny (W). |
| `amps` | int | Amperaje objetivo del DLB (A). |

> El fichero `google_apps_script.js` de este repo ya está alineado con estos parámetros, pero **hay que volver a desplegarlo** en Google Apps Script para que los cambios surtan efecto.

## Hardware Necesario

- **M5StickC Plus** (ESP32, pantalla LCD 135×240, AXP192, WiFi, botones A/B)
- **Cargador Beny** con interfaz de red UDP (puerto 3333)
- **Inversor Solar Huawei** con Smart Meter Modbus TCP (puerto 502)
- **Red WiFi** con acceso a Internet (para Telegram, ESIOS, Google Sheets)

## Configuración Inicial

1. **Editar `src/config.h`** con tus credenciales:
   - `WIFI_SSID` / `WIFI_PASSWORD` — Red WiFi.
   - `BOT_TOKEN` / `CHAT_ID` — Token del bot de Telegram y tu Chat ID.
   - `BENY_IP` / `BENY_PIN` / `BENY_SERIAL` — Datos del cargador Beny.
   - `INVERTER_IP` — IP del inversor Huawei.
   - `ESIOS_TOKEN` — Token de la API de ESIOS (REE).
   - `GOOGLE_SCRIPT_URL` — URL del Google Apps Script desplegado.

2. **Compilar y cargar** con PlatformIO (por USB — no hay actualización OTA):
   ```bash
   pio run -t upload
   ```

3. **Monitorizar** la salida serie:
   ```bash
   pio device monitor
   ```

## Persistencia

Los siguientes valores se guardan en la memoria flash (NVS) del ESP32 y sobreviven a reinicios:

| Clave | Tipo | Descripción |
|-------|------|-------------|
| `mode` | int | Modo de carga activo (0, 1). |
| `limit` | int | Objetivo de potencia de red del modo Balanceo (W). |

Las claves `t_pause`, `t_resume` y `r_margin` de la pausa automática ya no se leen ni se escriben. Quedan huérfanas en la NVS de los equipos actualizados, sin efecto.

## Particiones de Flash (sin OTA)

El proyecto **no incluye actualización OTA**: se carga siempre por USB. Eso permite usar un esquema de partición única en `platformio.ini`:

```ini
board_build.partitions = huge_app.csv
```

El esquema por defecto reservaba **dos ranuras de aplicación de 1,25 MB** para que el OTA pudiera alternar entre ellas, y el firmware ocupaba el 83 % de una de ellas. Con una sola partición de 3 MB, el mismo firmware baja al **33 %**, dejando margen holgado para crecer.

> ⚠️ Cambiar el esquema de particiones **exige una carga por USB** (no se puede migrar por red) y borra SPIFFS, que este proyecto no usa. La partición NVS mantiene offset y tamaño, así que el modo y el límite guardados sobreviven.

## Licencia

Proyecto personal. Uso bajo tu propia responsabilidad.
