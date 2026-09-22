# WORKLOG — EMG8 Bracelet

Bitácora de continuidad entre sesiones. Se anexan entradas, nunca se reescribe
el historial. La memoria durable de hechos vive en `memory/`; esto guarda el
relato. Ver también `improvements-workplan.md` (estado de bugs abiertos) y
`README.md` (protocolo autoritativo).

---

## 2026-08-27 — Channel-attribution fixed (single-shot), sensor-test mode, WiFi/UDP

**Hecho:**

- **Root-caused and fixed the channel-misattribution bug** that made raw EMG
  and envelope values swap columns at random (`374ca06`). Cause was *not* a
  fixed off-by-one: on the ADS1015 the input MUX only changes *after* the
  in-progress conversion completes, so a config write lands before or after
  that internal boundary depending on I2C timing — one stale conversion or
  none, unpredictably. An intermediate "discard one after each switch" fix
  (`7807df6`) wins that race only ~half the time and was **not** sufficient.
  Now uses **single-shot triggering**: each conversion is explicitly requested
  for a named channel, so the result can only belong to it. Confirmed as TI's
  own recommendation for frequent channel swapping.
  ⚠️ **Recordings made before `374ca06` have unreliable channel attribution
  and must not be used for analysis.**
- Earlier fixes this cycle: mixed-rate scheduler lockup that froze raw EMG
  ~17 ms into All mode (`1ffe73c`); SD file truncation on every pause/resume
  (`8cddee2`); mode-switch leaving a zombie recording state (`6216cd3`);
  non-functional ADC presence detection (`1db56f3`, two bugs cancelling out);
  ISR-time sample timestamps (`66045cb`); build at -O2 / 240 MHz / 8 MB flash
  (`675e99b`).
- **I2C driver migrated** from legacy `i2c_master_cmd_begin` to the new
  `i2c_master` API with cached device handles, plus one event-driven service
  task per I2C bus instead of a single polling task (`c35c779`).
- **WiFi SoftAP + full-rate UDP streaming** added, off by default, `W1`/`W0`
  (`1577192`). SoftAP `EMG8-<MAC>` / `emg8sense` / `192.168.4.1:3333`.
  Required a custom `partitions.csv` (3 MB app) — note `board_build.partitions`
  in `platformio.ini` is what actually works; the sdkconfig `singleapp_large`
  route was silently ignored by PlatformIO's partition generator.
- **Sensor-test mode (`4`)** added for per-electrode bench checks (`2ad6518`):
  one sEMG sensor at a time at the chip's full rate (~2400 Hz vs ~1100 Hz in
  All mode), selected with `S0`–`S7`, acknowledged as `#SENSOR:<n>,<adc>,<ch>`.
- `#CNT` extended to 7 fields: per-channel counts plus per-ADC I2C-error and
  stall-retrigger counters, so bus trouble is visible instead of silent.
- **Flashed and verified**: 939936 bytes written, `Hash of data verified`,
  hard reset OK. Binary was string-checked before flashing (`#SENSOR:%u,%u,%u`
  and `s%u_adc%u_%u` both present) per the build-artifact rule.

**Artefactos:**

- `.pio/build/esp32-s3-devkitc-1/firmware.bin` — 939936 B, built 2026-08-27
  07:18, **this is the build currently on the device**.
- `.pio/build/esp32-s3-devkitc-1/partitions.bin` — custom 3 MB app partition.
- Docs: `README.md` (authoritative protocol), `improvements-workplan.md`
  (resolved vs open), `lib/sensors-library/CHANGELOG.md` (driver API changes,
  incl. the breaking `ConversionCallback` signature change).
- Cross-repo sync doc for the datalogger agent:
  `D:\PhD\Code\IA-Arm_datalogger\README-emg8.md`.

**Pendiente:**

- **Hardware verification of the single-shot fix is the top priority**: raw
  columns should sit steady near ~600 at rest and envelope near 0, with no
  swapping; `#CNT` should keep its 20:1 fast:slow ratio and cross-ADC
  symmetry, with the two new counters at or near 0.
- Sensor-test mode (`4` / `S<n>`) is flashed but **not yet hardware-tested**.
- Open items in `improvements-workplan.md`: cross-ADC sync (post-hoc
  resampling preferred), semaphore-timeout logging, file-transfer task
  isolation, SD sequence numbers.
- A sub-millisecond window exists at each `S<n>` switch where one D-line can
  read the new sensor's slot before `#SENSOR:` is emitted. Carries stale/zero
  data; deliberately left alone rather than adding complexity. Documented to
  the datalogger agent so it isn't chased as a parser bug.

**Sin commitear:**

- `src/main.cpp`, `README.md` — sensor-protocol hardening: CSV header now
  reprints when `S<n>` changes the sensor mid-recording (it previously went
  stale and mislabeled the column, same failure class as the bug fixed above);
  `#SENSOR:` also emitted on entering mode `4`; `#ERR:SENSOR` on a malformed
  selection. **Built clean but NOT flashed** — the device still runs the
  previous build, which has the stale-header bug. Daniel is reviewing before
  committing.
- This `WORKLOG.md` itself (new file).

---

## 2026-08-27 (cont.) — Sensor-test bugs found by hardware testing, fixed + flashed

Follow-up within the same day. The datalogger session exercised sensor-test
mode against real hardware and surfaced three defects in code written earlier
today. All fixed and flashed.

**Hecho:**

- **`countdown()` was corrupting multi-byte commands.** It read UART bytes and
  compared them raw, aborting on any `'0'`. So the `'0'` inside `S0` aborted
  the recording instead of selecting sensor 0 (`#CD:ABORT` + `#STOP`,
  reproduced on hardware), and — worse, found while checking that — the `'0'`
  inside `L0,1` did the same. The datalogger sends `1` then `L0,1`
  back-to-back on every scripted test start, so that path was aborting
  **deterministically**, not intermittently. Fixed by routing countdown bytes
  through `feedUartByte()`, the same parser the command loops use: `L`/`G`
  lines are consumed whole, so only a genuinely standalone `'0'` can reach the
  abort test. Scope is countdown-window-only; `F`/`G`/`?` outside that window
  were never affected.
- **Single-shot round-robin stalled permanently after stop/start churn.**
  Evidence: `#CNT` showed all-zero per-channel counts with 95–668 retriggers
  and `i2c_err = 0` on every ADC — i.e. trigger writes landing but ALERT/RDY
  never pulsing. Two causes, both fixed in `lib/sensors-library` (see its
  CHANGELOG `[0.11.0]`): `stopContinuous()` disables the comparator while the
  `ThreshLow`/`ThreshHigh` conversion-ready registers were only written once at
  boot (now rewritten on every start); and an unsynchronised race between
  `serviceConversion()`/`retriggerIfStalled()` on the ADC service task and
  `startMixedContinuousExternal()`/`stopContinuous()` called from the main
  loop on the other core (now serialized by a per-instance mutex).
- Also flashed from the earlier unflashed batch: CSV header now reprints when
  `S<n>` changes sensor mid-recording (it previously went stale and mislabeled
  the column); `#SENSOR:` emitted on entering mode `4`; `#ERR:SENSOR` on a
  malformed selection, so silence now genuinely means "not received".

**Artefactos:**

- `.pio/build/esp32-s3-devkitc-1/firmware.bin` — 940352 B, `Hash of data
  verified`, **this is now the build on the device**. String-checked before
  flashing (`#ERR:SENSOR`, `#SENSOR:%u,%u,%u`, `s%u_adc%u_%u` all present).

**Pendiente:**

- **Re-test needed to confirm the stall fix**: rerun the `S0`–`S7` sweep and
  check `#CNT` — retriggers should drop to ~0 and per-channel counts should be
  non-zero. If retriggers persist there is a third mechanism not yet found.
- Confirm scripted-test starts now reach `#REC` instead of `#CD:ABORT`.
- The single-shot channel-attribution fix itself is **still not
  hardware-verified** for the main All/Raw/Env acquisition path — that remains
  the highest-value outstanding check.
- Open items unchanged in `improvements-workplan.md`.

**Sin commitear:** nothing — Daniel committed this work as `442d143`, then the
mutex revert as `9c501d1`.

---

## 2026-08-27 (end of day) — Mutex reverted, acquisition verified healthy

**Hecho:**

- **Reverted the ADC mutex and ALERT/RDY re-arm** (`9c501d1`, Daniel). They were
  added to explain the sensor-test stall, fixed nothing, and were suspected of
  destabilising sampling. The driver is back to the `2ad6518` state — the
  firmware that was on the device when the day's testing began.
- **Acquisition verified healthy on hardware by Daniel**: all 8 raw and all 8
  envelope channels reading correctly in All mode. Independently corroborated
  by a 10 s `#CNT` capture: 5541–5543 fast / 277 slow per ADC (exactly the 20:1
  divider ratio), symmetric across all four, with **zero** retriggers and
  **zero** I2C errors. Single-shot channel attribution — the whole point of the
  2026-07-20 work — is confirmed correct.
- Measured rates supersede earlier estimates: **~554 Hz/ch raw, ~28 Hz/ch
  envelope** in All mode at 3300 SPS. README updated.
- Kept (non-sampling, hardware-verified fixes): `countdown()` now routes bytes
  through the real parser, so `L0,1` sent right after a mode command no longer
  aborts the countdown — that had been killing scripted-test starts
  deterministically; FatFs long filenames enabled; `G` resolves against the
  same base `F` lists; SD directory/open failures reported instead of silent.

**A correction worth recording:** mid-session I claimed from sensor-test stall
data that *all* acquisition might be dead and pushed for a full revert to
free-running continuous mode. That was wrong — All mode was healthy the whole
time. Daniel's instinct to protect the known-good base was right. The lesson:
a stall confined to one code path was over-generalised without testing the
path that actually mattered; the All-mode `#CNT` check that settled it took
30 seconds and should have come first.

**Pendiente:**

- **Sensor-test mode (`4` / `S<n>`) is present but NOT FUNCTIONAL.** Commands
  ack correctly; no data is delivered. Zero conversions with retriggers
  climbing ~100/s — triggers issue, conversion-complete interrupt never
  returns. Specific to the stop/start cycle `S<n>` performs; not the mutex.
  Flagged prominently in README so nobody builds against it. Either fix it or
  remove mode 4 — leaving it advertised-but-broken is the worst of both.
- `G` (SD download) is fixed but **untested since the fix**. Note the behaviour
  change: session directories are now created for real, so recordings land in
  `s_<MAC>_<epoch>/` rather than the card root.
- WiFi/UDP streaming still not exercised end-to-end.
- Open items unchanged in `improvements-workplan.md`.

**Sin commitear:** `README.md`, `WORKLOG.md`,
`lib/sensors-library/CHANGELOG.md` — documentation accuracy pass only, no code.

**Coordinación:** a second Claude session (`ia-arm-datalogger-55`) owns
`D:\PhD\Code\IA-Arm_datalogger`; this session is firmware-only from
2026-08-27 onward. Note that some of that session's firmware work
(`startRecording()` refactor, button ISR / reed guard, `sendTrigger` /
`sendStartToSlave`) was swept into commits `4015ea4` and `374ca06` here by a
`git add -A` — nothing lost, but attribution is muddled. Ping before crossing
repos.

## 2026-09-02 — Panico en la ISR de SPI durante grabacion: aislado y corregido

**Hecho:** Las grabaciones morian con un `Guru Meditation Error (LoadProhibited)`
a los 5-41 s. Reproducible con la app nueva, con la vieja y con
`serial_capture.py`, es decir independiente del anfitrion. `#BOOT:` lo confirmo
como `reset=PANIC(4)`, no brownout ni watchdog.

Backtrace decodificado contra el ELF de la build:

    bg_exit_core          spi_bus_lock.c:554     <- deref nulo, EXCVADDR 0x8
    spi_bus_lock_bg_exit  spi_bus_lock.c:778
    spi_intr              spi_master.c:1027
    _xt_lowint1 / tarea idle

**Aislamiento**, midiendo en hardware (`capture_health.py`, 100-150 s por corrida):

| modo | tarea IMU | resultado |
|---|---|---|
| 1 (All) | activa | panico a los 41 s |
| 2 (Raw) | activa | sobrevivio 97 s |
| 3 (Env) | activa | panico a los 23 s |
| 1 (All) | **desactivada** | **sobrevivio 107 s** |
| 1 (All) | activa, **con el fix** | **sobrevivio 147 s** |

Descartados por medicion, no por intuicion: fuga de memoria (heap plano en
~171 kB), contrapresion de colas (picos muy por debajo del fondo, cero
descartes), volumen de escritura en SD (el modo 2 escribe mas y no falla),
y DTR/RTS del anfitrion (falla con las lineas afirmadas y sin afirmar).

**Causa:** `SPI::write/read/transfer` envolvian un `spi_device_transmit()` —la
llamada por interrupcion— dentro de `spi_device_acquire_bus()`/`release_bus()`.
ESP-IDF espera transacciones *polling* mientras el bus esta adquirido; mezclar
las dos deja inconsistente la invariante `acquiring_dev` / `acq_dev_bg_active`
que `bg_exit_core()` lee desde la ISR. Con el IMU transfiriendo a ~100 Hz
durante toda la grabacion, tarde o temprano la ISR entraba con el lock en un
estado imposible.

