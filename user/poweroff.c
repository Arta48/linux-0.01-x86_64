/* poweroff / halt / reboot: сбросить буферы на диск и выключить или перезагрузить компьютер */
#include "ulibc.h"

int main(int argc, char **argv)
{
    const char *n = argc > 0 ? argv[0] : "poweroff";
    const char *base = n;
    for (const char *p = n; *p; p++) if (*p == '/') base = p + 1;
    sync();
    reboot(strcmp(base, "reboot") == 0 ? 1 : strcmp(base, "halt") == 0 ? 2 : 0);
    return 0;
}
