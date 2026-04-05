# CargadorBenyV2 — Control Inteligente de Carga EV

Sistema de control de carga para vehículos eléctricos basado en **M5StickC Plus (ESP32)**, diseñado para gestionar un cargador **Beny** en combinación con un inversor solar **Huawei** y datos de precios eléctricos del mercado español (PVPC/ESIOS).

El objetivo principal es maximizar el autoconsumo solar, proteger la instalación eléctrica y permitir el control remoto total vía **Telegram**.

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

| Modo | ID | Descripción | Autoapagado |
|------|----|-------------|-------------|
| ☀️ **Solar** | 0 | Carga con excedentes (min 6A consumiendo de red si falta sol). | ✅ Activo (max 4.6kW) |
| ⚖️ **Balanceo** | 1 | Carga dinámica rápida aprovechando hasta la potencia contratada. | ✅ Activo (max 4.6kW) |
| 🌑 **OFF** | 2 | Cargador desactivado manualmente. | - |

**Modo por defecto:** Solar (ID 0).

## Sistema de Seguridad (Autoapagado / Autoreinicio)

Para proteger la instalación eléctrica, el sistema implementa un mecanismo automático de pausa y reanudación de la carga (activo en los modos Solar y Balanceo):

- **Autoapagado**: Si se excede el límite de red durante un tiempo configurable (por defecto 60s), se detiene la carga impidiendo que salte el ICP.
- **Autoreinicio**: Si hay un margen de potencia disponible (por defecto 1840W / 8A) durante un tiempo configurable (por defecto 60s), se reanuda la carga.

Todos los parámetros de tiempo y margen son configurables remotamente por Telegram y se persisten en la memoria flash del M5Stick.

## Control Dinámico por Pasos e Histéresis (DLB)

Para evitar oscilaciones bruscas del amperaje y proteger los contactores del cargador, el algoritmo DLB ajusta la corriente de carga a razón de **±1 Amperio por segundo**. 

Además, implementa un margen de **histéresis (zona muerta) de 200W** en torno al objetivo. Si la lectura de la red varía menos de 200W respecto a la meta, el cargador asimila ese pequeño margen temporal sin emitir órdenes continuas de ajuste.

## Gestión de Pantalla (Salvapantallas)

- La pantalla se apaga completamente tras **2 minutos** de inactividad (comando ST7789 DISPOFF) para proteger el panel.
- Se despierta inmediatamente al:
  - Pulsar el **Botón B** (lateral) — Acción dedicada: solo despierta la pantalla.
  - Pulsar el **Botón A** (frontal) — Si está dormida, despierta; si ya está encendida, cicla el modo de carga.
  - Cambiar el modo remotamente vía Telegram.
  - Producirse un evento de autoapagado o autoreinicio por exceso de red.

## Comandos de Telegram

### Información
| Comando | Descripción |
|---------|-------------|
| `/start` | Muestra el mensaje de bienvenida con todos los comandos. |
| `/help` | Lista todos los comandos disponibles. |
| `/status` | Muestra el estado completo del sistema: red, solar, cargador, modo activo, parámetros de configuración. |

### Modos de Carga
| Comando | Descripción |
|---------|-------------|
| `/solar` | Activa el modo Solar (Solo excedentes, min 6A). |
| `/balanceo` | Activa el modo Balanceo Dinámico (Límite Red). |
| `/off` / `/stop` | Desactiva el cargador completamente. |

### Configuración
| Comando | Descripción | Rango | Defecto |
|---------|-------------|-------|---------|
| `/set_limit <watts>` | Límite máximo de importación de red (W). | 1000 – 10000 | 4600 |
| `/set_pausa <segundos>` | Tiempo de tolerancia antes del autoapagado. | 10 – 3600 | 60 |
| `/set_reinicio <segundos>` | Tiempo de espera para autoreinicio. | 10 – 3600 | 60 |
| `/set_margen <vatios>` | Margen de potencia disp. necesario para reiniciar. | 100 – 8000 | 1840 |
| `/set_price <valor>` | Umbral de precio eléctrico (solo informativo). | > 0 | 0.05 |

### Notificaciones Automáticas
El sistema envía mensajes proactivos a Telegram cuando:
- Se inicia el sistema (indicando el modo activo).
- Se cambia de modo mediante el botón físico del M5Stick.

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
| `mode` | int | Modo activo (0=Solar, 1=Balanceo, 2=OFF). |
| `beny_w` | int | Potencia de carga del Beny (W). |
| `paused` | int | 1 si está en pausa automática, 0 si no. |

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

2. **Compilar y cargar** con PlatformIO:
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
| `mode` | int | Modo de carga activo (0, 1, 2). |
| `limit` | int | Límite de potencia de red (W). |
| `t_pause` | ulong | Tiempo de tolerancia de autoapagado (ms). |
| `t_resume` | ulong | Tiempo de espera de autoreinicio (ms). |
| `r_margin` | int | Margen de potencia para autoreinicio (W). |

## Actualización OTA

El firmware se puede actualizar por red WiFi sin cable USB:
```bash
pio run -t upload --upload-port BenyChargerV2.local
```

## Licencia

Proyecto personal. Uso bajo tu propia responsabilidad.
