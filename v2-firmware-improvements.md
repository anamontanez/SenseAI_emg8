# EMG8 — firmware para la placa v0.2 (estado al 2026-10-22)

Documento de traspaso para seguir iterando desde Claude Code. Resume qué se
hizo, qué se midió en hardware y qué queda abierto. Rama: `hardware-v2`.

## 1. Estado del repositorio

| | |
|---|---|
| Último commit | `4dea0cc` *Remap firmware pins to the v0.2 controller board* |
| Sin commitear | `src/main.cpp`, `src/emg8_types.hpp`, `lib/sensors-library/{include/ICM42605.hpp, src/ICM42605.cpp, src/ADS1015.cpp}`, `README.md`, `WORKLOG.md`, `.gitignore`, `tools/{bench_sd.py, bench_acquisition.py, test_network_priority.py}` |
| Nuevos sin commitear | `tools/test_imu_scheduling.py`, `tools/flash_noboot.py`, `tools/capture_boot.py`, `test/test_main.cpp` (firmware de prueba del colega, *Prueba_Boards*), este archivo |
| Tests de host | `python -B -m unittest discover -s tools -p "test_*.py"` → 50 OK |
| Build | `pio run -e esp32-s3-storage-bench` → OK, RAM 74 144 B, flash 943 590 B |

`storage-bench` es el firmware completo (SD real, I2C combinado, UDP y
diagnósticos de arranque). Es el entorno por defecto.

Artefactos en `.dist/` (ignorado por git):

| Archivo | Qué es |
|---|---|
| `emg8-v02-storage-bench.bin` (SHA256 `4b71ef11…e676`) | **Build más reciente, aún NO cargada en la placa** |
| `archivo/2026-10-22_…_diag2.bin` | Build cargada hoy en la placa (ELF SHA256 `8101118f…`): CS por software, `#CHIP`, `#I2CSCAN` |
| `archivo/2026-10-20_device-app-before-v02.bin` | Respaldo de la app que traía la placa (*Prueba_Boards*) |

Los logs de arranque están en `benchmarks/v02-bringup-2026-10-20/`. Esa carpeta
está ignorada por git: solo existe en este disco.

## 2. Cambios de firmware

### Fase 1 — pines v0.2 (commit `4dea0cc`)
- I2C0 SDA 4 / SCL 5 (ADC1/2) · I2C1 SDA 2 / SCL 48 (ADC3/4).
- LED WS2812 en GPIO45 · botón en GPIO8 · UART1 hacia el ESP auxiliar en TX17/RX18.
- SPI3 de IMU: MOSI 35, SCK 36, MISO 37. CS: 38 (placa), 21/40/41/42 (IMU1–4). INT: 1/14/39/47 (definidos, sin uso).
- Se eliminó el reed switch (su GPIO9 pasó a ser un RDY).
- Los 5 CS de IMU se ponen en alto antes del primer acceso a SPI3.

### Fase 2 — 5 IMU en la tarea IMU (sin commitear)
- `imuTask` lee en cada pasada de 5 ms todas las IMU que pasaron el init (200 Hz cada una).
- `ImuSample` sigue midiendo 20 B: el byte 18 es el índice de IMU (0 = placa, 1–4).
- Byte 28 de la cabecera `M` = máscara de IMU presentes.
- Nuevas líneas: `#IMU:<i>,CS=..,OK|FAIL,<err>`, `#IMUS:MASK=..,COUNT=a/b/c/d/e,ERRORS=..`. `#IMURATE` conserva su significado (IMU de placa).
- Prioridades sin cambios: workers ADC (`MAX-2`) > SD (5) > IMU (4), todos en core 1. Lo fija `tools/test_imu_scheduling.py`.
- Colas: un desborde en cualquier cola de SD detiene la grabación, así que la cola IMU de SD pasó de 400 a 1000 y la raw de 12000 a 10300 (heap neutro). El lote IMU del escritor SD pasó de 50 a 125.