**Corregido** en `cdf8608`: `spi_device_polling_transmit()`, que es el
emparejamiento documentado y ademas saca a estos dispositivos de la ruta de ISR.
Solo lo usan los drivers de IMU (ICM42605, LSM6DSOX); la SD va por `sdspi_host`
y no se toca.

**Artefactos:** `cdf8608` (fix), `65aafdf` (diagnosticos `#BOOT`/`#HEALTH` y el
interruptor de compilacion `EMG8_NO_IMU_TASK`, que ya estaban sin commitear en
el arbol). Herramientas del lado del anfitrion en
`D:\PhD\Code\IA-Arm_Monitorackend	ools\`: `capture_health.py`,
`crash_bisect.py`, `probe_device.py`.

**Pendiente:**
- Corrida larga (>10 min) para confirmar que no queda una cola mas lenta.
- El modo 2 subio de 67 a 300 de pico en la cola de crudo tras el fix: el
  camino de ADC esta rindiendo mas, sin descartes. Vale la pena volver a medir
  `#CNT` por canal contra la tasa nominal.
- UDP a tasa completa sigue sin ejercitarse contra hardware.
- `gpio_install_isr_service(502): already installed` sigue apareciendo 4 veces
  al arrancar una grabacion. Inofensivo hasta donde se ve, pero ahora que hubo
  un fallo relacionado con ISR conviene mirarlo.
- Modo 4 sigue sin entregar datos.

**Sin commitear:** nada; el arbol quedo limpio en `wireless-testing`.

**Coordinacion:** este trabajo lo hizo la sesion que tiene `IA-Arm_Monitor`,
cruzando a este repo con autorizacion explicita de Daniel y solo sobre la rama
`wireless-testing`. Sin `git add -A`: los dos commits listan sus rutas.


## 2026-09-02 (mas tarde) — UDP validado contra hardware por primera vez, y el UART fuera del camino de datos

**Hecho:** Con el panico de SPI ya corregido, se ejercito el flujo completo
serial -> UDP contra el equipo. Nunca se habia hecho.

El anfitrion hace la secuencia solo (`--source auto`): abre el puerto, `?`,
`W1`, espera `#WIFI:1`, suscribe UDP contra 192.168.4.1:3333 y promueve el
enlace a `streaming` en cuanto llega el primer paquete. Si el PC todavia no se
unio a la red del brazalete lo dice con esas palabras y reintenta cada 20 s.

**Resultado, modo 1, medido en hardware:**

|  | UART (lineas D) | UDP |
|---|---|---|
| registros crudos / 40 s | 12 632 | 128 786 |
| tasa medida | 47.6 Hz | 511 Hz agregada |
| perdida | — | 6 de 2020 paquetes (0.3 %) |

**Despues** de bajar la linea CSV a 1 Hz mientras el UDP entrega (`72d0a26`):

    trafico UART      5700 B/s  ->  138 B/s      (41x menos)
    crudo por canal    ~510 Hz  ->  ~885 Hz      (+74 %)
    envolvente          ~25 Hz  ->   ~44 Hz
    perdida de paquetes   0.3 % ->   0.05 %

El aumento de tasa no era el objetivo y es lo mas interesante: `uartTask`
estaba dejando sin CPU a las tareas de servicio de los ADC. Confirmado con los
contadores del propio equipo tras un stop limpio, sin depender del anfitrion:

    #CNT:1,63769,3188,63769,3188,0,0    razon rapido/lento exactamente 20.0
    #CNT:2,61463,3073,61463,3073,0,0    i2c_err = 0, retrig = 0 en los cuatro
    #CNT:3,61461,3073,61460,3073,0,0
    #CNT:4,63280,3164,63280,3163,0,0

499 946 conversiones en el equipo, 495 751 recibidas por el anfitrion
(99.16 %), cero desbordamientos de anillo.

**Hallazgo del protocolo:** `#NET:<ip>:<port>` NO anuncia el extremo del
brazalete, sino el del cliente que acaba de suscribirse
(`net_stream.cpp:123` imprime la direccion de origen del datagrama).
Verificado: suscribiendose desde 192.168.4.2 el equipo respondio
`#NET:192.168.4.2:54400`. Sigue sin haber forma de preguntarle al equipo cual
es su propia IP; queda como pedido P0 en `FIRMWARE-CONTRACT.md` del proyecto
del monitor.

**Pendiente:**
- El estimador de tasa del IMU reporta ~3.8 kHz con 35 % de huecos: los
  `ts_us` del IMU no se comportan como los del EMG. Revisar si
  `recordingTimestampUs()` se muestrea bien en `imuTask`. El camino de sEMG no
  se ve afectado.
- Modo estacion sigue siendo el pedido grande: el SoftAP obliga al PC a dejar
  su red, y eso impide que el enlace sea del todo automatico.
- Corrida larga (>10 min) con UDP para confirmar estabilidad.
- Modo 4 sigue sin entregar datos.

**Sin commitear:** nada.



## 2026-09-02 (correccion) — el +74 % de tasa no se sostiene

**Correccion a `72d0a26`.** Ese commit afirma que silenciar la linea CSV subio
la tasa de ~510 a ~885 Hz/canal (+74 %). La medicion era real —la confirmo el
`#CNT` del equipo— pero **no se reproduce**, y presentarla como resultado
estable fue un error.

Matriz controlada (50 s por condicion, medida siempre con `#CNT` dividido por
el tiempo entre `#REC` y `#STOP`, no con contadores del anfitrion):

| condicion | crudo Hz/canal | lineas D |
|---|---|---|
| radio apagada, D a ~48 Hz | 463.5 | 47.7 Hz |
| radio encendida, sin suscriptor, D a 1 Hz | 467.1 - 517.4 | 1.0 Hz |
| radio encendida + suscrito + transmitiendo | **727.9** | 1.0 Hz |

Lo que se puede afirmar:

- **Silenciar el UART vale ~+12 %**, no +74 %. Real, pero modesto.
- **Dos corridas de la misma condicion difieren ~2 %**, asi que la medida es
  repetible y las diferencias de arriba estan fuera del ruido.
- **Con UDP transmitiendo de verdad la tasa sube mucho (+56 % sobre la misma
  condicion sin suscriptor).** Es al reves de lo esperado: mandar mas datos
  deberia costar CPU, no regalarla. No hay explicacion todavia.
- **No es escalado de frecuencia de CPU**: `CONFIG_PM_ENABLE` no esta activo y
  la CPU esta fijada a 240 MHz.
- `i2c_err` y `retrig` en 0 en todas las condiciones.

**Siguiente diagnostico, en orden de costo:**
1. `esp_wifi_set_ps(WIFI_PS_NONE)` — una linea. El firmware no llama a
   `esp_wifi_set_ps`, asi que corre con el ahorro de energia por defecto
   (`WIFI_PS_MIN_MODEM`). Si las transiciones de modem-sleep estan retrasando
   la ISR de DRDY o la tarea de I2C, esto lo aplana.
2. Alternar w1_sub / w1_nosub varias veces para descartar deriva.
3. Contar en `adcBusTask` los despertares por cola frente a los por timeout:
   distingue "tarea sin CPU" de "flanco DRDY perdido".

Hasta que eso se entienda, la tasa util del equipo es **~460-520 Hz/canal**, y
los 885 Hz quedan como un dato aislado sin reproducir.

**Sin commitear:** nada.

## 2026-09-02 — Modo UDP-solo: `U0` / `U1`, y lo que NO arregla

**Hecho:** el equipo puede dejar de escribir por el UART entero mientras
entrega por UDP, y volver por tres caminos distintos.

`U0` / `U1` (dos bytes, sin salto de linea, como `V0` / `W1`), aceptados por
UART y por UDP. Acusan `#UART:0` / `#UART:1`.

Lo que calla `U0`:

- los ~77 `printf` de `main.cpp`, redirigidos con una sola linea
  (`static int hostPrintf(...)` + `#define printf hostPrintf`, tras los
  includes). Un gancho, ningun sitio de llamada olvidado;
- los logs del propio ESP-IDF (`esp_log_level_set("*", ESP_LOG_NONE)`), que no
  pasan por `printf`;
- el armado de la linea `D` en `uartTask`: se salta el bloque entero, no solo
  la impresion.

Lo que **no** toca es la recepcion del UART. Es deliberado: `U1` tiene que
funcionar a ciegas. Tres vias de vuelta, para que el equipo no pueda quedar
mudo y sordo a la vez:

1. el anfitrion manda `U1` solo si el UDP se cae o si va a descargar de la SD
   (`link.py`, `_restore_uart`);
2. el equipo se destapa solo tras 20 s sin datagramas del cliente
   (`kQuietWatchdogMs`) y avisa `#UART:1,watchdog`;
3. siempre queda `U1` a ciegas por el UART, o un reset por RTS.

Ademas `netStreamStop()` (o sea `W0`) devuelve el UART —sin radio no quedaria
por donde hablar— y el manejador de `G` se destapa solo, porque el cuerpo del
archivo sale por `uart_write_bytes`, que no pasa por `hostPrintf`.

El canal de comandos por UDP salio casi gratis: `pollSubscribe()` ya hacia un
`recvfrom` para aprender la direccion del cliente y tiraba el contenido. Ahora
lo que no sea el `HI` de suscripcion va a un manejador registrado
(`netSetCommandHandler`). Solo acepta `U0`/`U1`: son escrituras atomicas de una
bandera, sin efectos colaterales, y corren en la tarea de red. El resto del
juego de comandos sigue por UART, que nunca deja de escuchar.

**Verificado en hardware** (8 comprobaciones, todas pasan):
`?` contesta · `U0` acusa y luego 0 bytes en 3 s · sigue mudo ante `?` · `U1` a
ciegas lo recupera · idem con la radio encendida · **`U1` por UDP devuelve la
consola** · el watchdog de 20 s se dispara solo.

**Lo que NO arregla: la tasa.** La hipotesis era que el UART se estaba comiendo
las conversiones. No es eso, y ahora hay con que afirmarlo.

Primero, tres pares de 60 s (`#CNT`, `w1_sub` frente a `w1_quiet`):

| par | `w1_sub` | `w1_quiet` | cambio |
|---|---|---|---|
| 1 | 481.7 | **881.2** | +82.9 % |
| 2 | 460.0 | 524.5 | +14.0 % |
| 3 | 456.4 | 452.0 | −1.0 % |

`D 0.0 Hz` en las tres filas calladas: el silencio es real. Pero la dispersion
*dentro* de la condicion callada se come la diferencia.

Lo que lo cierra: **ocho corridas identicas seguidas**, misma condicion, sin
tocar nada entre una y otra:

```
808.4 · 463.8 · 461.0 · 462.2 · 455.9 · 459.8 · 460.1 · 459.3   Hz/canal
```

La tasa es **bimodal**: o ~460 Hz/canal o ~800-880 Hz/canal, un factor de casi
dos. Dentro del modo lento es repetible al ±1 % (455.9 a 463.8 en siete
corridas). Por eso cada comparacion pareada parecia concluyente: ganaba el lado
que hubiera caido en el modo rapido. El UART nunca fue la variable.

Cronologicamente, las 18 medidas de hoy —lento, RAPIDO, lento, 524, lento, lento /
RAPIDO ×4 / RAPIDO, lento ×7— no dan un patron limpio con ninguna de las
condiciones probadas. Las cuatro rapidas seguidas fueron la corrida en la que
el portatil habia perdido la asociacion y no llegaba ni un datagrama, asi que
tampoco es "mandar por UDP lo acelera".

**Un resultado util que si sale de esto:** en el modo rapido el anfitrion
recibe lo que el equipo convierte. 808.4 Hz/canal convertidos, **807.5
Hz/canal recibidos** por UDP (99.4 a 99.8 % en las dos modalidades). La ruta
UDP aguanta 800 Hz/canal de punta a punta; si el firmware llega a esa tasa de
forma estable, el anfitrion no es el cuello de botella.

**Artefactos:**
- `.pio/build/esp32-s3-devkitc-1/firmware.bin` (942 176 B), verificado con
  `grep -aoF` que contiene `#UART:0`, `#UART:1` y `#UART:1,watchdog` antes de
  grabarlo. Es lo que corre en el equipo ahora.

**Pendiente:**
- La variacion de tasa sigue sin explicacion. El siguiente diagnostico mas
  barato sigue siendo `esp_wifi_set_ps(WIFI_PS_NONE)`: el firmware nunca llama
  a `esp_wifi_set_ps`, asi que corre con `WIFI_PS_MIN_MODEM`.
- Detalle menor: `U1` deja el nivel de log global en `ESP_LOG_INFO`, asi que
  pisa el `esp_log_level_set("ICM42605", ESP_LOG_DEBUG)` de arranque. Sin
  efecto practico (ese driver no registra nada en caliente), pero esta ahi.

**Sin commitear:** nada.

## 2026-09-02 (bis) — El mapa crudo/envolvente no es el mismo en los cuatro ADC

