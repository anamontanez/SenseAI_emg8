"""Compile the actual UDP task against a deterministic storage-pressure probe."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class NetworkPriority(unittest.TestCase):
    def test_storage_backlog_pauses_tx_but_preserves_control_polling(self):
        root = Path(__file__).resolve().parents[1]
        source = (root/'src/net_stream.cpp').read_text(encoding='utf-8')
        body = source.split('void netTask(void*) {',1)[1].split('esp_err_t wifiInitOnce',1)[0]
        body = 'constexpr void netTask(void*) {' + body
        body = body.replace('while (true)', 'while (iterations < 2)')
        harness = r'''
using uint32_t = unsigned;
namespace std { constexpr int memory_order_acquire=0, memory_order_acq_rel=0; }
constexpr int pdTRUE=1;
constexpr int pdMS_TO_TICKS(int ms) { return ms; }
struct Sample { char bytes[8]; };
struct ImuSample { char bytes[20]; };
struct Flag {
    bool value;
    constexpr bool load(int) const { return value; }
    constexpr bool exchange(bool next,int) { bool old=value; value=next; return old; }
};
struct Probe {
    bool present=true, busy=true;
    constexpr operator bool() const { return present; }
    constexpr bool operator()() const { return busy; }
};
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)
struct Harness {
    Flag active{true}, stopRequested{false};
    Probe storagePressureProbe;
    int sock=1, stopped=0, iterations=0, polls=0, pumps=0, acknowledgements=0;
    int rawQ=0, envQ=1, imuQ=2, batches[3]{};
    bool quiet=true;
    uint32_t lastClientMs=1;
    static constexpr int kQuietWatchdogMs=20000, kTypeRaw=0, kTypeEnv=1, kTypeImu=2;
    static constexpr int kMaxRawRecs=174, kMaxImuRecs=69;
    constexpr long long esp_timer_get_time() { return 60000000; }
    constexpr void pollSubscribe() { ++polls; }
    constexpr bool hostUartQuiet() { return quiet; }
    constexpr void hostSetUartQuiet(bool next) { quiet=next; }
    constexpr void printf(const char*) {}
    constexpr void pumpQueue(int,int&,int,unsigned long long,int,uint32_t) { ++pumps; }
    constexpr void vTaskDelay(int) { ++iterations; storagePressureProbe.busy=false; }
    constexpr void ulTaskNotifyTake(int,int) { ++iterations; }
    constexpr void xSemaphoreGive(int) { ++acknowledgements; }
'''
        checks = r'''
    constexpr int run() {
        netTask(nullptr);
        CHECK(polls==2 && pumps==3 && !quiet); // only second iteration transmits
        iterations=polls=pumps=0; storagePressureProbe.present=false;
        netTask(nullptr);
        CHECK(polls==2 && pumps==6); // no SD probe: normal network operation
        iterations=polls=pumps=0; active.value=false; stopRequested.value=true;
        netTask(nullptr);
        CHECK(polls==0 && pumps==0 && acknowledgements==1);
        return 0;
    }
};
constexpr int result=[] { Harness h; return h.run(); }();
static_assert(result==0, "Network priority regression; result identifies CHECK line");
'''
        compiler=shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp=Path(folder)/'network.cpp'
            cpp.write_text(harness+body+checks)
            subprocess.run([compiler,'-std=c++17','-fsyntax-only',str(cpp)],check=True)


if __name__=='__main__':
    unittest.main()
