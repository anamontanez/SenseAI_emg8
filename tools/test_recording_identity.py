"""Exercise production identity/framing and SD preflight against injected failures."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RecordingIdentityTests(unittest.TestCase):
    def compile_checks(self, text):
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'identity.cpp'
            cpp.write_text(text, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-fsyntax-only', '-I',
                            str(ROOT / 'src'), str(cpp)], check=True)

    def test_selection_bounds_rejection_and_consumption(self):
        self.compile_checks(r'''
#include "recording_identity.hpp"
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
constexpr int run() {
 RecordingIdentitySelection s;
 CHECK(s.canStart() && !s.required && !s.armed);
 CHECK(s.set("S023,sS023_n1_20260929-100000",29));
 CHECK(s.canStart() && s.required && s.armed);
 s.consume(); CHECK(!s.canStart() && !s.value.subject[0]);
 s.clear(); CHECK(s.canStart() && !s.required);
 const char* bad[]={"", ",x", "x,", "a,b,c", "../x,b", "a,b/c", "a, b", "a,\"b", "a,b\n", "a,b\\x"};
 for(const char* text:bad) {
  int n=0; while(text[n]) ++n;
  CHECK(s.set("S00,run1",8));
  CHECK(!s.set(text,n) && !s.armed && !s.canStart() && !s.value.subject[0]);
 }
 char limit[99]{};
 for(int i=0;i<32;++i) limit[i]='S'; limit[32]=',';
 for(int i=33;i<97;++i) limit[i]='x';
 CHECK(s.set(limit,97) && s.value.subject[32]==0 && s.value.session[64]==0);
 limit[97]='x'; CHECK(!s.set(limit,98));
 char tooLongSubject[36]{};
 for(int i=0;i<33;++i) tooLongSubject[i]='s';
 tooLongSubject[33]=','; tooLongSubject[34]='x';
 CHECK(!s.set(tooLongSubject,35));
 const char binary[]={'s',',','x',0,'y'};
 CHECK(!s.set(binary,5));
 return 0;
}
constexpr int result=run();
static_assert(result==0,"Identity selection failure; result gives CHECK line");
''')

    def test_uart_rejects_truncation_and_embedded_nul_without_commands(self):
        src = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        body = src.split('static int feedUartByte(uint8_t b) {', 1)[1].split('/* ── SD directory', 1)[0]
        self.compile_checks(r'''
#include "recording_identity.hpp"
using uint8_t=unsigned char; using uint32_t=unsigned;
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
struct Board {
 char uartLineBuf[128]{}; int uartLinePos=0; bool uartLineInvalid=false;
 uint32_t uartLastCommandMs=0;
 bool recording=false, starting=false;
 RecordingIdentitySelection nextIdentity;
 int lines=0, errors=0;
 constexpr unsigned esp_timer_get_time() { return 1000; }
 constexpr void printf(const char*) { ++errors; }
 constexpr void processUartLine(const char*,int) { ++lines; }
 constexpr int feedUartByte(uint8_t b) {
''' + body + r'''
};
constexpr int run() {
 Board b; b.nextIdentity.set("S01,test",8);
 CHECK(b.feedUartByte('J')==0);
 for(int i=0;i<200;++i) CHECK(b.feedUartByte('1')==0);
 CHECK(b.feedUartByte('\n')==0 && b.errors==1 && b.lines==0);
 CHECK(!b.nextIdentity.canStart());
 CHECK(b.feedUartByte('0')=='0'); // never run trailing digits of an overflow
 b.feedUartByte('L'); b.feedUartByte('1'); b.feedUartByte(',');
 b.feedUartByte('2'); b.feedUartByte(0); b.feedUartByte('0'); b.feedUartByte('\r');
 CHECK(b.errors==2 && b.lines==0);
 b.feedUartByte('R'); for(int i=0;i<126;++i) b.feedUartByte('x');
 b.feedUartByte('\n'); CHECK(b.lines==1 && b.errors==2);
 b=Board{}; b.recording=true; b.nextIdentity.set("S01,test",8);
 b.feedUartByte('J'); b.feedUartByte(0); b.feedUartByte('\n');
 CHECK(b.nextIdentity.armed); // malformed input cannot change active settings
 return 0;
}
constexpr int result=run();
static_assert(result==0,"UART framing failure; result gives CHECK line");
''')

    def test_identity_commands_cannot_mutate_during_countdown_or_recording(self):
        src = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        branch = src.split("if (line[0] == 'J') {", 1)[1].split("} else if (line[0] == 'C')", 1)[0]
        self.compile_checks(r'''
#include "recording_identity.hpp"
constexpr int strcmp(const char* a,const char* b) {
 while(*a && *a==*b) {++a; ++b;} return *a-*b;
}
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
struct Board {
 RecordingIdentitySelection nextIdentity;
 bool recording=false, starting=false;
 int errors=0, replies=0;
 constexpr void printf(const char*) {++errors;}
 constexpr void printIdentityConfig() {++replies;}
 constexpr void command(const char* line,int len) {
''' + branch + r'''
 }
};
constexpr int run() {
 Board b;
 b.command("JS01,one",8); CHECK(b.nextIdentity.armed && b.replies==1);
 b.starting=true;
 b.command("J-",2); b.command("JS02,two",8);
 CHECK(b.errors==2 && b.nextIdentity.value.subject[2]=='1');
 b.command("J?",2); CHECK(b.replies==2 && b.nextIdentity.armed);
 b.starting=false; b.recording=true;
 b.command("J-",2); b.command("JS02,two",8);
 CHECK(b.errors==4 && b.nextIdentity.value.subject[2]=='1');
 b.recording=false; b.command("JS02,two",8);
 CHECK(b.nextIdentity.value.subject[2]=='2');
 b.command("JS02,",5); CHECK(!b.nextIdentity.canStart() && b.errors==5);
 b.command("J-",2); CHECK(b.nextIdentity.canStart() && !b.nextIdentity.required);
 return 0;
}
constexpr int result=run();
static_assert(result==0,"Busy identity command failure; result gives CHECK line");
''')

    def test_identity_file_preflight_errors_never_succeed(self):
        src = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        body = src[src.index('static bool sdWriteIdentity('):src.index('static bool sdOpenFiles(')]
        body = body.replace('static bool', 'constexpr bool')
        self.compile_checks(r'''
#include "recording_identity.hpp"
using UINT=unsigned;
namespace std { struct string { constexpr const char* c_str() const { return "safe/path"; } }; }
enum FRESULT { FR_OK, FR_DISK_ERR };
enum class SdFault { None, Open, Metadata, Write, Sync, Close };
constexpr int FA_CREATE_NEW=4, FA_WRITE=2;
struct FIL {};
struct Flag { constexpr bool load() const { return true; } };
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
struct Board {
 RecordingIdentity recordingIdentity{};
 std::string sdFileSet; const char* firmwareElfSha="0123456789";
 unsigned mode=1; int countdownSeconds=10;
 Flag recordingRate1000;
 FIL filMaster;
 bool sdOK=true;
 SdFault sdFault=SdFault::None;
 int opens=0, writes=0, syncs=0, closes=0, renderedBytes=400, fail=0;
 constexpr int snprintf(char*,unsigned long long,const char*,const char*,const char*,const char*,const char*,unsigned,const char*,int) { return renderedBytes; }
 constexpr void printf(const char*,...) {}
 constexpr FRESULT f_open(FIL*,const char*,int flags) {
  ++opens; return fail==1 || flags!=(FA_CREATE_NEW|FA_WRITE) ? FR_DISK_ERR:FR_OK;
 }
 constexpr bool sdWriteChecked(FIL*,const char*,const char*,UINT) {
  ++writes; if(fail==2) {sdOK=false; sdFault=SdFault::Write;} return sdOK;
 }
 constexpr bool sdSyncChecked(FIL*,const char*) {
  ++syncs; if(fail==3) {sdOK=false; sdFault=SdFault::Sync;} return sdOK;
 }
 constexpr void sdCloseChecked(FIL*,const char*) {
  ++closes; if(fail==4) {sdOK=false; sdFault=SdFault::Close;}
 }
''' + body + r'''
};
constexpr int run() {
 Board b; std::string path;
 CHECK(b.sdWriteIdentity(path) && b.opens==1 && b.writes==1 && b.syncs==1 && b.closes==1);
 for(int fail=1;fail<=4;++fail) {
  b=Board{}; b.fail=fail;
  CHECK(!b.sdWriteIdentity(path) && !b.sdOK && b.opens==1);
  CHECK(b.closes==(fail==1 ? 0:1));
  if(fail==2) CHECK(b.syncs==0);
 }
 b=Board{}; b.renderedBytes=640;
 CHECK(!b.sdWriteIdentity(path) && b.opens==0 && b.sdFault==SdFault::Metadata);
 return 0;
}
constexpr int result=run();
static_assert(result==0,"Identity storage failure; result gives CHECK line");
''')
