#include "ulibc.h"

static void handle_client(int client_fd)
{
    char req[512];
    int n = read(client_fd, req, sizeof(req) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    req[n] = '\0';

    char first_line[64];
    int fli = 0;
    while (req[fli] && req[fli] != '\r' && req[fli] != '\n' && fli < 63) {
        first_line[fli] = req[fli];
        fli++;
    }
    first_line[fli] = '\0';
    printf("[HTTP] Request: \"%s\"\n", first_line);

    int epoch = (int)time();

    /* Тело HTML страницы (размер ~450 байт, помещается в один пакет) */
    static char body[1024];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Linux 0.01 x86_64</title>"
        "<style>body{background:#121212;color:#00ff66;font-family:monospace;padding:25px;}"
        "h1{color:#fff;border-bottom:2px solid #00ff66;padding-bottom:8px;}li{line-height:1.8;}</style></head>"
        "<body><h1>Linux 0.01 (x86_64 Edition)</h1>"
        "<p><b>LIVE HTTP SERVER SERVED FROM RING 3!</b></p>"
        "<ul><li><b>Architecture:</b> 64-bit Long Mode (AMD64)</li>"
        "<li><b>Multiprocessing:</b> 2 CPU Cores Active (SMP)</li>"
        "<li><b>Disk FS:</b> Persistent Minix v1 on /mnt</li>"
        "<li><b>Network:</b> Intel e1000 + ARP + IPv4 + TCP</li>"
        "<li><b>System Time:</b> %d</li></ul>"
        "<hr style=\"border-color:#333;\"><p><i>Have fun hacking kernels!</i></p></body></html>\n", epoch);

    static char resp[1500];
    int resp_len = snprintf(resp, sizeof(resp),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", body_len, body);

    write(client_fd, resp, resp_len);
    close(client_fd);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("====================================================\n");
    printf("   User Space HTTP Web Server (/bin/httpd)          \n");
    printf("====================================================\n\n");

    int server_fd = socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (server_fd < 0) {
        printf("[FAIL] Failed to create socket\n");
        return 1;
    }

    if (bind(server_fd, 80) < 0) {
        printf("[FAIL] Failed to bind to port 80\n");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        printf("[FAIL] Failed to listen on socket\n");
        close(server_fd);
        return 1;
    }

    printf("[OK] HTTP Server listening on port 80...\n");
    printf("[INFO] From your Arch Linux host, open:\n");
    printf("       -> http://localhost:8080 (SLIRP) or http://10.0.2.15 (TAP)\n\n");

    while (1) {
        int client_fd = accept(server_fd);
        if (client_fd >= 0) {
            printf("[HTTP] Handling incoming HTTP request (Client FD: %d)...\n", client_fd);
            handle_client(client_fd);
        }
    }

    close(server_fd);
    return 0;
}
