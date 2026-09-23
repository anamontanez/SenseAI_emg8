"""Check short recordings and immediate file access after STOP, without resets.

Leaves all card files intact. Requires an idle device with mounted SD.
"""
import argparse
import io
import json
from pathlib import Path
import time

from bench_sd import Card, verify


def run(args):
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    log = io.StringIO()
    card = Card(args.port, log)
    active = False
    results = []
    try:
        card.until('#RATE:')  # belongs to Card's initial status query
        card.send('Rmax\n')
        assert card.until('#RATE:') == '#RATE:max'
        before = card.listing()
        for index, (mode, duration) in enumerate(((1, .005), (2, .05), (3, .15), (1, .02))):
            folder = out / str(index)
            saved, capture = folder / 'sd', folder / 'capture'
            saved.mkdir(parents=True)
            capture.mkdir()
            log_start = log.tell()
            card.send(str(mode))
            active = True
            card.until('#REC', seconds=35)
            time.sleep(duration)
            sent_stop = time.perf_counter()
            card.send('0')
            card.until('#STOP', seconds=30)
            stop_ms = (time.perf_counter() - sent_stop) * 1000
            active = False
            # Intentionally no grace period: STOP must mean drain + close done.
            after = card.listing()
            assert all(after.get(name) == size for name, size in before.items()), 'Existing file changed'
            created = {name: size for name, size in after.items()
                       if name not in before and not name.endswith('/')}
            assert len(created) == 4, created
            for name, size in created.items():
                card.download(name, size, saved / Path(name).name)
            counts = {}
            for line in log.getvalue()[log_start:].splitlines():
                rx = json.loads(line).get('rx', '')
                if rx.startswith('#CNT:'):
                    adc, *values = map(int, rx[5:].split(','))
                    counts[str(adc - 1)] = values
            assert len(counts) == 4 and all(values[4:] == [0, 0] for values in counts.values())
            card.send('?')
            status = list(map(int, card.until('#STATUS:').split(':')[1].split(',')))
            (capture / 'summary.json').write_text(json.dumps({
                'mode': mode, 'rate': 'max', 'failure': None,
                'counts': counts, 'final_status': status}))
            (capture / 'udp.bin').write_bytes(b'')
            result = verify(saved, created, capture)
            result.update(mode=mode, requested_seconds=duration, stop_ack_ms=round(stop_ms, 2))
            results.append(result)
            print(json.dumps(result), flush=True)
            before = after
        (out / 'summary.json').write_text(json.dumps(results, indent=2))
    finally:
        if active:
            card.send('0')
        card.close()
        (out / 'serial.jsonl').write_text(log.getvalue())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM9')
    parser.add_argument('--output', required=True)
    run(parser.parse_args())
