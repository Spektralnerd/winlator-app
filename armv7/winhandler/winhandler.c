/*
 * winhandler.exe für das 32-Bit-Wine (Win32-Präfix) von Winlator armv7.
 *
 * Nachbau des 64-Bit-winhandler.exe aus Winlator 11.x (dessen Quelltext nicht veröffentlicht
 * ist), Protokoll wie in com.winlator.winhandler.WinHandler: UDP über 127.0.0.1,
 * Winlator (Java) lauscht auf Port 7947, winhandler.exe auf Port 7946.
 *
 * Aufruf:  winhandler.exe [/dir <Ordner>] [<Programm> [Argumente ...]]
 *          winhandler.exe /url <URL>
 * Ohne Programm wird wfm.exe gestartet. winhandler.exe beendet sich, sobald nach dem
 * gestarteten Programm kein neuerer Prozess mehr läuft.
 *
 * Bauen: i686-w64-mingw32-gcc -O2 -mwindows -static-libgcc -o winhandler.exe winhandler.c -lws2_32
 */
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SERVER_PORT 7947
#define CLIENT_PORT 7946
#define PACKET_SIZE 64
#define RECV_SIZE 256

enum {
    REQUEST_EXIT = 0,
    REQUEST_INIT = 1,
    REQUEST_EXEC = 2,
    REQUEST_KILL_PROCESS = 3,
    REQUEST_LIST_PROCESSES = 4,
    REQUEST_GET_PROCESS = 5,
    REQUEST_SET_PROCESS_AFFINITY = 6,
    REQUEST_MOUSE_EVENT = 7,
    REQUEST_KEYBOARD_EVENT = 11,
    REQUEST_BRING_TO_FRONT = 12,
    REQUEST_CURSOR_POS_FEEDBACK = 13,
    REQUEST_SET_CLIPBOARD_DATA = 14,
    REQUEST_OPEN_URL = 15,
    REQUEST_SHOW_DESKTOP = 18,
    REQUEST_SHOW_WINDOW = 20,
    REQUEST_GET_EXECUTABLE_PATH = 21
};

static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in serverAddr;

static int32_t getInt(const unsigned char* p) { int32_t v; memcpy(&v, p, 4); return v; }
static int16_t getShort(const unsigned char* p) { int16_t v; memcpy(&v, p, 2); return v; }
static int64_t getLong(const unsigned char* p) { int64_t v; memcpy(&v, p, 8); return v; }
static void putInt(unsigned char* p, int32_t v) { memcpy(p, &v, 4); }
static void putShort(unsigned char* p, int16_t v) { memcpy(p, &v, 2); }

static void closeSocket(void) {
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
}

static BOOL initSocket(void) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return FALSE;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CLIENT_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    closeSocket();
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) goto error;

    BOOL reuse = TRUE;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) == SOCKET_ERROR) goto error;
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) goto error;

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    return TRUE;

error:
    closeSocket();
    WSACleanup();
    return FALSE;
}

static int sendData(const void* data, int size) {
    return sendto(sock, (const char*)data, size, 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
}

/* Kopie von <len> Bytes ab <src> als nullterminierte Zeichenkette (auf dem Heap) */
static char* copyString(const unsigned char* src, int len, const unsigned char* end) {
    if (len < 0) len = 0;
    if (src + len > end) len = (int)(end - src) > 0 ? (int)(end - src) : 0;
    char* str = malloc(len + 1);
    if (len > 0) memcpy(str, src, len);
    str[len] = '\0';
    return str;
}

static DWORD findProcessId(const char* name) {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        }
        while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

/* Gibt es noch einen Prozess, der nach winhandler.exe gestartet wurde? */
static BOOL hasNewerProcess(void) {
    BOOL found = FALSE;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return TRUE;
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        DWORD currentPid = GetCurrentProcessId();
        do {
            if (entry.th32ProcessID > currentPid) {
                found = TRUE;
                break;
            }
        }
        while (Process32Next(snapshot, &entry));
    }
    else found = TRUE;
    CloseHandle(snapshot);
    return found;
}

