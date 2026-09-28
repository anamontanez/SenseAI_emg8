"""Reference decoding for the unchanged u32 sample clock and v4 master extension 2.

Maintain one previous timestamp per ADC/channel (and one for IMU). Samples from
different ADCs may be interleaved slightly out of timestamp order. Each recording
starts a new file set and a new clock. Master kind 4 is an anchor, never a label.
"""
import struct

MODULUS = 1 << 32


def unwrap_sample(timestamp, previous=None):
    if not 0 <= timestamp < MODULUS:
        raise ValueError("Timestamp is not uint32")
    if previous is None:
        return timestamp
    high = previous & ~(MODULUS - 1)
    before = previous & (MODULUS - 1)
    if timestamp < before:
        if before - timestamp <= MODULUS // 2:
            raise ValueError("Timestamp reversed within a channel")
        high += MODULUS
    return high + timestamp


def master_events(data):
    if len(data) < 32 or data[:5] != b"EMG8\x04" or (len(data) - 32) % 12:
        raise ValueError("Invalid v4 master file")
    extension = data[26]
    if extension not in (0, 1, 2) or (extension == 2 and data[27] != 1):
        raise ValueError("Unknown metadata extension")
    previous = None
    for ts, grasp, rep, word in struct.iter_unpack("<IHHI", data[32:]):
        kind = word >> 8 if extension else 1
        phase = word & 255 if extension else None
        if extension and (phase not in (4, 5, 6) or kind not in (1, 2, 3, 4)
                          or (kind == 4 and extension != 2)):
            raise ValueError("Invalid phase/event metadata")
        if kind == 4:
            full = ((grasp | (rep << 16)) << 32) | ts
        else:
            full = unwrap_sample(ts, previous)
        if previous is not None and full < previous:
            raise ValueError("Master timestamp reversed")
        previous = full
        yield {"ts_us": full, "kind": kind, "phase": phase,
               "grasp": grasp if kind != 4 else None,
               "repetition": rep if kind != 4 else None}
