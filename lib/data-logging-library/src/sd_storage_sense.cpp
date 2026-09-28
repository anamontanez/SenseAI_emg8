/*******************************************************************************
 * @file sd_storage_sense.cpp
 * @brief Contains methods to handle SPI for SD card handling for ESP32 SoCs as
 * part of the Sense Ecosystem.
 *
 * @version 0.3.4
 * @date 2026-01-28
 * @author Mateo R.B. (mateor@sense-ai.co)
 *******************************************************************************
 *******************************************************************************/
#define __SD_SENSE_INTERNAL__

#include "sd_storage_sense.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef SD_SENSE_ENABLED

// Compile-time configuration validation
#ifndef CONFIG_FATFS_LFN_HEAP
#error \
    "CONFIG_FATFS_LFN_HEAP must be enabled for long filename support. \
    Add '-D CONFIG_FATFS_LFN_HEAP=1' to build_flags in platformio.ini"
#endif

#if !defined(CONFIG_FATFS_MAX_LFN) || (CONFIG_FATFS_MAX_LFN != 255)
#error \
    "CONFIG_FATFS_MAX_LFN must be set to 255 to avoid filename issues. \
    Add '-D CONFIG_FATFS_MAX_LFN=255' to build_flags in platformio.ini"
#endif

static const char* fatFsErr[] = {
    "FR_OK",              /* (0) Succeeded */
    "FR_DISK_ERR",        /* (1) A hard error occurred in the low level disk I/O layer
                           */
    "FR_INT_ERR",         /* (2) Assertion failed */
    "FR_NOT_READY",       /* (3) The physical drive cannot work */
    "FR_NO_FILE",         /* (4) Could not find the file */
    "FR_NO_PATH",         /* (5) Could not find the path */
    "FR_INVALID_NAME",    /* (6) The path name format is invalid */
    "FR_DENIED",          /* (7) Access denied due to prohibited access or directory full
                           */
    "FR_EXIST",           /* (8) Access denied due to prohibited access */
    "FR_INVALID_OBJECT",  /* (9) The file/directory object is invalid */
    "FR_WRITE_PROTECTED", /* (10) The physical drive is write protected */
    "FR_INVALID_DRIVE",   /* (11) The logical drive number is invalid */
    "FR_NOT_ENABLED",     /* (12) The volume has no work area */
    "FR_NO_FILESYSTEM",   /* (13) There is no valid FAT volume */
    "FR_MKFS_ABORTED",    /* (14) The f_mkfs() aborted due to any problem */
    "FR_TIMEOUT",         /* (15) Could not get a grant to access the volume within
                             defined period */
    "FR_LOCKED",          /* (16) The operation is rejected according to the file sharing
                             policy */
    "FR_NOT_ENOUGH_CORE", /* (17) Buffer could not be allocated */
    "FR_TOO_MANY_OPEN_FILES", /* (18) Number of open files > FF_FS_LOCK */
    "FR_INVALID_PARAMETER",   /* (19) Given parameter is invalid */
    nullptr};

SD::SD(SPI& spiHandle) : SD::SD(spiHandle, GPIO_NUM_13) {};

SD::SD(SPI& spiHandle, gpio_num_t csPin) : spiHandle_(spiHandle), csPin_(csPin) {};

SD::~SD() {
    deinit();
}

