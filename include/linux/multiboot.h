#ifndef _LINUX_MULTIBOOT_H
#define _LINUX_MULTIBOOT_H

#include <linux/types.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MB_FLAG_MEM                (1 << 0)
#define MB_FLAG_MODS               (1 << 3)
#define MB_FLAG_FB                 (1 << 12)  /* framebuffer_* поля валидны */
/* Расширение: ядро загружено нашим UEFI-загрузчиком, mem_upper = граница
 * непрерывной свободной области (КБ выше 1 МБ), проверенная по карте памяти UEFI */
#define MB_FLAG_UEFI               (1u << 31)

struct mb_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} __attribute__((packed));

struct mb_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
} __attribute__((packed));

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed));

#endif
