"""Exercise the real SD wrapper cleanup repeatedly with a one-slot fake diskio."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class MountCleanup(unittest.TestCase):
    def test_failed_mount_retry_releases_every_registration(self):
        root = Path(__file__).resolve().parents[1]
        src = (root / 'lib/data-logging-library/src/sd_storage_sense.cpp').read_text(encoding='utf-8')
        body = src[src.index('esp_err_t SD::deinit(void) {'):src.index('esp_err_t SD::readSector')]
        body = body.replace('esp_err_t SD::', 'constexpr esp_err_t ').replace('FRESULT SD::', 'constexpr FRESULT ')
        harness = r'''
using BYTE=unsigned char; using esp_err_t=int;
constexpr int ESP_OK=0;
enum FRESULT { FR_OK,FR_NOT_READY,FR_NOT_ENOUGH_CORE,FR_NO_FILESYSTEM };
enum class MountMode { kMountNow=1 };
enum class ObjOps { kMount };
struct FATFS {}; struct FIL {}; struct Card {};
struct Text { constexpr void clear() {} constexpr Text& operator=(const char*) { return *this; } };
struct Remove { int* count; constexpr int operator()(int) { ++*count; return 0; } };
struct Host { Remove deinit_p; int slot=0; };
#define CHECK(c) do { if(!(c)) return __LINE__; } while(0)
struct Board {
 bool deviceRegistered_=true,driveRegistered_=false;
 bool diskSlotFree=true,failMount=false,failDrive=false;
 int removes=0,registrations=0,unregistrations=0,closes=0,obj_=0;
 Host sdHost_{{&removes}};
 FATFS* pFatFs_=nullptr; FIL* pFile_=nullptr;
 BYTE driveNum_=0; char root_[3]{}; Text path_; Card sdCardInfo_;
 constexpr void f_close(FIL*) { ++closes; }
 constexpr int ff_diskio_get_drive(BYTE* drive) {
  if(!diskSlotFree || failDrive) return 1; *drive=0; return ESP_OK;
 }
 constexpr void ff_diskio_register_sdmmc(BYTE,Card*) { diskSlotFree=false; ++registrations; }
 constexpr void ff_sdmmc_set_disk_status_check(BYTE,bool) {}
 constexpr void ff_diskio_unregister(BYTE) { diskSlotFree=true; ++unregistrations; }
 constexpr FRESULT f_mount(FATFS* fs,const char*,BYTE) { return fs && failMount ? FR_NO_FILESYSTEM : FR_OK; }
 constexpr void updatePaths(ObjOps) {}
'''
        checks = r'''
};
constexpr int run() {
 Board b;
 for(int i=0;i<25;++i) {
  b.failMount=true;
  CHECK(b.mountCard()==FR_NO_FILESYSTEM);
  CHECK(b.pFatFs_==nullptr && !b.driveRegistered_ && b.diskSlotFree);
  b.failMount=false;
  CHECK(b.mountCard()==FR_OK && b.pFatFs_ && b.driveRegistered_);
  auto* fs=b.pFatFs_;
  CHECK(b.mountCard()==FR_OK && b.pFatFs_==fs); // idempotent, no allocation/drive leak
  b.deinit(); b.deinit();
  CHECK(b.removes==i+1 && b.diskSlotFree && !b.driveRegistered_);
  CHECK(b.mountCard()==FR_NOT_READY);
  b.deviceRegistered_=true;
 }
 CHECK(b.registrations==50 && b.unregistrations==50);
 b.failDrive=true;
 CHECK(b.mountCard()==FR_NOT_ENOUGH_CORE && b.pFatFs_==nullptr);
 b.deinit();
 return 0;
}
constexpr int result=run();
static_assert(result==0,"SD mount-cleanup regression: result identifies CHECK line");
'''
        compiler = shutil.which('clang++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'mount.cpp'
            cpp.write_text(harness + body + checks, encoding='utf-8')
            subprocess.run([compiler, '-std=c++20', '-fsyntax-only', str(cpp)], check=True)