**Hecho:** `kRawCh`/`kEnvCh` por ADC, lista de barrido armada por ADC, y
`isRawCh(id, ch)` decidiendo rapido/lento en `onSample`. La cabecera `H` gana
un sufijo `r`/`e` por columna.

Lo que suponia este firmware —crudo en ch0/ch2, envolvente en ch1/ch3, igual en
los cuatro— no es lo que hay en la placa:

| ADC | crudo | envolvente |
|---|---|---|
| 1 | 0, 3 | 1, 2 |
| 2 | 1, 3 | 0, 2 |
| 3 | 0, 3 | 1, 2 |
| 4 | 1, 3 | 0, 2 |

`ch2`/`ch3` intercambiados en los cuatro, y `ch0`/`ch1` ademas en los ADC 2 y 4
— los pares 1/3 y 2/4 estan en espejo.

**Como se midio:** captura por UDP muestra a muestra, modo 2 y modo 3 por
separado para que cada patilla corriera a tasa completa (el firmware quita el
divisor cuando no hay canales rapidos). Cada canal salio >=97.9 % en un solo
nivel y doce de dieciseis al 100 %: es estable, no una carrera de mux. La
bateria del sensor cargada, que es lo que pone el pedestal crudo en ~605.

**Por que importaba mas que una etiqueta:** con el mapa anterior, seis de los
ocho electrodos tenian su señal cruda entrando por una patilla que se
muestreaba con el divisor /20 a ~23 Hz, y su envolvente a ~460 Hz. Y cada
muestra iba al fichero equivocado en la tarjeta (R.bin frente a E.bin) y al
tipo de paquete UDP equivocado.

**Verificado tras grabar:** modo 2 entrega los ocho canales crudos a ~605 y
modo 3 los ocho de envolvente en ~0, ambos al 100 %. Antes eran 2 de 8 y 6 de
8. Tambien desaparece el 1-2 % de mezcla que se veia en el ADC 1.

**Artefactos:** `.pio/build/esp32-s3-devkitc-1/firmware.bin` (942 026 B),
verificado que contiene el formato nuevo de cabecera antes de grabarlo. Es lo
que corre en el equipo.

**Pendiente, para quien revise el esquematico:**
- Confirmar si el espejo de los ADC 2/4 es intencional (rutado) o un error, y
  si `ch2`/`ch3` estan invertidos en la placa o la convencion del firmware
  estaba equivocada desde el principio.
- **La cabecera maestra de la SD no guarda el mapa de canales.** Son 32 bytes
  con `[25-31]` reservados; escribir ahi los cuatro bytes de `kRawCh` haria que
  cada grabacion se explique sola y permitiria distinguir un fichero anterior a
  este arreglo de uno posterior. Compatible hacia atras.
- Toda grabacion anterior a este commit tiene R.bin y E.bin cruzados en seis de
  ocho electrodos. El mapa es determinista, asi que se pueden reinterpretar.
- Transitorios aislados en canales crudos (`adc4_1r`: pico a pico 301 con sd
  7.8). No los explica el cargador de la bateria: sobreviven al desenchufe.

**Sin commitear:** nada.

## 2026-09-05 — Sampling investigation, checkpoint 1 (in progress)

Scope agreed with Daniel: target >=1000 Hz on EACH of the eight raw channels
in All mode, retaining envelope /20, IMU, single-shot attribution and the public
protocols. Work only on `feature/samping-speed`; do not modify IA-Arm_Monitor.
ADCs ARE attached to the bench ESP32; analog sensor inputs are disconnected.
The SD card is faulty: no SD-enabled tests. Mode 4 repair is out of scope.
Work in small commits/checkpoints so usage resets do not lose progress.

Read README, complete prior worklog and improvement plan; audited ADC service,
I2C wrapper, SD queues/writer, network queues, and recording lifecycle. Confirmed
unused SD queues still fill when sdOK=false and no writer task is created.
The monitor's old rate_matrix.py assumes c0=raw on all ADCs, so its calculation
must NOT be reused after the channel-map fix. Leave that repo untouched.

New host tools: tools/bench_acquisition.py (per-channel serial/UDP capture),
tools/test_bench_acquisition.py (decoder fixtures), tools/bench_matrix.py
(resumable 8 x 4 matrix), tools/README.md. Captures and build backups are under
ignored benchmarks/baseline-d4ca979/. Original firmware artifacts/configs saved
with SHA256 manifest before any build/upload. Boot capture confirmed the
running ELF hash prefix 5cfadde14 matches the preserved ELF (full hash in
manifest). Boot app version is f815a21-dirty, compiled Sep 5 17:14:57; do not
infer binary identity from the checkout label alone. No firmware uploaded yet.

Connectivity: Ethernet connected; this bench unit's SSID is
EMG8-24EC4A368770 (NOT the previously saved bracelet profile ending 8790).
Added its Windows WPA2 profile using the README password. Windows netsh needs
elevated execution here. Working shell: cmd.exe, login=false, cwd=C:\Windows;
PowerShell tool calls from the workspace stalled. Python is
C:\Users\escob\.platformio\penv\Scripts\python.exe (pyserial available).
Use command-local `git -c safe.directory=D:/PhD/Code/em8/emg8_bracelet` in the
sandbox; do not change global Git configuration.

Preliminary hardware results, before any firmware modification:
- Earlier short All runs ~460-480 Hz/raw channel; Raw ~490 Hz/channel.
- Env-only run showed ADC1/4 accelerating while ADC2/3 stayed slow, with no
  I2C errors or retriggers. Thus the effect need not be global across ADCs.
- udp-01, 60 s All: raw 443-500 Hz/channel, 100% ADC delivery, zero packet
  gaps, I2C errors, or retriggers.
- quiet-01, 60 s All with U0: raw 812-855 Hz/channel, likewise 100% ADC
  delivery and zero errors/gaps. This is ONE fast run, not proof U0 causes it.
- UDP timestamps show dominant raw intervals near 2 ms and a second population
  near 1 ms on ADC1/4. Need firmware timing instrumentation to locate the delay.

After quiet-01 had stopped cleanly and returned STATUS idle/SD=0, COM9 later
stopped responding. Matrix refused to start (saved off-01 failure). A controlled
RTS reset restored operation; reset-capture.bin preserves ROM/app boot evidence
and subsequent STATUS:0,0,0,1. No cause established for the silence. Do not
mistake this for a measured sampling panic. Reset does not enable recording.

Full baseline matrix is now being collected, with failed attempts retained:

    python -B tools/bench_matrix.py --wifi-profile EMG8-24EC4A368770 --output benchmarks/baseline-d4ca979

It skips completed captures and stops on failure. Each run asserts idle and
SD unavailable, collects 60 s, drains trailing UDP batches, restores UART,
stops acquisition and Wi-Fi, and saves summary + metadata + full UDP + serial.
After interruption, inspect device status and saved results before resuming.

Next: finish/analyze baseline, add opt-in low-overhead ADC timing diagnostics
and an explicitly SD-disabled bench build, then make isolated measured changes:
inactive SD enqueue gate; trigger next conversion before publishing last;
per-bus lifecycle ownership; evaluate core-1 I2C/GPIO interrupt placement;
per-ADC recovery deadlines and acknowledged network shutdown. Preserve all
validated signal mapping/conversion/SPI fixes. No bus clock increase. Existing
1 MHz I2C also needs datasheet qualification (TI high-speed entry requirements).
SD writer broader fixes/physical validation deferred until a working card.
Acceptance remains eight 60 s repeats/condition and a 15 min All-mode UDP run,
>=1000 Hz EACH raw channel over complete 10 s acquisition windows, envelope
ratio intact, no firmware losses/errors/retriggers/resets, measured UDP delivery
>=99.5%. Signal quality cannot be validated with floating analog inputs.

## 2026-09-05   Sampling investigation, checkpoint 2 (diagnostics prepared)

Added opt-in esp32-s3-bench environment: EMG8_NO_SD skips all SD initialization
and EMG8_ADC_TIMING accumulates ADC timing histograms in RAM. No acquisition
ordering, bus frequency, core placement, scheduler, or queue behavior changed.
After stop, #TIMING describes config write, ISR-to-service, conversion read,
publication callback, trigger-to-ready, and ready-to-next-trigger delays.
#ACQ contains per-channel first/last DRDY timestamps and counts; #ADC_EVENTS
counts overflowing event queues and events without a pending conversion.
See tools/README.md for precise timing definitions and limitations.

Bench and normal environments both build successfully with ESP-IDF 5.4.0.
Verified bench config: 240 MHz, performance optimization, FreeRTOS 1000 Hz,
PM disabled, app_main core 0. Bench binary contains SD_DISABLED/TIMING/ACQ;
normal binary contains none of these markers. Removed an unused getCurrentDir
call in the F command (its return was never used), which the SD-free build's
constant-null analysis correctly rejected. Existing library warnings remain.
Seven host decoder/diagnostic fixtures pass, including 32-bit timestamp wrap.
Normal build log: benchmarks/diagnostic-normal-build.log.

Original firmware is STILL on COM9 while the eight-repeat baseline continues.
Do not flash/open COM9 while bench_matrix.py owns it. At this checkpoint the
first five repeat groups are in progress. Further baseline observations:
- Wi-Fi on/no subscriber can run fast too: nosub-05 reached 893-922 Hz/raw.
- quiet-02 through quiet-04 were slow (~444-465 Hz), so UART silence is not a
  reproducible fix.
- udp-05 delivered 99.2313% of acquired ADC samples, with 13 raw, 2 envelope,
  and 3 IMU missing packets. Acquisition remained ~443.7 Hz without I2C errors.
  This is genuine reception loss, distinct from the roughly twofold acquisition
  variation. Earlier UDP captures delivered 100%.
- Isolated retriggers occurred in udp-03, quiet-04 and off-05 (one each).
  Preserve them; the existing recovery only checks when both ADCs' queue is idle.
- Host reporting now uses null delivery fraction for conditions with no UDP
  receiver; older captures used 0.0, which was not evidence of failed delivery.

Next: finish baseline; archive/hash diagnostic build, flash ONLY bench
environment, collect timing evidence, then measure isolated fixes. All acceptance
criteria and exclusions from checkpoint 1 remain. No SD-enabled hardware tests.

## 2026-09-06   Baseline complete; diagnostic firmware uploaded

All 32 baseline runs completed (8 x off/nosub/udp/quiet, 60 s each). Aggregate
and full captures: benchmarks/baseline-d4ca979/aggregate.json. Added architecture
review and results table in docs/acquisition-review.md. Raw channel ranges:
off 460.3-640.6, nosub 465.5-922.1, UDP 442.4-675.2, quiet 443.2-855.3 Hz.
Zero I2C errors; seven retriggers total. No condition met the target.
Minimum ADC UDP delivery 97.826% (udp-08), quiet 98.541% (quiet-06).
All firmware network queue drops were zero. Socket-send errors: quiet-06=20,
udp-08=88; other reception-loss runs had zero socket errors. Thus the earlier
zero-socket-error observation was true for those captures, not the full matrix.
Host summary parser now preserves final #NET TX/ERR/DROP counters; tests pass.

Diagnostic image from b3f3612 source archived before upload:
benchmarks/diagnostic-b3f3612/firmware/manifest.json. App bin SHA256
fb0842d944d3c1db6457a0c73b2a51a7c6ce3e2df723038dca43c6e12d032e9c.
ELF SHA256 a6a9b76a038370ff71522025e498975bc369bab7b8fb89236b0c6ccae211557c.
Flashed only app at 0x10000 after asserting idle/SD0; esptool verified flash hash.
Existing compatible bootloader/partitions/NVS preserved. Boot at 115200 shows
ELF a6a9b76a0, app version a363b15-dirty (built immediately before b3f3612 commit).
Application uses 460800 baud. Captures prove #BENCH:SD_DISABLED and ADC/IMU OK,
STATUS idle/SD0. Initial upload script's console printing failed on Windows
encoding after successful upload; separate boot verification passed. Do not
interpret this host Unicode error as a firmware failure.

First four 60-second diagnostic captures are running via bench_matrix.py
--repeat 1 --seconds 60 --output benchmarks/diagnostic-b3f3612. Still no
acquisition optimizations applied. COM9 is owned by this process until it exits.
Timing evidence will determine the first performance changes.

## 2026-09-06   Isolated probe identifies reversed DRDY routing

The first diagnostic off capture measured trigger-to-ISR ~907-919 us,
ISR-to-service 14-23 us, read 111-124 us, callback ~4 us. Later fast captures
had ready timestamps preceding the recorded trigger (unsigned wrap made their
ready means invalid). ADC1/4 and ADC2/3 also tended to have matching counts.
This motivated testing the actual ready wiring before scheduler changes.

Added a bench-only startup probe under EMG8_ADC_TIMING. Before workers or GPIO
handlers start, disable all comparators; trigger exactly one ADC at 3300 SPS;
poll ALL four ready pins for 5 ms; read configuration and conversion registers.
Repeat three times per ADC. Normal build omits this probe. No modes were changed.

