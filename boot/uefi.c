#include <uefi.h>
#include <linux/multiboot.h>

static EFI_GUID gop_guid          = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID fs_guid           = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

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

#define PG                 4096ULL
#define KERNEL_BASE        0x100000ULL
#define KERNEL_ENTRY_PTR   (KERNEL_BASE + 0x10)       /* uefi_entry_ptr в .multiboot */
#define POOL_MAX           (128ULL * 1024 * 1024)    /* размер mem_map ядра */
#define IDMAP_LIMIT        (1ULL << 30)              /* identity-map ядра: 1 ГБ */
#define MMAP_PAGES         32                         /* 128 КБ под карту памяти */

static EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con;

static void puts16(const CHAR16 *s) { con->OutputString(con, s); }

static void put_hex(uint64_t v)
{
    CHAR16 b[19];
    b[0] = u'0'; b[1] = u'x';
    for (int i = 0; i < 16; i++) {
        uint32_t d = (v >> (60 - 4 * i)) & 0xF;
        b[2 + i] = (CHAR16)(d < 10 ? u'0' + d : u'A' + d - 10);
    }
    b[18] = 0;
    puts16(b);
}

/* Фатальная ошибка: показываем причину и остаёмся на экране (а не перезагружаемся) */
static void fail(const CHAR16 *msg, uint64_t code)
{
    puts16((const CHAR16 *)u"\r\n[FAIL] ");
    puts16(msg);
    puts16((const CHAR16 *)u" (");
    put_hex(code);
    puts16((const CHAR16 *)u")\r\nSystem halted. Power off manually.\r\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

static void fill(void *dst, uint8_t v, uint64_t n)
{
    __asm__ volatile ("rep stosb" : "+D"(dst), "+c"(n) : "a"(v) : "memory");
}

/* Открывает файл и возвращает его размер */
static EFI_FILE_PROTOCOL *open_file(EFI_FILE_PROTOCOL *root, const CHAR16 *name, uint64_t *size)
{
    EFI_FILE_PROTOCOL *f = NULL;
    if (root->Open(root, &f, name, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) return NULL;
    uint64_t info[64];
    uint64_t isz = sizeof(info);
    if (f->GetInfo(f, &file_info_guid, &isz, info) != EFI_SUCCESS) {
        f->Close(f);
        return NULL;
    }
    *size = ((EFI_FILE_INFO *)info)->FileSize;
    return f;
}

/* Читает ровно n байт (Read может вернуть меньше) */
static int read_exact(EFI_FILE_PROTOCOL *f, void *dst, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n) {
        uint64_t chunk = n > (1ULL << 20) ? (1ULL << 20) : n;
        if (f->Read(f, &chunk, p) != EFI_SUCCESS || chunk == 0) return -1;
        p += chunk;
        n -= chunk;
    }
    return 0;
}

static int usable_type(uint32_t t)
{
    return t == EfiLoaderCode || t == EfiLoaderData || t == EfiBootServicesCode ||
           t == EfiBootServicesData || t == EfiConventionalMemory;
}

/* .text.startup линкуется первым: entry point PE = начало образа (RVA 0x1000) */
__attribute__((section(".text.startup")))
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    con = SystemTable->ConOut;
    EFI_STATUS st;

    bs->SetWatchdogTimer(0, 0, 0, NULL);

    con->ClearScreen(con);
    puts16((const CHAR16 *)u"==============================================\r\n");
    puts16((const CHAR16 *)u"   Linux 0.01 (x86_64) UEFI Bare Metal Boot   \r\n");
    puts16((const CHAR16 *)u"==============================================\r\n\r\n");
    puts16((const CHAR16 *)u"[UEFI] Probing Hardware & GOP...\r\n");

    struct mb_info *mbi;
    uint64_t mbi_fb_addr = 0, fb_w = 0, fb_h = 0, fb_pitch = 0;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    bs->LocateProtocol(&gop_guid, NULL, (void **)&gop);
    if (gop && gop->Mode && gop->Mode->Info &&
        gop->Mode->Info->PixelFormat != 3 /* PixelBltOnly: нет линейного буфера */ &&
        gop->Mode->FrameBufferBase) {
        mbi_fb_addr = gop->Mode->FrameBufferBase;
        fb_w = gop->Mode->Info->HorizontalResolution;
        fb_h = gop->Mode->Info->VerticalResolution;
        fb_pitch = (uint64_t)gop->Mode->Info->PixelsPerScanLine * 4;
    }

    /* 1. Загрузочный раздел FAT32 */
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    st = bs->HandleProtocol(ImageHandle, &loaded_image_guid, (void **)&loaded_image);
    if (st != EFI_SUCCESS || !loaded_image) fail((const CHAR16 *)u"LoadedImage protocol", st);

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    st = bs->HandleProtocol(loaded_image->DeviceHandle, &fs_guid, (void **)&fs);
    if (st != EFI_SUCCESS || !fs) fail((const CHAR16 *)u"SimpleFileSystem protocol", st);

    EFI_FILE_PROTOCOL *root = NULL;
    st = fs->OpenVolume(fs, &root);
    if (st != EFI_SUCCESS) fail((const CHAR16 *)u"OpenVolume", st);

    /* 2. Ядро: читаем заголовки ELF, РЕЗЕРВИРУЕМ у UEFI диапазон ядра
     *    (AllocateAddress) и только потом читаем сегменты прямо на место. */
    puts16((const CHAR16 *)u"[UEFI] Loading /Image into memory...\r\n");
    uint64_t kfile_size = 0;
    EFI_FILE_PROTOCOL *kf = open_file(root, (const CHAR16 *)u"Image", &kfile_size);
    if (!kf) fail((const CHAR16 *)u"Cannot open /Image on the boot partition", 0);

    uint64_t hdr_buf[512];  /* 4 КБ, выровнено */
    uint64_t hdr_len = kfile_size < sizeof(hdr_buf) ? kfile_size : sizeof(hdr_buf);
    if (read_exact(kf, hdr_buf, hdr_len) != 0) fail((const CHAR16 *)u"Read /Image header", 0);

    Elf32_Ehdr *eh = (Elf32_Ehdr *)hdr_buf;
    if (hdr_len < sizeof(Elf32_Ehdr) || eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' || eh->e_ident[4] != 1)
        fail((const CHAR16 *)u"/Image is not an ELF32 file", 0);
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > hdr_len)
        fail((const CHAR16 *)u"ELF program headers outside first 4 KB", 0);

    uint64_t k_lo = ~0ULL, k_hi = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)((uint8_t *)hdr_buf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != 1) continue;
        if ((uint64_t)ph->p_offset + ph->p_filesz > kfile_size)
            fail((const CHAR16 *)u"ELF segment beyond end of file", i);
        if (ph->p_paddr < k_lo) k_lo = ph->p_paddr;
        if ((uint64_t)ph->p_paddr + ph->p_memsz > k_hi) k_hi = (uint64_t)ph->p_paddr + ph->p_memsz;
    }
    if (k_lo != KERNEL_BASE || k_hi <= k_lo)
        fail((const CHAR16 *)u"Kernel must be linked at 1 MB", k_lo);

    uint64_t k_end = (k_hi + PG - 1) & ~(PG - 1);
    uint64_t k_addr = KERNEL_BASE;
    st = bs->AllocatePages(AllocateAddress, EfiLoaderData, (k_end - k_addr) / PG, &k_addr);
    if (st != EFI_SUCCESS)
        fail((const CHAR16 *)u"Firmware did not give memory at 1 MB for the kernel", st);
    fill((void *)k_addr, 0, k_end - k_addr);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)((uint8_t *)hdr_buf + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != 1 || ph->p_filesz == 0) continue;
        if (kf->SetPosition(kf, ph->p_offset) != EFI_SUCCESS ||
            read_exact(kf, (void *)(uint64_t)ph->p_paddr, ph->p_filesz) != 0)
            fail((const CHAR16 *)u"Read kernel segment", i);
    }
    kf->Close(kf);

    uint64_t entry = *(uint64_t *)KERNEL_ENTRY_PTR;
    if (entry < KERNEL_BASE || entry >= k_end)
        fail((const CHAR16 *)u"Bad uefi_entry pointer in kernel image", entry);

    /* 3. Initrd сразу за ядром (чтобы ядро видело один непрерывный занятый блок) */
    puts16((const CHAR16 *)u"[UEFI] Loading /rootfs.tar (Initrd)...\r\n");
    uint64_t initrd_addr = k_end, initrd_size = 0, cur = k_end;
    EFI_FILE_PROTOCOL *rf = open_file(root, (const CHAR16 *)u"rootfs.tar", &initrd_size);
    if (!rf) {
        puts16((const CHAR16 *)u"[WARN] /rootfs.tar not found, booting without initrd\r\n");
        initrd_size = 0;
    } else {
        uint64_t pages = (initrd_size + PG - 1) / PG;
        st = bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &initrd_addr);
        if (st != EFI_SUCCESS) fail((const CHAR16 *)u"Cannot reserve memory for initrd", st);
        if (read_exact(rf, (void *)initrd_addr, initrd_size) != 0) fail((const CHAR16 *)u"Read /rootfs.tar", 0);
        rf->Close(rf);
        cur = initrd_addr + pages * PG;
    }
    root->Close(root);

    /* 4. Страница под mb_info + модуль -- сразу за initrd (ядро резервирует её само) */
    uint64_t mbi_page = cur;
    st = bs->AllocatePages(AllocateAddress, EfiLoaderData, 1, &mbi_page);
    if (st != EFI_SUCCESS) fail((const CHAR16 *)u"Cannot reserve mb_info page", st);
    fill((void *)mbi_page, 0, PG);
    mbi = (struct mb_info *)mbi_page;
    struct mb_module *mod = (struct mb_module *)(mbi_page + 512);

    mbi->flags = MB_FLAG_UEFI;
    if (initrd_size) {
        mod->mod_start = (uint32_t)initrd_addr;
        mod->mod_end   = (uint32_t)(initrd_addr + initrd_size);
        mbi->flags |= MB_FLAG_MODS;
        mbi->mods_count = 1;
        mbi->mods_addr = (uint32_t)(uint64_t)mod;
    }
    if (mbi_fb_addr) {
        mbi->flags |= MB_FLAG_FB;
        mbi->framebuffer_addr = mbi_fb_addr;
        mbi->framebuffer_width = (uint32_t)fb_w;
        mbi->framebuffer_height = (uint32_t)fb_h;
        mbi->framebuffer_pitch = (uint32_t)fb_pitch;
        mbi->framebuffer_bpp = 32;
        mbi->framebuffer_type = 1;
    }

    /* 5. Буфер под карту памяти -- из UEFI-страниц, а не из .bss
     *    (образ собирается как flat binary, .bss в нём не существует). */
    uint64_t mmap_buf = 0;
    st = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, MMAP_PAGES, &mmap_buf);
    if (st != EFI_SUCCESS) fail((const CHAR16 *)u"Cannot allocate memory-map buffer", st);

    puts16((const CHAR16 *)u"[OK] Exiting Boot Services & Launching Kernel...\r\n");

    uint64_t mmap_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    st = 1;
    for (int tries = 0; tries < 5 && st != EFI_SUCCESS; tries++) {
        mmap_size = MMAP_PAGES * PG;
        EFI_STATUS ms = bs->GetMemoryMap(&mmap_size, (void *)mmap_buf, &map_key, &desc_size, &desc_ver);
        if (ms != EFI_SUCCESS) fail((const CHAR16 *)u"GetMemoryMap", ms);
        st = bs->ExitBootServices(ImageHandle, map_key);
    }
    if (st != EFI_SUCCESS) fail((const CHAR16 *)u"ExitBootServices", st);

    /* ---- Boot Services больше нет: никаких вызовов прошивки ---- */

    /* 6. Непрерывный свободный участок после mb_info: только типы памяти, которые
     *    ядро вправе занять, и только в пределах identity-map ядра. */
    uint64_t pool_start = mbi_page + PG;
    uint64_t pool_end = pool_start;
    uint64_t limit = pool_start + POOL_MAX;
    if (limit > IDMAP_LIMIT) limit = IDMAP_LIMIT;
    int progress = 1;
    while (progress && pool_end < limit) {
        progress = 0;
        for (uint64_t off = 0; off + desc_size <= mmap_size; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmap_buf + off);
            uint64_t s = d->PhysicalStart, e = s + d->NumberOfPages * PG;
            if (usable_type(d->Type) && s <= pool_end && e > pool_end) {
                pool_end = e;
                progress = 1;
            }
        }
    }
    if (pool_end > limit) pool_end = limit;
    mbi->flags |= MB_FLAG_MEM;
    mbi->mem_lower = 640;
    mbi->mem_upper = (uint32_t)((pool_end - KERNEL_BASE) / 1024);

    uefi_kernel_jump(MULTIBOOT_BOOTLOADER_MAGIC, mbi_page, entry);

    return EFI_SUCCESS;
}
