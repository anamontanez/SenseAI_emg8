"""Exercise UART phase commands, quiet RX, link cadence and saved SD metadata."""
import argparse
import collections
import json
from pathlib import Path
import struct
import time
from bench_sd import Card

def run(args):
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    counts = {}
    csv_times = [[], []]
    log = (out / 'serial.jsonl').open('w', encoding='utf-8')
    card = Card(args.port, log)
    active = False
    try:
        card.until('#RATE:')
        card.send('W0')
        card.until('#WIFI:0')
        before = card.listing()
        card.send('L7,3\nPrest\n')
        card.until('#LABEL:7,3')
        card.until('#PHASE:rest')
        card.send('?')
        link_before = card.until('#LINK:')
        card.until('#RATE:')
        card.send('1')
        active = True
        card.until('#REC')
        start = time.perf_counter()
        commands = [(3, 'Pgrasp\n'), (5, 'U0L8,4\nPrest\n'),
                    (7, 'U1P?\n'), (8, 'W1'), (12, 'Pdemo\n'),
                    (16, 'Prest\n'), (18, 'Pgrasp\n')]
        events = []
        while time.perf_counter() - start < 33:
            elapsed = time.perf_counter() - start
            if commands and elapsed >= commands[0][0]:
                card.send(commands.pop(0)[1])
            line = card.s.readline().decode('ascii', errors='backslashreplace').strip()
            if not line:
                continue
            log.write(json.dumps({'s': elapsed, 'rx': line}) + '\n')
            if '#BOOT:' in line or '#ERR:' in line or 'Guru Meditation' in line:
                raise AssertionError(line)
            if line.startswith('D,'):
                if .5 < elapsed < 2.8:
                    csv_times[0].append(int(line.split(',')[1]))
                if 20 < elapsed < 30:
                    csv_times[1].append(int(line.split(',')[1]))
            if line.startswith('#PHASE:'):
                events.append((elapsed, line))
        card.send('0')
        for _ in range(4):
            values = list(map(int, card.until('#CNT:').split(':')[1].split(',')))
            counts[values[0]-1] = values[1:]
        card.until('#STOP')
        active = False
        card.send('?')
        link_after = card.until('#LINK:')
        card.until('#RATE:')
        after = card.listing()
        assert all(after.get(name) == size for name, size in before.items())
        added = {name:size for name,size in after.items() if name not in before}
        assert len(added)==4, added
        disk_counts = collections.Counter()
        metadata = []
        for name,size in sorted(added.items()):
            target = out / Path(name).name
            card.download(name, size, target)
            data = target.read_bytes()
            if target.name.startswith('M'):
                assert data[26]==1 and (len(data)-32)%12==0
                metadata = list(struct.iter_unpack('<IHHI', data[32:]))
            if target.name[0] in 'RE':
                for ts,adc,ch,value in struct.iter_unpack('<IBBh',data):
                    disk_counts[adc,ch]+=1
        for adc,values in counts.items():
            for ch in range(4):
                assert disk_counts[adc,ch]==values[ch]
        expected = [(7,3,0x305),(7,3,0x204),(8,4,0x104),(8,4,0x205),
                    (8,4,0x206),(8,4,0x205),(8,4,0x204)]
        assert [row[1:] for row in metadata]==expected, metadata
        assert metadata[0][0]==0
        assert all(a[0]<=b[0] for a,b in zip(metadata,metadata[1:]))
        assert any(7<=t<8 and line=='#PHASE:rest' for t,line in events), events
        def stats(line):
            return {k:int(v) for k,v in (f.split('=') for f in line.split(':')[1].split(','))}
        delta = {k:stats(link_after)[k]-v for k,v in stats(link_before).items()}
        assert delta==dict(START=1,STOP=1,PHASE=6,SYNC=3,QERR=0,TXERR=0), delta
        rates = [(len(ts)-1)*1e6/(ts[-1]-ts[0]) for ts in csv_times]
        assert 30<rates[0]<55 and .8<rates[1]<1.2, rates
        result = dict(link_delta=delta, csv_hz_radio_off_on=rates,
                      metadata=metadata, adc_counts_match=True, phase_events=events,
                      adc_error_counters={a:v[4:] for a,v in counts.items()})
        (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result))
    finally:
        card.send('U1')
        if active:
            card.send('0')
            card.until('#STOP')
        card.send('W0Pdemo\nL0,0\n')
        card.until('#LABEL:0,0')
        card.close()
        log.close()

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',default='COM9')
    ap.add_argument('--output',required=True)
    run(ap.parse_args())