Flashed and verified probe ELF eeff83804549127701425b97bf41e7408500c3e68adef499099d38bd2094a5af.
Artifacts, source diff, upload log and boot captures: benchmarks/rdy-probe/.
Every I2C operation succeeded and each config readback was C3C0 as requested.
All 12 trials agreed:
  I2C0 0x48 (ADC1) -> GPIO15, falling edge 389-395 us after trigger start
  I2C0 0x49 (ADC2) -> GPIO42, falling edge 391-396 us
  I2C1 0x48 (ADC3) -> GPIO41, falling edge 382-387 us
  I2C1 0x49 (ADC4) -> GPIO40, falling edge 378-388 us
Current kRDY is {40,41,42,15}: exactly reversed on the attached setup.
Thus each worker is consuming a different chip's ready event. This explains
why ready-event timing cannot be treated as conversion timing with that map.
Do not claim a rate fix until measuring the corrected map.

Asked Daniel whether this setup shares the bracelet's wiring or is separate.
Until clarified, test the measured mapping only in the diagnostic build;
preserve the normal build's existing map. No SD queue/scheduler optimization
has yet been applied. COM9 is idle with SD disabled after the routing probe.

## 2026-09-06   First isolated ready-map correction measurement

Only the diagnostic build now uses measured kRDY={15,42,41,40}; normal default
remains {40,41,42,15} pending the wiring clarification. Boot routing probe
confirms all four ADCs now match their assigned pin in all three trials.
Flashed ELF a0a118b12b679053abeb13bd0c362928c14aa094867cb81752c0cac22828a414;
artifacts/source diff/upload/boot and acquisition captures in benchmarks/rdy-corrected.

First 60 s Wi-Fi-off All capture: every raw channel 898.22-898.25 Hz by device
timestamps, envelopes ~44.91 Hz, zero I2C errors/retriggers/event queue drops or
spurious events. Prior diagnostic off run was ~451.4 Hz. Ready timing is now
379.6-392.3 us, consistent with the isolated probe; read 109.9-117.2 us,
wake 19.2-24.1 us, publication 3.7-4.0 us. No scheduling/queue/bus changes.
Other conditions are being measured; this one run is not final acceptance.

The map fix removes the dominant mismatch, but the raw target still needs
about 11% additional throughput. Measure inactive SD enqueue removal next;
callback time alone suggests that cannot supply the whole improvement.
The remaining read/trigger overhead makes core-local I2C/GPIO handling a
useful controlled comparison after lifecycle ownership is made explicit.

## 2026-09-06   Inactive SD queue gate verified

Gate raw/env/IMU/label storage enqueues on SD availability; keep UDP and label
state independent. sdOK is now atomic because the writer can clear it.
Queue allocation and writer behavior are unchanged. Both builds pass.
Flashed bench ELF f69970d617237d7133b7fe630ceba43dc57c98943185d7b83f49a7305caa587c;
source diff, binaries, boot and captures: benchmarks/sd-gate/.

Two 60 s All runs (off, UDP): all raw/env/IMU storage queue depths and drop
counters remained zero. UDP delivered 100% with zero socket errors/network
queue drops/I2C errors/retriggers. Off rates ~901.6-901.7 Hz/raw; UDP ~836.6-916.8.
Callback ~2.4-2.5 us off, ~4.6-5.1 us UDP (previous ~3.7-4.0 / 6.3-7.3 us).
A small reduction in callback cost, not enough for target acceptance.
Early/stale ready timing can still occur after starts despite the corrected
physical map. Next prioritize worker-owned start/stop and draining stale ready
notifications before publishing/tracing more performance changes.

## 2026-09-06 - Worker-owned lifecycle and ready validation

ADC start/stop now runs on each owning bus worker and main waits for completion.
This serializes configuration with reads and puts stop counters after the last
ADC callback. Stale semaphore/queue notifications are drained between runs.
Start failures stop both workers and report an error instead of reporting REC.
Mode/channel/divider/gain selection remains unchanged.

Ready validation rejects timestamps preceding the trigger and requires the
physical ready pin to be asserted before reading a single-shot result. Trigger
time is captured before the write so a legitimate edge during a delayed write
return is not rejected. New optional ADC_EVENTS fields expose rejected events.
No minimum conversion-time heuristic or extra I2C read was introduced.

Both firmware builds pass and all seven host parser tests pass. Intermediate
lifecycle-only firmware passed six start/stop cycles across All/Raw/Env modes.
Artifacts retain each isolated build: benchmarks/lifecycle, lifecycle-guard,
and lifecycle-ready. Latest flashed ELF SHA256:
f2733fab9a7570183f8d1e45c20c9b6a86eef1435f4c04654d4ceefa83481a85.

Latest 30 s All captures: off ~900.6-900.7 Hz/raw, UDP ~853.4-853.6 Hz/raw.
No I2C errors, retriggers, event queue drops; UDP delivery 100% and zero packet
gaps. Off rejected no events. UDP ADC4 rejected 3 old timestamps and 14,857
notifications with ready inactive. ADC1-3 rejected none. The prior timestamp-only
build accepted very early positive events and showed ADC4 ~962 Hz versus its
partner ~816 Hz; physical-level validation removes that count skew in this run.
The cause of extra ADC4 notifications is unresolved. Its accepted ready minimum
was still 68 us, so do not claim every ISR timestamp is a clean conversion edge;
an old notification can be serviced after the real ready level arrives.

Next isolated comparison: allocate I2C and shared GPIO interrupts on core 1,
where the acquisition workers execute. Current trigger/read overhead remains
about 95/110-119 us off and 103/119-123 us UDP. Normal ready mapping remains
unchanged pending confirmation of bench-versus-bracelet wiring. No SD tests.

## 2026-09-06 - Interrupt placement comparisons (not retained)

Two isolated SD-free experiments built normally and as bench firmware; both
boot identities and routing probes passed. Each ran 30 s All with Wi-Fi off
and with UDP, zero I2C errors/retriggers, and 100% ADC UDP delivery.

- All acquisition I2C/GPIO interrupts on core 1, workers still core 1:
  off ~865.6 Hz/raw, UDP ~814.1-814.6. Trigger/read overhead increased.
  Artifacts: benchmarks/core1; ELF
  d00cbc7f7f3e3fe459887f198a0dc629b7b3c50f55a3434e38ca3755caa62898.
- Split bus workers/I2C interrupts across cores, GPIO on core 1:
  off bus0 ~908.4, bus1 ~920.1; UDP bus0 ~847.8, bus1 ~888.5 Hz/raw.
  Artifacts: benchmarks/split-core; ELF
  1548c6efb87766c4a5962161fa9200227d841e069796d27929ee99cc2de576aa.

Neither improves the slowest streaming channels sufficiently to justify the
core-layout change. Restored src/main.cpp from 2313bd4 after preserving patches.
The original core arrangement is retained; no claim of a general affinity fix.
ADC4 extra notifications persist across these placements. Next test changes
only GPIO pin glitch filters, which reject pulses shorter than two IO-MUX
sample clocks, using ESP-IDF's gpio_new_pin_glitch_filter API.

## 2026-09-07 - Hardware DRDY filter verified

Enabled the ESP32-S3 two-IO-MUX-clock GPIO pin glitch filter on all four fixed
ready inputs. No analog filtering, clock rate or conversion schedule changed.
This removes very short input pulses before interrupt dispatch. The diagnostic
ready-level/timestamp guards remain enabled as a second check.

Both builds passed. Verified flashed ELF
2d4a1ef899aa3795f40f90019f798f6b1fca8fe2b9c1fc5197884f18c94b4a35.
Artifacts: benchmarks/rdy-filter (30 s off + UDP), plus validation (3 x 60 s UDP).
The 30 s runs and first two 60 s repeats had complete diagnostics with zero
invalid ready events on every ADC. All runs had zero I2C errors/retriggers and
100% ADC UDP delivery with no packet gaps. The third repeat lost part of ADC4
serial diagnostics; its ready-event count is unknown, not zero. Accepted ready minima returned
to 388/390/380/377 us. Previously ADC4 produced 14,857 inactive-ready events
in a single 30 s UDP run. Evidence supports short input glitches; their physical
source is not established. Floating analog inputs do not validate signal quality.

Off remains ~900 Hz/raw; the three longer UDP runs are ~845.8-846.6 Hz/raw.
This is a correctness checkpoint, not achievement of the 1000 Hz target.
The original core layout is retained after the unsuccessful affinity trials.
Next: boot-only RMT capture of actual SCL pulse durations to distinguish wire
transfer time from firmware overhead. No SD tests and no monitor changes.

## 2026-09-07 - I2C wire timing and diagnostic completeness

Added bench-only RMT RX capture at 20 MHz on each existing SCL input. It keeps
I2C output routing/open-drain intact, restores the original disabled internal
pull-up immediately after RMT setup, and deletes the receiver after boot probes.
Normal builds omit the probe. Both builds passed (full rebuild after new header).
Flashed/verified ELF
7930344bd5ca39e3379026bcc9ce039939e29e1b8f4ec0e388afd280673c0c35.
Artifacts and complete SCL symbols: benchmarks/scl-probe.

All 12 write/read captures succeeded. Regular SCL low/high durations were each
11 x 50 ns = 550 ns, about 909 kHz, with a longer repeated-start high in reads.
Write capture: 36 cycles plus final low; read: 46 cycles plus final low.
Observed clocking spans about 40-51 us; the RMT observer's own interrupt can
lengthen its printed software-call time, so use ordinary acquisition TIMING
for that comparison. This is digital timing, not electrical/Hs qualification.
Post-probe off/UDP 30 s runs passed with complete diagnostics, zero invalid
ready events/I2C errors/retriggers, and 100% UDP delivery. Rates ~898 Hz off,
~839 Hz UDP. No persistent capture resources remain during acquisition.

Corrected the prior filter entry: validation/udp-03 serial capture truncates
ADC4's ready metric and omits turnaround/ADC_EVENTS. Its UDP counts/delivery
are complete; ADC4's diagnostic count is UNKNOWN. Host parser now rejects
malformed timing histograms and marks incomplete diagnostic sets as a failed
capture, preserving raw logs. Eight parser tests pass, including that exact
truncation pattern; absent diagnostics remain valid for original firmware.

Next isolated change: start the next single-shot conversion after reading the
completed value but before its publication callback. Preserve captured channel,
value and ready timestamp, including when the next trigger fails.

## 2026-09-07 - Trigger before publication checkpoint

Service still reads the completed conversion first and keeps its channel/value/
timestamp local. It now starts the next conversion before invoking the callback,
so publication overlaps ADC conversion. A next-trigger failure cannot discard
the successfully read previous sample. Turnaround timing ends at trigger entry.
Both builds passed; flashed bench ELF
0b3c32d5b425811af440b8f5d2f71e98c81c2d7ee5e44a88e618e5be50c88b41.
Artifacts: benchmarks/trigger-first. Off/UDP 30 s captures both passed complete
diagnostics, zero I2C errors/retriggers/invalid ready events, 100% UDP delivery.
Rates ~904 Hz off and ~845 Hz UDP: small improvement against the immediately
preceding ~898/~839 Hz runs, still well below target.

Preparing a separate optional legacy-I2C bench environment using ESP-IDF's
stack-backed convenience transfers, preserving the current workers and ADC
logic. This does not restore the old heap-allocated command links or polling
workers. The normal driver remains the current i2c_master implementation.

## 2026-09-07 - Optional static-buffer I2C driver comparison

Built esp32-s3-legacy-bench, an SD-disabled optional environment. Normal and
standard bench configurations still select the existing new i2c_master driver.
The comparison uses the IDF legacy convenience helpers, whose command lists
are stack-backed rather than allocated/freed on the heap per transfer. It keeps
20 ms timeouts, configured 1 MHz, pull-ups disabled and I2C filter count 7.
ADC workers, single-shot scheduling, gains, mapping, DRDY filtering and trigger
before publication remain unchanged. This is not a return to the old polling
architecture. Only the optional environment has been compiled with this path.

Flashed/verified ELF
b6894b1b89f0cc4bb96f7104bcf740ab44be40decfd5b123562b7a5560c923f2.
Artifacts: benchmarks/legacy-static. Boot clock captures retain a regular
22 x 50 ns clock period (~909 kHz), with extra transfer-boundary low periods.
Ready routing remains correct. All-mode off/UDP 30 s tests both passed complete
diagnostics, zero invalid ready events/I2C errors/retriggers, 100% UDP delivery.
Off ~951 Hz/raw; UDP ~934-937 Hz/raw, versus ~904/~845 for the new driver.
UDP trigger/read means ~74-77/~104-105 us. Target remains unmet.

An isolated core-1 interrupt placement comparison for this driver is building;
this should be measured independently of the previous new-driver affinity
results. The alternate driver is deprecated upstream and stays optional until
its performance and maintenance trade-off are decided.

The legacy-driver core-1 interrupt test also regressed: off ~897-898 Hz/raw,
UDP ~880-884 Hz/raw, still complete diagnostics, no invalid ready events/I2C
errors/retriggers and 100% delivery. ELF
89c71bd5e4922948d797e02b89da9241b23e29138f215416ffc93ccc5ac2ce4a;
artifacts benchmarks/legacy-core1. Restored the original core arrangement.
The normal and optional driver paths now both use the original placement.