static void handleExec(const unsigned char* data, const unsigned char* end) {
    int filenameLength = getInt(data);
    char* filename = copyString(data + 4, filenameLength, end);
    const unsigned char* p = data + 4 + (filenameLength > 0 ? filenameLength : 0);
    char* parameters = p + 4 <= end ? copyString(p + 4, getInt(p), end) : copyString(p, 0, end);
    ShellExecuteA(NULL, "open", filename, parameters, NULL, SW_SHOWNORMAL);
    free(filename);
    free(parameters);
}

static void handleKillProcess(const unsigned char* data, const unsigned char* end) {
    int nameLength = getInt(data);
    if (nameLength < 0 || data + 4 + nameLength + 4 > end) return;
    DWORD pid = (DWORD)getInt(data + 4 + nameLength);
    if (pid == 0 && nameLength > 0) {
        char* name = copyString(data + 4, nameLength, end);
        pid = findProcessId(name);
        free(name);
    }
    if (pid == 0) return;
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process) {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
}

#pragma pack(push, 1)
typedef struct {
    int32_t pid;
    int64_t memoryUsage;
    int32_t affinityMask;
    uint8_t wow64Process;
    char name[32];
} ProcessRecord;
#pragma pack(pop)

static void handleListProcesses(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    ProcessRecord* records = NULL;
    int count = 0;
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
            if (!process) continue;

            PROCESS_MEMORY_COUNTERS memoryCounters = {0};
            memoryCounters.cb = sizeof(memoryCounters);
            GetProcessMemoryInfo(process, &memoryCounters, sizeof(memoryCounters));
            DWORD_PTR processMask = 0, systemMask = 0;
            GetProcessAffinityMask(process, &processMask, &systemMask);
            BOOL wow64 = FALSE;
            IsWow64Process(process, &wow64);
            CloseHandle(process);

            records = realloc(records, (count + 1) * sizeof(ProcessRecord));
            ProcessRecord* record = &records[count++];
            memset(record, 0, sizeof(ProcessRecord));
            record->pid = (int32_t)entry.th32ProcessID;
            record->memoryUsage = (int64_t)memoryCounters.WorkingSetSize;
            record->affinityMask = (int32_t)processMask;
            record->wow64Process = wow64 ? 1 : 0;
            memcpy(record->name, entry.szExeFile, sizeof(record->name));
        }
        while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);

    unsigned char packet[PACKET_SIZE];
    for (int i = 0; i < count; i++) {
        memset(packet, 0, sizeof(packet));
        packet[0] = REQUEST_GET_PROCESS;
        putInt(packet + 1, PACKET_SIZE);
        putShort(packet + 5, (int16_t)count);
        putShort(packet + 7, (int16_t)i);
        memcpy(packet + 9, &records[i], sizeof(ProcessRecord));
        if (sendData(packet, PACKET_SIZE) == SOCKET_ERROR) break;
    }
    free(records);
}

static void handleSetProcessAffinity(const unsigned char* data, const unsigned char* end) {
    if (data + 13 > end) return;
    DWORD pid = (DWORD)getInt(data + 4);
    DWORD_PTR affinityMask = (DWORD_PTR)(uint32_t)getInt(data + 8);
    int nameLength = (signed char)data[12];
    if (pid == 0 && nameLength > 0) {
        char* name = copyString(data + 13, nameLength, end);
        pid = findProcessId(name);
        free(name);
    }
    if (pid == 0) return;
    HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process) {
        SetProcessAffinityMask(process, affinityMask);
        CloseHandle(process);
    }
}