// Some SD v2 cards reject CMD59 while idle, but accept it once ready.
// Apply this only to that exact rejection. CRC must still be enabled before
// the normal SDK initialization proceeds to any data-register/block transfers.
static esp_err_t sdInitTransaction(int slot, sdmmc_command_t* cmd) {
    esp_err_t err = sdspi_host_do_transaction(slot, cmd);
    if (cmd->opcode != SD_CRC_ON_OFF || cmd->arg != 1 ||
        err != ESP_ERR_NOT_SUPPORTED ||
        SD_SPI_R1(cmd->response) != (SD_SPI_R1_IDLE_STATE | SD_SPI_R1_ILLEGAL_CMD))
        return err;

    sdmmc_command_t probe = {};
    probe.opcode = SD_SEND_IF_COND;
    probe.arg = 0x1aa;  // Confirm SD v2 at the existing 3.3 V interface.
    probe.flags = SCF_CMD_BCR | SCF_RSP_R7;
    probe.timeout_ms = cmd->timeout_ms;
    esp_err_t probeErr = sdspi_host_do_transaction(slot, &probe);
    if (probeErr != ESP_OK || (probe.response[0] & 0xfff) != 0x1aa)
        return err;

    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        probe = {};
        probe.opcode = MMC_APP_CMD;
        probe.flags = SCF_CMD_AC | SCF_RSP_R1;
        probe.timeout_ms = cmd->timeout_ms;
        probeErr = sdspi_host_do_transaction(slot, &probe);
        if (probeErr != ESP_OK) return probeErr;

        probe = {};
        probe.opcode = SD_APP_OP_COND;
        probe.arg = SD_OCR_SDHC_CAP;
        probe.flags = SCF_CMD_BCR | SCF_RSP_R3;
        probe.timeout_ms = cmd->timeout_ms;
        probeErr = sdspi_host_do_transaction(slot, &probe);
        if (probeErr != ESP_OK) return probeErr;
        if (!(SD_SPI_R1(probe.response) & SD_SPI_R1_IDLE_STATE)) {
            err = sdspi_host_do_transaction(slot, cmd);
            if (err == ESP_OK) printf("#SD:CRC_AFTER_READY\n");
            return err;  // Never report success unless the real CMD59 succeeds.
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t SD::init(void) {
    // Recovery reuses this object; do not leak a device slot after failed init.
    deinit();
    sdspi_device_config_t sdConfig = SDSPI_DEVICE_CONFIG_DEFAULT();
    sdConfig = {
        .host_id = spiHandle_.getHost(),
        .gpio_cs = csPin_,
        .gpio_cd = SDSPI_SLOT_NO_CD,
        .gpio_wp = SDSPI_SLOT_NO_WP,
        .gpio_int = GPIO_NUM_NC,
        .gpio_wp_polarity = SDSPI_IO_ACTIVE_LOW,
        .duty_cycle_pos = 0,
    };

    esp_err_t err = sdspi_host_init_device(&sdConfig, &sdHandler_);
    if (err) {
        return err;
    }
    deviceRegistered_ = true;

    sdHost_ = {
        .flags = SDMMC_HOST_FLAG_SPI | SDMMC_HOST_FLAG_DEINIT_ARG,
        .slot = sdHandler_,
        .max_freq_khz = SDMMC_FREQ_DEFAULT,
        .io_voltage = 3.3f,
        .driver_strength = SDMMC_DRIVER_STRENGTH_B,
        .current_limit = SDMMC_CURRENT_LIMIT_200MA,
        .init = &sdspi_host_init,
        .set_bus_width = NULL,
        .get_bus_width = NULL,
        .set_bus_ddr_mode = NULL,
        .set_card_clk = &sdspi_host_set_card_clk,
        .set_cclk_always_on = NULL,
        .do_transaction = &sdspi_host_do_transaction,
        .deinit_p = &sdspi_host_remove_device,
        .io_int_enable = &sdspi_host_io_int_enable,
        .io_int_wait = &sdspi_host_io_int_wait,
        .command_timeout_ms = 0,
        .get_real_freq = &sdspi_host_get_real_freq,
        .input_delay_phase = SDMMC_DELAY_PHASE_0,
        .set_input_delay = NULL,
        .dma_aligned_buffer = NULL,
        .pwr_ctrl_handle = NULL,
        .get_dma_info = &sdspi_host_get_dma_info,
        .is_slot_set_to_uhs1 = NULL,
    };

    sdHost_.do_transaction = &sdInitTransaction;
    err = sdmmc_card_init(&sdHost_, &sdCardInfo_);
    // The compatibility wrapper is initialization-only, with no runtime cost.
    sdHost_.do_transaction = &sdspi_host_do_transaction;
    sdCardInfo_.host.do_transaction = &sdspi_host_do_transaction;
    if (err != ESP_OK) deinit();
    return err;
}

esp_err_t SD::deinit(void) {
    if (pFile_ != nullptr) {
        f_close(pFile_);
        delete pFile_;
        pFile_ = nullptr;
    }
    if (pFatFs_ != nullptr || driveRegistered_) unmountCard();

    esp_err_t err = ESP_OK;
    if (deviceRegistered_) {
        err = sdHost_.deinit_p(sdHost_.slot);
        deviceRegistered_ = false;
    }

    path_.clear();
    obj_ = {};

    return err;
}

FRESULT SD::mountCard(void) {
    if (!deviceRegistered_) return FR_NOT_READY;
    if (pFatFs_) return FR_OK;

    // To prepare FatFs driver
    if (ff_diskio_get_drive(&driveNum_) != ESP_OK) return FR_NOT_ENOUGH_CORE;
    pFatFs_ = new FATFS{};
    ff_diskio_register_sdmmc(driveNum_, &sdCardInfo_);
    driveRegistered_ = true;
    ff_sdmmc_set_disk_status_check(driveNum_, true);

    root_[0] = (char)('0' + driveNum_);
    root_[1] = ':';
    root_[2] = 0;

    FRESULT err = f_mount(pFatFs_, root_, (BYTE)MountMode::kMountNow);
    if (err != FR_OK) {
        unmountCard();
        return err;
    }

    path_ = root_;
    updatePaths(ObjOps::kMount);
    return err;
}

FRESULT SD::unmountCard(void) {
    FRESULT err = pFatFs_ ? f_mount(nullptr, root_, 0) : FR_OK;
    if (driveRegistered_) {
        ff_diskio_unregister(driveNum_);
        driveRegistered_ = false;
    }
    delete pFatFs_;
    pFatFs_ = nullptr;
    path_.clear();

    return err;
}

esp_err_t SD::readSector(uint32_t sector, void* buffer) {
    if (!deviceRegistered_ || buffer == nullptr) return ESP_ERR_INVALID_STATE;
    if (sector >= static_cast<uint32_t>(sdCardInfo_.csd.capacity))
        return ESP_ERR_INVALID_ARG;
    return sdmmc_read_sectors(&sdCardInfo_, buffer, sector, 1);
}

uint32_t SD::getSectorSize(void) const {
    return sdCardInfo_.csd.sector_size > 0 ?
        static_cast<uint32_t>(sdCardInfo_.csd.sector_size) : 0;
}

uint32_t SD::getSectorCount(void) const {
    return sdCardInfo_.csd.capacity > 0 ?
        static_cast<uint32_t>(sdCardInfo_.csd.capacity) : 0;
}

const char* SD::getFastFsErrName(FRESULT err) {
    if (err > -1 && err < 20) {
        return fatFsErr[err];
    }

    return "Error not found";
}

std::string SD::getCurrentDir(void) {
    if (obj_.type == ObjType::kDir && obj_.lastOpDone == ObjOps::kGoToDir) {
        return obj_.path;
    }

    if (obj_.type == ObjType::kFile || obj_.type == ObjType::kDir) {
        return obj_.parentDirPath;
    }

    return root_;
}

FRESULT SD::getFileSize(uint32_t* sizeInBytes) {
    if (pFile_ == nullptr) {
        return FR_NO_FILE;
    }

    obj_.size = f_size(pFile_);
    *sizeInBytes = obj_.size;
    return FR_OK;
}

FRESULT SD::createFile(const std::string& fileName) {
    pFile_ = new FIL;
    std::string auxString = buildPath(fileName);

    FRESULT err = f_open(pFile_, auxString.c_str(), FA_CREATE_NEW | FA_WRITE);
    if (err) {
        delete pFile_;
        pFile_ = nullptr;
        return err;
    }

    err = f_close(pFile_);
    delete pFile_;
    pFile_ = nullptr;
    if (err) {
        return err;
    }

    path_ = auxString;
    updatePaths(ObjOps::kCreateFile);
    return err;
}

FRESULT SD::createDir(const std::string& folderName) {
    std::string auxString = buildPath(folderName);
    FRESULT err = f_mkdir(auxString.c_str());
    if (err) {
        return err;
    }

    path_ = auxString;
    updatePaths(ObjOps::kCreateDir);
    return err;
}

FRESULT SD::deleteObj(const std::string& objPath) {
    FRESULT err = f_unlink(objPath.c_str());
    if (err) {
        return err;
    }

    path_ = objPath;
    updatePaths(ObjOps::kDelete);
    return err;
}

FRESULT SD::renameObj(const std::string& oldName, const std::string& newName) {
    std::string oldPath = buildPath(oldName);
    std::string newPath = buildPath(newName);

    FRESULT err = f_rename(oldPath.c_str(), newPath.c_str());
    if (err) {
        return err;
    }

    path_ = newPath;
    updatePaths(ObjOps::kRename);
    return err;
}

FRESULT SD::openFile(const std::string& fileName, OpenMode mode) {
    pFile_ = new FIL;
    std::string auxString = buildPath(fileName);

    FRESULT err = f_open(pFile_, auxString.c_str(), (BYTE)mode);
    if (err) {
        delete pFile_;
        pFile_ = nullptr;
        return err;
    }

    path_ = auxString;
    updatePaths(ObjOps::kOpen);
    return err;
}

FRESULT SD::closeFile(void) {
    if (pFile_ == nullptr) {
        return FR_OK;
    }

    FRESULT err = f_close(pFile_);
    delete pFile_;
    pFile_ = nullptr;

    if (err) {
        return err;
    }

    updatePaths(ObjOps::kClose);
    return err;
}

FRESULT SD::fileWrite(const std::string& bufferToWrite) {
    if (pFile_ == nullptr) {
        return FR_NO_FILE;
    }

    unsigned int aux = 0;
    FRESULT err = f_write(pFile_, bufferToWrite.data(), bufferToWrite.size(), &aux);
    if (err) {
        return err;
    }

    err = f_sync(pFile_);
    if (err) {
        return err;
    }

    updatePaths(ObjOps::kWrite);
    return err;
}

FRESULT SD::fileRead(std::string& _bufferToReceive, size_t bufferSize) {
    if (pFile_ == nullptr) {
        return FR_NO_FILE;
    }

    obj_.size = f_size(pFile_);

    if (obj_.size == 0) {
        return FR_NO_FILE;
    }

    if (bufferSize > maxChunkSize_) {
        return FR_NOT_ENOUGH_CORE;
    }

    unsigned int aux = 0;
    _bufferToReceive.resize(bufferSize);

    FRESULT err = f_read(pFile_, &_bufferToReceive[0], bufferSize, &aux);
    if (err) {
        return err;
    }

    updatePaths(ObjOps::kRead);
    return err;
}

FRESULT SD::fileRead(std::string& _bufferToReceive) {
    if (pFile_ == nullptr) {
        return FR_NO_FILE;
    }

    obj_.size = f_size(pFile_);
    unsigned int bufferSize = obj_.size;

    if (obj_.size == 0) {
        return FR_OK;
    }

    if (obj_.size > maxChunkSize_) {
        bufferSize = maxChunkSize_;
        return FR_NOT_ENOUGH_CORE;
    }

    unsigned int aux = 0;
    _bufferToReceive.resize(bufferSize);

    FRESULT err = f_read(pFile_, &_bufferToReceive[0], bufferSize, &aux);
    if (err) {
        return err;
    }

    updatePaths(ObjOps::kRead);
    return err;
}

FRESULT SD::clearFile(const std::string& fileName) {
    FRESULT err = openFile(fileName, OpenMode::kOpenOverwrite);
    if (err) {
        return err;
    }

    return closeFile();
}

FRESULT SD::objSetTimestamp(const std::string& objPath, const std::string&, int year,
                            int month, int mday, int hour, int min, int sec) {
    FILINFO fno;

    fno.fdate = (WORD)(((year - 1980) * 512U) | month * 32U | mday);
    fno.ftime = (WORD)(hour * 2048U | min * 32U | sec / 2U);

    return f_utime(objPath.c_str(), &fno);
}

FRESULT SD::objSetAttr(const std::string& objPath, ObjAttribute AttrToToggle) {
    return f_chmod(objPath.c_str(), (BYTE)AttrToToggle, (BYTE)AttrToToggle);
}

FRESULT SD::inFileGoTo(uint32_t byteToGo) {
    FRESULT err = f_lseek(pFile_, byteToGo);
    return err;
}

FRESULT SD::listDir(const std::string& dirPath, Index_t* _pIndex) {
    static FF_DIR dir;
    static FILINFO fno;
    static std::string currentDir = "";
    static bool initialized = false;

    // Open directory on first call or if the path changed
    if (!initialized || dirPath != currentDir) {
        f_closedir(&dir);  // close previous dir (safe to call even if not opened)
        FRESULT err = f_opendir(&dir, dirPath.c_str());
        if (err != FR_OK) {
            initialized = false;
            return err;  // failed to open
        }

        currentDir = dirPath;
        path_ = dirPath;
        initialized = true;
    }

    // Read next entry
    FRESULT err = f_readdir(&dir, &fno);
    if (err != FR_OK || fno.fname[0] == '\0') {
        // End of directory or error
        f_closedir(&dir);
        initialized = false;
        _pIndex->objPath = "\0";
        _pIndex->currentDirPath = currentDir;
        _pIndex->type = ObjType::kNone;

        return err;
    }

    if (std::string(fno.fname).compare(".") == 0 ||
        std::string(fno.fname).compare("..") == 0) {
        return listDir(dirPath, _pIndex);  // recurse for next entry
    }

    // Return full path
    _pIndex->objPath = currentDir + "/" + std::string(fno.fname);
    _pIndex->currentDirPath = currentDir;
    if (fno.fattrib & AM_DIR) {
        _pIndex->type = ObjType::kDir;
        _pIndex->cntOfDirectories++;
    } else {
        _pIndex->type = ObjType::kFile;
        _pIndex->cntOfFiles++;
    }

    return err;
}

FRESULT SD::lookFor(const std::string& dirPath, const std::string& targetName,
                    std::string& _dirPath) {
    FF_DIR dir;
    FILINFO fno;
    FRESULT err;

    err = f_opendir(&dir, dirPath.c_str());
    if (err != FR_OK) {
        return err;
    }

    while (true) {
        err = f_readdir(&dir, &fno);
        if (err != FR_OK || fno.fname[0] == '\0') {
            break;
        }  // End or error

        std::string name(fno.fname);
        if (name == "." || name == "..") {
            continue;
        }

        std::string fullPath = dirPath + "/" + name;
        if (name == targetName) {
            _dirPath = (fno.fattrib & AM_DIR) ? fullPath : dirPath;
            f_closedir(&dir);
            return FR_OK;
        }

        if (fno.fattrib & AM_DIR) {
            // Recurse into subdirectory
            err = lookFor(fullPath, targetName, _dirPath);
            if (err == FR_OK && !_dirPath.empty()) {
                f_closedir(&dir);  // Close early if found
                return FR_OK;
            }
        }
    }

    f_closedir(&dir);
    return FR_NO_FILE;  // Not found
}

FRESULT SD::goToDir(const std::string& dirPath) {
    FF_DIR dir;
    FRESULT err = f_opendir(&dir, dirPath.c_str());
    if (err) {
        return err;
    }

    path_ = dirPath;
    updatePaths(ObjOps::kGoToDir);
    return f_closedir(&dir);
}

std::string SD::buildPath(const std::string& objName) {
    std::string auxString = getCurrentDir();
    auxString += "/";
    auxString += objName;

    return auxString;
}

void SD::updatePaths(ObjOps operation) {
    switch (operation) {
        case ObjOps::kMount:  // Initialize Object handler
            obj_.path = path_;
            obj_.parentDirPath = path_;
            obj_.size = 0;
            obj_.type = ObjType::kNone;  // Starts at root
            obj_.lastOpDone = ObjOps::kMount;
            break;
        case ObjOps::kCreateFile:
            obj_.name = getFileName();
            obj_.path = path_;
            obj_.parentDirPath = getDirPath();
            obj_.size = 0;
            obj_.type = ObjType::kFile;
            obj_.lastOpDone = ObjOps::kCreateFile;
            break;
        case ObjOps::kCreateDir:
            obj_.name = getFileName();
            obj_.path = path_;
            obj_.parentDirPath = getDirPath();
            obj_.size = 0;
            obj_.type = ObjType::kDir;
            obj_.lastOpDone = ObjOps::kCreateDir;
            break;
        case ObjOps::kOpen:
            obj_.name = getFileName();
            obj_.path = path_;
            obj_.parentDirPath = getDirPath();
            obj_.size = f_size(pFile_);
            obj_.type = ObjType::kFile;
            obj_.lastOpDone = ObjOps::kOpen;
            break;
        case ObjOps::kClose:
            obj_.lastOpDone = ObjOps::kClose;
            break;
        case ObjOps::kWrite:
            obj_.size = f_size(pFile_);
            obj_.lastOpDone = ObjOps::kWrite;
            break;
        case ObjOps::kRead:
            obj_.lastOpDone = ObjOps::kRead;
            break;
        case ObjOps::kDelete:
            obj_.name = getFileName();
            obj_.path = path_;
            obj_.parentDirPath = getDirPath();
            obj_.size = 0;
            obj_.type = ObjType::kNone;
            obj_.lastOpDone = ObjOps::kDelete;
            break;
        case ObjOps::kGoToDir:
            obj_.name = getFileName();
            obj_.path = path_;
            obj_.parentDirPath = getDirPath();
            obj_.size = 0;
            obj_.type = ObjType::kDir;
            obj_.lastOpDone = ObjOps::kGoToDir;
            break;
        case ObjOps::kRename:
            obj_.name = getFileName();
            obj_.path = path_;
            obj_.parentDirPath = getDirPath();
            obj_.lastOpDone = ObjOps::kRename;
            break;
        case ObjOps::kAttrChange:
            break;

        default:
            break;
    }
}

std::string SD::getFileName(void) {
    size_t pos = path_.find_last_of("/");
    return (pos != std::string::npos) ? path_.substr(pos + 1) : path_;
}

std::string SD::getDirPath(void) {
    size_t pos = path_.find_last_of("/");
    return (pos != std::string::npos) ? path_.substr(0, pos) : "0:";
}

#endif  // SD_SENSE_ENABLED