Preparing esp32-s3-throughput-bench to measure instrumentation overhead: same
SD-free optional driver and corrected bench ready map, without EMG8_ADC_TIMING.
EMG8_BENCH_RDY_MAP selects only that physical map independently of diagnostics.
Normal bracelet mapping remains unchanged pending wiring clarification.
Nine host tests pass; diagnostic-recheck.json preserves the filter runs' exact
completeness result without rewriting their original captures.

## 2026-09-07 - Instrumentation overhead control

Built/flashed esp32-s3-throughput-bench (SD off, measured ready map, optional
static-buffer driver, original core layout, no detailed timing). ELF
018c60cc9643beabe6fb5dfb3832ecef4ee14d4855828dc2216d31c8212a56be.
Artifacts: benchmarks/throughput. Bin contains SD_DISABLED and no TIMING.
Off/UDP 60 s tests: ~958 Hz/raw off, ~948-950 Hz/raw UDP. Zero I2C errors or
retriggers and 100% ADC UDP delivery, no gaps. Five complete 10 s UDP windows
per channel ranged 947.3-949.9 Hz; target remains unmet. This control has no
ready-event diagnostic counters, but retains both physical validation/filtering.
Removing instrumentation improves only ~7 Hz off / ~12-14 Hz UDP versus the
instrumented optional driver; it does not explain the remaining shortfall.

Next: per-ADC recovery deadlines even while a partner keeps the bus queue
active, verified with a one-ADC missed-notification injection. Then evaluate
combining read of the completed register and write of the next trigger in one
I2C command sequence, preserving read-before-trigger ordering. Any ambiguous
transfer failure must suppress publication and wait for a clean re-arm rather
than attributing an uncertain register to a channel. No such combined path
is implemented yet. SD-enabled validation and normal-board pin-map confirmation
remain excluded/pending as documented above.

Final comparison validation: normal, standard diagnostic and optional legacy
diagnostic builds passed together; the no-timing throughput build passed
separately before its hardware run. All nine host parser tests pass.

## 2026-09-07 - Per-ADC recovery while the partner remains active

The bus worker now checks each ADC deadline once per RTOS tick while processing
events, in addition to queue-idle wakes. The existing ADC timeout is 5 ms.
Previously a healthy partner could prevent the queue from timing out forever.
The watchdog first services an already-pending valid completion, so delayed or
lost queue delivery does not discard a result merely because it is old.

Temporary SD-free tests dropped the 1000th notification on ADC1 and ADC3
(semaphore plus queue delivery), then the 2000th queue notification only.
Both partners continued delivering 3-5 raw samples during each recovery.
Off/UDP 30 s runs recovered the full notification loss in 4.944-5.659 ms,
with exactly one re-arm per affected ADC. Queue-only losses recovered in
5.312-6.024 ms without a re-arm, preserving their completed sample.
All four ADCs resumed; complete diagnostics, zero invalid ready events/I2C
errors, exactly two deliberate retriggers per run. UDP delivery 100%, no gaps.
Artifacts: benchmarks/recovery-injection-v2; ELF
92a438a0cef87eba9aed447a43e917f37d0aef675f0f0fa6a17e8244f42b8d68.
All temporary injection code was removed from source after these captures.

The first experiment (benchmarks/recovery-injection, ELF 5b273c6c...) exposed
one unnecessary startup re-arm before the pending-completion check was added;
it also had one UDP packet gap (99.926% delivery). Those results are preserved,
not counted as a clean acceptance run. Rates remain below 1000 Hz/raw.

Normal and optional diagnostic builds passed without injection. Clean firmware
ELF 96f70f717edaafff853bfc43298aad08a44826a8ebd6c8471844c3c2bc349798
was verified on COM9 and contains no STALL_TEST marker. A further 30 s UDP
capture (benchmarks/recovery-clean) passed complete diagnostics, zero invalid
ready events/I2C errors/retriggers, 100% delivery and no packet gaps.
Raw rates ~934-938 Hz with detailed timing enabled.

## 2026-09-07 - Combined read then trigger crosses 1000 Hz in short tests

The optional legacy driver now submits one stack-backed command list:
select conversion register, read two bytes (final NACK), repeated start,
write the next single-shot configuration, stop. Reading precedes the next
trigger regardless of task/ISR delays, preserving the old result's channel.
Normal i2c_master builds retain separate transfers. No bus speed, gain, schedule
or packet changes. The optional metric exchange replaces read and measures
the entire operation; its ready/turnaround reference is exchange entry.
Nine host parser tests pass, including missing combined-exchange diagnostics.

Any combined error discards publication and keeps the explicitly named next
channel for recovery. Both standalone trigger and exchange errors now wait a
full 5 ms interval measured AFTER the error returns (not before a possible
20 ms transaction timeout); recovery does not advance the scheduler a second
time. An accepted-but-reported-failed trigger is treated as ambiguous until
a fresh re-arm. Targeted simulated-error tests are next.

SD-free diagnostic ELF
750ce62ba2a5106801fc113463280faf1fc1fe4cb0384be07a44d8d26f57c8c5
passed off/UDP 30 s tests (benchmarks/combined). Off ~1017 Hz/raw; UDP device
rates ~1005.1-1006.1 Hz/raw, all complete 10 s raw windows 1005.0-1005.9.
Envelopes remain /20; IMU ~200 Hz. Complete diagnostics, zero invalid ready
events/I2C errors/retriggers, 100% ADC delivery, no packet gaps.
This is the first short bench result above target; it is not long-run acceptance.

Simulated error-path test: SD-free diagnostic ELF
e51ef0c3097ca37dcbb2bf70aebf8969b61561e70977ca03c81f628bed54a34a,
benchmarks/combined-fault/udp-01, 30 s UDP. On ADC1 and ADC3 the wrapper
deliberately reported failure before submission, after a successful command,
and after success plus a 20 ms delay. These were synthetic return errors, not
physical bus faults. All six re-arms occurred 5.030-6.288 ms after error return.
Exactly six I2C errors and six retriggers were counted; each affected ADC's
published count equalled exchange count minus its three failed operations.
Each discarded two later ready notifications from ambiguously accepted triggers,
as expected; partners had zero spurious/invalid events. No queue drops or other
invalid ready events. Complete diagnostics, 100% UDP delivery, no packet gaps.
All temporary EXCHANGE_TEST code was removed. Nine host parser tests pass.

All four clean configurations passed (combined-validation-build.log).
Build/flash process note: PlatformIO processed environments in configuration
order, so an attempted early restoration archived/flashed the old cached
throughput image in benchmarks/combined-throughput. Its verified ELF 018c60cc
made the mismatch visible before any acquisition test. That folder is annotated;
the freshly completed build is archived separately in combined-throughput-v2.
Wait for the entire build command to finish before any dependent flash.

Clean no-timing firmware ELF
d1997986587da605271df3917e789128bb31584f01e040f675fa02cff59e68a5
was verified on COM9, with no TIMING or fault-injection markers. Three 60 s UDP
runs (benchmarks/combined-throughput-v2) completed before the app session reset.
All eight raw channels averaged 1010.27-1010.83 Hz; all 120 complete 10 s raw
windows were 1010.0-1011.2 Hz. Every run delivered 100% of acquired ADC records,
with zero packet gaps/I2C errors/retriggers and no backward timestamps.
Envelopes remain /20 and IMU ~200 Hz. This is average throughput; the existing
mixed-rate schedule still produces nonuniform sample intervals.
All four builds and nine host tests passed; temporary injection is absent.
Next checkpoint: full eight-repeat off/nosub/UDP/quiet matrix and 15-minute All
UDP soak, plus mode/lifecycle checks. SD validation remains excluded and normal
ready mapping remains pending the bench-versus-bracelet wiring confirmation.

## 2026-09-07 - Three repeats per condition; UDP send failure found

Completed three 60 s runs each of off, nosub, UDP and quiet on clean combined
throughput ELF d1997986. All raw means and all available complete 10 s UDP
windows remain above 1000 Hz; no I2C errors/retriggers in any capture.
Results are consolidated in benchmarks/combined-throughput-v2/three-repeat-results.json.
The first quiet run delivered 99.9287% of ADC samples; later quiet runs and
all ordinary UDP runs delivered 100%. Firmware reported exactly five failed
send calls, zero queue drops, matching five packet gaps (two raw, one envelope,
two IMU). This was local send rejection, not unexplained reception loss.

The network sender currently clears a batch even when sendto fails. Retain
that bounded batch and its sequence for a later pump instead; the existing
queues absorb the backlog, and retries yield between attempts. Also replace
the fixed 20 ms shutdown delay with an explicit network-worker acknowledgment
before closing/resetting resources. Current public commands/datagrams remain.
Temporary SD-free tests will reject three sends of each packet type and pause
an in-flight network iteration for 100 ms during a requested shutdown.

## 2026-09-07 - UDP retry and shutdown fault tests

The sender now retains a failed batch and sequence for the next 5 ms pump.
A full pending batch is retried before dequeuing more samples. Memory remains
bounded by existing queues/batches; persistent congestion can still overflow
those queues and is counted. ERR counts failed send attempts, not necessarily
lost packets once retries are enabled.

A binary acknowledgment now proves the network worker left its poll/pump
iteration before W0 closes the socket or resets batches. Start/stop use task
notifications to wake the idle worker. Resource publication uses acquire/release;
start/stop remain serialized by the host control task, never the UDP callback.

Temporary no-SD tests rejected three sends of each packet type. First build
c4f0b71687fca53f1e077ec035581306c91d4db7818242ab03b8f668a4ef5456
(net-retry-fault) delivered 100% with no packet gaps, queue drops, ADC errors
or retriggers in 30 s quiet mode (~1012 Hz/raw). It reported 726 failed attempts:
nine forced and 717 additional attempts whose error codes were not captured.
Do not describe that run as having only nine errors.
The diagnostic repeat bed63bf494d774955c2a2fd981847ee6953715277049f917521c7d92d0602997
(net-retry-errors) separately counted exactly 3 forced/0 other failures per
type; all nine retries recovered, 100% delivery, no gaps/drop/ADC errors/
retriggers, ~1010 Hz/raw. The extra failures did not recur; their cause is unknown.

Shutdown test fd0a4b21d88b47d513257406c213a5dcb42cb3ed58102623568f0e5a2fe6dd64
(net-shutdown-marker) paused an in-flight network iteration for 100 ms.
The host sent W0 during that pause. A marker immediately before socket close
followed the worker's pause-end marker, 103.39 ms after W0, proving cleanup
waited for quiescence. The earlier net-retry-fault test only observed the later
NET statistics report and is not by itself direct proof of close ordering.
All temporary network fault/pause/error-detail hooks have been removed.
Clean validation: all four environments built successfully
(benchmarks/net-retry-clean-build.log); all nine host tests pass.
Verified clean throughput ELF
5d69048811140fb62df875ae5a6697a88a8b946ff50e429e2ea1587d1c33d402
has SD disabled and no timing/network/ADC fault hooks. Four 60 s All-mode runs
(net-retry-clean/{off,nosub,udp,quiet}-01) passed with zero I2C errors/retriggers.
Raw host rates: off 1021.07-1021.19 Hz; no subscriber 1024.65-1024.72;
UDP 1010.14-1010.64; quiet 1010.55-1011.10. All available complete 10 s
raw UDP windows were 1009.6-1012.0 Hz. Both UDP runs delivered 100% of ADC
records with no packet gaps, send errors or queue drops; IMU ~200 Hz.
Off/no-subscriber windows and ready-event counters are not observable in this
uninstrumented build. A single run per condition is preliminary validation.

Raw-only and envelope-only UDP 60 s checks also passed, respectively
1062.50-1062.90 and 1062.29-1062.71 Hz per active channel, with 100% delivery,
no gaps, I2C errors or retriggers. The 15-minute All UDP soak is running.

## 2026-09-07 - Fifteen-minute clean UDP soak and lifecycle checkpoint

Same clean throughput ELF 5d69048811140fb62df875ae5a6697a88a8b946ff50e429e2ea1587d1c33d402
(source implementation committed as 412facd), SD disabled throughout.
Artifacts: benchmarks/net-retry-clean/soak-15m, including acceptance-details.json.

The 900 s All-mode run passed the specified throughput and >=99.5% delivery
thresholds. Every raw channel acquired 1009.518-1010.022 Hz by host-timed
counters; received device-time rates were 1009.220-1009.732 Hz. All 712 complete
10 s raw-channel windows were 1004.2-1012.1 Hz. Envelope counts preserve /20
within the end-of-recording boundary; received IMU rate 199.974 Hz.

ADC delivery was 99.9724386%, with 2104 ADC records absent from the capture.
There were 16 packet gaps (12 raw, 1 envelope, 3 IMU), no duplicate/reordered/
invalid packets, and no I2C errors, retriggers or reset markers. NET reported
87739 successful sends, 2283 failed attempts, DROP=0. Successful-send count
minus received packet count equals exactly those 16 gaps. Thus accepted local
sends can still fail to reach the capture; the loss location after send
acceptance is not established. Retries address local send rejection, not UDP
end-to-end reliability. The failure errno was not instrumented in this clean
image, so the cause of the 2283 failed attempts remains unknown.
Raw received interval p99 was 1908-1911 us; maximum 23614 us includes reception
loss and cannot be called acquisition jitter. Average >=1 kHz does not imply
uniform 1 ms sampling. No ready-event counters exist in this no-timing build.

