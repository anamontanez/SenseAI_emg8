"""Captura el arranque de la app tras un Reset manual y pide estado con '?'.

Abre COM4 como el monitor de PlatformIO (pyserial por defecto: DTR/RTS
activos). Con DTR/RTS desactivados esta placa no recibe nada por RX y, si se
presiona Reset en ese estado, arranca en modo descarga.

Uso: python tools/capture_boot.py <salida.log> [--port COM4] [--seconds 40]
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
    args = ap.parse_args()
    s = serial.Serial(args.port, 460800, timeout=0.05)
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
