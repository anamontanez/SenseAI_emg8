"""Fault injection for the actual recording-open recovery and save-result policy."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class StorageRecovery(unittest.TestCase):
    def test_recovery_is_bounded_and_incomplete_is_not_ok(self):
        text = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text(encoding='utf-8')
        prep = text.split('static bool sdPrepareRecording() {', 1)[1].split('static void sdWriteTask', 1)[0]
        summary = text.split('static void printSdSummary() {', 1)[1].split('static void waitImuIdle', 1)[0]
        body = 'constexpr bool sdPrepareRecording() {' + prep + 'constexpr void printSdSummary() {' + summary
        harness = r'''
enum class SdFault { None, Init, Open, Write, Sync, Close };
template<class T> struct Flag {
 T value{}; constexpr operator T() const { return value; }
 constexpr T load() const { return value; }
 constexpr Flag& operator=(T v) { value=v; return *this; }
};
struct Text { constexpr const char* c_str() const { return "new/000.bin"; } };
#define CHECK(c) do { if(!(c)) return __LINE__; } while(0)
struct Board {
 Flag<bool> sdOK{true}; Flag<SdFault> sdFault{SdFault::None};
 Flag<unsigned> rawDrops{},envDrops{},imuDrops{},metadataDrops{};
 Text sessionDir,sdFileSet;
 unsigned sdRawRecords=0,sdEnvRecords=0,sdImuRecords=0;
 unsigned long long sdBytes=32;
 bool sdRecordingStarted=true,sdSummaryPending=true;
 bool recoverOK=true;
 int recoveries=0,opens=0,failOpens=0,summaries=0;
 bool resultOK=false;
 constexpr bool sdRecover() { ++recoveries; sdOK=recoverOK; return recoverOK; }
 constexpr bool sdOpenFiles(Text) {
  ++opens; if(opens<=failOpens) { sdOK=false; sdFault=SdFault::Open; return false; }
  sdOK=true; return true;
 }
 constexpr const char* sdFaultName(SdFault) { return "FAULT"; }
 constexpr unsigned long long recordingElapsedUs() { return 1000; }
 constexpr void printf(const char*,const char*,unsigned long long) {}
 constexpr void printf(const char*,const char*,unsigned long,unsigned long,unsigned long,
                       unsigned long long,const char* result) {
  ++summaries; resultOK=(result[0]=='O');
 }
'''
        checks = r'''
};
constexpr int run() {
 Board b; CHECK(b.sdPrepareRecording() && b.recoveries==0 && b.opens==1);
 b=Board{}; b.sdOK=false;
 CHECK(b.sdPrepareRecording() && b.recoveries==1 && b.opens==1);
 b=Board{}; b.sdOK=false; b.recoverOK=false;
 CHECK(!b.sdPrepareRecording() && b.recoveries==1 && b.opens==0);
 b=Board{}; b.failOpens=1;
 CHECK(b.sdPrepareRecording() && b.recoveries==1 && b.opens==2);
 b=Board{}; b.failOpens=100;
 CHECK(!b.sdPrepareRecording() && b.recoveries==1 && b.opens==2);
 b=Board{}; b.sdOK=false; b.failOpens=100;
 CHECK(!b.sdPrepareRecording() && b.recoveries==1 && b.opens==1);
 b=Board{}; b.printSdSummary(); CHECK(b.resultOK && b.summaries==1);
 b.printSdSummary(); CHECK(b.summaries==1);
 b=Board{}; b.sdFault=SdFault::Close; b.printSdSummary(); CHECK(!b.resultOK);
 b=Board{}; b.sdOK=false; b.printSdSummary(); CHECK(!b.resultOK);
 b=Board{}; b.rawDrops=1; b.printSdSummary(); CHECK(!b.resultOK);
 b=Board{}; b.metadataDrops=1; b.printSdSummary(); CHECK(!b.resultOK);
 b=Board{}; b.sdRecordingStarted=false; b.printSdSummary(); CHECK(!b.resultOK);
 return 0;
}
constexpr int result=run();
static_assert(result==0,"SD recovery/save-result regression: result identifies CHECK line");
'''
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'recovery.cpp'
            cpp.write_text(harness + body + checks, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-fsyntax-only', str(cpp)], check=True)
