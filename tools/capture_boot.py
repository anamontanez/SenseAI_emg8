"""Captura el arranque de la app tras un Reset manual y pide estado con '?'.

Abre COM4 como el monitor de PlatformIO (pyserial por defecto: DTR/RTS
activos). Con DTR/RTS desactivados esta placa no recibe nada por RX y, si se
presiona Reset en ese estado, arranca en modo descarga.

Con --reset no hace falta tocar la placa: pulsa EN por RTS igual que el
hard_reset de esptool (el mismo circuito con el que `pio run -t upload` carga).

Uso: python tools/capture_boot.py <salida.log> [--port COM4] [--seconds 40] [--reset]
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('output')
    ap.add_argument('--port', default='COM4')
    ap.add_argument('--seconds', type=float, default=40)
    ap.add_argument('--reset', action='store_true',
                    help='reiniciar por RTS (como esptool hard_reset) en vez de Reset manual')
    args = ap.parse_args()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, 460800, 0.05
    if args.reset:
        # Abrir con ambas lineas inactivas: en el circuito de auto-reset de dos
        # transistores, niveles iguales no tocan EN ni IO0.
        s.dtr = False
        s.rts = False
    s.open()
    if args.reset:
        s.rts = True    # EN en bajo (DTR inactivo -> IO0 alto: arranque normal)
        time.sleep(0.1)
        s.rts = False
        # Esperar a que se latcheen los strapping: activar DTR con RTS inactivo
        # baja IO0, y justo al soltar EN eso seria modo descarga.
        time.sleep(0.5)
        # Volver al estado del monitor (ambas activas). DTR primero: activar RTS
        # con DTR aun inactivo pondria EN en bajo otra vez.
        s.dtr = True
        s.rts = True
        print('Reset por RTS enviado.', flush=True)
    else:
        print('Escuchando; presiona Reset.', flush=True)
    b = b''
    t0 = time.time()
    while not b and time.time() - t0 < 300:
        b += s.read(8192)
    t = time.time()
    marks, sent = [], set()
    while time.time() - t < args.seconds:
        for at in (12, 30):
            if time.time() - t >= at and at not in sent:
                sent.add(at)
                s.write(b'?')
                marks.append((at, len(b)))
        b += s.read(8192)
    s.close()
    open(args.output, 'wb').write(b)
    txt = b.decode('latin1').encode('ascii', 'replace').decode()
    print('bytes=%d' % len(b))
    for at, pos in marks:
        print("--- tras '?' a los %ds: %d bytes" % (at, len(b) - pos))
    drop = ('#NETDIAG', '#LINK', '#PHASE', '#IDENTITY', '#CDCFG', '#SDIO', '#FIRMWARE')
    keep = [l for l in txt.splitlines()
            if l.startswith(('#', 'ADC', 'Guru', 'Backtrace', 'abort', 'rst:', 'E (', 'W ('))
            and not l.startswith(drop)]
    print('\n'.join(keep))
    return 0


if __name__ == '__main__':
    sys.exit(main())
