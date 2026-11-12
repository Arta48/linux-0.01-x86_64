#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: mkefi <input.bin> <output.efi>\n");
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("fopen input"); return 1; }
    fseek(fin, 0, SEEK_END);
    long code_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    uint8_t *code = malloc(code_size);
    if (fread(code, 1, code_size, fin) != (size_t)code_size) { perror("fread"); return 1; }
    fclose(fin);

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("fopen output"); return 1; }

    uint8_t header[512];
    memset(header, 0, sizeof(header));

    /* MS-DOS Header */
    header[0] = 'M'; header[1] = 'Z';
    uint32_t pe_offset = 0x80;
    *(uint32_t *)(header + 0x3C) = pe_offset;

    /* PE Signature */
    uint8_t *pe = header + pe_offset;
    pe[0] = 'P'; pe[1] = 'E'; pe[2] = 0; pe[3] = 0;

    /* COFF File Header (AMD64) */
    uint16_t *coff = (uint16_t *)(pe + 4);
    coff[0] = 0x8664; /* Machine: AMD64 */
    coff[1] = 2;      /* NumberOfSections: 2 (.text, .reloc) */
    *(uint32_t *)(pe + 8) = 0; /* TimeDateStamp */
    *(uint32_t *)(pe + 12) = 0; /* PointerToSymbolTable */
    *(uint32_t *)(pe + 16) = 0; /* NumberOfSymbols */
    *(uint16_t *)(pe + 20) = 0xF0; /* SizeOfOptionalHeader */
    *(uint16_t *)(pe + 22) = 0x0206; /* Characteristics: Executable, Large Address, Relocatable */

    /* Optional Header (PE32+ 64-bit) */
    uint8_t *opt = pe + 24;
    *(uint16_t *)(opt + 0) = 0x020B; /* PE32+ Magic */
    *(uint8_t  *)(opt + 2) = 0x02;   /* MajorLinkerVersion */
    *(uint8_t  *)(opt + 3) = 0x1E;   /* MinorLinkerVersion */
    uint32_t code_aligned_file = (uint32_t)((code_size + 511) & ~511);
    /* +4 КБ запаса: .bss (NOBITS) в плоском бинарнике отсутствует, но лежит сразу за ним */
    uint32_t code_aligned_mem  = (uint32_t)((code_size + 4096 + 4095) & ~4095);
    uint32_t reloc_rva         = 0x1000 + code_aligned_mem;
    *(uint32_t *)(opt + 4) = code_aligned_file; /* SizeOfCode */
    *(uint32_t *)(opt + 8) = 512;               /* SizeOfInitializedData */
    *(uint32_t *)(opt + 16) = 0x1000; /* AddressOfEntryPoint (RVA) */
    *(uint32_t *)(opt + 20) = 0x1000; /* BaseOfCode */
    *(uint64_t *)(opt + 24) = 0x400000ULL; /* ImageBase */
    *(uint32_t *)(opt + 32) = 0x1000; /* SectionAlignment */
    *(uint32_t *)(opt + 36) = 0x200;  /* FileAlignment */
    *(uint16_t *)(opt + 68) = 10;     /* Subsystem: EFI Application */
    *(uint32_t *)(opt + 56) = reloc_rva + 0x1000; /* SizeOfImage */
    *(uint32_t *)(opt + 60) = 0x200;  /* SizeOfHeaders */
    *(uint32_t *)(opt + 108) = 16;    /* NumberOfRvaAndSizes = 16 */

    /* Data Directory 5: Base Relocation Table */
    *(uint32_t *)(opt + 112 + 5 * 8)     = reloc_rva; /* RVA таблицы релокаций */
    *(uint32_t *)(opt + 112 + 5 * 8 + 4) = 10;        /* Размер: 10 байт */

    /* Section Header: .text */
    uint8_t *sec = opt + 0xF0;
    memcpy(sec, ".text\0\0\0", 8);
    *(uint32_t *)(sec + 8)  = code_aligned_mem; /* VirtualSize (с запасом под .bss) */
    *(uint32_t *)(sec + 12) = 0x1000; /* VirtualAddress */
    *(uint32_t *)(sec + 16) = code_aligned_file;   /* SizeOfRawData */
    *(uint32_t *)(sec + 20) = 0x200; /* PointerToRawData */
    *(uint32_t *)(sec + 36) = 0xE0000060; /* Code | InitData | Execute | Read | Write (.data/.bss пишутся) */

    /* Section Header: .reloc */
    uint8_t *sec_rel = sec + 40;
    memcpy(sec_rel, ".reloc\0\0", 8);
    *(uint32_t *)(sec_rel + 8)  = 10;
    *(uint32_t *)(sec_rel + 12) = reloc_rva;
    *(uint32_t *)(sec_rel + 16) = 512;
    *(uint32_t *)(sec_rel + 20) = 0x200 + code_aligned_file;
    *(uint32_t *)(sec_rel + 36) = 0x42000040; /* Initialized Data, Discardable, Read */

    fwrite(header, 1, sizeof(header), fout);
    fwrite(code, 1, code_size, fout);

    /* Выравнивание файла до 512 байт */
    long total_written = sizeof(header) + code_size;
    long padding = (512 - (total_written % 512)) % 512;
    uint8_t zero = 0;
    while (padding-- > 0) fwrite(&zero, 1, 1, fout);

    /* Запись валидного базового блока .reloc (10 байт + нули до 512) */
    uint8_t reloc_buf[512];
    memset(reloc_buf, 0, sizeof(reloc_buf));
    *(uint32_t *)(reloc_buf + 0) = 0x1000; /* Page RVA */
    *(uint32_t *)(reloc_buf + 4) = 10;     /* Block Size */
    *(uint16_t *)(reloc_buf + 8) = 0;      /* Type 0 (ABSOLUTE / No-op) */
    fwrite(reloc_buf, 1, sizeof(reloc_buf), fout);

    fclose(fout);
    free(code);
    printf("[MKEFI] Generated valid UEFI PE32+ application: %s (%ld bytes)\n", argv[2], code_size + 512);
    return 0;
}