Raw-only and envelope-only 60 s runs also had zero network errors/drops and
no invalid/duplicate/reordered records. Active-channel complete 10 s windows
were 1062.2-1063.6 Hz (Raw) and 1061.8-1062.9 Hz (Env), with 100% ADC delivery.

The COM9 lifecycle probe passed: All -> Raw -> Env -> All while recording,
same-mode command without restarting, countdown cancellation followed by a
successful restart, and three W1/W0 cycles while acquisition stayed active.
All four ADC counters matched each selected mode, inactive channels stayed
zero, and errors/retriggers stayed zero. Artifacts and the exact probe script:
benchmarks/net-retry-clean/lifecycle and lifecycle_probe.py. This exercised
UART control, not physical button/reed presses or companion-device reception.
Final verified status: recording stopped, SD unavailable, IMU OK, storage
drops zero. Radio off and UART enabled; no test process retains COM9.

README now reflects the source channel pairs, measured-versus-normal ready
routing, worker ownership, UDP retries, UART rate reduction and explicit build
environments. Historical Sensor-mode failure is identified as not revalidated,
rather than a new test result. No monitor files changed and no push performed.

Next small step: characterize intermittent real send errors with error-path
diagnostics, then complete the remaining seven 60 s repeats per condition on
the final selected image (one clean repeat per condition currently complete).
Do not combine the earlier d1997986 matrix with this newer sender as if they
were one build. The 15-minute soak and mode/lifecycle checks are now complete.
Normal deployment remains pending actual-bracelet ready-wiring confirmation
and the optional legacy-driver decision. Working-card SD validation is deferred.


## 2026-09-07 - Mounted-card test preparation and initialization failure

User supplied another card and authorized SD tests. Added explicit storage
bench environment (measured ready map, combined driver, real SD) plus bounded
error-path UDP errno counters. Before any recording, replaced overwrite file
opens with FA_CREATE_NEW and made session directory creation skip existing
uptime-based names rather than reuse them or fall back into the card root.
These storage protections compiled; physical recording verification is pending.

Added bench_sd.py: lists before/after, retrieves only newly created recording
files, compares saved per-channel counts with CNT, validates record structure
and monotonic timestamps, and compares UDP records with saved bytes. Mounted
SD acquisition requires the new explicit --require-sd option; old no-SD
benchmark behavior remains the default. Tests cover missing/partial/corrupted
saved records, header mismatch and UDP loss/duplication.

Both normal and storage builds passed (storage-baseline-build.log).
Flashed and verified storage ELF
ff8c53c6ad1af3384ffc399000be719f8a8630995c909efe1faf096319f39f37.
The SD SPI host initializes, but sdmmc_init_spi_crc / CMD59 returns
ESP_ERR_NOT_SUPPORTED (0x106), before filesystem mounting. No SD files were
opened or changed. User reports FAT32 formatting and successful test.txt
creation in Windows. Repeated controller reset gave the same failure.

Hardware testing paused at an automatic approval usage-limit rejection, then
resumed after the user's continuation. User saved changes in 55a9be0.
The build cache had been cleared, so the temporary SD startup trace requires
a rebuild. On resumption the board was running older d4ca979 firmware
(boot ELF prefix 9bf8d94e7), with a recording active and unavailable-SD queue
drops. Stopped it before testing. That older image has the identical CMD59
failure, demonstrating that this initialization issue predates our changes.
The rejected/aborted net-errors-01 capture is not a sampling result.
Artifacts: benchmarks/storage-baseline; no monitor modifications.


## 2026-09-07 - Card initialization compatibility and UART transfer stack fix

The available card rejects CMD59 with R1=0x05 (idle + illegal command).
A temporary probe established that it accepts CMD59 after CMD55/ACMD41
reaches ready. Added a narrowly gated initialization-only fallback for that
exact SD-v2 rejection: confirm CMD8, poll readiness, then retry the real
CRC-enable command. Success is never fabricated and CRC remains enabled.
Normal SDK initialization continues, and the direct transaction callback is
restored afterwards. Removed temporary command traces/private SDK hooks.
The card now mounts. Existing test.txt was read without modification; its
8-byte SHA256 is
1d5f671fbc083af9a0ac801f24b93569fc6f9702af3fceee0ea7ca1a0018f001.
No formatting or deletion was performed.

A subsequent file-read / Wi-Fi-start sequence crashed with a kernel debug
exception. Disassembly confirmed processUartLine reserved 0x1290 (4752)
bytes on the 3584-byte main task stack. Its local FIL contains the configured
4096-byte sector cache. Made FIL and the 512-byte transfer buffer static;
UART commands are serialized in the main task. The compiler then inlined
the handler into feedUartByte with a 144-byte frame. No task stack increase
or transfer protocol change was needed.

Storage image b2ee2a5cda0b05d85cd08784d70998e86843be8e7f98c312df5db9547320bb23
passes the previous file-read/Wi-Fi-start sequence. A 15-second All-mode
SD+UDP capture acquired 1002.638-1003.971 Hz per raw channel, with zero
I2C errors/retriggers/storage drops. SD contains 120400 raw, 6017 envelope
and 2976 IMU records; every record matches UDP byte for byte, and every
saved ADC count matches CNT. test.txt hash remains unchanged.
Artifacts: benchmarks/storage-transfer-stack, including exact binary/ELF.
Earlier SD-only 10-second capture on d666de85 also had all saved counts
matching CNT, raw 1013.845-1014.944 Hz, no errors/drops.
Artifacts: benchmarks/storage-crc-ready/sd-off-smoke.
Session directory collisions after reset now create a new suffixed directory;
the bench verifier confirms existing names and sizes remain unchanged.

A 90-second quiet UDP capture on the temporary trace image identified all
152 send failures as errno 12 (ENOMEM). No network queue drops occurred;
ADC delivery was 99.92965%, raw 1009.653-1010.075 Hz, no ADC errors/retriggers.
This establishes transient send-buffer pressure, not the precise allocation
site; it does not retroactively classify every earlier uninstrumented error.
The existing retained-batch delayed retry remains in place.
Artifacts: benchmarks/storage-init-trace/net-errors-01.
Host parser/verifier suites: 17 tests pass, including duplicate UDP handling.

Remaining storage review: checked writes/short writes/sync/close errors,
elapsed-time periodic sync, and explicit stop/drain/close ownership before
download or restart. These are still pending; short successful recordings
do not validate failure paths or power-loss durability. Physical button/reed
tests, actual-bracelet ready mapping, and final deployment choices remain open.

One-minute SD+UDP follow-up on the same b2ee2a5c image passed SD verification:
481245 raw + 24056 envelope + 11993 IMU records, saved ADC counts exactly
match CNT, strictly increasing channel timestamps, zero storage drops.
All eight raw acquisition averages were 1001.943-1003.226 Hz. UDP received
99.91154% of ADC records: 447 raw records present on SD were absent from UDP,
with two raw packet sequence gaps and 99 network queue drops.
NET reported TX=5563, ERR=221; all errors were ENOMEM (errno 12).
Every received UDP record matched an SD record byte for byte.
This is complete SD capture for this run, NOT lossless network delivery.
Artifacts: benchmarks/storage-transfer-stack/sd-udp-60.
Next checkpoint should investigate sender backpressure with SD enabled and
finish the storage writer error/boundary review listed above.

Normal and storage configurations both build successfully. Final test.txt
hash is unchanged; device stopped, SD mounted, radio off, UART enabled,
COM9 released. No monitor changes or pushes.


## 2026-09-07 - Selectable 1000 Hz average acquisition ceiling

User requested continued tests plus a 1000/max rate option. Implemented UART
line commands R1000, Rmax, R? (newline terminated). Default remains max,
configuration lasts until reset, changes while recording return ERR:BUSY.
Ordinary status adds a separate RATE line without altering STATUS fields.
Monitor app is unchanged. Selection is independent of All/Raw/Env/Sensor.
All mode caps raw channels at 1000 Hz average and retains envelopes /20;
Raw/Env-only cap their active channels, Sensor caps its single channel.

This is an average limiter in groups of 20 fast cycles per 20 ms, not a
uniform 1 ms sample clock. It preserves the channel scheduler, single-shot
attribution, hardware data rate and original DRDY timestamps. At each group
boundary the ADC completes its due envelope conversions, then its worker
defers the read/next-trigger exchange until the pacing deadline. The other
ADC on the bus is not blocked. A per-bus esp_timer wakes the worker through
its existing queue; intentional waits are excluded from stall recovery.
The limiter preserves phase under ordinary jitter and discards multi-frame
catch-up credit after long interruption. Max bypasses pacing.
The SD master v4 header remains 32 bytes: formerly reserved byte 25 now
stores 0=max, 1=1000; bytes 26-31 remain reserved. Rate is snapshotted at
recording start for the writer. Sample and UDP formats are unchanged.

Storage test ELF:
a4ba748dedbf3ae3e03b33692052b097bbdef55c53cb88ba1dceefad7c490c5a.
Normal configuration also builds; its compiled firmware is archived alongside.
Artifacts under benchmarks/rate-cap include firmware/ELF and captures.
Added --rate to the host acquisition/SD tools and support for Sensor mode.
Verifier checks saved rate metadata and timestamp-based acquisition ceiling,
with allowance for one pacing group and conversion timestamp jitter.
19 offline parser/verifier tests pass, including rate metadata mismatch and
an over-rate recording incorrectly marked capped.

Capped SD-only 10 s All: 80029 raw, 4000 envelope, 1998 IMU records.
Raw-only 10 s capped: 80092 raw, 1998 IMU records.
Both: saved ADC counts equal CNT, no errors/retriggers/storage drops.
Complete device-time one-second raw windows contained 999-1001 samples.
Short host averages can exceed 1000 slightly due to the initial pacing group
and control/timestamp boundaries; this is not a strict per-interval cap.

Capped All SD+UDP 60 s: raw host averages 1000.124-1000.240 Hz,
envelopes 49.999 Hz. SD contains 480098 raw, 24000 envelope, 11908 IMU.
Every saved ADC count equals CNT; no ADC errors/retriggers/storage drops.
All received UDP records match SD bytes; SD contains 174 raw and 16 envelope
records missing from UDP (two packet gaps). ADC delivery 99.96231%.
NET TX=5569 ERR=0 DROP=0 for this run. This single run does not establish
that the cap eliminates backpressure, and UDP is still not lossless.
59 complete one-second windows per raw channel contained 998-1003 samples.
Files were reverified offline with the additional actual-rate bound.

Restoring max in Raw-only SD capture (10 s) gives 1068.266-1069.466 Hz,
85522 raw +1994 IMU records; all counts match, no ADC errors/retriggers/drops.
The first Env test aborted before recording because the host checker required
RATE to be the last received line; a normal HEALTH line followed the valid
acknowledgement. Fixed the checker to search only the response interval.
This was a host assertion failure, not a firmware fault.

Env-only capped capture then passed (40084 envelope +974 IMU records, 5 s),
including the saved-rate bound, with zero errors/retriggers/storage drops.
Sensor capture exposed stale diagnostics: inactive ADCs retained counters
from Env mode. The single active sensor saved 5014 raw samples without
errors, but stale inactive counts caused the verifier to fail.
Added owner-only resetAcquisitionDiagnostics and clear inactive ADCs at each
recording start (including errors/retriggers/timing metrics). Active ADCs use
the same reset on start. No acquisition ordering changes were needed.
Final storage ELF f103ebbbbd533a9a963934eeaaee16e1adfa2d057be33e100e2cc6d1c6651831
is archived and flashed, with exact completed-build binary/ELF match verified.
Artifacts for retests: benchmarks/rate-cap-final.

Final-image Env -> Sensor transition passes: inactive counters are zero,
all 5014 active-sensor samples match SD, and intentional pacing waits longer
than the 5 ms watchdog threshold cause zero false retriggers.
Final-image capped All SD+UDP 20 s: raw 1000.032-1000.532 Hz host averages;
160048 raw +8000 envelope +3990 IMU records all match SD/UDP byte for byte,
zero packet gaps, ADC errors/retriggers or storage drops.
Busy/invalid rate commands were verified on hardware: R999 rejected,
Rmax rejected while Sensor recording continued with RATE:1000, R? worked
during acquisition, stop then Rmax accepted. An initial command-probe attempt
consumed the RATE line from the preflight STATUS query as if it were a command
acknowledgement; fixed the probe to consume that line first, then retested.
The final probe script/log are archived. test.txt hash is still unchanged.
Both final normal/storage builds and all 19 offline tests pass.
Final board state: stopped, max selected, SD mounted, UART enabled, radio off.
COM9 is free. No monitor changes or push; unrelated .vscode edit preserved.

Next small checkpoint: finish SD writer checked write/short-write/sync/close
handling and explicit recording stop/drain/close ownership, then longer
SD+UDP tests including queue backpressure. Current successful captures do
not validate full-card/error/power-loss behavior or guarantee UDP delivery.
Actual-bracelet DRDY mapping and optional legacy I2C deployment decision
remain pending. The average limiter does not imply evenly spaced sampling.


