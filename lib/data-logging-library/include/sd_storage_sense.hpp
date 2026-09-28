/*******************************************************************************
 * @file sd_storage_sense.hpp
 * @brief Contains methods to handle SPI for SD card handling for ESP32 SoCs as
 * part of the Sense Ecosystem.
 *
 * @version 0.3.4
 * @date 2026-01-28
 * @author Mateo R.B. (mateor@sense-ai.co)

 *******************************************************************************
 *******************************************************************************/

#pragma once

#if !defined(SD_SENSE_ENABLED) && !defined(__SD_SENSE_INTERNAL__)
#error \
    "SD Storage module is not enabled. \
    Add '-D SD_SENSE_ENABLED' to build_flags in platformio.ini to use this module."
#endif

#include <string>

#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdspi_host.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "spi_gp_sense.hpp"

/******************************************************************************/
/*                              SD STORAGE */
/******************************************************************************/

/**
 * @class SD
 *
 * @brief A class to manage SD card storage operations via SPI interface
 *
 * This class provides a comprehensive interface for SD card operations
 * including file and directory management, reading/writing operations, and
 * FAT filesystem support. It uses the FatFs library for filesystem
 * operations and communicates with the SD card via SPI protocol.
 */
class SD {
public:
    /**
     * @enum ObjType
     * @brief Enumeration for filesystem object types
     */
    enum class ObjType : int8_t {
        kDir = 0,   ///< Directory/folder object
        kFile = 1,  ///< File object
        kNone = -1  ///< No object or undefined type
    };

    /**
     * @enum MountMode
     * @brief Enumeration for SD card mounting behavior
     */
    enum class MountMode : uint8_t {
        kMountLater = 0,  ///< Initialize but don't mount immediately
        kMountNow = 1     ///< Initialize and mount immediately
    };

    /**
     * @enum openMode
     * @brief Enumeration for file opening modes
     * @note These modes control how files are opened and what operations are
     * allowed
     */
    enum class OpenMode : uint8_t {
        kOpenReadOnly =
            FA_OPEN_EXISTING | FA_READ,  ///< Open existing file for reading only
        kOpenIfExist =
            FA_OPEN_EXISTING | FA_READ | FA_WRITE,  ///< Open existing file for read/write
        kOpenOverwrite = FA_CREATE_ALWAYS | FA_READ |
                         FA_WRITE,  ///< Create new file or overwrite existing
        kOpenAppend = FA_OPEN_APPEND | FA_READ |
                      FA_WRITE  ///< Open for appending data to end of file
    };

    /**
     * @enum ObjAttribute
     * @brief Enumeration for filesystem object attributes
     * @note These attributes control file/directory properties and access
     * rights
     */
    enum class ObjAttribute : uint8_t {
        kReadOnly = AM_RDO,  ///< Read-only attribute
        kHidden = AM_HID,    ///< Hidden attribute
        kSysObj = AM_SYS,    ///< System object attribute
        kArchived = AM_ARC,  ///< Archive attribute
    };

    /**
     * @enum ObjOps
     * @brief Enumeration for tracking filesystem operations
     * @note Used internally to track the last operation performed for error
     * handling and state management
     */
    enum class ObjOps : uint8_t {
        kMount = 0x0,       ///< Mount operation
        kCreateFile = 0x1,  ///< File creation operation
        kCreateDir = 0x2,   ///< Directory creation operation
        kOpen = 0x3,        ///< File/directory open operation
        kClose = 0x4,       ///< File close operation
        kWrite = 0x5,       ///< Write operation
        kRead = 0x6,        ///< Read operation
        kDelete = 0x7,      ///< Delete operation
        kGoToDir = 0x8,     ///< Directory navigation operation
        kAttrChange = 0x9,  ///< Attribute change operation
        kRename = 0xA,      ///< Rename operation
    };

    /**
     * @struct Index_t
     * @brief Structure for directory indexing and navigation
     */
    typedef struct {
        std::string objName;         ///< Name of the object (file or directory)
        std::string objPath;         ///< Full path to the object
        SD::ObjType type;            ///< Type of object (file or directory)
        std::string currentDirPath;  ///< Current directory path
        int cntOfFiles;              ///< Count of files in current directory
        int cntOfDirectories;        ///< Count of directories in current directory
    } Index_t;

