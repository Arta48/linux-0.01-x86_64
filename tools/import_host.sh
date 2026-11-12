#!/bin/sh
# Копирует обычные (динамические, glibc) программы хоста и все их библиотеки в rootfs.
# Использование: tools/import_host.sh <rootfs> <имя-или-путь> ...
# Ядро запускает такие программы как есть: оно загружает ld-linux из PT_INTERP,
# а пути /usr/bin, /usr/lib, /lib64 отображаются на /bin и /lib (см. fs/ramfs.c).
ROOT=${1:?rootfs}; shift
mkdir -p "$ROOT/bin" "$ROOT/lib"

# Путь к исполняемому файлу: `command -v` отдаёт для echo, test, kill и т.п. встроенные команды shell,
# поэтому ищем настоящий файл в PATH.
find_app() {
    case "$1" in
        */*) [ -x "$1" ] && echo "$1" && return 0; return 1 ;;
    esac
    oldifs=$IFS; IFS=:
    for d in $PATH; do
        if [ -f "$d/$1" ] && [ -x "$d/$1" ]; then IFS=$oldifs; echo "$d/$1"; return 0; fi
    done
    IFS=$oldifs
    return 1
}

# pkg:ИМЯ -- все исполняемые файлы пакета (/usr/bin, /usr/sbin, /bin, /sbin) по списку файлов менеджера пакетов
# хоста: pacman (Arch), dpkg (Debian/Ubuntu) или rpm (Fedora). Пакеты на хосте без менеджера пропускаются.
pkg_files() {
    if command -v pacman >/dev/null 2>&1; then pacman -Qlq "$1" 2>/dev/null
    elif command -v dpkg >/dev/null 2>&1; then dpkg -L "$1" 2>/dev/null
    elif command -v rpm >/dev/null 2>&1; then rpm -ql "$1" 2>/dev/null
    else return 1; fi
}
expand_args() {
    for a in "$@"; do
        case "$a" in
            pkg:*)
                n=${a#pkg:}
                if ! pkg_files "$n" > /tmp/import_host.$$ ; then
                    echo "import_host: нет pacman/dpkg/rpm, пакет $n пропущен" >&2
                elif [ ! -s /tmp/import_host.$$ ]; then
                    echo "import_host: пакет $n не установлен на хосте" >&2
                else
                    grep -E '^/(usr/)?s?bin/[^/]+$' /tmp/import_host.$$ | while read -r f; do [ -f "$f" ] && echo "$f"; done
                fi
                rm -f /tmp/import_host.$$ ;;
            *) echo "$a" ;;
        esac
    done
}
APPS=$(expand_args "$@")

for app in $APPS; do
    src=$(find_app "$app") || { echo "import_host: $app не найден на хосте, пропускаю"; continue; }
    src=$(readlink -f "$src")
    cp -f "$src" "$ROOT/bin/$(basename "$app")"
    ldd "$src" 2>/dev/null | while read -r name arrow path rest; do
        case "$name" in
            linux-vdso*|statically|not) continue ;;
        esac
        base=$(basename "$name")
        if [ "$arrow" = "=>" ]; then
            [ -f "$path" ] && cp -fL "$path" "$ROOT/lib/$base"
        elif [ -f "$name" ]; then
            cp -fL "$name" "$ROOT/lib/$base"
        fi
    done
    echo "import_host: $app -> /bin/$(basename "$app")"
done

# Скрипты (gunzip, zcat, egrep...) начинаются с #!/bin/sh: настоящий bash, вызванный как sh.
[ -f "$ROOT/bin/bash" ] && [ ! -e "$ROOT/bin/sh" ] && cp -f "$ROOT/bin/bash" "$ROOT/bin/sh"

# База terminfo для ncurses-программ (nano, less...): TERM=linux берётся с хоста.
for d in /usr/share/terminfo /lib/terminfo /etc/terminfo; do
    for t in linux xterm xterm-256color vt100 ansi; do
        f=$(ls "$d"/?/$t "$d"/??/$t 2>/dev/null | head -1)
        [ -n "$f" ] && mkdir -p "$ROOT/lib/terminfo/$(basename "$(dirname "$f")")" && cp -fL "$f" "$ROOT/lib/terminfo/$(basename "$(dirname "$f")")/$t"
    done
done
# Локаль C.UTF-8 (bash/readline показывают кириллицу, а не \320\277...; init задаёт LANG=C.UTF-8).
for d in /usr/lib/locale/C.utf8 /usr/lib/locale/C.UTF-8 /lib/locale/C.utf8; do
    if [ -d "$d" ]; then
        mkdir -p "$ROOT/lib/locale"
        rm -rf "$ROOT/lib/locale/C.utf8"; cp -rL "$d" "$ROOT/lib/locale/C.utf8"
        break
    fi
done

# Корневые сертификаты для https (curl, wget): куда указывает путь по умолчанию в curl.
for f in /etc/ssl/certs/ca-certificates.crt /etc/ca-certificates/extracted/tls-ca-bundle.pem /etc/pki/tls/certs/ca-bundle.crt; do
    if [ -f "$f" ]; then
        mkdir -p "$ROOT/etc/ssl/certs"
        cp -fL "$f" "$ROOT/etc/ssl/certs/ca-certificates.crt"
        break
    fi
done
# База имён устройств для lspci.
for f in /usr/share/hwdata/pci.ids /usr/share/misc/pci.ids /usr/share/pci.ids; do
    if [ -f "$f" ]; then
        mkdir -p "$ROOT/usr/share/hwdata"
        cp -fL "$f" "$ROOT/usr/share/hwdata/pci.ids"
        break
    fi
done
exit 0
