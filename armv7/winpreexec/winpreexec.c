/*
 * winpreexec für Winlator armv7 (nativ armhf).
 *
 * Nachbau des x86_64-Programms /usr/local/bin/winpreexec aus Winlator 11.x: Wine ruft es
 * (Umgebungsvariable WINPREEXEC) vor dem Start eines Programms mit dessen Pfad auf. Es fragt
 * Winlator (WinHandler, UDP 127.0.0.1:7947, Anfrage PRE_EXEC = 22), ob der Start abgebrochen
 * werden soll.
 *
 * Rückgabewert: 0 = starten, 2 = nicht starten, 1 = Fehler
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLIENT_PORT 7952
#define SERVER_PORT 7947
#define REQUEST_PRE_EXEC 22

int main(int argc, char** argv) {
    if (argc < 2) return 1;

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return 1;

    int reuse = 1;
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(CLIENT_PORT);
    local.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        close(fd);
        return 1;
    }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_PORT);
    server.sin_addr.s_addr = inet_addr("127.0.0.1");

    const char* path = argv[1];
    int32_t length = (int32_t)strlen(path);
    unsigned char packet[64] = {0};
    packet[0] = REQUEST_PRE_EXEC;
    memcpy(packet + 1, &length, 4);

    if (sendto(fd, packet, sizeof(packet), 0, (struct sockaddr*)&server, sizeof(server)) < 0 ||
        (length > 0 && sendto(fd, path, length, 0, (struct sockaddr*)&server, sizeof(server)) < 0)) {
        close(fd);
        return 1;
    }

    unsigned char shouldExit = 0;
    if (recvfrom(fd, &shouldExit, 1, 0, NULL, NULL) < 0) {
        close(fd);
        return 1;
    }

    close(fd);
    return shouldExit ? 2 : 0;
}
