"""Compile the firmware's actual checked-I/O helpers against scripted FatFs calls.

Requires clang++. Evaluates assertions at compile time; creates no executable.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class CheckedStorageIO(unittest.TestCase):
    def test_short_write_and_io_failures_are_reported(self):
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler, 'clang++ is required for this firmware helper test')
        source = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text(encoding='utf-8')
        start = source.index('static void sdReportError(')
        end = source.index('static void sdCloseFiles();', start)
        helpers = source[start:end]
        helpers = helpers.replace('static void ', 'constexpr void ').replace('static bool ', 'constexpr bool ')
        harness = r'''
namespace std { constexpr int memory_order_relaxed = 0; }
struct TestFlag {
    bool value = true;
    constexpr void store(bool next, int) { value = next; }
    constexpr operator bool() const { return value; }
    constexpr void operator=(bool next) { value = next; }
};
using UINT = unsigned;
enum FRESULT { FR_OK, FR_DISK_ERR };
enum class SdFault { None, Init, Open, Write, Sync, Close };
struct FIL {};
constexpr int strcmp(const char* a, const char* b) {
    for (; *a && *b && *a == *b; ++a, ++b) {}
    return *a - *b;
}
#define assert(condition) do { if (!(condition)) return __LINE__; } while (0)
struct TestContext {
    TestFlag sdOK;
    struct FaultFlag {
        SdFault value = SdFault::None;
        constexpr void store(SdFault next, int) { value = next; }
    } sdFault;
    int writes = 0, syncs = 0, closes = 0;
    FRESULT result = FR_OK;
    UINT returnedBytes = 8;
    constexpr int printf(const char*, ...) { return 0; }
    constexpr FRESULT f_write(FIL*, const void*, UINT, UINT* written) {
        ++writes; *written = returnedBytes; return result;
    }
    constexpr FRESULT f_sync(FIL*) { ++syncs; return result; }
    constexpr FRESULT f_close(FIL*) { ++closes; return result; }
'''
        checks = r'''
constexpr int run() {
    FIL file{}; char data[8] = {};
    assert(sdWriteChecked(&file, "R", data, sizeof(data)));
    assert(sdOK && writes == 1 && sdFault.value == SdFault::None);
    // Full card: FatFs reports success but accepts fewer bytes.
    returnedBytes = 7;
    assert(!sdWriteChecked(&file, "R", data, sizeof(data)));
    assert(!sdOK && writes == 2 && sdFault.value == SdFault::Write);
    sdOK = true; returnedBytes = 0;
    assert(!sdWriteChecked(&file, "R", data, sizeof(data)));
    assert(!sdOK && writes == 3);  // no retry of an ambiguous write
    sdOK = true; result = FR_DISK_ERR; returnedBytes = 8;
    assert(!sdWriteChecked(&file, "R", data, sizeof(data)));
    assert(!sdOK && writes == 4);  // bytes alone do not establish success
    sdOK = true;
    assert(!sdSyncChecked(&file, "R"));
    assert(!sdOK && syncs == 1 && sdFault.value == SdFault::Sync);
    sdOK = true;
    sdCloseChecked(&file, "R");
    assert(!sdOK && closes == 1 && sdFault.value == SdFault::Close);
    // Even after failure the other handles must still be closable.
    result = FR_OK;
    sdCloseChecked(&file, "E");
    assert(!sdOK && closes == 2);
    sdOK = true;
    assert(sdSyncChecked(&file, "M"));
    sdCloseChecked(&file, "M");
    assert(sdOK && syncs == 2 && closes == 3);
    return 0;
}
};
constexpr int checked = [] { TestContext context; return context.run(); }();
static_assert(checked == 0, "Checked I/O failure case: see assertion line number");
'''
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cpp = root / 'checked_io.cpp'
            cpp.write_text(harness + helpers + checks, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-fsyntax-only', str(cpp)],
                           check=True, text=True)


if __name__ == '__main__':
    unittest.main()