static void handleMouseEvent(const unsigned char* data, const unsigned char* end) {
    if (data + 15 > end) return;
    DWORD flags = (DWORD)getInt(data + 4);
    int dx = getShort(data + 8);
    int dy = getShort(data + 10);
    int wheelDelta = getShort(data + 12);
    BOOL cursorPosFeedback = data[14] == 1;

    mouse_event(flags, dx, dy, (DWORD)wheelDelta, 0);

    if (cursorPosFeedback) {
        POINT point;
        if (GetCursorPos(&point)) {
            unsigned char packet[PACKET_SIZE] = {0};
            packet[0] = REQUEST_CURSOR_POS_FEEDBACK;
            putShort(packet + 1, (int16_t)point.x);
            putShort(packet + 3, (int16_t)point.y);
            sendData(packet, PACKET_SIZE);
        }
    }
}

static void handleKeyboardEvent(const unsigned char* data, const unsigned char* end) {
    if (data + 5 > end) return;
    keybd_event(data[0], 0, (DWORD)getInt(data + 1), 0);
}

static void activateWindow(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SwitchToThisWindow(window, TRUE);
}

static void handleBringToFront(const unsigned char* data, const unsigned char* end) {
    int nameLength = getInt(data);
    if (nameLength < 0 || data + 4 + nameLength + 8 > end) return;
    int64_t handle = getLong(data + 4 + nameLength);
    if (handle > 0) {
        activateWindow((HWND)(intptr_t)handle);
        return;
    }

    char* name = copyString(data + 4, nameLength, end);
    DWORD pid = findProcessId(name);
    free(name);
    if (pid == 0) return;

    HWND window = NULL;
    while ((window = FindWindowExA(NULL, window, NULL, NULL)) != NULL) {
        DWORD windowPid = 0;
        if (!GetWindowThreadProcessId(window, &windowPid) || windowPid != pid) continue;
        if (GetWindowTextLengthA(window) == 0 || !IsWindowVisible(window)) continue;
        if (GetParent(window) != NULL || GetWindow(window, GW_OWNER) != NULL) continue;
        if (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        activateWindow(window);
        break;
    }
}

static void handleShowDesktop(void) {
    HWND desktop = GetDesktopWindow();
    HWND window = NULL;
    char className[256];
    while ((window = FindWindowExA(NULL, window, NULL, NULL)) != NULL) {
        if (!IsWindowVisible(window) || window == desktop) continue;
        GetClassNameA(window, className, sizeof(className));
        if (strcmp(className, "Shell_TrayWnd") == 0) continue;
        ShowWindow(window, SW_MINIMIZE);
    }
}

static void handleShowWindow(const unsigned char* data, const unsigned char* end) {
    if (data + 12 > end) return;
    int64_t handle = getLong(data);
    if (handle != 0) ShowWindow((HWND)(intptr_t)handle, getInt(data + 8));
}

static void handleSetClipboardData(const unsigned char* data, const unsigned char* end) {
    int length = getInt(data);
    char* text = copyString(data + 4, length, end);
    size_t size = strlen(text) + 1;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory) {
        memcpy(GlobalLock(memory), text, size);
        GlobalUnlock(memory);
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_TEXT, memory)) GlobalFree(memory);
            CloseClipboard();
        }
        else GlobalFree(memory);
    }
    free(text);
}

static void handleGetExecutablePath(const unsigned char* data, const unsigned char* end) {
    if (data + 4 > end) return;
    DWORD pid = (DWORD)getInt(data);
    WCHAR path[MAX_PATH + 1] = {0};
    int length = 0;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) length = (int)wcslen(path) * (int)sizeof(WCHAR);
        CloseHandle(process);
    }

    unsigned char packet[PACKET_SIZE] = {0};
    packet[0] = REQUEST_GET_EXECUTABLE_PATH;
    putInt(packet + 1, length);
    if (sendData(packet, PACKET_SIZE) != SOCKET_ERROR && length > 0) sendData(path, length);
}

