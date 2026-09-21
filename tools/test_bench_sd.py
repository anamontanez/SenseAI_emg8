"""Offline checks for the saved-record verification used on hardware."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from bench_sd import verify, RAW, ENV


class SavedRecords(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sd = self.root / 'sd'
        self.sd.mkdir()
        self.capture = self.root / 'capture'
        self.capture.mkdir()
        self.files = ['s_TEST_0/' + n + '000.bin' for n in 'REIM']
        streams = []
        for channels in (RAW, ENV):
            data = b''.join(struct.pack('<IBBh', ts, a, c, a*10+c)
                            for a in range(4) for c in channels[a] for ts in (100, 1100))
            streams.append(data)
        streams.append(b''.join(struct.pack('<I8h', ts, *([0]*8)) for ts in (100, 5100)))
        header = bytearray(32)
        header[:8] = b'EMG8\x04\x04\x04\x14'
        header[24] = 1
        for name, data in zip('REIM', streams+[header]):
            (self.sd / (name+'000.bin')).write_bytes(data)
        packets = bytearray()
        for kind, data in enumerate(streams):
            size = 20 if kind == 2 else 8
            packet = struct.pack('<2sBBIHH', b'E8', 1, kind, 0, len(data)//size, 0) + data
            packets += struct.pack('<QH', 0, len(packet)) + packet
        (self.capture / 'udp.bin').write_bytes(packets)
        self.summary = {'mode': 1, 'counts': {str(a): [2, 2, 2, 2, 0, 0] for a in range(4)},
                        'failure': None, 'final_status': [1, 0, 1, 1, 0, 0, 0, 0, 0]}
        self.write_summary()

    def write_summary(self):
        (self.capture / 'summary.json').write_text(json.dumps(self.summary))

    def check(self):
        return verify(self.sd, self.files, self.capture)

    def test_matching_saved_bytes(self):
        result = self.check()
        self.assertEqual(result['sd_records'], [16, 16, 2])
        self.assertEqual(result['sd_absent_from_udp'], [0, 0, 0])

    def test_phase_metadata_extension_and_corruption(self):
        path = self.sd / 'M000.bin'
        header = bytearray(path.read_bytes())
        header[26] = 1
        events = b''.join(struct.pack('<IHHI', *row) for row in
                          [(0, 7, 3, 0x305), (200, 7, 3, 0x204), (300, 8, 4, 0x104)])
        path.write_bytes(header + events)
        master = self.check()['masters'][0]
        self.assertEqual((master['event_count'], master['label_count']), (3, 1))
        bad = bytearray(events)
        bad[8] = 7
        path.write_bytes(header + bad)
        with self.assertRaisesRegex(AssertionError, 'Invalid phase'):
            self.check()
        path.write_bytes(header)
        with self.assertRaisesRegex(AssertionError, 'Missing initial'):
            self.check()

    def test_rate_metadata_matches_requested_capture(self):
        self.summary['rate'] = '1000'
        self.write_summary()
        with self.assertRaises(AssertionError):
            self.check()
        path = self.sd / 'M000.bin'
        data = bytearray(path.read_bytes())
        data[25] = 1
        path.write_bytes(data)
        self.assertEqual(self.check()['masters'][0]['rate_code'], 1)

    def test_capped_file_cannot_hide_excess_acquisition_rate(self):
        self.summary['rate'] = '1000'
        for a, channels in enumerate(RAW):
            for ch in channels:
                self.summary['counts'][str(a)][ch] = 0
        self.summary['counts']['0'][0] = 40
        self.write_summary()
        (self.sd / 'R000.bin').write_bytes(
            b''.join(struct.pack('<IBBh', i * 100, 0, 0, 0) for i in range(40)))
        path = self.sd / 'M000.bin'
        data = bytearray(path.read_bytes())
        data[25] = 1
        path.write_bytes(data)
        with self.assertRaisesRegex(AssertionError, 'Rate cap exceeded'):
            self.check()

    def test_udp_loss_does_not_imply_sd_loss(self):
        f = self.capture / 'udp.bin'
        data = f.read_bytes()
        size = struct.unpack_from('<H', data, 8)[0]
        f.write_bytes(data[10+size:])
        self.assertEqual(self.check()['sd_absent_from_udp'], [16, 0, 0])

    def test_missing_saved_sample_fails_counts(self):
        f = self.sd / 'R000.bin'
        f.write_bytes(f.read_bytes()[:-8])
        with self.assertRaises(AssertionError): self.check()

    def test_duplicate_udp_packet_is_not_a_disk_mismatch(self):
        f = self.capture / 'udp.bin'
        data = f.read_bytes()
        size = struct.unpack_from('<H', data, 8)[0]
        f.write_bytes(data + data[:10+size])
        self.assertEqual(self.check()['udp_absent_from_sd'], [0, 0, 0])

    def test_partial_saved_record_fails(self):
        f = self.sd / 'R000.bin'
        f.write_bytes(f.read_bytes()[:-1])
        with self.assertRaises(AssertionError): self.check()

    def test_wrong_value_fails_byte_comparison(self):
        f = self.sd / 'R000.bin'
        data = bytearray(f.read_bytes())
        struct.pack_into('<h', data, 6, 555)
        f.write_bytes(data)
        with self.assertRaises(AssertionError): self.check()

    def test_duplicate_timestamp_fails(self):
        f = self.sd / 'R000.bin'
        data = bytearray(f.read_bytes())
        data[8:12] = data[:4]
        f.write_bytes(data)
        with self.assertRaises(AssertionError): self.check()

    def test_wrong_recording_header_fails(self):
        f = self.sd / 'M000.bin'
        data = bytearray(f.read_bytes())
        data[24] = 3
        f.write_bytes(data)
        with self.assertRaises(AssertionError): self.check()


if __name__ == '__main__':
    unittest.main()
