import struct
import unittest
from sd_clock import master_events, unwrap_sample


class RecordingClock(unittest.TestCase):
    def test_two_hours_without_hardware(self):
        previous = None
        for expected in range(0, 7200000001, 1000000):
            previous = unwrap_sample(expected & 0xffffffff, previous)
            self.assertEqual(previous, expected)
        with self.assertRaises(ValueError):
            unwrap_sample(999, 1000)

    def test_wrap_anchors_are_not_labels(self):
        header = bytearray(32)
        header[:5] = b'EMG8\x04'
        header[26:28] = bytes((2, 1))
        rows = [(0, 9, 1, 0x306), (0, 0, 0, 0x406),
                (0xfffffff0, 9, 2, 0x104), (10, 9, 3, 0x104),
                (20, 1, 0, 0x404), (2905032704, 1, 0, 0x404)]
        data = header + b''.join(struct.pack('<IHHI', *row) for row in rows)
        events = list(master_events(data))
        self.assertEqual([e['ts_us'] for e in events],
                         [0, 0, 0xfffffff0, 2**32+10, 2**32+20, 7200000000])
        self.assertEqual([e['grasp'] for e in events if e['kind'] == 1], [9, 9])
        self.assertTrue(all(e['grasp'] is None for e in events if e['kind'] == 4))
        data[27] = 0
        with self.assertRaisesRegex(ValueError, 'extension'):
            list(master_events(data))
