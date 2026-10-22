"""Flashea la app en la placa v0.2, que no tiene boton BOOT.

Con el CP2102 de la cinta, Reset con el puerto abierto y DTR/RTS desactivados
deja GPIO0 en bajo: el ROM arranca en modo descarga. Pero en ese estado de
lineas el RX hacia la placa no pasa, y esptool reabre el puerto. Por eso aqui
el puerto se abre UNA vez, se espera el "waiting for download" tras el Reset
manual, se activan DTR/RTS (no resetean esta placa) y se le pasa a esptool.

Despues reabre el puerto como el monitor (DTR/RTS activos, arranque normal),
espera el segundo Reset y guarda el arranque de la app.

Uso:
  python tools/flash_noboot.py --bin .dist/emg8-v02-storage-bench.bin \
      --capture benchmarks/<carpeta>/boot.log
"""
import argparse
import sys
import time
from pathlib import Path

import serial

ESPTOOL = Path.home() / '.platformio/packages/tool-esptoolpy'
sys.path.insert(0, str(ESPTOOL))
import esptool  # noqa: E402
from esptool.targets import ESP32S3ROM  # noqa: E402


def wait_for(port, needle, seconds):
    data = b''
    end = time.time() + seconds
    while time.time() < end and needle not in data:
        data += port.read(4096)
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='COM4')
    ap.add_argument('--bin', required=True)
    ap.add_argument('--offset', default='0x10000')
    ap.add_argument('--baud', default='460800')
    ap.add_argument('--capture', help='archivo donde guardar el arranque posterior')
    ap.add_argument('--capture-seconds', type=float, default=40)
    args = ap.parse_args()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, 115200, 0.05
    s.dtr = False
    s.rts = False
    s.open()
    print('PASO 1: presiona Reset (una vez).', flush=True)
    banner = wait_for(s, b'waiting for download', 300)
    if b'waiting for download' not in banner:
        print('No entro en modo descarga:', banner[-300:].decode('latin1'), flush=True)
        return 1
    print('Modo descarga OK; flasheando...', flush=True)
    # GPIO0 solo cuenta en el instante del Reset. Con DTR/RTS desactivados el
    # RX hacia la placa no pasa (ni el '?' de la app ni el sync del ROM), y
    # cambiarlos no resetea esta placa: activarlos antes del sync.
    s.rts = True
    s.dtr = True
    time.sleep(0.1)
    s.reset_input_buffer()
    esp = ESP32S3ROM(s, 115200)
    esp.connect('no_reset')
    esptool.main(['--chip', 'esp32s3', '--port', args.port, '--baud', args.baud,
                  '--before', 'no_reset', '--after', 'no_reset',
                  'write_flash', args.offset, args.bin], esp=esp)
    try:
        s.close()
    except Exception:
        pass
    if not args.capture:
        print('Flasheo terminado. Presiona Reset para arrancar la app.', flush=True)
        return 0

    # Como el monitor de PlatformIO: DTR/RTS activos -> arranque normal.
    c = serial.Serial(args.port, 460800, timeout=0.05)
    print('PASO 2: presiona Reset otra vez para arrancar la app.', flush=True)
    log = b''
    t0 = time.time()
    while not log and time.time() - t0 < 300:
        log += c.read(8192)
    t = time.time()
    sent = set()
    while time.time() - t < args.capture_seconds:
        for at in (12, 30):
            if time.time() - t >= at and at not in sent:
                sent.add(at)
                c.write(b'?')
        log += c.read(8192)
    c.close()
    Path(args.capture).parent.mkdir(parents=True, exist_ok=True)
    Path(args.capture).write_bytes(log)
    print(f'Arranque guardado en {args.capture} ({len(log)} bytes)', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
