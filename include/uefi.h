#pragma once

#include <stdint.h>

typedef uint8_t  BOOLEAN;
typedef uint8_t  UINT8;
typedef int16_t  INT16;
typedef uint16_t CHAR16;
typedef char     CHAR8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int32_t  INT32;
typedef uint64_t UINT64;
typedef int64_t  INT64;
typedef uintptr_t UINTN;
typedef void     VOID;
typedef VOID    *EFI_HANDLE;
typedef UINT64   EFI_STATUS;
typedef UINT64   EFI_PHYSICAL_ADDRESS;
typedef UINT64   EFI_VIRTUAL_ADDRESS;
typedef UINTN    EFI_TPL;

#define EFIAPI __attribute__((ms_abi))
#define EFI_ERROR(status) ((INT64)(status) < 0)
#define EFIERR(code) (0x8000000000000000ULL | (UINT64)(code))

#define EFI_SUCCESS             0
#define EFI_LOAD_ERROR          EFIERR(1)
#define EFI_INVALID_PARAMETER   EFIERR(2)
#define EFI_UNSUPPORTED         EFIERR(3)
#define EFI_BAD_BUFFER_SIZE     EFIERR(4)
#define EFI_BUFFER_TOO_SMALL    EFIERR(5)
#define EFI_NOT_READY           EFIERR(6)
#define EFI_DEVICE_ERROR        EFIERR(7)
#define EFI_WRITE_PROTECTED     EFIERR(8)
#define EFI_OUT_OF_RESOURCES    EFIERR(9)
#define EFI_VOLUME_CORRUPTED    EFIERR(10)
#define EFI_VOLUME_FULL         EFIERR(11)
#define EFI_NO_MEDIA            EFIERR(12)
#define EFI_MEDIA_CHANGED       EFIERR(13)
#define EFI_NOT_FOUND            EFIERR(14)
#define EFI_ACCESS_DENIED       EFIERR(15)
#define EFI_NO_RESPONSE         EFIERR(16)
#define EFI_NO_MAPPING          EFIERR(17)
#define EFI_TIMEOUT             EFIERR(18)
#define EFI_NOT_STARTED         EFIERR(19)
#define EFI_ALREADY_STARTED     EFIERR(20)
#define EFI_ABORTED             EFIERR(21)
#define EFI_ICMP_ERROR          EFIERR(22)
#define EFI_TFTP_ERROR          EFIERR(23)
#define EFI_PROTOCOL_ERROR     EFIERR(24)
#define EFI_INCOMPATIBLE_VERSION EFIERR(25)
#define EFI_SECURITY_VIOLATION  EFIERR(26)
#define EFI_CRC_ERROR           EFIERR(27)
#define EFI_END_OF_MEDIA        EFIERR(28)
#define EFI_END_OF_FILE         EFIERR(31)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    VOID *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    struct EFI_RUNTIME_SERVICES *RuntimeServices;
    struct EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    struct EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct EFI_CONFIGURATION_TABLE {
    EFI_GUID VendorGuid;
    VOID *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(VOID *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(VOID *This,
                                            BOOLEAN ExtendedVerification);

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET Reset;
    EFI_TEXT_STRING OutputString;
    VOID *TestString;
    VOID *QueryMode;
    VOID *SetMode;
    VOID *SetAttribute;
    VOID *ClearScreen;
    VOID *SetCursorPosition;
    VOID *EnableCursor;
    VOID *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory,
                                            UINTN Pages);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
    UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(EFI_MEMORY_TYPE PoolType,
                                                UINTN Size, VOID **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle,
                                                    UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle,
                                                 EFI_GUID *Protocol,
                                                 VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_OPEN_PROTOCOL)(
    EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface,
    EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle, UINT32 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_CLOSE_PROTOCOL)(EFI_HANDLE Handle,
                                                EFI_GUID *Protocol,
                                                EFI_HANDLE AgentHandle,
                                                EFI_HANDLE ControllerHandle);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(EFI_GUID *Protocol,
                                                 VOID *Registration,
                                                 VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    UINT32 SearchType, EFI_GUID *Protocol, VOID *SearchKey, UINTN *NoHandles,
    EFI_HANDLE **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);
typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(
    CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 *Attributes,
    UINTN *DataSize, VOID *Data);
typedef EFI_STATUS (EFIAPI *EFI_SET_VARIABLE)(
    CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 Attributes,
    UINTN DataSize, VOID *Data);

#define EFI_VARIABLE_NON_VOLATILE       0x00000001U
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002U
#define EFI_VARIABLE_RUNTIME_ACCESS     0x00000004U

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;
    EFI_FREE_PAGES FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;
    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;
    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    VOID *GetNextMonotonicCount;
    VOID *Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
    VOID *ConnectController;
    VOID *DisconnectController;
    EFI_OPEN_PROTOCOL OpenProtocol;
    EFI_CLOSE_PROTOCOL CloseProtocol;
    VOID *OpenProtocolInformation;
    VOID *ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;
    VOID *CalculateCrc32;
    VOID *CopyMem;
    VOID *SetMem;
    VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct EFI_RUNTIME_SERVICES {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (EFIAPI *GetTime)(VOID *Time, VOID *Capabilities);
    EFI_STATUS (EFIAPI *SetTime)(VOID *Time);
    VOID *GetWakeupTime;
    VOID *SetWakeupTime;
    VOID *SetVirtualAddressMap;
    VOID *ConvertPointer;
    EFI_GET_VARIABLE GetVariable;
    VOID *GetNextVariableName;
    EFI_SET_VARIABLE SetVariable;
    VOID *GetNextHighMonotonicCount;
    VOID *ResetSystem;
    VOID *UpdateCapsule;
    VOID *QueryCapsuleCapabilities;
    VOID *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct {
    UINT32 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(VOID *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef UINT64 EFI_FILE_HANDLE_MODE;
#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_DIRECTORY   0x0000000000000010ULL

struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *This,
                              struct EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, EFI_FILE_HANDLE_MODE OpenMode,
                              UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(struct EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *This,
                              UINTN *BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *Write)(struct EFI_FILE_PROTOCOL *This,
                               UINTN *BufferSize, VOID *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *This,
                                     UINT64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *This,
                                     UINT64 Position);
    EFI_STATUS (EFIAPI *GetInfo)(struct EFI_FILE_PROTOCOL *This,
                                 EFI_GUID *InformationType, UINTN *BufferSize,
                                 VOID *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(struct EFI_FILE_PROTOCOL *This,
                                 EFI_GUID *InformationType, UINTN BufferSize,
                                 VOID *Buffer);
    EFI_STATUS (EFIAPI *Flush)(struct EFI_FILE_PROTOCOL *This);
    VOID *OpenEx;
    VOID *ReadEx;
    VOID *WriteEx;
    VOID *FlushEx;
};

typedef struct {
    UINT16 Year;
    UINT8 Month;
    UINT8 Day;
    UINT8 Hour;
    UINT8 Minute;
    UINT8 Second;
    UINT8 Pad1;
    UINT32 Nanosecond;
    INT16 TimeZone;
    UINT8 Daylight;
    UINT8 Pad2;
} EFI_TIME;

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    UINT32 PixelInformation[4];
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(
        struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber,
        UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                                 UINT32 ModeNumber);
    EFI_STATUS (EFIAPI *Blt)(VOID *This, VOID *BltBuffer, UINT32 BltOperation,
                             UINTN SourceX, UINTN SourceY, UINTN DestinationX,
                             UINTN DestinationY, UINTN Width, UINTN Height,
                             UINTN Delta);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct EFI_RNG_PROTOCOL {
    EFI_STATUS (EFIAPI *GetInfo)(VOID *This, UINTN InformationType,
                                 UINTN *Input, VOID *Output);
    EFI_STATUS (EFIAPI *GetRNG)(VOID *This, EFI_GUID *RNGAlgorithm,
                                UINTN Length, UINT8 *Out);
} EFI_RNG_PROTOCOL;

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    VOID *FilePath;
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct __attribute__((packed)) {
    UINT8 Type;
    UINT8 SubType;
    UINT16 Length;
} EFI_DEVICE_PATH_PROTOCOL;

#define EFI_DEVICE_PATH_TYPE_MEDIA 0x04U
#define EFI_DEVICE_PATH_TYPE_END 0x7FU
#define EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE 0xFFU
#define EFI_DEVICE_PATH_SUBTYPE_HARDDRIVE 0x01U

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x00000001U
#define EFI_NATIVE_INTERFACE ((VOID *)0)
#define EFI_LOADER_DATA EfiLoaderData
#define EFI_LOADER_CODE EfiLoaderCode

extern const EFI_GUID gEfiLoadedImageProtocolGuid;
extern const EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern const EFI_GUID gEfiDevicePathProtocolGuid;
extern const EFI_GUID gEfiFileInfoGuid;
extern const EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern const EFI_GUID gEfiRngProtocolGuid;
extern const EFI_GUID gEfiAcpi20TableGuid;
extern const EFI_GUID gEfiAcpi10TableGuid;
extern const EFI_GUID gEfiSmbios3TableGuid;
extern const EFI_GUID gEfiSmbiosTableGuid;
