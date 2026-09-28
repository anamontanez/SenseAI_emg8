import struct
import contextlib
import io
import json
from pathlib import Path
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from bench_acquisition import (HEADER, SAMPLE, Records, firmware_diagnostics,
                               recording_stop_kind, stop_drain_complete,
                               validate_sd_summary, run)


def packet(seq, records, kind=0):
    return HEADER.pack(b'E8', 1, kind, seq, len(records), 0) + b''.join(
        SAMPLE.pack(*record) for record in records)


class DecoderTests(unittest.TestCase):
    def test_missing_or_truncated_diagnostics_are_not_zero_errors(self):
        result = firmware_diagnostics(['#TIMING:4,ready,106623,40858021,377,0',
                                       '#ADC_EVENTS:1,0,0,0,0'])
        self.assertFalse(result['diagnostics_complete'])
        self.assertNotIn('4:ready', result['timing'])
        self.assertIn('ADC_EVENTS:4', result['missing_diagnostics'])
        self.assertEqual(len(result['malformed_diagnostics']), 1)
        self.assertIsNone(firmware_diagnostics([])['diagnostics_complete'])

    def test_complete_diagnostics_and_corrupt_numeric_field(self):
        lines = []
        for adc in range(1, 5):
            for name in ('trigger', 'wake', 'read', 'publish', 'ready', 'turnaround'):
                lines.append(f'#TIMING:{adc},{name},0,0,0,0,0,0,0,0,0,0,0,0')
            lines.append(f'#ADC_EVENTS:{adc},0,0,0,0')
            lines.extend(f'#ACQ:{adc},{ch},0,0,0' for ch in range(4))
        self.assertTrue(firmware_diagnostics(lines)['diagnostics_complete'])
        combined = [line.replace(',read,', ',exchange,') for line in lines]
        result = firmware_diagnostics(combined)
        self.assertTrue(result['diagnostics_complete'])
        self.assertIn('1:exchange', result['timing'])
        self.assertNotIn('1:read', result['timing'])
        combined = [line for line in combined if not line.startswith('#TIMING:4,exchange,')]
        result = firmware_diagnostics(combined)
        self.assertFalse(result['diagnostics_complete'])
        self.assertIn('4:exchange', result['missing_diagnostics'])
        lines[-1] = '#ACQ:4,3,broken,0,0'
        result = firmware_diagnostics(lines)
        self.assertFalse(result['diagnostics_complete'])
        self.assertEqual(result['malformed_diagnostics'], [lines[-1]])

    def test_firmware_timing_and_acquisition_wrap(self):
        result = firmware_diagnostics([
            '#TIMING:1,read,2,120,50,70,0,0,2,0,0,0,0,0',
            '#TIMING:1,wake,0,0,0,0,0,0,0,0,0,0,0,0',
            '#ADC_EVENTS:1,3,4',
            '#ADC_EVENTS:2,0,0,5,7',
            '#NET:192.168.4.2:1234',
            '#NET:TX=100,ERR=2,DROP=3',
            '#ACQ:1,0,3,4294967040,1744',
            '#ACQ:1,1,0,0,0',
        ])
        self.assertEqual(result['timing']['1:read']['mean_us'], 60)
        self.assertEqual(sum(result['timing']['1:read']['bins']), 2)
        self.assertIsNone(result['timing']['1:wake']['mean_us'])
        self.assertEqual(result['device_acquisition']['0:0']['hz'], 1000)
        self.assertIsNone(result['device_acquisition']['0:1']['hz'])
        self.assertEqual(result['adc_events']['1'], {'queue_drops': 3, 'spurious': 4})
        self.assertEqual(result['firmware_network'], {'tx': 100, 'err': 2, 'drop': 3})
        self.assertEqual(result['adc_events']['2']['early_ready'], 5)
        self.assertEqual(result['adc_events']['2']['unasserted_ready'], 7)

    def test_current_mapping_and_rates(self):
        r = Records()
        r.feed(packet(1, [(100, 1, 1, -12), (1100, 1, 1, 3), (2100, 1, 1, 7)]))
        self.assertEqual(r.summary()['channels']['0:1:1']['received_hz'], 1000)
        r.feed(packet(2, [(100, 1, 0, 3)]))  # ADC2 ch0 is envelope, not raw
        self.assertEqual(r.invalid, 1)

    def test_sequence_wrap_gaps_and_duplicates(self):
        r = Records()
        for seq in (0xffffffff, 0, 2, 2):
            r.feed(packet(seq, [(100, 0, 0, 1)]))
        self.assertEqual(r.gaps[0], 1)
        self.assertEqual(r.duplicates[0], 1)
        self.assertEqual(len(r.timestamps['0:0:0']), 3)

    def test_truncated_datagram_is_rejected(self):
        r = Records()
        r.feed(packet(0, [(1, 0, 0, 0)])[:-1])
        self.assertEqual(r.invalid, 1)
        self.assertFalse(r.timestamps)

    def test_reordered_packet_recovers_gap_without_losing_records(self):
        r = Records()
        for seq, ts in ((0, 100), (2, 2100), (1, 1100)):
            r.feed(packet(seq, [(ts, 0, 0, 1)]))
        self.assertEqual(r.gaps[0], 0)
        self.assertEqual(r.reordered[0], 1)
        self.assertEqual(r.summary()['channels']['0:0:0']['received_hz'], 1000)

    def test_timestamp_wrap(self):
        r = Records()
        r.feed(packet(0, [(0xffffff00, 0, 0, 0), (744, 0, 0, 0)]))
        self.assertEqual(r.summary()['channels']['0:0:0']['received_hz'], 1000)

    def test_complete_windows_exclude_partial_edges(self):
        r = Records()
        for ts in range(1000, 20002000, 1000):
            r.timestamps['0:0:0'].append(ts)
        self.assertEqual(r.summary()['channels']['0:0:0']['complete_10s_received_hz'], [1000])