## 2026-09-08 - Checked SD I/O and explicit recording boundaries

Fresh 2 s baseline on the previous firmware passed: all 16261 raw +808
envelope ADC records matched CNT, 400 IMU records saved, no ADC errors/drops.
Artifacts: benchmarks/sd-writer-baseline/all-off-2. Card mounted, max selected.

The writer previously ignored short writes and all sync/close errors, synced
every 25 productive loop iterations rather than elapsed 500 ms, and inferred
start/stop from a shared flag. Added checked writes (both result and exact
byte count), checked sync/close, and elapsed-time dirty syncing. On failure,
mark SD unavailable, report operation/file/result/requested/written, attempt
to close all handles and discard pending storage records. ADC/UDP continue.
Dropped records include uncertain failed write batches; the counter is not a
byte-exact measure of data physically absent from the card. Header failures
also close the newly created handles. Existing FA_CREATE_NEW protections remain.

Main now sends explicit Open/Close requests to the writer and waits for
acknowledgement. Files are opened before acquisition; Close drains queues and
closes handles before stop/pause is acknowledged or another recording starts.
The writer is created before the initial mode-selection loop, so very short
first and subsequent recordings cannot be missed by its old 100 ms idle poll.
Batch buffers are static, single-owner storage, removing about 5.9 KB from
the writer's call stack. recording is atomic. An IMU mutex plus recheck gates
measurement/publication so stop waits for in-flight work before draining SD.
Reed pause now uses the same stop path. ADC-start failure also drains/closes SD.

Compiled the actual checked-I/O helper bodies against scripted FatFs outcomes:
full success, FR_OK short/zero write, error with apparently complete byte count,
sync error, close error and remaining handle cleanup. All pass.
The tests use constexpr evaluation with clang -fsyntax-only and emit no binary.
The first native-test approach could not link the host's incomplete CRT; its
freestanding fallback executable was flagged by Windows and not run. No
security settings were changed. Switched to compiler-only assertions.
These are simulated I/O results, not physical full-card/removal/power-loss tests.
tools/test_sd_io.py and all 19 existing host verifier tests pass.

First storage image:
7d89da35d7a50dcdbe5ec0546bf1271c7b4c4068a62f799ef8ec917f85a6bf20.
Four short captures (requested 5, 50, 150, 20 ms; All/Raw/Env/All) all passed
immediate F/G access after STOP with no grace period. Every saved ADC count
matched CNT, distinct file sets, zero storage drops/errors/retriggers.
Observed stop acknowledgements 15-31 ms. Host request durations are not exact
ADC-active durations, especially the first start while tasks are initialized.
Artifacts: benchmarks/sd-writer-clean/short-lifecycle.
The 60 s All SD+UDP capture on this image acquired 1001.561-1002.611 Hz per
raw channel, zero ADC errors/retriggers/storage drops. All 481014 raw +24044
envelope +11999 IMU records matched SD/UDP byte for byte; no packet gaps.
A final ordering adjustment sends the companion stop notification before
SD drain/close, avoiding a slow card delaying that signal.
Both final configurations build (sd-writer-final-build.log). Retests follow.

Final storage ELF:
e2e9debd4266aa4be167eca6fb9a7708197eeb5a12c17527ebc4bc10fe133577.
Final-image short lifecycle repeats all pass with immediate file access,
matching counts and separate file sets. Initial stop-latency measurements
used Windows' coarse monotonic clock (0-31 ms reported); the tool now uses
perf_counter for future measurements. File/count validation is unaffected.
Final-image capped SD+UDP 20 s: 160137 raw +8001 envelope +3973 IMU records;
every saved ADC count matches CNT, no ADC errors/retriggers/storage drops.
UDP missed one raw packet (174 records, all present on SD); all received
records match SD bytes. ADC reception 99.89651%. No lossless-UDP claim.
Artifacts: benchmarks/sd-writer-final, including binaries and final captures.

The IMU still uses its existing 100 ms idle poll before a recording; very
short recordings can have no IMU samples. Initial REC also precedes worker
startup on the first recording. These startup details are not changed here.
Next small step: longer mixed SD/UDP soak and runtime failure injection if
needed, plus review of startup IMU timing. Physical card removal/full-card
and power-loss tests remain unperformed; simulated helper checks are not
a substitute for those hardware failure tests. Physical button/reed and
companion reception, actual-bracelet ready wiring and final driver choice
remain pending. Monitor files were not modified.

Final capped run NET: TX=1844 ERR=0 DROP=0. Final verification: test.txt
hash unchanged, recording stopped, SD mounted, max selected, radio off,
UART enabled, COM9 released. No push; unrelated .vscode edit preserved.

## 2026-09-21 - Companion task and phase protocol in the new fork

Work continues in D:/PhD/Code/SenseAI_emg8, branch feature/samping-speed,
from Ana's 31ef79a. Monitor/auxiliary firmware reviewed read-only, not edited.
User approved newline Pgrasp/Prest/Pdemo alongside L<id>,<rep>.

Archived original COM9 application: benchmarks/sync-baseline/device-app.bin,
embedded ELF dd3a7d2d07ff5f36f433ab3519f3705d24a6dcd33e7a96eba97a2677c74cfa9b.
Descriptor reports 7e56da7, Sep 15; this does not prove source equivalence to
the fork. Image contains original RDY array 40/41/42/15. Untouched fork normal
build passes; archived fork-normal-firmware.bin/.elf.

Original-device All/max baselines, 35 s each:
- SD/radio off: ~463.7 Hz/raw, 129846 raw +6488 envelope +7001 IMU.
- SD/UDP: ~447.6-448.4 Hz/raw, 125442 raw +6268 envelope +6990 IMU.
Both save every ADC count reported by CNT, zero I2C errors/storage drops,
one watchdog retrigger. UDP received every saved record without gaps.
Existing card entries/sizes preserved. Artifacts: benchmarks/sync-baseline.

Sync already ran core0 inside CSV task priority3. Main and CSV shared mutable
int64 last-sync state with recording enabled before reset/start notification.
Setting last-sync=0 did not force immediate sync before uptime30s.
No evidence five bytes/30s explains halved throughput.

Dedicated companion task owns UART1 TX on core0 priority6, above UDP5.
Bounded queue serializes start/phase/stop; Start captures immutable epoch.
Immediate sync, one early retry at1s, then30s intervals; pauses/mode boundaries
cancel old schedule. ADC callbacks/SD never do companion UART I/O.
LINK counters expose queue/TX failures; host ACK means local acceptance only.

Pgrasp/Prest/Pdemo/P? implemented; phase defaults demo, persists across stops.
SD master v4 byte26=1 announces metadata extension; unchanged12-byte event:
reserved byte8=phase, byte9=kind (1 label,2 phase,3 initial snapshot).
Timestamp0 snapshot retains pre-start labels/phase. Queue full rejects commands.
Sample/IMU/UDP layouts unchanged. Bounded strict label parsing added.
See docs/companion-protocol.md for monitor/Ana handoff.

Receiver issues documented for Ana: start resets/flush can erase first sync,
volatile64 clock state is not synchronized, active repeated-start does not
establish new epoch, new phase bytes ignored, parser overflow/wrap handling.
Early retry only mitigates current start flush. No cross-board accuracy claim.

First normal test image ELF:
307e8ecfd281416943e12b8320a6197af3bdcbea7be55c7254f9ed65a4370050.
normal-phase-33 passes: CSV47.619Hz radiooff,0.999Hz radioon. U0 accepts label/
phase commands. All seven metadata events and every acquired ADC count saved.
LINK delta START1 STOP1 PHASE6 SYNC3 QERR0 TXERR0. Zero I2C errors,1 retrigger.
Raw timestamp average468.74-468.76Hz across mixed radio conditions: relocation
alone does not restore1000Hz on original mapping. Artifacts: benchmarks/sync-task.

Compiler-only tests execute actual task with deterministic UART/RTOS stubs:
frame ordering, first sync before uptime30s, pause/restart,32-bit wrap, TX
timeout/short write and queue failure. No executable emitted.22 host tests pass.
Both final configurations build. Storage diagnostic now probes actual ready
routing before worker startup; CONFIG status identifies I2C path and pin map.

Storage image 1e9be117f41b5c763530ba80c81384cbaad116c565bed81e053237e283570643:
boot probe repeated three isolated conversions per ADC. Each consistently
fell on its expected bit in 15/42/41/40; all I2C results0. This independently
confirms actual wiring without waiting for the user's optional wiring reply.

All/max SD+UDP35s recovered1028.6-1028.9Hz/raw but FAILED storage verification:
321 raw records dropped from full SD queue; zero I2C errors/retriggers.
NET TX3203 ERR728 (ENOMEM) DROP2280, ADC delivery99.1311%. The overflow occurred
~4-5s after start, not at the31s sync. Kept failed capture for review.
SD-only max35s then PASSED:1040.5-1040.6Hz/raw,291358 raw+14564 env+6997 IMU,
all ADC counts saved, no errors/retriggers/drops. The comparison does not
identify the card's exact internal latency mechanism.

In response, reserved12000 rather than8000 raw records for SD; reduced UDP
queues to1000raw/256env/100IMU. Net RAM cost about20KB. UDP sender now yields
packet transmission at SD raw backlog>=3000, while still polling subscribers.
No extra queue checks added to ADC callbacks. Added max SD write/sync-group
latency counters to status. Further combined-load validation follows.
Default PlatformIO environment now selects verified storage configuration,
preventing accidental deployment of the mismatched historical default.

Storage-priority image ELF39d93a9bd4471ba73cd6e8255ca017b975be1b8db40dd3212995b7a02425fca0:
60-second max SD+UDP capture reached1022.18-1022.31Hz/raw, zero ADC errors,
retriggers or reported storage drops. NET TX5543 ERR237 DROP0; ADC reception
99.8987%. Longest measured write197097us, sync group48764us.
However IMU saved only8884 samples (~148Hz), with long gaps/catch-up bursts.
The writer's continuous tiny batches can keep priority5 runnable indefinitely,
starving IMU priority4. Added one-tick blocking only when all queues are low
and not closing; a high backlog still drains without that pause. Avoided timing
calls for empty write batches. This needs fresh hardware verification.

First R000 download was corrupted: FDONE appeared384 bytes before its expected
position and health lines filled the requested byte count. Device stayed idle,
mounted and responsive. Enlarged the Windows host-test RX buffer to1MiB and
retrying the unchanged file into sd-retry; original failed download preserved.
This is separate from the earlier321-record SD queue overflow.

## Final handoff - 2026-09-21

Final flashed ELF:
4c76ce4444b9c5fa07075a3ea3f30bbfb45985442197ee6a82991a3f8244205f.
Both PlatformIO environments build; all 24 host tests pass.
Artifacts: benchmarks/companion-final (firmware, logs, captures).
The earlier unchanged-file retry passed after increasing the host RX buffer.

Final-image All/max SD+UDP, 60 seconds: PASS storage verification.
Raw channels averaged 1021.024-1021.207 Hz; 490148 raw, 24504 envelope,
11888 IMU records saved (~198.1 Hz IMU). Every acquired ADC count matched
SD, with zero reported SD drops, I2C errors or retriggers.
UDP ADC delivery was 99.2774%; NET TX5670 ERR749 DROP2469.
Maximum measured SD write 199208 us, sync group 353301 us.

Final-image All/1000 SD+UDP, 35 seconds: FAILED.
Raw channels averaged 1000.211-1000.383 Hz; zero I2C errors/retriggers.
Device reported 200 raw SD queue drops. File size corresponds to 279889
raw records versus 280089 acquired; envelope size corresponds to 14000
records and IMU to 5120 (~146.3 Hz). These sizes are not content verification.
Maximum measured SD write 268533 us, sync group 642324 us.
UDP ADC delivery was 94.9627%. Bulk UART download timed out with missing
#FDONE despite the enlarged RX buffer. No retry after the user requested
completion. Storage reliability and bulk UART download reliability remain
unresolved; the buffer change is a mitigation, not a demonstrated full fix.

The final-image phase/lifecycle run planned after this capture did not run.
Phase/label behavior passed on the earlier dedicated-task image as recorded
above; host tests cover the implementation. End-to-end auxiliary receipt
and clock accuracy remain unverified and require Ana's receiver changes.

Stopped further testing at the user's request. Benchmark cleanup had issued
stop/radio-off. A final COM9 session sent UART-on, radio-off, demo, label0/0,
max and status commands, but returned no serial response within 3 seconds;
therefore final device state was not independently confirmed. COM9 is closed
and released. No monitor or auxiliary source was modified; no SD files were
deleted or card formatted. No git push performed by this task.

## 2026-09-21 - Default to the 1000 Hz average ceiling

User requested capped operation by default after observing UDP batching.
Set the startup rate selection to 1000 Hz; Rmax remains available while
stopped, and reset returns to 1000 Hz. Scheduler, sampling modes, storage,
and UDP behavior are unchanged. This does not establish a fix for UDP
pauses or the previously observed capped-run SD drops. README updated.
Validation: esp32-s3-storage-bench build passed (30.47 s); diff check
passed. No new tests added for the startup-default change. Not flashed:
the user is using the current firmware through the monitor.


