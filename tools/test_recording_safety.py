"""Compile the real start/supervision paths against injected SD/control failures."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class RecordingSafety(unittest.TestCase):
    def test_start_and_runtime_faults(self):
        src = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn("if (mode != Mode::Idle) startRecording();", src)
        reed_start = src.index("// ---- Reed switch toggle")
        reed_end = src.index("// ---- Physical backup/OR button", reed_start)
        reed = src[reed_start:reed_end]
        self.assertIn("if (mode == Mode::Idle) mode = Mode::All;", reed)
        start = src[src.index("static bool startRecording() {"):src.index("// ─", src.index("static bool startRecording() {"))]
        safety = src[src.index("static void serviceRecordingSafety() {"):src.index("// ─", src.index("static void serviceRecordingSafety() {"))]
        # Local-static cache is per simulated board instead of per executable.
        safety = safety.replace("    static SdFault announced = SdFault::None;", "")
        body = (start + safety).replace("static bool ", "constexpr bool ").replace("static void ", "constexpr void ")
        harness = r'''
using uint8_t=unsigned char; using uint16_t=unsigned short;
using uint32_t=unsigned; using uint64_t=unsigned long long;
namespace std { constexpr int memory_order_relaxed=0; }
template<class T> struct Flag {
 T value{};
 constexpr operator T() const { return value; }
 constexpr T load(int=0) const { return value; }
 constexpr void store(T v,int=0) { value=v; }
 constexpr Flag& operator=(T v) { value=v; return *this; }
};
enum class Mode { Idle, All, Raw, Env, Sensor };
enum class SdCommand { Open, Close };
enum class SdFault { None, Init, Open, Write, Sync, Close, Overflow, Metadata };
enum class SessionPhase { Demo };
struct Text { constexpr const char* c_str() const { return "new/000.bin"; } };
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
struct Board {
 Mode mode=Mode::All;
 bool adcOK=true, limitFastRate1000=true, sdRecordingStarted=false;
 Flag<bool> recording{false}, recordingRate1000{}, sdOK{true};
 Flag<uint64_t> recStart{};
 Flag<unsigned> rawDrops{},envDrops{},imuDrops{},metadataDrops{};
 Flag<SdFault> sdFault{SdFault::None};
 SdFault announced=SdFault::None;
 SessionPhase currentPhase=SessionPhase::Demo;
 unsigned curGrasp=1, curRep=2, reportedWrap=0;
 int countdownSeconds=10;
 Text sdFileSet;
 bool openOK=true, countdownOK=true, linkOK=true, adcStartOK=true, boundaryOK=true;
 int opens=0, closes=0, countdowns=0, starts=0, recAcks=0, stopAcks=0;
 int companionStops=0;
 int stops=0, summaries=0, metadata=0, wrapEvents=0;
 uint64_t elapsed=0, streamStart=0;
 constexpr void printf(const char* fmt, ...) {
  if (fmt[0]=='#' && fmt[1]=='R' && fmt[2]=='E' && fmt[3]=='C') ++recAcks;
  if (fmt[0]=='#' && fmt[1]=='S' && fmt[2]=='T' && fmt[3]=='O' && fmt[4]=='P' && fmt[5]=='\n') ++stopAcks;
 }
 constexpr void printSensorLine() {}
 constexpr void updateStatusLed() {}
 constexpr void resetDropCounters() {}
 constexpr bool configureAdcsForMode() { return mode != Mode::Idle; }
 constexpr const char* sdFaultName(SdFault) { return "FAIL"; }
 constexpr bool commandSdWriter(SdCommand c) {
  if(c==SdCommand::Close) { ++closes; return true; }
  ++opens; sdOK=openOK; return openOK;
 }
 constexpr void printSdSummary() { ++summaries; }
 constexpr void waitImuIdle() {}
 constexpr bool countdown(int) { ++countdowns; return countdownOK; }
 constexpr bool netBeginRecording() { return boundaryOK; }
 constexpr uint64_t esp_timer_get_time() { return 100; }
 constexpr uint64_t recordingElapsedUs() { return elapsed; }
 constexpr bool sendStartToSlave() { return linkOK; }
 constexpr void stopCompanion(bool) { ++companionStops; }
 constexpr bool startADCs() { ++starts; return adcStartOK; }
 constexpr bool saveMetadata(unsigned kind,SessionPhase,unsigned,unsigned,uint32_t) {
  ++metadata; if(kind==4) ++wrapEvents; return true;
 }
 constexpr uint64_t netTakeStreamStart() { auto s=streamStart; streamStart=0; return s; }
 constexpr void stopTest() { recording=false; ++stops; }
'''
        checks = r'''
};
constexpr int run() {
 Board b; b.mode=Mode::Idle;
 CHECK(!b.startRecording() && b.opens==0 && b.starts==0 && b.recAcks==0);
 b=Board{}; b.openOK=false;
 CHECK(!b.startRecording() && !b.recording && b.opens==1 && b.countdowns==0);
 CHECK(b.recAcks==0 && b.starts==0 && b.mode==Mode::All);
 b=Board{}; b.adcOK=false;
 CHECK(!b.startRecording() && b.opens==0 && b.starts==0 && b.recAcks==0);
 b=Board{}; b.countdownOK=false;
 CHECK(!b.startRecording() && b.opens==1 && b.closes==1 && b.recAcks==0);
 b=Board{}; b.linkOK=false;
 CHECK(!b.startRecording() && !b.recording && b.closes==1 && b.starts==0);
 b=Board{}; b.boundaryOK=false;
 CHECK(!b.startRecording() && !b.recording && b.closes==1 && b.starts==0);
 b=Board{}; b.adcStartOK=false;
 CHECK(!b.startRecording() && !b.recording && b.closes==1 &&
       b.companionStops==1 && b.stopAcks==1 && b.recAcks==0);
 b=Board{};
 CHECK(b.startRecording() && b.recording && b.recAcks==1 && b.starts==1);
 CHECK(b.metadata==2 && b.wrapEvents==1);
 b.sdOK=false; b.sdFault=SdFault::Write;
 b.serviceRecordingSafety();
 CHECK(!b.recording && b.stops==1);
 b.serviceRecordingSafety(); CHECK(b.stops==1);
 b=Board{}; b.recording=true; b.rawDrops=1;
 b.serviceRecordingSafety(); CHECK(b.stops==1 && b.sdFault.load()==SdFault::Overflow);
 b=Board{}; b.recording=true; b.metadataDrops=1;
 b.serviceRecordingSafety(); CHECK(b.stops==1 && b.sdFault.load()==SdFault::Metadata);
 b=Board{}; b.recording=true; b.elapsed=(1ULL<<32)+123;
 b.serviceRecordingSafety(); CHECK(b.reportedWrap==1 && b.wrapEvents==1 && b.stops==0);
 b.serviceRecordingSafety(); CHECK(b.wrapEvents==1);
 b.elapsed=(2ULL<<32)+456;
 b.serviceRecordingSafety(); CHECK(b.reportedWrap==2 && b.wrapEvents==2);
 return 0;
}
constexpr int result=run();
static_assert(result==0,"Recording safety regression: result identifies CHECK line");
'''
        compiler = shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / "safety.cpp"
            cpp.write_text(harness + body + checks, encoding="utf-8")
            subprocess.run([compiler, "-std=c++17", "-fsyntax-only", str(cpp)], check=True)