class StopDrainTests(unittest.TestCase):
    def test_refusal_unexpected_stop_and_drain_gates(self):
        self.assertEqual(recording_stop_kind(False, False, False), None)
        self.assertEqual(recording_stop_kind(True, False, False), 'refused')
        self.assertEqual(recording_stop_kind(True, False, True), 'unexpected')
        self.assertEqual(recording_stop_kind(True, True, True), 'ack')
        self.assertFalse(stop_drain_complete(False, 4, 2, 1, True))
        self.assertFalse(stop_drain_complete(True, 3, 2, 1, True))
        self.assertFalse(stop_drain_complete(True, 4, 1, 1, True))
        self.assertTrue(stop_drain_complete(True, 4, 2, 1, True))
        self.assertTrue(stop_drain_complete(True, 4, 1, 1, False))

    def test_missing_and_incomplete_saved_summary_fail(self):
        with self.assertRaisesRegex(TimeoutError, 'Missing SD final summary'):
            validate_sd_summary(None, True)
        with self.assertRaisesRegex(RuntimeError, 'incomplete'):
            validate_sd_summary({'result': 'INCOMPLETE'}, True)
        validate_sd_summary({'result': 'OK'}, True)

    def test_stop_waits_for_close_delayed_over_one_second(self):
        class FakeSerial:
            instance = None

            def __init__(self, **_kwargs):
                type(self).instance = self
                self.port = None
                self.dtr = self.rts = False
                self.timeout = 0
                self.is_open = False
                self.rx = bytearray()
                self.stop_requested_at = None
                self.close_delay = None
                self.final_lines_queued = False

            def open(self):
                self.is_open = True

            def close(self):
                self.is_open = False

            def write(self, data):
                command = data.decode('ascii')
                if command == '?':
                    self.rx.extend(b'#STATUS:1,0,1,1,0,0,0,0,0\n')
                elif command == 'R1000\n':
                    self.rx.extend(b'#RATE:1000\n')
                elif command == '1':
                    self.rx.extend(b'#REC\n')
                elif command == '0' and self.stop_requested_at is None:
                    self.stop_requested_at = time.monotonic()
                return len(data)

            def _release_close(self):
                if (self.stop_requested_at is not None and
                        not self.final_lines_queued and
                        time.monotonic() - self.stop_requested_at >= 1.1):
                    self.close_delay = time.monotonic() - self.stop_requested_at
                    rows = [
                        '#SDSUM:s_TEST_0/000.bin,16,16,2,328,OK',
                        '#CNT:1,2,2,2,2,0,0',
                        '#CNT:2,2,2,2,2,0,0',
                        '#CNT:3,2,2,2,2,0,0',
                        '#CNT:4,2,2,2,2,0,0',
                        '#STOP',
                    ]
                    self.rx.extend(('\n'.join(rows) + '\n').encode('ascii'))
                    self.final_lines_queued = True

            @property
            def in_waiting(self):
                self._release_close()
                return len(self.rx)

            def read(self, size=1):
                self._release_close()
                if not self.rx:
                    return b''
                result = self.rx[:size]
                del self.rx[:size]
                return bytes(result)

        with tempfile.TemporaryDirectory() as folder:
            args = SimpleNamespace(port='COM5', host='192.168.4.1', condition='off',
                mode=1, rate='1000', require_sd=True, status_interval=0,
                seconds=0.05, connect_wait=0, wifi_profile=None,
                output=str(Path(folder) / 'capture'))
            fake_module = SimpleNamespace(Serial=FakeSerial)
            with patch.dict('sys.modules', {'serial': fake_module}), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run(args), 0)
            self.assertGreaterEqual(FakeSerial.instance.close_delay, 1.0)
            result = json.loads((Path(args.output) / 'summary.json').read_text())
            self.assertIsNone(result['failure'])
            self.assertEqual(result['sd_summary']['result'], 'OK')
            self.assertEqual(len(result['counts']), 4)


if __name__ == '__main__':
    unittest.main()