    /**
     * @brief Construct a new SD object with custom CS pin
     *
     * @param spiHandle Reference to an initialized SPI object for communication
     * @param csPin GPIO pin number for Chip Select (CS) signal
     *
     * @note The SPI object must be initialized before creating the SD object
     */
    SD(SPI& spiHandle, gpio_num_t csPin);

    /**
     * @brief Construct a new SD object using default CS pin from SPI
     * configuration
     *
     * @param spiHandle Reference to an initialized SPI object for communication
     *
     * @note Uses the default CS pin configuration from the SPI object
     */
    SD(SPI& spiHandle);

    /**
     * @brief Destructor for the SD object
     *
     * Automatically unmounts the card and frees allocated resources
     */
    ~SD();

    /**
     * @brief Initialize the SD card interface
     *
     * @return ESP_OK on success, ESP_FAIL on initialization failure
     *
     * @note Must be called before any SD card operations
     */
    esp_err_t init(void);

    /**
     * @brief Deinitialize the SD card interface and free resources
     *
     * @return ESP_OK on success, ESP_FAIL on deinitialization failure
     */
    esp_err_t deinit(void);

    // Read-only sector access used only by the mount-failure diagnostic.
    // The caller must provide a DMA-capable buffer at least one sector large.
    esp_err_t readSector(uint32_t sector, void* buffer);
    uint32_t getSectorSize(void) const;
    uint32_t getSectorCount(void) const;

    /**
     * @brief Mount the SD card filesystem
     *
     * @return FR_OK on success, other FRESULT codes on failure
     *
     * @note Must be called before accessing files or directories
     */
    FRESULT mountCard(void);

    /**
     * @brief Unmount the SD card filesystem
     *
     * @return FR_OK on success, other FRESULT codes on failure
     *
     * @note Should be called before removing the SD card or shutting down
     */
    FRESULT unmountCard(void);

    // Getters
    /**
     * @brief Get the human-readable name of a FatFs error code
     *
     * @param err FRESULT error code
     * @return const char* String description of the error
     */
    const char* getFastFsErrName(FRESULT err);

    /**
     * @brief Get the current working directory path
     *
     * @return std::string Current directory path
     */
    std::string getCurrentDir(void);

    /**
     * @brief Get the size of the currently opened file
     *
     * @param sizeInBytes Pointer to store the file size in bytes
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT getFileSize(uint32_t* sizeInBytes);

    /**
     * @brief Check if the SD card is currently mounted
     *
     * @return true if card is mounted, false otherwise
     */
    inline bool isCardMounted(void) {
        return (pFatFs_ != nullptr);
    }

    // File and Directory Operations
    /**
     * @brief Create a new directory
     *
     * @param folderName Name of the directory to create
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT createDir(const std::string& folderName);

    /**
     * @brief Create a new file
     *
     * @param fileName Name of the file to create
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT createFile(const std::string& fileName);

    /**
     * @brief Delete a file or directory
     *
     * @param objName Name of the object to delete
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT deleteObj(const std::string& objName);

    /**
     * @brief Rename a file or directory
     *
     * @param oldName Current name of the object to rename
     * @param newName New name for the object
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT renameObj(const std::string& oldName, const std::string& newName);

    /**
     * @brief List contents of a directory
     *
     * @param dirPath Path to the directory to list
     * @param _pIndex Pointer to Index_t structure to store directory
     * information
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT listDir(const std::string& dirPath, Index_t* _pIndex);

    /**
     * @brief Search for a file or directory
     *
     * @param dirPath Directory path to search in
     * @param targetName Name of the target file/directory
     * @param _dirPath Reference to store the found object's directory path
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT lookFor(const std::string& dirPath, const std::string& targetName,
                    std::string& _dirPath);

    /**
     * @brief Navigate to a specific directory
     *
     * @param dirPath Path to the target directory
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT goToDir(const std::string& dirPath);

    // File Operations
    /**
     * @brief Open a file in the current directory
     *
     * @param fileName Name of the file to open (e.g., "file.txt")
     * @param mode Opening mode (see openMode enum)
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT openFile(const std::string& fileName, OpenMode mode);

    /**
     * @brief Close the currently opened file
     *
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT closeFile(void);

    /**
     * @brief Write data to the currently opened file
     *
     * @param bufferToWrite String containing data to write
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT fileWrite(const std::string& bufferToWrite);

    /**
     * @brief Read data from the currently opened file with specified buffer
     * size
     *
     * @param _bufferToReceive Reference to string to store read data
     * @param bufferSize Maximum number of bytes to read
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT fileRead(std::string& _bufferToReceive, size_t bufferSize);

    /**
     * @brief Read all remaining data from the currently opened file
     *
     * @param _bufferToReceive Reference to string to store read data
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT fileRead(std::string& _bufferToReceive);

    /**
     * @brief Clear all contents of a file
     *
     * @param fileName Name of the file to clear
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT clearFile(const std::string& fileName);

    // Advanced Operations
    /**
     * @brief Set timestamp for a file or directory
     *
     * @param objPath Path to the object
     * @param year Year (e.g., 2025)
     * @param month Month (1-12)
     * @param mday Day of month (1-31)
     * @param hour Hour (0-23)
     * @param min Minute (0-59)
     * @param sec Second (0-59)
     * @return FR_OK on success, other FRESULT codes on failure
     */
    FRESULT objSetTimestamp(const std::string& objPath, const std::string&, int year,
                            int month, int mday, int hour, int min, int sec);

