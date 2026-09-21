"""Compile the actual companion task against a deterministic UART/RTOS harness.

No executable is produced. Compile-time assertions exercise frame order,
session boundaries, clock wrap, quiet independence and transport failures.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class CompanionTask(unittest.TestCase):
    def test_label_parser_rejects_overflow_and_fragments(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'src/main.cpp').read_text(encoding='utf-8')
        parser = source.split('static bool parseLabel', 1)[1].split('/** Process', 1)[0]
        parser = 'constexpr bool parseLabel' + parser
        checks = '''
using uint16_t = unsigned short;
using uint32_t = unsigned;
'''
        checks += parser + '''
constexpr bool verify() {
    uint16_t id=0, rep=0;
    if (!parseLabel("65535,0",id,rep) || id!=65535 || rep!=0) return false;
    if (!parseLabel("7,3",id,rep) || id!=7 || rep!=3) return false;
    for (const char* bad : {"", "7", "7,", ",3", "-1,3", "65536,3",
                            "7,65536", "7,3x", "999999999999999999999,3"})
        if (parseLabel(bad,id,rep)) return false;
    return true;
}
static_assert(verify(), "Label bounds/framing regression");
'''
        # Avoid host standard-library dependencies in this compile-only test.
        checks = checks.replace('for (const char* bad : {',
            'const char* invalid[] = {').replace('999999999999999999999,3"})',
            '999999999999999999999,3"}; for (const char* bad : invalid)')
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp=Path(folder)/'label.cpp'
            cpp.write_text(checks)
            subprocess.run([compiler,'-std=c++17','-fsyntax-only',str(cpp)],check=True)

    def test_owner_orders_frames_and_resets_epoch(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'src/companion_link.cpp').read_text()
        core = source.split('namespace {', 1)[1].split('} // namespace', 1)[0]
        core = core.replace('constexpr uart_port_t', 'static constexpr uart_port_t')
        core = core.replace('constexpr int64_t', 'static constexpr int64_t')
        core = core.replace('std::atomic<uint32_t>', 'Counter')
        for name in ('writeFrame', 'enqueue'):
            core = core.replace('bool ' + name, 'constexpr bool ' + name)
        for name in ('writeByte', 'sendSync', 'linkTask'):
            core = core.replace('void ' + name, 'constexpr void ' + name)
        core = core.replace('while (true)', 'while (step < 12)')
        prelude = r'''
using uint8_t = unsigned char;
using uint32_t = unsigned;
using int64_t = long long;
using size_t = unsigned long long;
using TickType_t = unsigned;
using uart_port_t = int;
using QueueHandle_t = int;
constexpr int UART_NUM_1 = 1, ESP_OK = 0, pdTRUE = 1;
constexpr unsigned portMAX_DELAY = ~0u;
constexpr unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
enum class SessionPhase : uint8_t { Grasp=4, Rest=5, Demo=6 };
struct Counter {
    unsigned value = 0;
    constexpr void operator++() { ++value; }
};
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)
struct Harness {
'''
        core = core.replace('QueueHandle_t events = nullptr', 'QueueHandle_t events = 1')
        harness = r'''
    int step = 0, used = 0, sends = 0, pos = 0;
    int64_t now = 10;
    uint8_t wire[100]{};
    bool failWait = false, failWrite = false, failQueue = false;
    constexpr int64_t esp_timer_get_time() { return now; }
    constexpr int uart_write_bytes(int, const uint8_t* data, size_t size) {
        ++sends;
        if (failWrite) return 0;
        for (size_t i=0; i<size; ++i) wire[used++] = data[i];
        return int(size);
    }
    constexpr int uart_wait_tx_done(int, unsigned) { return failWait ? 1 : ESP_OK; }
    constexpr int uxQueueMessagesWaiting(int) { return 0; }
    constexpr int xQueueSend(int, Event*, unsigned) { return !failQueue; }
    constexpr int xQueueReceive(int, Event* e, unsigned wait) {
        switch (step++) {
        case 0:
            if (wait != portMAX_DELAY) return 0;
            now=10; *e={Kind::Start,SessionPhase::Grasp,false,10}; return 1;
        case 1: now=1000010; return 0;
        case 2: now=1000020; *e={Kind::Phase,SessionPhase::Rest,false,0}; return 1;
        case 3: now=1000030; *e={Kind::Stop,SessionPhase::Demo,false,0}; return 1;
        case 4: now=40000000; return 0; // paused: no stale sync at old deadline
        case 5: now=50000000; *e={Kind::Start,SessionPhase::Demo,false,now}; return 1;
        case 6: now=51000000; return 0;
        case 7: now=81000000; return 0;
        case 8: now=81000010; *e={Kind::Stop,SessionPhase::Demo,true,0}; return 1;
        case 9: now=100000000; *e={Kind::Start,SessionPhase::Grasp,false,now}; return 1;
        case 10: now=100000000LL+0x100000000LL+1234; return 0;
        default: *e={Kind::Stop,SessionPhase::Demo,true,0}; return 1;
        }
    }
    constexpr bool byte(unsigned expected) { return wire[pos++] == expected; }
    constexpr bool sync(unsigned expected) {
        if (!byte(2)) return false;
        unsigned value=0;
        for (unsigned i=0; i<4; ++i) value |= unsigned(wire[pos++]) << (8*i);
        return value == expected;
    }
    constexpr int run() {
        linkTask(nullptr);
        CHECK(byte(3) && byte(4) && sync(0)); // immediate even before uptime 30 s
        CHECK(sync(1000000) && byte(5));
        CHECK(byte(3) && byte(6) && sync(0)); // fresh epoch after paused old session
        CHECK(sync(1000000) && sync(31000000) && byte(1));
        CHECK(byte(3) && byte(4) && sync(0));
        CHECK(sync(1234) && byte(1)); // uint32 wire clock wraps, no sign extension
        CHECK(pos == used && used == 44);
        CHECK(starts.value==3 && stops.value==2 && phases.value==4 && syncs.value==7);
        CHECK(queueErrors.value==0 && txErrors.value==0);
        failWait=true; sendSync(now);
        CHECK(txErrors.value==1 && used==44);
        failWait=false; failWrite=true; sendSync(now);
        CHECK(txErrors.value==2 && syncs.value==7 && used==44);
        failQueue=true;
        CHECK(!enqueue({Kind::Stop,SessionPhase::Demo,true,0}) && queueErrors.value==1);
        return 0;
    }
};
constexpr int result = [] { Harness h; return h.run(); }();
static_assert(result==0, "Companion test failed; result identifies CHECK line");
'''
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler, 'clang++ is required')
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'companion.cpp'
            cpp.write_text(prelude + core + harness)
            subprocess.run([compiler, '-std=c++17', '-fsyntax-only', str(cpp)], check=True)


if __name__ == '__main__':
    unittest.main()
