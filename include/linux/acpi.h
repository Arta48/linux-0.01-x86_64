#ifndef _LINUX_ACPI_H
#define _LINUX_ACPI_H

#include <linux/types.h>

/* Разбирает ACPI (RSDP от UEFI-загрузчика или поиск в BIOS-области при rsdp == 0):
 * FADT (порты PM1, регистр сброса) и _S5 из DSDT/SSDT. Вызывать один раз при загрузке. */
int  acpi_init(uint64_t rsdp);

/* Выключение / перезагрузка ПК; при неудаче возвращают управление не гарантированно */
void acpi_poweroff(void);
void acpi_reboot(void);

/* sys_reboot(): cmd 0 = выключить, 1 = перезагрузить, 2 = остановить CPU */
int64_t sys_reboot(int cmd);

/* Номер процесса корневого шелла: его завершение выключает компьютер */
extern int root_task_pid;

#endif