static void openUrl(const char* url) {
    unsigned char packet[PACKET_SIZE] = {0};
    int length = (int)strlen(url);
    packet[0] = REQUEST_OPEN_URL;
    putInt(packet + 1, length);
    if (sendData(packet, PACKET_SIZE) > 0) sendData(url, length);
}

/* Argumente ab Index <first> wieder zu einer Kommandozeile zusammensetzen.
 * Anders als das Original werden Argumente mit Leerzeichen dabei wieder in Anführungszeichen gesetzt. */
static char* joinArguments(int argc, char** argv, int first) {
    size_t size = 1;
    for (int i = first; i < argc; i++) size += strlen(argv[i]) + 3;
    char* result = malloc(size);
    result[0] = '\0';
    for (int i = first; i < argc; i++) {
        if (i > first) strcat(result, " ");
        BOOL quote = strchr(argv[i], ' ') != NULL || argv[i][0] == '\0';
        if (quote) strcat(result, "\"");
        strcat(result, argv[i]);
        if (quote) strcat(result, "\"");
    }
    return result;
}

int main(int argc, char** argv) {
    const char* directory = NULL;
    const char* filename = NULL;
    char* parameters = NULL;
    BOOL urlMode = FALSE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "/dir") == 0) {
            if (i + 1 < argc) directory = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "/url") == 0) {
            urlMode = TRUE;
            continue;
        }
        filename = argv[i];
        if (!urlMode && i + 1 < argc) parameters = joinArguments(argc, argv, i + 1);
        break;
    }
    if (!filename) filename = "wfm.exe";

    if (urlMode) {
        if (initSocket()) {
            openUrl(filename);
            closeSocket();
            WSACleanup();
        }
        return 0;
    }

    SHELLEXECUTEINFOA info = {0};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpFile = filename;
    info.lpParameters = parameters;
    info.lpDirectory = directory;
    info.nShow = SW_SHOW;
    ShellExecuteExA(&info);
    if (info.hProcess) CloseHandle(info.hProcess);

    if (!initSocket()) return 0;

    unsigned char buffer[RECV_SIZE];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = REQUEST_INIT;
    while (sendData(buffer, PACKET_SIZE) == SOCKET_ERROR) Sleep(16);

    DWORD lastCheck = GetTickCount();
    int idleChecks = 0;

    for (;;) {
        int received = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, NULL, NULL);
        if (received > 0) {
            const unsigned char* data = buffer + 1;
            const unsigned char* end = buffer + received;
            switch (buffer[0]) {
                case REQUEST_EXIT:
                    closeSocket();
                    WSACleanup();
                    ExitProcess(0);
                    break;
                case REQUEST_EXEC: handleExec(data, end); break;
                case REQUEST_KILL_PROCESS: handleKillProcess(data, end); break;
                case REQUEST_LIST_PROCESSES: handleListProcesses(); break;
                case REQUEST_SET_PROCESS_AFFINITY: handleSetProcessAffinity(data, end); break;
                case REQUEST_MOUSE_EVENT: handleMouseEvent(data, end); break;
                case REQUEST_KEYBOARD_EVENT: handleKeyboardEvent(data, end); break;
                case REQUEST_BRING_TO_FRONT: handleBringToFront(data, end); break;
                case REQUEST_SET_CLIPBOARD_DATA: handleSetClipboardData(data, end); break;
                case REQUEST_SHOW_DESKTOP: handleShowDesktop(); break;
                case REQUEST_SHOW_WINDOW: handleShowWindow(data, end); break;
                case REQUEST_GET_EXECUTABLE_PATH: handleGetExecutablePath(data, end); break;
            }
        }
        else if (WSAGetLastError() == WSAEWOULDBLOCK) Sleep(16);
        else break;

        DWORD now = GetTickCount();
        if (now - lastCheck >= 1000) {
            lastCheck = now;
            if (hasNewerProcess()) idleChecks = 1;
            else if (idleChecks++ >= 2) break;
        }
    }

    closeSocket();
    WSACleanup();
    return 0;
}