## 2026-09-21 - Direct UDP timing and runtime RAM investigation

User released COM9 and authorized further tests after monitor-visible bursts.
Monitor and auxiliary source remain unchanged. Passive comparison artifacts:
benchmarks/monitor-live (MAX) and monitor-live-1000. The latter had zero new
packet sequence gaps in its45-second window but raw backend frame intervals
up to390ms. This is not proof of lossless original samples or sole causation
by the rate cap. SD-pressure and network timing needed direct measurement.

Added #MEM internal byte-addressable free/minimum/largest heap and #NETDIAG
TX/errors/queue drops, SD-pressure pause count/total/current/max duration,
sendto maximum duration and sampled UDP queue high-water marks. Network
fields are atomic individual readings; queue maxima are lower bounds.
Pause accounting is network-task owned and reset while inactive, after the
stop acknowledgement. No additional per-sample instrumentation in ADC code.
Details in docs/streaming-diagnostics.md. Tests compile actual pause logic
and network task/pump bodies;26 host checks pass.

Direct old-default baseline35s (1000Hz): zero reported SD drops/I2C errors/
retriggers, UDP delivery99.6443%, NET ERR154 DROP334, sequence gaps4raw/1env/2IMU.
Raw receive interval max331.750ms. Artifacts: benchmarks/udp-investigation.

Diagnostic image ELF:
ccf7f2d0cf881d6c46675adbaaadeb73fbba0503cbb3a370d015ca3f4f1cf019.
60s at1000Hz, unchanged queues: UDP delivery99.6063%, NET ERR479 (ENOMEM),
DROP1462, raw queue reached1000. Only one80ms SD-pressure deferral.
Sendto max2580us; raw arrival max414.670ms. No reported SD drops/I2C errors/
retriggers. Minimum internal heap16536 bytes, despite idle readings much
higher. All raw acquisition rates999.925-1000.025Hz. IMU~124.6Hz (existing
under-load shortfall remains, not attributable to receiver packet loss).
Two earlier attempts ended before recording because Windows could not see
the restarted AP. A fresh WlanScan restored the saved-profile connection.
No Windows power settings changed. Test reconnection now retries five times.


Buffer-only image ELF:
fddf663cd2421af14217dcbd3b79c5e4db65cb6f7d7cfa1f9834dbf8944bf3e6.
Raised UDP raw queue1000->1500 (+4000 bytes), leaving SD12000/env1000/IMU400
and UDP env256/IMU100 unchanged. An8KB increase was considered before the
full capture's lower heap minimum became available; chose4KB instead.
60s test verified every acquired ADC count on SD:479984raw+23994env,
7666IMU (~127.8Hz), initial metadata event present, original card entries/
sizes preserved. Zero SD drops/I2C errors/retriggers. UART file transfer
completed this time; this does not resolve its previous intermittent failure.
UDP missed2193raw+16env records: raw9packet gaps (1566records) plus627 firmware
queue drops; all received records matched SD. Delivery99.5617%.
ERR255 ENOMEM; pressure4pauses totaling73ms, max34ms; sendto max2969us.
Raw receive max416.585ms, minimum heap13964bytes. Extra buffering alone did
not eliminate loss; separate runs have variable radio/card conditions.

Next bounded sender change: one packet per stream per pass, retaining
failed batches and30ms partial flush. Existing5ms delay/SD priority retained.
This limits catch-up bursts rather than consuming more runtime heap.


Bounded sender with original full-deferral policy, ELF:
7425ad1a986c9c01913241b2e68a874fd7bc041b154279a172a07381b9badf8a.
60s SD verification passed:479979raw+23993env+8829IMU (~147.2Hz),
all acquired ADC counts match files and all UDP records match SD.
UDP missing6054raw+67env: NET DROP5077, ERR348 ENOMEM,6raw packet gaps.
Delivery98.7854%. A708587us SD sync coincided with a maximum804ms measured
SD-pressure deferral, producing an819.872ms raw arrival gap. Total deferral
808ms across2pauses; sendto max3989us. Minimum heap13152bytes.
No SD drops/I2C errors/retriggers. The available RAM cannot buffer an
additional0.8s of raw UDP data (~51KB payload). More RAM alone is insufficient.

Revised pressure policy: SD raw backlog3000-8999 slows bounded sender passes
from5ms to10ms (nominal17.4k raw records/s capacity versus8k arriving).
At9000 (75% of12000), retain complete UDP deferral and recheck every5ms.
Sampling and SD queues/tasks remain unchanged. THROTTLE diagnostic counters
separate moderate pacing from urgent PAUSE time.27 host checks cover actual
thresholds, both pressure paths, control responsiveness, stop acknowledgement,
packet retry/budget/partial flush, timing wrap/reset and existing functionality.
Final hardware validation follows; this policy is not a lossless-SD guarantee.


Final two-level policy image ELF:
2b8b674a305960b63d41ebb986f75a9134641c104dca15c841890e4d192c3175.
Default storage environment built successfully; all 27 host checks passed.
Static RAM is 68040 bytes (+88 from the prior firmware); UDP payload queue
allocation increased by 4000 runtime bytes. SD allocations remain unchanged.

Final 60-second All/1000 Hz SD+UDP test PASSED SD file verification:
- Raw acquisition: 999.914-999.998 Hz per channel.
- Saved 479990 raw, 23995 envelope and 6521 IMU records.
- Every acquired ADC count matched SD; all received UDP records matched SD.
- Zero SD drops, I2C errors, retriggers, UDP queue drops, malformed packets,
  reordered packets or duplicate packets.
- Three raw UDP packets were absent at the receiver: 522 raw samples, all
  present on SD. ADC delivery 99.8964%. This is not lossless UDP.
- NET: TX5570, ERR362 ENOMEM (retained/retried), DROP0; raw queue peak1305.
- Moderate-pressure pacing: one 19 ms interval; no urgent full deferral.
- Maximum sendto call3143us. Raw receive p99=155.657ms, max313.294ms.
- Minimum internal heap21544 bytes; lowest queried largest block20480 bytes.
- Maximum SD write213148us, sync384573us. File download completed correctly.
- Original SD file names/sizes preserved. No files deleted or card formatting.

The last test exercised only a short moderate-pressure interval; it did not
repeat the earlier804ms full-deferral event under identical card conditions.
Near-full deferral is covered by actual-code host checks, not hardware fault
injection. Sequential runs vary in radio/card behavior, so do not attribute
all observed improvement to one change or claim future lossless recording.
IMU remains below its nominal200Hz under load (~108.7Hz in the final test);
all received IMU samples matched SD. This separate scheduling shortfall and
the earlier intermittent UART bulk-download failure remain unresolved.

Final status verified: recording stopped, SD mounted, demo phase,1000 Hz,
UART enabled; test cleanup had turned radio off. LINK start/stop1/1, sync3,
QERR/TXERR0. COM9 closed and released. Latest firmware is already flashed.
Monitor/auxiliary source unchanged; no push performed by this task.

## 2026-09-21 - Auxiliary measurement UART relay

User requested a read-only review of Ana's update and UART1-to-PC UART0
forwarding without modifying her firmware or the monitor. Reviewed committed
diff585cc5d..973534b (working diff only an existing sdkconfig change).
Ana transmits imp:[frequency,impedance;...] after a complete sweep and
p1:<kPa>,p2:<kPa>,temp:<C> after each sensor reading (2000ms during grasp).
No measurement timestamp or checksum is transmitted.

Implemented bounded RX in the existing core0 priority6 companion owner:
1024-byte UART driver ring, error-event queue16, nonblocking512byte budget
each <=10ms under normal scheduling. Due commands and sync retain precedence.
A4096byte SPSC ring publishes only complete known-tag lines, limit3078chars,
and rejects/drop-counts malformed, oversized, overflowed messages. Wire errors
flush and discard through LF. No large parser/snapshot copies or added task.
Existing core0 priority3 UART preview forwards unchanged complete lines in
up-to1Hz batches while recording/U1. Stopped/U0 discards pending complete
lines. ADC/SD priorities, queues, CSV columns, UDP layout remain unchanged.
AUX status counters expose RX/TX/BAD/DROP/MUTED/UARTERR. File downloads now
lock stdout across header/binary/body/end against printf-based preview lines.

Build storage-bench successful (full340.52s rebuild). Static RAM72200bytes,
flash933238bytes. Static RAM grew4160bytes; UART runtime allocation also grew
by768bytes plus the16-entry event queue/driver bookkeeping. No PSRAM assumed.
ELF SHA256 e9e7924c2ad0553dd87fbec8faab9f634ab7c463f832d170aed10c7dca0601fe
BIN SHA256 1433c64e3f61e84562ac5c160dc997fba9abb55754a86fdbc76f43ff08671060
Flashed app at0x10000 on COM9, esptool hash verified. Host suite28checks PASS,
including actual companion TX ordering after RX integration and production
ring fragmentation/CRLF, maximum-length lines, consumer snapshot boundaries,
wraparound, pinned-view overflow protection, rejection and LF recovery.

Physical auxiliary connected: 30s recording with grasp/rest/grasp transitions.
Received/forwarded2impedance sweeps and9sensor messages; RX=TX=11, all AUX
error/drop/muted counters zero. Sweeps1953/1965characters with99numeric pairs,
2kHz..100kHz, both closing brackets intact. Per-channel raw counts29985..29992
over~30s (~1000Hz); I2Cerrors/retriggers/SDqueue drops zero. This smoke run
did not download its SD files. Analog accuracy and auxiliary clock alignment
are not established by this transport test. Artifacts: benchmarks/aux-relay.
Combined SD+UDP acquisition/readback validation follows.

Combined35s All/1000Hz SD+UDP validation:
- Received one1965character99point sweep and one pressure/temperature line;
  cumulative AUX RX=TX=13, BAD/DROP/MUTED/UARTERR=0 across both physical tests.
- Raw device-timestamp rates1000.012..1000.213Hz; complete10s windows
  999.9..1000.1Hz. No I2C errors, retriggers, or SD/UDP queue drops.
- All UDP streams had zero packet gaps/reordering/duplicates/invalid records.
  NET TX3274, ERR78 (retried), DROP0; raw queue peak607. One99ms moderate
  SD-pressure interval, no urgent pauses; sendto max3896us.
- Internal heap minimum18468bytes; final FREE48720,LARGEST31744.
  SD maxwrite251918us, sync56084us. IMU~131.2Hz; previous under-load IMU
  rate shortfall remains, unrelated to claiming success for this relay.
- Initial raw-file UART download timed out short; the known intermittent
  large-download issue persists. Read-only retry of the same raw file passed.
  Verification using that retry: SD280088raw +14000env +4585IMU;
  EVERY acquired ADC count matched SD, and all received UDP records matched
  SD byte-for-byte, with zero records missing in either direction. Master
  metadata passed. Existing card entries/sizes preserved; no files deleted.
- First failed transfer retained separately; see sd-udp-35/summary.json and
  retry-summary.json for both outcomes. This short run does not prove future
  lossless radio/card behavior or accurate auxiliary timestamps/analog values.

Final28host tests passed and git diff --check passed. Monitor and Ana source
unchanged (Ana retains her pre-existing sdkconfig modification). Current
firmware remains flashed; no push performed.

## 2026-09-21 - UDP failure isolation, recovery, and UART buffering

The first combined SD+UDP runs with the default Wi-Fi transmit ceiling reset
with `#BOOT:reset=BROWNOUT(9)`, including an SD-free run. This separated the
failure from the SD queue: the present auxiliary-board 3.3 V rail cannot hold
the radio's default current peaks. A 13 dBm trial avoided reset but produced
sustained Wi-Fi buffer failures (`NETDIAG DROP=10007`). The stable tested
firmware setting is the ESP-IDF 8 dBm step (`esp_wifi_set_max_tx_power(40)`).
Restoring the CP2102 3.3 V connection may help only if that source is backed by
a regulator and decoupling capable of the ESP32-S3 plus SD peak current; the
CP2102 3.3 V output alone should not be treated as a power upgrade.

The raw UDP sender now permits two packets per service pass only while a raw
backlog is present, allowing recovery after transient `ENOMEM` without
creating a steady-state burst. Envelope and IMU partial batches flush after
100 ms instead of 30 ms, avoiding needless small datagrams during SD writes.
The final 30 s file-level run at 8 dBm (`benchmarks/final-fix-lowrate-sd-udp-30`)
had `NETDIAG TX=1916 ERR=0 DROP=0`, zero SD queue drops, matching ADC counts,
and no reset. The received UDP records all matched the SD records; the host
still observed a few over-air/host gaps (`raw=7, env=1, imu=1`), so SD remains
the authoritative complete stream and monitor-side gap indicators/smoothing
are still appropriate.

UART1 was then changed to a 1024-byte RX ring plus a 512-byte driver TX ring.
The companion task now runs at core-0 priority 4 below UDP priority 5 and
above the host preview priority 3. Start, phase (grasp/rest/demo), stop, and
sync frames still pass through the ordered 32-entry control queue; only the
low-rate auxiliary receive/forward path is allowed to wait in the buffers.
The 28-test host suite passes after this change.