### Correcciones de bring-up (sin commitear)
- **SPI3 del S3 solo tiene 3 CS por hardware** (`SOC_SPI_PERIPH_CS_NUM`). Las 5 IMU comparten ahora un dispositivo SPI sin CS, y `ICM42605` mueve su GPIO de CS por software (constructor nuevo con `sharedDevice`).
- **Tasa por defecto `max`** (antes `R1000`). `R1000` sigue disponible.
- **Direcciones y RDY de los ADC según lo medido** (ver §3.3).
- **Blindaje de RDY:** `configureAlertPin` deja la interrupción deshabilitada, y solo se habilita al registrar el ISR al empezar la adquisición (`gpio_intr_enable`/`disable` en `ADS1015.cpp`). *No probado aún en hardware.*
- **Diagnósticos de arranque:**
  - `#CHIP:PSRAM_CAP,FLASH_CAP,PKG`
  - `#I2CSCAN:<bus>,SDA=,SCL=,<dirs>`
  - `#RDYLINE:<adc>,<gpio>,<nivel>,<flancos en 20 ms>`
  - `#ADCCH:<adc>,<addr>,<c0..c3>,<mapa r/e>`
  - `#IDLE:<ms>` cada 5 s en reposo (temporal; quitar tras validar)

## 3. Resultados en hardware

### 3.1 Placa y periféricos
- Chip sin PSRAM (`#CHIP:PSRAM_CAP=0`): los GPIO 35–37 están libres para el SPI de IMU.
- SD lista (`#SDSTATE:READY`). El LED en GPIO45 funciona (rojo fijo cuando falta algún ADC).
- Heap libre en reposo: unos 134 KB.
- `#AUX … UARTERR` sube (1 → 16) con las bandas conectadas: ruido en el RX de UART1 (GPIO18). Pendiente de revisar.

### 3.2 Bandas conectadas al revés (incidente)
- Las dos bandas se conectaron primero invertidas: cada conector es el espejo del otro (pata *k* sobre la 15−*k*). En ese estado MISO quedó en alto (todas las IMU leían `0xFF`) y los buses I2C vacíos.
- Corregida la orientación, la comunicación volvió. No hay evidencia de daño, pero GPIO21/41 (CS) y GPIO35 (MOSI) estuvieron cortocircuitados a través de diodos durante algunos minutos.
- Una banda sin alimentar pero conectada arrastra SDA y MISO a 0 y tumba incluso a la IMU de placa.

### 3.3 Comunicación con los 8 módulos (`boot15-eight-modules.log`)

**ADC: los 4 se comunican** (escaneo, escritura de config, relectura `C3C0`, conversión y flanco RDY, 3 veces cada uno, `err=0`):

| ADC | Chip (esquemático) | Bus | Dirección | RDY real | Rótulo en el cerebro |
|---|---|---|---|---|---|
| ADC1 | U5, S1/S2 | I2C0 | 0x48 | **GPIO7** | ADC2_RDY |
| ADC2 | U6, S3/S4 | I2C0 | 0x49 | **GPIO6** | ADC1_RDY |
| ADC3 | U5, S1/S2 | I2C1 | 0x48 | **GPIO15** | ADC4_RDY |
| ADC4 | U6, S3/S4 | I2C1 | 0x49 | **GPIO9** | ADC3_RDY |

Las redes RDY1/RDY2 de la banda llegan cruzadas respecto a los rótulos del
cerebro. El firmware ya sigue el ruteo medido (`kRDY = {7, 6, 15, 9}`); queda
para la revisión de hardware.

**IMU: las 5 responden por SPI:**

| IMU | CS | WHO_AM_I |
|---|---|---|
| Placa | 38 | 0x42 ✅ |
| IMU1 | 21 | **0x44** |
| IMU2 | 40 | 0x42 ✅ |
| IMU3 | 41 | **0x44** |
| IMU4 | 42 | 0x42 ✅ |

Todas son ICM-42605 (confirmado por el equipo de hardware). IMU1 leía `0x42` con solo la
banda izquierda conectada (`boot13`, `boot14`), y pasó a `0x44` al añadir la
derecha. `0x44` (`0100 0100`) es `0x42` (`0100 0010`) con un bit saltado, lo
típico de un flanco de SCK de más por rebote o diafonía. Diagnóstico:
**integridad de señal en SCK** con las dos bandas cargando el bus. En el conector
de 14 pines, GND solo está en un extremo y SCK va entre MOSI y MISO.

### 3.4 Cuelgue después del arranque con ADC presentes
- Con al menos un ADC presente, la app imprime `#READY` y `#MAC` y luego no responde a `?` (0 bytes a los 12 s y a los 30 s). No aparece watchdog, pánico ni brownout. Solo con el cerebro, sí responde.
- Hipótesis: el pin RDY con interrupción habilitada desde el arranque, sin handler, cuyo ruido ocupa el servicio de ISR de GPIO en core 0. El blindaje de §2 lo ataca, y `#RDYLINE` / `#IDLE` lo confirmarán. **Requiere cargar la build nueva.**

