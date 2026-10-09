#include <uefi.h>
#include <linux/multiboot.h>

static EFI_GUID gop_guid          = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID fs_guid           = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static uint8_t mem_map_buf[16384];
static struct mb_info mbi;
static struct mb_module mod_initrd;

/* Структуры заголовка ELF32 для парсинга ядра Image */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

extern void uefi_kernel_jump(uint64_t magic, uint64_t mbi_addr, uint64_t entry_point);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out = SystemTable->ConOut;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;

    con_out->ClearScreen(con_out);
    con_out->OutputString(con_out, (const CHAR16 *)u"==============================================\r\n");
    con_out->OutputString(con_out, (const CHAR16 *)u"   Linux 0.01 (x86_64) UEFI Bare Metal Boot   \r\n");
    con_out->OutputString(con_out, (const CHAR16 *)u"==============================================\r\n\r\n");

    con_out->OutputString(con_out, (const CHAR16 *)u"[UEFI] Probing Hardware & GOP...\r\n");

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    bs->LocateProtocol(&gop_guid, NULL, (void **)&gop);

    /* 1. Доступ к загрузочному разделу FAT32 */
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    bs->HandleProtocol(ImageHandle, &loaded_image_guid, (void **)&loaded_image);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    bs->HandleProtocol(loaded_image->DeviceHandle, &fs_guid, (void **)&fs);

    EFI_FILE_PROTOCOL *root = NULL;
    fs->OpenVolume(fs, &root);

    /* 2. Загрузка и развертывание ELF-сегментов ядра Image */
    con_out->OutputString(con_out, (const CHAR16 *)u"[UEFI] Loading /Image into memory...\r\n");
    EFI_FILE_PROTOCOL *kernel_file = NULL;
    root->Open(root, &kernel_file, (const CHAR16 *)u"Image", EFI_FILE_MODE_READ, 0);

    uint64_t temp_elf_buf = 0x3FFFFFFF; /* Ограничиваем < 1 ГБ для нашей identity-map */
    bs->AllocatePages(1, 2, 128, &temp_elf_buf); /* 1 = AllocateMaxAddress */
    uint64_t kernel_read_size = 512 * 1024;
    kernel_file->Read(kernel_file, &kernel_read_size, (void *)temp_elf_buf);
    kernel_file->Close(kernel_file);

    /* Копируем PT_LOAD сегменты ядра на их физический адрес (1 МБ = 0x100000) */
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)temp_elf_buf;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(temp_elf_buf + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type == 1) { /* PT_LOAD */
            uint8_t *src = (uint8_t *)(temp_elf_buf + ph->p_offset);
            uint8_t *dst = (uint8_t *)(uint64_t)ph->p_paddr;
            for (uint32_t k = 0; k < ph->p_filesz; k++) dst[k] = src[k];
            for (uint32_t k = ph->p_filesz; k < ph->p_memsz; k++) dst[k] = 0;
        }
    }

    /* 3. Загрузка Initrd архива rootfs.tar */
    con_out->OutputString(con_out, (const CHAR16 *)u"[UEFI] Loading /rootfs.tar (Initrd)...\r\n");
    EFI_FILE_PROTOCOL *initrd_file = NULL;
    root->Open(root, &initrd_file, (const CHAR16 *)u"rootfs.tar", EFI_FILE_MODE_READ, 0);

    uint64_t initrd_addr = 0x3FFFFFFF; /* Ограничиваем < 1 ГБ */
    bs->AllocatePages(1, 2, 256, &initrd_addr); /* 1 = AllocateMaxAddress */
    uint64_t initrd_size = 1024 * 1024;
    initrd_file->Read(initrd_file, &initrd_size, (void *)initrd_addr);
    initrd_file->Close(initrd_file);
    root->Close(root);

    /* 4. Настройка Multiboot структуры параметров для ядра */
    mod_initrd.mod_start = (uint32_t)initrd_addr;
    mod_initrd.mod_end   = (uint32_t)(initrd_addr + initrd_size);
    mbi.flags = MB_FLAG_MODS;
    mbi.mods_count = 1;
    mbi.mods_addr = (uint32_t)(uint64_t)&mod_initrd;

    con_out->OutputString(con_out, (const CHAR16 *)u"[OK] Exiting Boot Services & Launching Kernel...\r\n");

    /* 5. Забор карты памяти и выход из UEFI Boot Services */
    uint64_t mmap_size = sizeof(mem_map_buf);
    uint64_t map_key = 0;
    uint64_t desc_size = 0;
    uint32_t desc_ver = 0;
    bs->GetMemoryMap(&mmap_size, mem_map_buf, &map_key, &desc_size, &desc_ver);

    EFI_STATUS status = bs->ExitBootServices(ImageHandle, map_key);
    if (status != EFI_SUCCESS) {
        mmap_size = sizeof(mem_map_buf);
        bs->GetMemoryMap(&mmap_size, mem_map_buf, &map_key, &desc_size, &desc_ver);
        bs->ExitBootServices(ImageHandle, map_key);
    }

    /* 6. Считываем 64-битный адрес uefi_entry ядра по смещению 0x100010 */
    uint64_t uefi_entry_addr = *(uint64_t *)0x100010ULL;

    uefi_kernel_jump(MULTIBOOT_BOOTLOADER_MAGIC, (uint64_t)&mbi, uefi_entry_addr);

    return EFI_SUCCESS;
}
