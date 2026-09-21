"""Record to mounted SD, retrieve only new files, and compare SD/UDP records.

Never mounts, formats or deletes card contents. Uses the existing UART F/G
protocol and requires an idle device. Recordings remain on the card.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

import serial
from bench_acquisition import RAW, ENV


class Card:
    def __init__(self, port, log):
        self.log = log
        self.s = serial.Serial(port=None, baudrate=460800, timeout=.1, write_timeout=2)
        self.s.dtr = False
        self.s.rts = False
        self.s.port = port
        self.s.open()
        if hasattr(self.s, 'set_buffer_size'):
            # Windows' small default RX buffer can overrun during multi-MB
            # 460800-baud downloads; this does not guarantee lossless transfer.
            self.s.set_buffer_size(rx_size=1024 * 1024, tx_size=65536)
        self.send('U1?')
        status = self.until('#STATUS:')
        fields = list(map(int, status.split(':')[1].split(',')))
        if fields[1:3] != [0, 1]:
            self.s.close()
            raise RuntimeError('Requires idle device with mounted SD: ' + status)

    def send(self, cmd):
        self.log.write(json.dumps({'tx': cmd}) + '\n')
        self.log.flush()
        self.s.write(cmd.encode('ascii'))

    def until(self, prefix, seconds=15):
        deadline = time.monotonic() + seconds
        pending = bytearray()
        while time.monotonic() < deadline:
            pending.extend(self.s.read(1))
            if pending.endswith(b'\n'):
                line = pending.decode('ascii', errors='backslashreplace').strip()
                pending.clear()
                self.log.write(json.dumps({'rx': line}) + '\n')
                self.log.flush()
                if line.startswith('#ERR:') or line.startswith('#BOOT:') or 'Guru Meditation' in line:
                    raise RuntimeError(line)
                if line.startswith(prefix):
                    return line
        raise TimeoutError('Missing ' + prefix)

    def listing(self):
        self.send('F')
        self.until('#FLIST:')
        result = {}
        while True:
            line = self.until('#F')
            if line == '#FEND':
                return result
            if line.startswith('#F:'):
                name, size = line[3:].rsplit(',', 1)
                result[name] = int(size)

    def download(self, name, size, destination):
        if not re.fullmatch(r's_[A-Za-z0-9_]+/[REIM][0-9]+\.bin', name):
            raise ValueError('Unexpected recording filename: ' + name)
        self.send('G' + name + '\n')
        header = self.until('#FDATA:')
        reported = int(header.rsplit(',', 1)[1])
        if reported != size:
            raise RuntimeError('File changed after listing: ' + name)
        deadline = time.monotonic() + 20 + size / 30000
        left = size
        with destination.open('xb') as f:
            while left:
                data = self.s.read(min(left, 16384))
                f.write(data)
                left -= len(data)
                if time.monotonic() > deadline:
                    raise TimeoutError('Incomplete download: ' + name)
        self.until('#FDONE')

    def close(self):
        self.s.close()


def verify(folder, files, capture):
    summary = json.loads((capture / 'summary.json').read_text())
    disk = [collections.Counter() for _ in range(3)]
    counts = collections.Counter()
    last = {}
    first = {}
    masters = []
    for name in files:
        data = (folder / Path(name).name).read_bytes()
        kind = name.rsplit('/', 1)[1][0]
        size = 8 if kind in 'RE' else 20 if kind == 'I' else 12
        if kind == 'M':
            assert len(data) >= 32 and data[:8] == b'EMG8\x04\x04\x04\x14'
            assert data[24] == summary['mode'] and (len(data) - 32) % 12 == 0
            if summary.get('rate'):
                assert data[25] == (1 if summary['rate'] == '1000' else 0), 'Wrong rate metadata'
            events = list(struct.iter_unpack('<IHHI', data[32:]))
            extension = data[26]
            assert extension in (0, 1), 'Unknown metadata extension'
            label_count = len(events)
            if extension == 1:
                assert events and events[0][0] == 0 and events[0][3] >> 8 == 3, 'Missing initial metadata'
                assert all(word & 255 in (4, 5, 6) and word >> 8 in (1, 2, 3)
                           for _, _, _, word in events), 'Invalid phase/event metadata'
                assert all(a[0] <= b[0] for a, b in zip(events, events[1:])), 'Metadata time reversed'
                label_count = sum(word >> 8 == 1 for _, _, _, word in events)
            masters.append({'file': name, 'mode': data[24], 'rate_code': data[25],
                            'metadata_extension': extension, 'event_count': len(events),
                            'label_count': label_count})
            continue
        assert len(data) % size == 0, name
        k = 'REI'.index(kind)
        for off in range(0, len(data), size):
            rec = data[off:off+size]
            ts = struct.unpack_from('<I', rec)[0]
            if kind in 'RE':
                _, adc, ch, value = struct.unpack('<IBBh', rec)
                assert adc < 4 and ch in (RAW if kind == 'R' else ENV)[adc]
                assert -2048 <= value <= 2047
                key = (adc, ch)
                counts[key] += 1
            else:
                key = ('imu',)
            assert key not in last or ts > last[key], (name, key, ts, last.get(key))
            first.setdefault(key, ts)
            last[key] = ts
            disk[k][rec] += 1
    for adc, values in summary['counts'].items():
        for ch in range(4):
            assert counts[int(adc), ch] == values[ch], (adc, ch, counts[int(adc), ch], values[ch])
    if summary.get('rate') == '1000':
        for (adc, ch), count in counts.items():
            hz = 50 if summary['mode'] == 1 and ch in ENV[adc] else 1000
            # Bound the actual saved acquisition rate, allowing a pacing-frame
            # boundary plus two conversions of timestamp/scheduling tolerance.
            allowance = 2 if hz == 50 else 22
            span = last[adc, ch] - first[adc, ch]
            assert count <= span * hz / 1_000_000 + allowance, ('Rate cap exceeded', adc, ch)
    received = [collections.Counter() for _ in range(3)]
    seen = [set() for _ in range(3)]
    with (capture / 'udp.bin').open('rb') as f:
        while h := f.read(10):
            assert len(h) == 10
            _, n = struct.unpack('<QH', h)
            pkt = f.read(n)
            assert len(pkt) == n and pkt[:3] == b'E8\x01'
            kind = pkt[3]
            seq = struct.unpack_from('<I', pkt, 4)[0]
            if seq in seen[kind]:
                continue
            seen[kind].add(seq)
            size = 20 if kind == 2 else 8
            for off in range(12, len(pkt), size):
                received[kind][pkt[off:off+size]] += 1
    unexpected = [sum((rx - sd).values()) for rx, sd in zip(received, disk)]
    missing = [sum((sd - rx).values()) for rx, sd in zip(received, disk)]
    assert not any(unexpected), unexpected
    assert len(masters) == 1
    assert summary['failure'] is None and summary['final_status'][2] == 1
    assert not any(summary['final_status'][6:9]), summary['final_status']
    return {'sd_records': [sum(x.values()) for x in disk], 'udp_absent_from_sd': unexpected,
            'sd_absent_from_udp': missing, 'masters': masters,
            'adc_counts_match': True, 'storage_drops': summary['final_status'][6:9]}


def run(args):
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    failure = None
    result = {}
    card = None
    with (out / 'card-serial.jsonl').open('w') as log:
        try:
            card = Card(args.port, log)
            before = card.listing()
            (out / 'listing-before.json').write_text(json.dumps(before, indent=2))
            card.close()
            card = None
            cmd = [sys.executable, '-B', str(Path(__file__).with_name('bench_acquisition.py')),
                   '--port', args.port, '--require-sd', '--condition', args.condition,
                   '--mode', str(args.mode), '--seconds', str(args.seconds), '--output', str(out/'capture')]
            if args.status_interval:
                cmd += ['--status-interval', str(args.status_interval)]
            if args.rate:
                cmd += ['--rate', args.rate]
            if args.wifi_profile:
                cmd += ['--wifi-profile', args.wifi_profile]
            subprocess.run(cmd, check=True)
            card = Card(args.port, log)
            after = card.listing()
            (out / 'listing-after.json').write_text(json.dumps(after, indent=2))
            assert all(after.get(k) == v for k, v in before.items()), 'Existing file changed'
            created = {k: v for k, v in after.items() if k not in before and not k.endswith('/')}
            assert len(created) == 4, created
            saved = out / 'sd'
            saved.mkdir()
            for name, size in sorted(created.items()):
                print('DOWNLOAD', name, size, flush=True)
                card.download(name, size, saved / Path(name).name)
            result = verify(saved, created, out / 'capture')
        except Exception as e:
            failure = repr(e)
        finally:
            if card:
                card.close()
    result['failure'] = failure
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return int(failure is not None)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', default='COM9')
    ap.add_argument('--condition', choices=('off', 'udp', 'quiet'), default='udp')
    ap.add_argument('--mode', choices=(1, 2, 3, 4), type=int, default=1)
    ap.add_argument('--rate', choices=('max', '1000'))
    ap.add_argument('--status-interval', type=float, default=0)
    ap.add_argument('--seconds', type=float, default=60)
    ap.add_argument('--wifi-profile')
    ap.add_argument('--output', required=True)
    raise SystemExit(run(ap.parse_args()))