## 4. Carga y puerto serie (problema abierto)

Observaciones con el CP2102 de la cinta (COM4, VID:PID 10C4:EA60):

1. **No hay auto-reset fiable.** Ninguna combinación de DTR/RTS produce un banner del ROM.
2. **Reset con el puerto abierto y DTR/RTS desactivados** → `boot:0x1 DOWNLOAD`, de forma reproducible.
3. **En ese estado de líneas, el RX hacia la placa no pasa**: ni el sync de esptool ni el `?` de la app reciben respuesta. Activar DTR/RTS después del banner tampoco lo arregló.
4. **Con DTR/RTS activados** (pyserial por defecto, como el monitor de PlatformIO), la app arranca normal y responde. **Para hablar con la app, abrir el puerto así.**
5. El usuario logró cargar a mano desde VS Code algunas veces. No quedó registrado el procedimiento exacto que funcionó; **es lo primero que hay que documentar.**
6. La placa no tiene botón BOOT. GPIO0 solo se controla desde las líneas del adaptador.

Herramientas:
- `tools/capture_boot.py <salida.log>`: escucha, pide Reset manual, guarda 40 s y manda `?` a los 12 y a los 30 s.
- `tools/flash_noboot.py`: intento de cargar sin BOOT sosteniendo el puerto. Entra en modo descarga pero el sync falla (punto 3). No resuelto.

Sugerencias para la próxima sesión:
- Medir con multímetro u osciloscopio a qué van conectados DTR y RTS en la placa (¿par de transistores de auto-reset o conexión directa?) y en qué estado deja cada uno a EN e IO0.
- Comprobar que el TX del CP2102 llega a GPIO44 (RX0) cuando DTR/RTS están desactivados.
- Si el cableado no permite auto-reset: añadir un pulsador o un jumper de GPIO0 a GND en la próxima revisión, o usar el USB nativo del S3 (GPIO19/20) si se rutea.

## 5. Requisitos de tasa (pendiente de medir)

- Modo `max` por defecto. **Requisito: raw ≥ 1000 Hz por canal y envolvente ≥ 50 Hz, con todas las IMU leyendo.** La envolvente es 1/20 del ciclo raw, así que raw ≥ 1000 ya implica envolvente ≥ 50.
- Si hace falta sacrificar algo, que sean las IMU. Por diseño la IMU no debería robar tiempo al sEMG (menor prioridad, otro bus). Si la medición muestra lo contrario, añadir un regulador que baje la tasa IMU (200 → 100 → 50 Hz) cuando raw se acerque a 1000.
- Referencia v0.1: `max` SD-only ≈ 1015 Hz raw, 50,8 Hz envolvente, IMU ≈ 200 Hz.
- Herramientas: `tools/bench_sd.py` y `tools/bench_acquisition.py` (ya separan las IMU por índice).

## 6. Pendientes, en orden

1. **Resolver la carga** (§4) y cargar `.dist/emg8-v02-storage-bench.bin`.
2. Verificar el arranque con la build nueva: `#RDYLINE` (¿algún RDY oscila?), `#IDLE` presente y respuesta a `?`. Esto cierra §3.4.
3. **Mapa raw/envolvente** con `#ADCCH`: los canales raw en el pedestal (~600), los de envolvente cerca de 0. Actualizar `kRawCh`/`kEnvCh`, que hoy son los de la v0.1.
4. **Corrupción de SCK** (§3.3):
   - Firmware: bajar la fuerza de salida de SCK, MOSI y CS a `GPIO_DRIVE_CAP_0` (~5 mA) y probar 500 kHz.
   - Medir con un test de arranque `#IMUCHK:<imu>,<ok>,<mal>` (1000 lecturas de WHO_AM_I por IMU) para comparar configuraciones con números.
   - Hardware, si no basta: resistencia serie de 22–47 Ω en SCK/MOSI junto al ESP32, y GND junto a SCK en el cable o el pinout de la próxima revisión.
5. Medir tasas en `max` con las 5 IMU (§5) y, si hace falta, el regulador de IMU.
6. Quitar `#IDLE` cuando esté validado. Revisar `UARTERR` de UART1.
7. Commitear (pedir autorización): fase 2, correcciones de bring-up y herramientas.
