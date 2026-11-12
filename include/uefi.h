#ifndef _UEFI_H
#define _UEFI_H

#include <linux/types.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef void    *EFI_HANDLE;
typedef uint16_t CHAR16;

#define EFI_SUCCESS 0ULL

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    uint8_t ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    const CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET        Reset;
    EFI_TEXT_STRING       OutputString;
    void                 *TestString;
    void                 *QueryMode;
    void                 *SetMode;
    void                 *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    void                 *SetCursorPosition;
    void                 *EnableCursor;
    void                 *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Graphics Output Protocol (GOP) */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
{ 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                             MaxMode;
    uint32_t                             Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    uint64_t                             SizeOfInfo;
    uint64_t                             FrameBufferBase;
    uint64_t                             FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void                              *QueryMode;
    void                              *SetMode;
    void                              *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* Boot Services */
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    uint32_t Type,
    uint32_t MemoryType,
    uint64_t Pages,
    uint64_t *Memory
);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    uint64_t *MemoryMapSize,
    void     *MemoryMap,
    uint64_t *MapKey,
    uint64_t *DescriptorSize,
    uint32_t *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    uint64_t   MapKey
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    const EFI_GUID *Protocol,
    void           *Registration,
    void          **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE       Handle,
    const EFI_GUID  *Protocol,
    void           **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    uint64_t Timeout,
    uint64_t WatchdogCode,
    uint64_t DataSize,
    CHAR16  *WatchdogData
);

/* Типы памяти UEFI */
#define EfiLoaderCode          1
#define EfiLoaderData          2
#define EfiBootServicesCode    3
#define EfiBootServicesData    4
#define EfiConventionalMemory  7

/* Типы AllocatePages */
#define AllocateAnyPages       0
#define AllocateMaxAddress     1
#define AllocateAddress        2

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    char                 Hdr[24];
    void                *RaiseTPL;
    void                *RestoreTPL;
    EFI_ALLOCATE_PAGES   AllocatePages;
    void                *FreePages;
    EFI_GET_MEMORY_MAP   GetMemoryMap;
    void                *AllocatePool;
    void                *FreePool;
    void                *CreateEvent;
    void                *SetTimer;
    void                *WaitForEvent;
    void                *SignalEvent;
    void                *CloseEvent;
    void                *CheckEvent;
    void                *InstallProtocolInterface;
    void                *ReinstallProtocolInterface;
    void                *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL  HandleProtocol;
    void                *Void;
    void                *RegisterProtocolNotify;
    void                *LocateHandle;
    void                *LocateDevicePath;
    void                *InstallConfigurationTable;
    void                *LoadImage;
    void                *StartImage;
    void                *Exit;
    void                *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void                *GetNextMonotonicCount;
    void                *Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
    void                *ConnectController;
    void                *DisconnectController;
    void                *OpenProtocol;
    void                *CloseProtocol;
    void                *OpenProtocolInformation;
    void                *ProtocolsPerHandle;
    void                *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL  LocateProtocol;
} EFI_BOOT_SERVICES;

typedef struct {
    char                            Hdr[24];
    CHAR16                         *FirmwareVendor;
    uint32_t                        FirmwareRevision;
    void                           *ConsoleInHandle;
    void                           *ConIn;
    EFI_HANDLE                      ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    void                           *StandardErrorHandle;
    void                           *StdErr;
    void                           *RuntimeServices;
    EFI_BOOT_SERVICES              *BootServices;
    uint64_t                        NumberOfTableEntries;
    void                           *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* Loaded Image & File System Protocols */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
} EFI_LOADED_IMAGE_PROTOCOL;

struct _EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    struct _EFI_FILE_PROTOCOL *This,
    struct _EFI_FILE_PROTOCOL **NewHandle,
    const CHAR16 *FileName,
    uint64_t OpenMode,
    uint64_t Attributes
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(struct _EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(struct _EFI_FILE_PROTOCOL *This, uint64_t *BufferSize, void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(struct _EFI_FILE_PROTOCOL *This, uint64_t Position);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(struct _EFI_FILE_PROTOCOL *This, const EFI_GUID *InformationType, uint64_t *BufferSize, void *Buffer);

typedef struct _EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_FILE_OPEN  Open;
    EFI_FILE_CLOSE Close;
    void          *Delete;
    EFI_FILE_READ  Read;
    void          *Write;
    void          *GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO     GetInfo;
    void          *SetInfo;
    void          *Flush;
} EFI_FILE_PROTOCOL;

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* EFI_TIME = 16 байт, три штуки в EFI_FILE_INFO */
typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint8_t  Times[48];
    uint64_t Attribute;
    CHAR16   FileName[1];
} EFI_FILE_INFO;

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#endif
