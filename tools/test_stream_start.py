"""First-stream notification is emitted for an actual successful data send only."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class StreamStart(unittest.TestCase):
    def test_reset_backlog_and_only_report_success(self):
        text = (Path(__file__).resolve().parents[1] / 'src/net_stream.cpp').read_text(encoding='utf-8')
        boundary = text.split('void applyRecordingBoundary() {', 1)[1].split('void writeHeader', 1)[0]
        send = text.split('bool sendBatch(', 1)[1].split('/** Send a bounded', 1)[0]
        body = ('constexpr void applyRecordingBoundary() {' + boundary.replace('void noteFirstPacket', 'constexpr void noteFirstPacket')
                + 'constexpr bool sendBatch(' + send)
        harness = r'''
using uint8_t=unsigned char; using uint32_t=unsigned; using uint64_t=unsigned long long;
using int64_t=long long; using size_t=unsigned long long;
namespace std { constexpr int memory_order_relaxed=0,memory_order_release=0,memory_order_acquire=0; }
template<class T> struct Flag {
 T value{}; constexpr T load(int=0) const { return value; }
 constexpr void store(T v,int=0) { value=v; }
 constexpr Flag& operator=(T v) { value=v; return *this; }
 constexpr void operator++(int) { ++value; }
 constexpr void fetch_add(T v,int=0) { value+=v; }
};
struct sockaddr {};
struct Batch { char buf[1404]{}; unsigned count=0,seq=0,firstMs=0; };
#define CHECK(c) do { if(!(c)) return __LINE__; } while(0)
struct Board {
 Flag<bool> beginRequested{true}; Flag<uint64_t> streamStartedAt{77};
 bool streamArmed=false,clientKnown=true,failSend=false;
 Flag<uint32_t> txErrors{},txPackets{},sendMaxUs{};
 int rawQ=1,envQ=2,imuQ=3,resetMask=0,sock=1,errno=12;
 sockaddr clientAddr;
 Batch batches[3];
 static constexpr uint8_t kTypeRaw=0,kTypeEnv=1,kTypeImu=2;
 static constexpr int kHdrSize=12;
 constexpr void xQueueReset(int q) { resetMask|=1<<q; }
 constexpr int64_t esp_timer_get_time() { return 100; }
 constexpr uint64_t recordingElapsedUs() { return 123456; }
 constexpr void writeHeader(Batch&,uint8_t) {}
 constexpr void observeMaximum(Flag<uint32_t>&,uint32_t) {}
 constexpr int sendto(int,const char*,size_t n,int,sockaddr*,size_t) { return failSend?-1:int(n); }
'''
        checks = r'''
};
constexpr int run() {
 Board b; b.batches[0].count=10; b.batches[0].seq=44;
 b.applyRecordingBoundary();
 CHECK(b.resetMask==14 && b.batches[0].count==0 && b.batches[0].seq==44);
 CHECK(b.streamArmed && b.streamStartedAt.load()==0 && !b.beginRequested.load());
 auto& packet=b.batches[0]; packet.count=2;
 b.clientKnown=false; CHECK(b.sendBatch(packet,0,8) && b.streamArmed);
 b.clientKnown=true; b.failSend=true; packet.count=2;
 CHECK(!b.sendBatch(packet,0,8) && b.streamArmed && packet.count==2);
 b.failSend=false; CHECK(b.sendBatch(packet,2,20) && b.streamArmed);
 packet.count=2; CHECK(b.sendBatch(packet,0,8) && !b.streamArmed);
 CHECK(b.streamStartedAt.load()==123456);
 b.streamStartedAt=0; packet.count=2; b.sendBatch(packet,0,8);
 CHECK(b.streamStartedAt.load()==0); // exactly once per recording
 return 0;
}
constexpr int result=run();
static_assert(result==0,"Stream-start regression: result identifies CHECK line");
'''
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'stream.cpp'
            cpp.write_text(harness + body + checks, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-fsyntax-only', str(cpp)], check=True)
