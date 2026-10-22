"""Guard the sEMG-first scheduling and the multi-IMU record contract."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def task_priorities(src):
    # ESP-IDF: configMAX_PRIORITIES = 25.
    tasks = {}
    for name, prio, core in re.findall(
            r'xTaskCreatePinnedToCore\(\w+,\s*"(\w+)",\s*\d+,\s*[^,]+,\s*([^,]+),\s*nullptr,\s*(\d)\)', src):
        prio = prio.strip()
        tasks[name] = (25 - 2 if prio == 'configMAX_PRIORITIES - 2' else int(prio), int(core))
    return tasks


class ImuScheduling(unittest.TestCase):
    def setUp(self):
        self.src = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')

    def test_semg_workers_outrank_imu_and_storage(self):
        tasks = task_priorities(self.src)
        for name in ('adc0', 'adc1', 'imu', 'sd'):
            self.assertIn(name, tasks)
        # Same core: the ADC workers must preempt both the SD writer and the IMU.
        self.assertEqual({tasks[n][1] for n in ('adc0', 'adc1', 'imu', 'sd')}, {1})
        self.assertGreater(tasks['adc0'][0], tasks['sd'][0])
        self.assertGreater(tasks['sd'][0], tasks['imu'][0])

    def test_every_imu_index_is_read_and_tagged(self):
        body = self.src[self.src.index('static void imuTask(void*) {'):]
        body = body[:body.index('/* ── Countdown')]
        self.assertIn('for (int i = 0; i < kNUM_IMUS; ++i)', body)
        self.assertIn('if (!(imuMask & (1U << i))) continue;', body)
        self.assertIn('s.imu = (uint8_t)i;', body)
        cs = re.search(r'kImuCs\[kNUM_IMUS\] = \{([^}]*)\}', self.src).group(1)
        self.assertEqual([c.strip() for c in cs.split(',')],
                         ['kIMU_CS', 'kExtImuCs[0]', 'kExtImuCs[1]', 'kExtImuCs[2]', 'kExtImuCs[3]'])

    def test_imu_queue_and_batch_keep_up_with_five_imus(self):
        qlen = int(re.search(r'kIMU_QLEN\s*=\s*(\d+);', self.src).group(1))
        batch = int(re.search(r'static ImuSample imuBuf\[(\d+)\];', self.src).group(1))
        # Five IMUs at 200 Hz: ride out at least a second of SD stall, and drain
        # faster than the ~62 records that arrive while 500 raw records are written.
        self.assertGreaterEqual(qlen, 5 * 200)
        self.assertGreater(batch, 500 * 1000 // 8000)
        self.assertIn(f'writeBatch(imuQ, imuBuf, {batch},', self.src)


if __name__ == '__main__':
    unittest.main()