    /**
     * @brief Change attributes of a file or directory
     *
     * @param objPath Path to the object
     * @param AttrToToggle Attribute to toggle (see ObjAttribute enum)
     * @return FR_OK on success, other FRESULT codes on failure
     *
     * @note See ObjAttribute enum for available attributes (ReadOnly, Hidden,
     * System, Archive)
     */
    FRESULT objSetAttr(const std::string& objPath, ObjAttribute AttrToToggle);

    /**
     * @brief Move the file pointer to a specific byte position
     *
     * @param byteToGo Byte position to move the file pointer to
     * @return FR_OK on success, other FRESULT codes on failure
     *
     * @note This is like a cursor that can be moved during read/write
     * operations
     */
    FRESULT inFileGoTo(uint32_t byteToGo);

private:
    sdspi_dev_handle_t sdHandler_{};  ///< SD card SPI device handle
    bool deviceRegistered_ = false;
    SPI& spiHandle_;                ///< Reference to SPI interface object
    gpio_num_t csPin_;              ///< Chip Select GPIO pin
    gpio_num_t cdPin_;              ///< Card Detect GPIO pin (optional)
    gpio_num_t wpPin_;              ///< Write Protect GPIO pin (optional)
    gpio_num_t intPin_;             ///< Interrupt GPIO pin (optional)

    sdmmc_host_t sdHost_{};      ///< SD/MMC host configuration
    sdmmc_card_t sdCardInfo_{};  ///< SD card information structure

    std::string path_ = "";  ///< Current working path on SD card

    FATFS* pFatFs_ = nullptr;  ///< Pointer to FatFs filesystem object
    uint8_t driveNum_ = 0;     ///< Drive number for FatFs
    bool driveRegistered_ = false;
    char root_[3]{};             ///< Root directory string

    uint16_t maxChunkSize_ = 4095;  ///< Maximum chunk size for read/write operations

    FIL* pFile_ = nullptr;  ///< Pointer to currently opened file

    /**
     * @struct ObjectHandle
     * @brief Internal structure for tracking current object state
     */
    struct ObjectHandle {
        std::string name;           ///< Object name
        ObjType type;               ///< Object type (file/directory)
        uint32_t size;              ///< Object size in bytes
        std::string path;           ///< Full object path
        std::string parentDirPath;  ///< Parent directory path
        ObjOps lastOpDone;          ///< Last operation performed
    };

    ObjectHandle obj_;  ///< Current object handle

    /**
     * @brief Build full path from object name
     *
     * @param objName Name of the object
     * @return std::string Full path to the object
     */
    std::string buildPath(const std::string& objName);

    /**
     * @brief Get the directory path portion from current path
     *
     * @return std::string Directory path
     */
    std::string getDirPath(void);

    /**
     * @brief Get the filename portion from current path
     *
     * @return std::string Filename
     */
    std::string getFileName(void);

    /**
     * @brief Update internal paths after an operation
     *
     * @param operation The operation that was performed
     */
    void updatePaths(ObjOps operation);
};
