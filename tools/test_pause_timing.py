"""Compile actual pause accounting, including clock wrap and reset."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PauseAccounting(unittest.TestCase):
    def test_elapsed_pauses_wrap_and_reset(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'src/pause_timing.hpp').read_text(encoding='utf-8')
        source = source.replace('#pragma once', '').replace('#include <cstdint>',
                                                            'using uint32_t = unsigned;')
        checks = r"""
constexpr bool check() {
    PauseTiming p;
    p.update(false, 10);
    if (p.count || p.completedMs || p.currentMs || p.maxMs) return false;
    p.update(true, 20);
    p.update(true, 35);
    if (p.count != 1 || p.currentMs != 15 || p.maxMs != 15) return false;
    p.update(false, 40);
    p.update(false, 70); // idle time must not be charged to a pause
    if (p.active || p.completedMs != 20 || p.currentMs || p.maxMs != 20) return false;
    p.update(true, 80);
    p.update(false, 85);
    if (p.count != 2 || p.completedMs != 25 || p.maxMs != 20) return false;
    p = {};
    p.update(true, 0xfffffff0u);
    p.update(false, 0x10u);
    if (p.count != 1 || p.completedMs != 32 || p.maxMs != 32) return false;
    p = {};
    return !p.active && !p.count && !p.completedMs && !p.currentMs && !p.maxMs;
}
static_assert(check(), "Pause elapsed accounting regression");
"""
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'pause.cpp'
            cpp.write_text(source + checks)
            subprocess.run([compiler, '-std=c++17', '-fsyntax-only', str(cpp)], check=True)


if __name__ == '__main__':
    unittest.main()
