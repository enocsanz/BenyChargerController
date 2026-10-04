# CargadorBenyV2 — Control Inteligente de Carga EV

Sistema de control de carga para vehículos eléctricos basado en un **M5Stack StampS3 (ESP32-S3)**, diseñado para gestionar un cargador **Beny** en combinación con un inversor solar **Huawei** y datos de precios eléctricos del mercado español (PVPC/ESIOS). También gestiona el **termo eléctrico (ACS)** y la **depuradora de la piscina** mediante relés Tuya controlados en local.

El objetivo principal es maximizar el autoconsumo solar, proteger la instalación eléctrica y permitir el control remoto total vía **Telegram**.

No tiene pantalla: todos los datos se consultan por Telegram (`/status`), y el StampS3 solo tiene un **LED de estado** y un **botón** para cambiar de modo. Ver [LED y botón](#led-y-botón-stamps3).

> Versiones anteriores, congeladas en etiquetas git: **M5StickC Plus** en [`v1-m5stickcplus`](../../tree/v1-m5stickcplus) y **M5Stack Dial** (pantalla redonda) en [`v2-m5dial`](../../tree/v2-m5dial). El Dial lleva dentro un StampS3A con el mismo chip, así que todo lo que habla con la red es idéntico; solo cambian la pantalla y los controles.

## Mejoras de Estabilidad

Versiones M5Dial y StampS3 (ESP32-S3):

- **Seguro en los relés si el StampS3 cae**: la cuenta atrás propia de cada relé Tuya hace de *dead man's switch*: el termo vuelve a encenderse y la depuradora se apaga solos si el StampS3 deja de renovarla. Ver [Seguro si el StampS3 deja de funcionar](#seguro-si-el-stamps3-deja-de-funcionar).
- **Vigilante con aviso por Telegram**: el script de Google avisa si el StampS3 lleva más de 20 min sin enviar datos (apagones, WiFi, cuelgues). Ver [Vigilante](#vigilante-aviso-si-el-stamps3-deja-de-dar-señales).
- **DLB con pasos suaves de 1A**, que actúa solo con lecturas nuevas de red y espera a que el coche siga cada paso. Ver [Control Dinámico](#control-dinámico-dlb-pasos-suaves-de-1a).
- **Actualización por WiFi (OTA)**: el StampS3 va junto al termo y ya no hace falta el cable. Ver [Actualizar el firmware](#actualizar-el-firmware-ota).
- **Varios servidores NTP**: el StampS3 no tiene reloj con pila (el Dial sí, y tapaba que NTP a veces tardaba minutos).
- **Hora sin condición de carrera**: `getLocalTime(&t, 0)` devolvía "sin hora" al azar y el precio salía como desconocido un segundo cada pocos minutos, lo que encendía y apagaba el termo (190 veces en un día). Se usa `timeNow()` (`time()` + `localtime_r`).
- **Watchdog alimentado entre tareas de red**: varias conexiones lentas en la misma vuelta del bucle llegaron a sumar más de 30 s.
- **Precios ESIOS por día**: se recargan al cambiar de día (antes, tras medianoche, se servían los del día anterior) y un fallo se reintenta una vez por minuto.
- **Diagnóstico en Google Sheets**, por minuto y con eventos, que permite revisar todo el funcionamiento. Ver [Diagnóstico](#diagnóstico-en-google-sheets).

Versión M5StickC Plus:

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
└─────────────┘                    │  M5Stack StampS3 │
                                   │    (ESP32-S3)    │
┌─────────────┐     UDP/TCP        │                  │
│  Beny       │◄──────────────────►│  - DLB Logic     │
│  Cargador   │  (Control carga)   │  - LED / botón   │
└─────────────┘                    │  - Telegram Bot  │
                                   │  - Google Sheets │
┌─────────────┐     HTTPS          │  - ESIOS/PVPC   │
│  Telegram   │◄──────────────────►│  - Termo (ACS)   │
│  Bot API    │  (Comandos/Notif)  │  - Depuradora    │
└─────────────┘                    │                  │
                                   │                  │
┌─────────────┐   Tuya local 3.5   │                  │
│  Relé termo │◄──────────────────►│                  │
│  (Tongou)   │  (TCP 6668, AES)   │                  │
└─────────────┘                    │                  │
┌─────────────┐   Tuya local 3.4   │                  │
│ Relé piscina│◄──────────────────►│                  │
│  (Tongou)   │  (TCP 6668, AES)   └──────────────────┘
└─────────────┘
```

## Modos de Carga

| Modo | ID | Objetivo de red | Descripción |
|------|----|-----------------|-------------|
| ☀️ **Solar** | 0 | `SOLAR_GRID_TARGET` (0W) | Carga con excedentes, ajustando el amperaje para no importar de red. |
| ⚖️ **Balanceo** | 1 | `/set_limit` (4600W) | Carga dinámica aprovechando hasta el límite de red configurado. |

**Modo por defecto:** Solar (ID 0).

En ambos modos el control es el mismo: **solo se modula el amperaje**, entre `BENY_MIN_AMPS` (6A) y `BENY_MAX_AMPS` (32A), de 1 en 1 A en ambos sentidos. Lo único que cambia entre modos es el objetivo de potencia de red al que apunta el DLB.

## El mínimo de 6A es el suelo del sistema

**No existe pausa automática.** El cargador funciona en modo *Plug and Charge* e **ignora la orden de STOP**: el coche lo rearranca por su cuenta, y en ese rearranque el Beny usa **su propio máximo (32A)** en vez del amperaje que le habíamos fijado. Además, el STOP viaja por UDP sin confirmación. El resultado era que la "pausa" terminaba cargando a plena potencia — exactamente lo contrario de su propósito — así que el mecanismo se ha eliminado por completo.

Consecuencias que hay que tener presentes:

- El consumo mínimo del cargador mientras hay un coche enchufado es **6A (~1.4kW)**. Si no hay sol, ese consumo se toma de la red incluso en modo Solar.
- **Configura `/set_limit` contando con ese consumo residual.** El límite debe dejar margen para los 6A del cargador por debajo de tu potencia contratada; el DLB no puede bajar de ahí.
- Cuando el sistema está en el suelo de 6A lo señala explícitamente: `/status` incluye un aviso, y el LED se pone naranja si además se supera el límite.
- Si quieres un corte real de la carga, hay que **desactivar Plug and Charge en el propio cargador**; desde el StampS3 no es posible garantizarlo.
- **Limita también el máximo en la app del Beny** (por ejemplo a 16A). Si el StampS3 se apaga o se cuelga, nadie modula la carga, y al rearrancar la sesión el Beny sube a su propio máximo: ese límite es la única protección que queda.

## Control Dinámico (DLB): pasos suaves de 1A

El DLB compara la potencia de red con el objetivo del modo y ajusta el amperaje del Beny. Dos retardos condicionan el diseño:

- **La lectura de red llega con retraso**: cada ~1 s, o cada 10 s si el inversor responde lento (throttling para no colgar el EW11).
- **El coche tarda varios segundos** en seguir un nuevo objetivo de amperaje.

La primera versión ajustaba ±1A cada segundo aunque la lectura fuese la misma, acumulando correcciones antes de ver su efecto: con un límite de 4,6 kW la red oscilaba entre ~2,7 y ~5,9 kW. Después se probó a bajar de golpe al valor calculado, pero las bajadas resultaban demasiado bruscas y no hacían falta: la distribuidora tolera minutos por encima del límite (4,6 kW es la potencia contratada para pagar menos, la instalación aguanta bastante más). Ahora:

1. **Solo actúa con una muestra nueva de red** (`grid_sample_count` en `HuaweiTask`). Si el dato no ha cambiado, no hace nada.
2. **Pasos de 1A por muestra** (cada 1-2 s), en ambos sentidos, siempre desde el objetivo anterior.
3. **Espera a que el coche siga el paso anterior**: baja solo si la corriente real no supera el objetivo en más de 1A, y sube solo si ha llegado a menos de 1A del objetivo. Así los pasos no se acumulan, y si el coche se limita solo (por ejemplo cerca del 100 %) no se sigue subiendo.
4. **Zona muerta de 200W** (`GRID_DEADBAND`) en torno al objetivo: dentro de ella no se emiten órdenes.
5. **Resincronización**: si la corriente real difiere más de 2A del objetivo (paquete UDP perdido), el objetivo se reenvía como mucho cada 10 s. Si el coche consume **más** de lo pedido, se reenvía a los **3 s**: al rearrancar la sesión, el Beny sube por su cuenta hasta su máximo (se vio llegar a 27A con 19A pedidos y la red a 8,1 kW).

La variación que queda se debe sobre todo a los consumos de la casa (lavadora, secadora, horno…), que por sí solos oscilan ±1 kW.

## Termo Eléctrico (ACS)

El termo tiene su propio termostato mecánico. Delante lleva un relé de carril **Tongou TO-Q-SY1-JWT** con medición de potencia. El StampS3 no hace calentar al termo: solo **habilita o corta** el relé, y con el relé cerrado sigue mandando el termostato.

### Reglas

| Regla | Condición | Acción |
|-------|-----------|--------|
| **Precio** | PVPC de la hora > umbral (`/set_termo_precio`, 0,20 €/kWh por defecto) | Relé abierto hasta que el precio baje. |
| **Sobrecarga** | Red > `CONTRACTED_POWER` + 200 W durante **30 s**, **con el coche ya al mínimo** (6A) o sin cargar | Relé abierto. Aviso por Telegram. |
| **Vuelta tras sobrecarga** | Al menos **5 min** cortado y **2 min seguidos** con sitio para el termo | Relé cerrado. Aviso por Telegram. |
| **Aviso de sobrecarga sin salida** | Media de red del último minuto > `CONTRACTED_POWER` + 200 W durante **3 min**, con el coche al mínimo (o sin cargar) y el termo sin consumo | 🚨 Aviso por Telegram: ya no queda nada que cortar, hay que apagar algo o parar el coche. Otro aviso cuando la media vuelve bajo el límite 2 min. |
| **Encendido con sitio** | Al volver a estar permitido (baja el precio, `/termo_auto`, arranque) | Solo se cierra si el termo cabe; si no, espera a que haya sitio. Sin lectura de red todavía (arranque), espera hasta 5 min por ella y después se cierra igualmente. |
| **Sin precio** | ESIOS no responde o no hay hora durante más de **10 min** | El precio **no bloquea**: mejor una hora cara que quedarse sin agua caliente. Un hueco más corto mantiene el último precio conocido. |

**El coche cede primero.** Si no cabe todo, el DLB baja el coche hasta 6A. El termo solo se corta si con el coche al mínimo sigue habiendo sobrecarga (el horno, la vitro, el secador…). Los picos cortos no cuentan, porque la distribuidora los tolera.

Para saber si el termo cabe se usa su **potencia real**, que el relé mide cada vez que calienta (hasta la primera medida se toman `TERMO_DEFAULT_POWER` = 2600 W; el termo mide 2,55-2,58 kW). La cuenta es: red actual − lo que el coche aún podría ceder hasta 6A − lo que consume ahora el termo + potencia del termo ≤ contratada − 200 W. No se actúa sobre una lectura de red de más de 30 s.

### Ciclos de calentamiento

Cada vez que el termo calienta queda en Eventos (`TERMO`), para analizar a qué horas funciona: `Empieza a calentar (2550 W, precio …, red …)` y, al terminar, `Deja de calentar: 48 min, 2.10 kWh, precio medio 0.205 E/kWh, 0.43 EUR`. Empieza por encima de 500 W y termina por debajo de 100 W (histéresis contra el ruido de la medida); la energía y el coste se suman cada segundo con el precio de cada momento, así que un ciclo que cruza un cambio de hora sale con su precio medio real.

### Modos

| Modo | Precio | Sobrecarga |
|------|--------|------------|
| `AUTO` (por defecto) | Corta | Corta |
| `ON` | Se ignora | Corta |
| `OFF` | Relé siempre abierto | — |

En `AUTO` el StampS3 manda sobre el relé: si se enciende o apaga desde la app Smart Life, lo devuelve a su estado en unos segundos. Para mandar a mano, usa `/termo_on` o `/termo_off`.

### Control local (Tuya 3.4 / 3.5)

Los relés se controlan **en local**, sin la nube de Tuya (`src/TuyaLocal.cpp`), por TCP 6668 y con una clave de sesión negociada en cada conexión (mbedtls del ESP32):

| Versión | Tramas | Cifrado | Relé |
|---------|--------|---------|------|
| 3.5 | `0x6699` | AES-128-GCM | Termo |
| 3.4 | `0x55AA` | AES-128-ECB + HMAC-SHA256 | Depuradora |

Se consulta el estado cada 10 s, y el relé además avisa de cada cambio. Si la conexión cae, se reintenta cada 30 s. Tras reiniciar el StampS3, es normal que el relé cierre la primera sesión (aún tiene abierta la anterior) y que entre al segundo intento.

La **clave local** solo se obtiene una vez desde la nube:

1. Proyecto *Smart Home* en [iot.tuya.com](https://iot.tuya.com) (centro de datos *Central Europe*) y vincular la app en *Devices → Link App Account*. Si aparece `IoT Core service subscription has expired`, hay que pedir *Extend Trial Period* en *Cloud Services → IoT Core*.
2. `python -m tinytuya wizard` en la carpeta del proyecto. Genera `devices.json` (con las claves) y `tinytuya.json` (con el API Secret), que están en `.gitignore`.
3. Copiar la clave de cada relé a `TERMO_LOCAL_KEY` / `PISCINA_LOCAL_KEY` en `config.h`. La versión de protocolo de cada relé la da `python -m tinytuya scan`.

Después la suscripción a la nube puede caducar, porque el StampS3 no la usa. La clave cambia si el relé se vuelve a emparejar en la app.

> ⚠️ Dale a cada relé una **IP fija por DHCP** en el router. El StampS3 se conecta a `TERMO_IP` / `PISCINA_IP`: si el router le cambia la IP, deja de encontrarlo.

### Seguro si el StampS3 deja de funcionar

Los relés tienen una **cuenta atrás propia** (DP 9, `countdown_1`): cuando llega a cero, el relé **invierte su estado él solo**, sin el StampS3 (comprobado: apagado + cuenta atrás de 10 s → se encendió a los 9 s). El StampS3 la usa como *dead man's switch* (`TuyaLocal::keepFailsafe`):

| Relé | Cuándo se arma | Si el StampS3 deja de renovarla |
|------|----------------|--------------------------------|
| **Termo** | Mientras está cortado (precio o sobrecarga), salvo con `/termo_off` | Se **enciende** solo: no os quedáis sin agua caliente. |
| **Depuradora** | Mientras funciona en `AUTO` | Se **apaga** sola: no sigue funcionando horas sin control. |

La cuenta atrás es de **15 min** y se renueva cada 5 min, así que solo llega a cero si el StampS3 lleva más de 10 min sin poder hablar con el relé. Los cambios de armado quedan en Eventos (`seguro activado` / `seguro desactivado`). El termo está configurado para recordar su estado tras un corte de luz (`relay_status = memory`), y ningún relé tiene temporizadores propios en la app.

## Depuradora de la Piscina

La depuradora (motor de velocidad variable + clorador salino) va detrás de otro relé **Tongou TO-Q-SY1-JWT**. Funciona **con el sol sobrante**, pocas horas al día, sin encenderse y apagarse continuamente.

### Sol sobrante

`sobrante = producción solar − consumo de la casa sin el coche ni la depuradora` (= coche + depuradora − red, limitado a la producción solar). Como no descuenta el coche, **la depuradora tiene prioridad sobre el coche** en cualquier modo de carga; el DLB le da al coche lo que queda.

El sobrante se promedia (media móvil exponencial de **5 min**) para que una nube no la pare. Durante el primer minuto tras arrancar el StampS3 se usa una media simple y no se toca el relé.

### Reglas

| Regla | Condición |
|-------|-----------|
| **Arranque** | Sobrante medio ≥ `PISCINA_POWER` + 100 W (500 W), parada desde hace ≥ 15 min y por debajo del máximo de hoy. |
| **Parada** | Sobrante medio < 50 % de `PISCINA_POWER` (200 W) tras ≥ 30 min encendida, o máximo de hoy cumplido. |
| **Mínimo** | Si un día no llega al mínimo, lo que falte se completa **esa madrugada (00-08 h) en las horas más baratas** (PVPC). Sin precios, se completa en cuanto empieza la madrugada. Lo no completado a las 08 h se descarta. |
| **Arranque del StampS3** | Si el relé está encendido, se adopta como funcionamiento con sol recién empezado: se le respetan los 30 min y solo se para por las reglas normales (sobrante < 200 W o máximo diario), aunque el sobrante no llegue al umbral de arranque. Si está apagado, puede arrancar sin esperar. |

`PISCINA_POWER` es **fijo (400 W)**: el motor es de velocidad variable y cambia de consumo cada cierto tiempo (se han medido 150-460 W), así que aprender la potencia movería los umbrales.

### Horas por mes

| | Ene | Feb | Mar | Abr | May | Jun | Jul | Ago | Sep | Oct | Nov | Dic |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Máximo (h) | 2 | 2 | 3 | 4 | 5 | 6 | 8 | 8 | 6 | 4 | 2 | 2 |
| Mínimo (h) | 1 | 1 | 1 | 2 | 2 | 3 | 4 | 4 | 3 | 2 | 1 | 1 |

Valores por defecto en `PISCINA_MAX_HOURS` / `PISCINA_MIN_HOURS`. `/set_piscina_horas MAX MIN` cambia los del mes en curso y se guardan. El tiempo de hoy se guarda cada 5 min, así que sobrevive a un reinicio.

> Quita cualquier **programación horaria** del relé en la app Smart Life: en `AUTO` el StampS3 manda sobre él y la devolvería a su estado.

## Sonda del Termo — en pruebas

Junto al termo, en otra planta, la temperatura del agua (sonda DS18B20, pendiente) la medirá un **M5StickC Plus** que se la manda al StampS3. Ahora mismo es una **prueba de enlace**: la sonda envía un mensaje numerado por segundo (`include/SondaPacket.h`, compartido por los dos firmwares) y el StampS3 cuenta cuántos llegan. `/sonda` en Telegram da el % recibido, por qué vía y la hora del último mensaje, y cada hora queda un resumen en Eventos (`SONDA`).

**Vía: WiFi de casa (UDP, puerto 4210).** La primera prueba fue por **ESP-NOW** (directo entre los dos chips, sin router): 100 % mientras el StampS3 estuvo en la casa. Pero el StampS3 se trasladó a la caseta de la depuradora, junto a otro punto Google Wifi, y ESP-NOW no llega tan lejos: no pasa por los puntos de la red mesh. Así que la sonda se une a la WiFi y envía por UDP al StampS3 (192.168.86.41), que **devuelve cada mensaje** como confirmación. El StampS3 sigue escuchando también ESP-NOW por si vuelve a estar al alcance.

- **Pantalla de la sonda**: su señal WiFi (verde ≥ −67, amarillo ≥ −75, rojo por debajo), el % confirmado en el último minuto (verde ≥ 80 %, amarillo ≥ 50 %, rojo por debajo), el total, y su IP y punto de acceso.
- **Red mesh**: tanto el StampS3 como la sonda escanean todos los canales y se unen al punto con **más señal**. Por defecto el ESP32 se une al primero que encuentra: en la caseta, con un punto allí mismo, el StampS3 daba −79/−82 dBm; con este cambio, −48.
- **Cambio de punto (*roaming*)**: aun así, el escaneo del arranque a veces no oye el punto cercano (tras una OTA el StampS3 se quedó en −81 con el de la caseta a −46), y el ESP32 nunca cambia de punto por su cuenta. Con señal por debajo de −70 dBm, cada 5 min (la primera, al minuto de arrancar) escanea en segundo plano y se pasa a un punto al menos 10 dB mejor; queda en Eventos (`WIFI: Cambio de punto de acceso`). Código común en `include/WifiRoam.h`.
- **Recuperación automática de la sonda**: el 03/10 a las 22:53 perdió la WiFi (−88 dBm) y estuvo 21 h sin enviar nada, aunque al volver la WiFi respondía al ping: el socket UDP quedó inservible tras la reconexión. Ahora: a los 30 s sin WiFi, reconexión completa (sin fijar punto de acceso); al reconectar, socket UDP nuevo; conectada pero 2 min sin confirmaciones, socket nuevo; 10 min sin WiFi o sin confirmaciones, reinicio; y watchdog de 30 s.
- **Firmware de la sonda**: carpeta `sonda/` (proyecto PlatformIO propio). La primera vez por USB (`cd sonda && pio run -t upload`); después por WiFi (`pio run -e m5stickcplus_ota -t upload`, nombre `beny-sonda.local`; conviene fijarle la IP en el router). Usa la WiFi y la contraseña OTA de `include/config.h` y `secrets.ini`.

## Sensor de Ambiente (ENV III) en el StampS3

Una unidad **M5Stack ENV III** (SHT30 de temperatura y humedad en 0x44, QMP6988 de presión en 0x70) mide la temperatura y la humedad de la caseta. El StampS3 no tiene conector Grove, así que va con un cable Grove a Dupont:

| Grove (ENV III) | StampS3 |
|---|---|
| Rojo, 5V | 5V |
| Negro, GND | G (GND) |
| Amarillo, SDA | **G13** |
| Blanco, SCL | **G15** |

- Al arrancar prueba primero G13/G15; si no responde, prueba las parejas de pines libres (sin G0, G3, G19/G20, G21, G26-G37, G45/G46) y guarda la que encuentra (NVS `env_sda` / `env_scl`). El resultado llega por Telegram.
- Bus a **20 kHz** (a 100 kHz fallaba a ratos: sin conector Grove, el bus puede depender de las pull-ups internas del ESP32, muy débiles) y orden de medida con hasta 3 reintentos.
- Lee cada 30 s. `/status` lo muestra en la línea `🌡️ Caseta`; si no hay lectura, dice en qué paso falla.
- Cada medida trae un CRC-8 que se comprueba: con un contacto flojo llegó a leer −45,0 °C y 100 % (todo ceros y todo unos), y ahora esas lecturas se descartan.
- **`/i2c`** diagnostica el bus: nivel en reposo de SDA y SCL (deben ser 1), direcciones que responden (deben ser 0x44 y 0x70; si responden todas, hay una línea bloqueada) y el código de la orden de medida.

## Precio de los Excedentes

Además del PVPC (indicador ESIOS **1001**, lo que cuesta la energía de la red), se descarga cada hora el **1739**: lo que se paga por la energía vertida con la compensación simplificada del PVPC. Puede ser **negativo** en días de mucho sol y poca demanda (en dos semanas de septiembre-octubre: 8 horas, entre −0,0002 y −0,0009 €/kWh). `/status` lo muestra y el diagnóstico lo guarda en cada muestra.

**Solo se registra, sin ninguna acción**: los periodos con precio negativo quedan en Eventos (`EXCEDENTES`, al empezar y al terminar) y el precio en cada muestra. No cambia nada del coche, el termo ni la depuradora, ni se avisa por Telegram.

**Pendiente (más adelante)**: limitar el vertido en el propio inversor Huawei cuando el precio sea claramente negativo (por ejemplo, por debajo de −0,01 €/kWh). Por ahora no compensa: los negativos han sido de céntimos y escribir en la configuración del inversor tiene riesgos (quedarse limitado si algo falla). Se haría leyendo antes los registros y volviendo siempre a vertido libre ante cualquier fallo.

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
- **Las tareas de red se saltan sin enlace.** Telegram, ESIOS, Google Sheets, Huawei y Beny no se ejecutan mientras no hay WiFi: intentar conexiones imposibles consume el ciclo (un handshake TLS puede tardar segundos) y dejaba sin tiempo a la propia lógica de reconexión y al botón.
- **El DLB también se detiene sin enlace**, porque las lecturas de Beny y Huawei estarían obsoletas y ninguna orden llegaría al cargador.
- El **LED se pone rojo** mientras dura la caída.

## LED y Botón (StampS3)

Sin pantalla: los datos se consultan por Telegram. El StampS3 tiene un LED RGB (G21) y un botón (G0).

### LED de estado

| Color | Significado |
|-------|-------------|
| 🔴 Rojo | Sin WiFi: no se puede leer ni mandar nada. |
| 🟠 Naranja | Sobrecarga: termo cortado por sobrecarga, o coche ya a 6A y la red por encima del límite. |
| 🔵 Azul | Coche cargando. Azul fijo también mientras arranca. |
| 🟢 Verde | Todo en orden. |

Cada 2 s da un destello blanco tenue: indica que el programa está vivo. Si el LED se queda fijo sin destello, el programa está colgado (el watchdog lo reiniciará). El brillo está muy bajado (`LED_LEVEL`) porque a tope deslumbra.

### Botón

**Pulsar el botón** alterna el modo Solar ↔ Balanceo y avisa por Telegram.

> El botón es también el **G0** de arranque: si se mantiene pulsado mientras se conecta la alimentación, el StampS3 entra en modo descarga (para programarlo) en vez de arrancar el programa. En funcionamiento normal no afecta.

## Comandos de Telegram

### Información
| Comando | Descripción |
|---------|-------------|
| `/start` | Muestra el mensaje de bienvenida con todos los comandos. |
| `/help` | Lista todos los comandos disponibles. |
| `/status` | Estado completo: precio, red, solar, cargador, amperaje objetivo/real, modo activo, termo, depuradora una línea de la sonda del termo (% recibido, su WiFi y la temperatura del agua) y una del controlador (WiFi, tiempo encendido, memoria libre). |

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

### Termo (ACS)
| Comando | Descripción |
|---------|-------------|
| `/termo` | Estado del termo: calentando / en reposo / cortado (y por qué), modo, umbral y temperatura del agua (si la sonda la envía). |
| `/termo_auto` | Modo AUTO: se corta por precio y por sobrecarga. |
| `/termo_on` | Ignora el precio; la protección por sobrecarga sigue activa. |
| `/termo_off` | Relé abierto hasta volver a `/termo_auto` o `/termo_on`. |
| `/set_termo_precio <valor>` | Precio (€/kWh) a partir del cual se corta en AUTO. Entre 0 y 1. Defecto 0.20. |

### Depuradora (piscina)
| Comando | Descripción |
|---------|-------------|
| `/piscina` | Estado, horas de hoy (máximo y mínimo del mes), pendiente de completar y sobrante medio. |
| `/piscina_auto` | Modo AUTO: con sol, con máximo y mínimo diarios. |
| `/piscina_on` | Encendida hasta volver a `/piscina_auto`. |
| `/piscina_off` | Apagada hasta volver a `/piscina_auto`. |
| `/set_piscina_horas <max> <min>` | Horas máximas y mínimas al día del mes en curso (`min ≤ max ≤ 24`). |

`/status` incluye también el estado del termo y de la depuradora.

### Diagnóstico
| Comando | Descripción |
|---------|-------------|
| `/diag` | Estado del registro en Google Sheets: pendiente de enviar y resultado del último envío (con su duración: la conexión TLS es lo primero que notaría una CPU más lenta). |
| `/sonda` | Enlace con la sonda del termo: % de mensajes recibidos (último minuto y total), vía (WiFi o ESP-NOW) y último mensaje. |
| `/i2c` | Diagnóstico del bus I²C del sensor de ambiente: niveles de SDA/SCL, direcciones que responden y resultado de la orden de medida. |
| `/version` | Versión del firmware (fecha y hora de compilación), IP y cómo actualizarlo; lo mismo de la sonda del termo. |
| `/wifi` | Intensidad de la señal WiFi del controlador (dBm y valoración: buena > −67, aceptable > −75, justa > −80), red, canal e IP. También la señal de la sonda del termo. |
| `/diag_on` / `/diag_off` | Activa o para el registro por minuto y de eventos. |

### Comandos retirados
`/set_pausa`, `/set_reinicio`, `/set_margen` (pausa automática eliminada), `/off`, `/stop` y `/turbo`. Siguen reconociéndose para responder con una explicación en vez de fallar en silencio.

### Notificaciones Automáticas
El sistema envía mensajes proactivos a Telegram cuando:
- Se inicia el sistema, con la versión del firmware, la causa del reinicio (`encendido` = falta de alimentación, `task_wdt` = cuelgue, `brownout` = bajada de tensión…) y el modo activo.
- Se cambia de modo con el botón del StampS3.
- El termo se corta por sobrecarga, y cuando se reactiva.
- Hay sobrecarga sostenida que el sistema ya no puede resolver (coche al mínimo, termo sin consumo), y cuando se resuelve.
- El StampS3 lleva más de 20 min sin enviar datos, y cuando vuelve. Este aviso no lo manda el StampS3 (no podría), sino el [vigilante](#vigilante-aviso-si-el-stamps3-deja-de-dar-señales) del script de Google.

Las notificaciones **no se envían desde el punto donde se generan**: se encolan (hasta 4) y `loopTelegram()` entrega una por ciclo de polling. `bot.sendMessage()` es una petición HTTPS bloqueante de varios segundos, y llamarla desde `setup()` o desde la lógica de control podía agotar el watchdog de 30s y reiniciar el equipo.

## Estructura del Proyecto

```
CargadorBenyV2/
├── include/
│   ├── BenyTask.h          # Interfaz del cargador Beny (struct BenyData + comandos)
│   ├── EsiosTask.h         # Interfaz de precios PVPC (struct PriceState)
│   ├── GoogleSheetsTask.h  # Interfaz del datalogger
│   ├── HuaweiTask.h        # Interfaz del inversor Huawei (grid + PV power)
│   ├── PiscinaTask.h       # Interfaz de la depuradora (modos, estado)
│   ├── TelegramTask.h      # Interfaz del bot de Telegram
│   ├── TermoTask.h         # Interfaz del termo (modos, estado)
│   ├── TuyaLocal.h         # Cliente Tuya local 3.4 / 3.5
│   └── config.h            # Credenciales (no se sube: .gitignore)
├── secrets.ini             # Contraseña OTA para PlatformIO (no se sube: .gitignore)
├── src/
│   ├── main.cpp            # Setup, loop, lógica DLB, botón y LED de estado
│   ├── BenyTask.cpp        # Comunicación UDP con el cargador Beny
│   ├── HuaweiTask.cpp      # Lectura Modbus TCP del inversor Huawei
│   ├── TelegramTask.cpp    # Bot de Telegram (comandos + notificaciones)
│   ├── EsiosTask.cpp       # Consulta de precios PVPC vía API ESIOS
│   ├── GoogleSheetsTask.cpp# Registro horario + diagnóstico (muestras y eventos)
│   ├── TermoTask.cpp       # Reglas del termo: precio y sobrecarga
│   ├── PiscinaTask.cpp     # Reglas de la depuradora: sol, horas, mínimo nocturno
│   ├── TuyaLocal.cpp       # Protocolo Tuya 3.4 / 3.5 (sesión, AES) para los relés
│   └── config.h.example    # Plantilla de include/config.h (credenciales y constantes)
├── google_apps_script.js   # Apps Script: registro, diagnóstico, exportación CSV y vigilante
└── platformio.ini          # Configuración de PlatformIO
```

## Frecuencias de Polling

| Tarea | Intervalo | Descripción |
|-------|----------|-------------|
| Huawei (Modbus) | 1s | Lectura de potencia de red y solar. |
| Beny (UDP) | 2s | Lectura de estado del cargador. |
| Lógica DLB | 1s (actúa solo con muestra nueva de red) | Ajuste de amperaje de 1A por muestra, en ambos sentidos. |
| Telegram | 2s | Polling de mensajes entrantes. |
| LED de estado | 200ms | Color según el estado. |
| Google Sheets | 10s (check) / 1h (envío) | Envío de datos cada hora en punto. |
| Diagnóstico (Sheets) | 1 min (muestra) / 5 min (envío) | Muestras y eventos por lotes. |
| Precios ESIOS | 1h, y al cambiar de día | Precios PVPC del día. Tras un fallo, reintento cada minuto. |
| Termo y depuradora (Tuya local) | 10s + avisos del relé | Estado y potencia de los relés. Reglas cada 1s. |

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

## Diagnóstico en Google Sheets

Registro detallado para analizar durante unos días el funcionamiento de todo el sistema. Va aparte del registro horario, que sigue igual en la primera pestaña.

| Pestaña | Contenido |
|---------|-----------|
| **Muestras** | Una fila por minuto: red (actual, mínimo y máximo del minuto, para ver los picos), solar, precio, modo, Beny (W, estado, amperios objetivo y reales), termo (estado, W, relé), depuradora (estado, W, relé, horas de hoy, sobrante medio) y salud del StampS3 (heap libre, RSSI WiFi, minutos encendido, temperatura del chip) y temperatura y humedad de la caseta (vacías si el sensor no da una lectura válida). |
| **Eventos** | Arranques (con la causa del reinicio: `panic`, `task_wdt`, `brownout`…), cambios de estado del Beny, decisiones y órdenes del termo y la depuradora, resumen diario de la depuradora, conexiones y desconexiones de los relés y del Huawei, reconexiones WiFi, cambios de modo con el botón y todos los comandos de Telegram. |

- Las pestañas se crean solas, con cabecera, en el primer envío.
- Se envía **por lotes cada 5 min** con un `POST` JSON: una petición TLS por lote en vez de una por minuto, porque cada una para el bucle un par de segundos.
- Si un envío falla, el lote se guarda (hasta 30 muestras y 40 eventos) y se reintenta. Solo cuenta como enviado un **302** de Apps Script: un 200 es la página de error de Google, por ejemplo con una implementación antigua sin `doPost`.
- Los eventos aparecen también en el monitor serie como `EVENTO <tipo>: ...`.
- `/diag` muestra el estado y el último envío; `/diag_on` y `/diag_off` lo activan y lo paran (se guarda en la NVS, clave `diag`). Viene **activado**.

### Exportar a CSV

`GOOGLE_SCRIPT_URL?export=Muestras&key=<EXPORT_KEY>&rows=N` devuelve las últimas N filas (20000 por defecto) en CSV; igual con `export=Eventos`. `EXPORT_KEY` se define en el script. Mientras tenga el valor por defecto, la exportación está desactivada.

> Los datos dejan ver, por ejemplo, cuándo hay gente en casa: la URL del script y la clave no deben publicarse.

### Vigilante (aviso si el StampS3 deja de dar señales)

El script apunta la hora de cada lote recibido (también los vacíos). Un temporizador de Google ejecuta `checkWatchdog` cada 5 min: si llevan **más de 20 min sin llegar datos**, manda un Telegram ("⚠️ El StampS3 no envía datos desde las …"), y otro cuando vuelven. Con `/diag_off` el StampS3 sigue mandando cada 5 min un lote vacío como señal de vida, para que el vigilante no dé falsas alarmas.

Configuración, una sola vez, en el editor de Apps Script:

1. *Configuración del proyecto → Propiedades de la secuencia de comandos*: añadir `TELEGRAM_TOKEN` y `TELEGRAM_CHAT` (los mismos `BOT_TOKEN` y `CHAT_ID` de `config.h`). Van ahí y no en el código, para que el token no acabe en el repositorio.
2. Ejecutar `setupWatchdog` (instala el temporizador; Google pide permisos) y, para probar, `testTelegram`.

### Desplegar el script

En la hoja de cálculo, *Extensiones → Apps Script* (o desde [script.google.com/home](https://script.google.com/home)): pegar `google_apps_script.js`, cambiar `EXPORT_KEY`, guardar y en *Implementar → Gestionar implementaciones* editar la implementación existente con **Versión: nueva versión**. Así la URL no cambia. Una implementación nueva tendría otra URL.

## Hardware Necesario

- **M5Stack StampS3** (ESP32-S3FN8, 8 MB de flash, sin PSRAM; LED RGB en G21 y botón en G0). Sin reloj con pila: la hora sale de NTP al arrancar. Funciona a **160 MHz** en vez de 240 (`board_build.f_cpu`), para que se caliente y consuma menos una vez encapsulado; la WiFi se mantiene siempre despierta (`WiFi.setSleep(false)`) porque el ahorro de la radio retrasaría las respuestas del Beny y del Huawei.
- **Cargador Beny** con interfaz de red UDP (puerto 3333)
- **Inversor Solar Huawei** con Smart Meter Modbus TCP (puerto 502)
- **Relé Tongou TO-Q-SY1-JWT** (Tuya WiFi, carril DIN, con medición) delante del termo eléctrico
- Otro **Tongou TO-Q-SY1-JWT** delante de la depuradora y el clorador de la piscina
- **Red WiFi** con acceso a Internet (para Telegram, ESIOS, Google Sheets)
- **Alimentación fiable para el StampS3** (cargador de calidad, 5 V / 2 A): los apagones del StampS3 dejan el coche sin control (los relés se protegen solos con su seguro)

> ⚠️ **El inversor Huawei solo admite un cliente Modbus TCP a la vez.** Si hay otro equipo conectado (por ejemplo el M5StickC antiguo, Home Assistant…), el StampS3 conecta y el inversor corta la conexión al instante: en el log aparece `Connected successfully!` → `Lost Connection` y errores `0xE4`, y red y solar se quedan a 0.

## Configuración Inicial

1. **Copiar `src/config.h.example` a `include/config.h`** y rellenar tus credenciales (`include/config.h` está en `.gitignore`):
   - `WIFI_SSID` / `WIFI_PASSWORD` — Red WiFi.
   - `BOT_TOKEN` / `CHAT_ID` — Token del bot de Telegram y tu Chat ID.
   - `BENY_IP` / `BENY_PIN` / `BENY_SERIAL` — Datos del cargador Beny.
   - `INVERTER_IP` — IP del inversor Huawei.
   - `ESIOS_TOKEN` — Token de la API de ESIOS (REE).
   - `GOOGLE_SCRIPT_URL` — URL del Google Apps Script desplegado.
   - `TERMO_IP` / `TERMO_LOCAL_KEY` — Relé del termo (ver [Control local](#control-local-tuya-35)).
   - `TERMO_MAX_PRICE`, `TERMO_DEFAULT_POWER`, `CONTRACTED_POWER` — Umbral de precio, potencia del termo hasta medirla y potencia contratada.
   - `PISCINA_IP` / `PISCINA_LOCAL_KEY` / `PISCINA_POWER` / `PISCINA_MAX_HOURS` / `PISCINA_MIN_HOURS` — Relé de la depuradora, su potencia y las horas por mes.
   - `OTA_HOSTNAME` / `OTA_PASSWORD` — Actualización por WiFi. La misma contraseña va en `secrets.ini` (ver [Actualizar el firmware](#actualizar-el-firmware-ota)).

2. **Compilar y cargar** con PlatformIO (por USB — no hay actualización OTA):
   ```bash
   pio run -e stamps3 -t upload
   ```
   El StampS3 se programa por el USB nativo del ESP32-S3 (aparece como *Dispositivo serie USB*). Si no aparece ningún puerto — típicamente la primera vez, con el firmware de fábrica —, mantén pulsado su botón (**G0**) mientras conectas el cable para entrar en modo descarga.

   > Abrir el puerto serie (monitor) puede **reiniciar** el StampS3 por las líneas DTR/RTS del USB; en el log aparece como reinicio `desconocido`. Sin el PC conectado no pasa.

   Las siguientes veces se puede actualizar **por WiFi**: ver [Actualizar el firmware](#actualizar-el-firmware-ota).

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
| `t_mode` | int | Modo del termo (0=AUTO, 1=ON, 2=OFF). |
| `t_price` | float | Umbral de precio del termo (€/kWh). |
| `p_mode` | int | Modo de la depuradora (0=AUTO, 1=ON, 2=OFF). |
| `p_day`, `p_mon`, `p_run`, `p_def` | int | Día en curso, su mes, segundos de depuradora hoy y segundos pendientes de completar. |
| `p_max0`…`p_max11`, `p_min0`…`p_min11` | float | Horas máximas y mínimas por mes, si se cambiaron por Telegram. |
| `diag` | bool | Diagnóstico en Google Sheets activado. |

En un equipo recién programado la NVS está vacía y el arranque muestra `nvs_open failed: NOT_FOUND`: es normal, se usan los valores por defecto hasta el primer guardado.

Las claves `t_pause`, `t_resume` y `r_margin` de la pausa automática ya no se leen ni se escriben. Quedan huérfanas en la NVS de los equipos actualizados, sin efecto.

## Actualizar el Firmware (OTA)

El StampS3 se actualiza **por WiFi**, sin cable, desde el PC y en la misma red:

```bash
pio run -e stamps3_ota -t upload
```

1. Compila y envía el firmware a `192.168.86.41` (IP fija en el router, `upload_port` en `platformio.ini`). Tarda ~1 min.
2. Durante la transferencia el **LED parpadea en morado** y el control se pausa: el Beny se queda en el último amperaje y los relés conservan su [seguro](#seguro-si-el-stamps3-deja-de-funcionar).
3. Se reinicia solo y avisa por Telegram con la **fecha de compilación** del firmware nuevo (`/version` también la muestra).

Detalles:

- **Contraseña**: `OTA_PASSWORD` en `include/config.h` y la misma en `secrets.ini` (`upload_flags = --auth=...` del entorno `stamps3_ota`, que `platformio.ini` carga con `extra_configs`). Ninguno de los dos se sube a git: sin la contraseña nadie de la red puede cargarle otro firmware.
- **La primera vez** que se instala un firmware con OTA tiene que ser por USB (`pio run -e stamps3 -t upload`), porque el anterior no sabe recibir por WiFi.
- **Si la transferencia falla a medias**, el StampS3 sigue con el firmware anterior: el nuevo se escribe en la otra ranura de la flash y solo se activa al terminar bien.
- **Windows** puede pedir permiso en el firewall la primera vez: el PC abre un puerto al que el StampS3 se conecta para descargar el firmware.
- **Último recurso**, si un firmware nuevo no arrancase: por USB, como la primera vez. Si no aparece el puerto, mantener pulsado el botón (G0) al conectar.

`/help` en Telegram incluye estos pasos.

## Particiones de Flash

La placa `m5stack-stamps3` usa `default_8MB.csv`: **dos ranuras de aplicación de 3,3 MB** (`app0`/`app1`), que es lo que necesita la OTA para escribir el firmware nuevo sin tocar el que está funcionando. El firmware ocupa en torno al **30 %** de una ranura, con margen holgado para crecer.

`platformio.ini` activa además `-DARDUINO_USB_CDC_ON_BOOT=1`; sin esa opción, `Serial` no sale por el USB nativo del ESP32-S3 y el monitor serie queda mudo.

## Licencia

Proyecto personal. Uso bajo tu propia responsabilidad.
