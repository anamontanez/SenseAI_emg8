"""Compile the actual packet pump with deterministic queue/send outcomes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class NetworkPump(unittest.TestCase):
    def test_bounded_pass_retry_and_partial_flush(self):
        source = (Path(__file__).resolve().parents[1] / 'src/net_stream.cpp').read_text(encoding='utf-8')
        body = source.split('void pumpQueue(', 1)[1].split('void pollSubscribe()', 1)[0]
        body = 'constexpr void pumpQueue(' + body
        harness = r"""
using uint32_t=unsigned;
using uint8_t=unsigned char;
using size_t=unsigned long long;
using QueueHandle_t=int;
constexpr int pdTRUE=1, kHdrSize=12;
constexpr unsigned kFlushMs=30;
struct Batch { char buf[1404]{}; unsigned count=0; uint32_t firstMs=0; };
struct Harness {
    int available=10, sends=0, delivered=0;
    bool fail=false;
    constexpr int xQueueReceive(int,char*,int) {
        if (!available) return 0;
        --available; return pdTRUE;
    }
    constexpr bool sendBatch(Batch& b,uint8_t,size_t) {
        ++sends;
        if (fail) return false;
        delivered += b.count; b.count=0; return true;
    }
"""
        checks = r"""
    constexpr bool run() {
        Batch b;
        pumpQueue(1,b,0,8,3,0);
        if (sends!=1 || delivered!=3 || available!=7 || b.count) return false;
        fail=true;
        pumpQueue(1,b,0,8,3,5);
        if (sends!=2 || available!=4 || b.count!=3) return false;
        pumpQueue(1,b,0,8,3,10);
        if (sends!=3 || available!=4 || b.count!=3) return false;
        fail=false;
        pumpQueue(1,b,0,8,3,15);
        if (sends!=4 || delivered!=6 || available!=4 || b.count) return false;
        pumpQueue(1,b,0,8,3,20);
        if (sends!=5 || delivered!=9 || available!=1 || b.count) return false;
        pumpQueue(1,b,0,8,3,100);
        if (sends!=5 || available || b.count!=1) return false;
        pumpQueue(1,b,0,8,3,129);
        if (sends!=5 || b.count!=1) return false;
        pumpQueue(1,b,0,8,3,130);
        return sends==6 && delivered==10 && b.count==0;
    }
};
static_assert([] { Harness h; return h.run(); }(), "Packet pump regression");
"""
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp=Path(folder)/'pump.cpp'
            cpp.write_text(harness+body+checks)
            subprocess.run([compiler,'-std=c++17','-fsyntax-only',str(cpp)],check=True)


if __name__=='__main__':
    unittest.main()
