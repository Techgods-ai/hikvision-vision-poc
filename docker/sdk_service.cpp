// Service SDK Hikvision : maintient les sessions NVR et sert les snapshots en HTTP.
//   GET /api/nvrs            -> inventaire JSON (NVR, canaux)
//   GET /snapshot/<nvr>/<ch> -> JPEG du canal
//   GET /health              -> état des sessions
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include "HCNetSDK.h"

struct Nvr {
    std::string id, host, user, pass, model;
    WORD port = 8000;
    LONG uid = -1;
    int firstChan = 33, nChan = 0;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
};

static std::vector<Nvr> g_nvrs;

static bool nvrLogin(Nvr &n) {
    NET_DVR_USER_LOGIN_INFO li = {0};
    NET_DVR_DEVICEINFO_V40 dev = {0};
    strncpy(li.sDeviceAddress, n.host.c_str(), NET_DVR_DEV_ADDRESS_MAX_LEN - 1);
    strncpy(li.sUserName, n.user.c_str(), NET_DVR_LOGIN_USERNAME_MAX_LEN - 1);
    strncpy(li.sPassword, n.pass.c_str(), NET_DVR_LOGIN_PASSWD_MAX_LEN - 1);
    li.wPort = n.port;
    li.bUseAsynLogin = 0;

    n.uid = NET_DVR_Login_V40(&li, &dev);
    if (n.uid < 0) {
        printf("[%s] login echec code=%u\n", n.id.c_str(), NET_DVR_GetLastError());
        return false;
    }
    const NET_DVR_DEVICEINFO_V30 &d = dev.struDeviceV30;
    n.nChan = d.byIPChanNum + 256 * d.byHighDChanNum;
    n.firstChan = d.byStartDChan ? d.byStartDChan : 33;
    n.model.assign((const char*)d.sSerialNumber, 0, 16);
    printf("[%s] login OK uid=%ld  %d canaux depuis %d\n", n.id.c_str(), (long)n.uid, n.nChan, n.firstChan);
    return true;
}

// Capture un canal. Relogin transparent si la session a expiré.
static bool grab(Nvr &n, int chan, std::string &jpeg) {
    static const DWORD cap = 4 << 20;
    std::vector<char> buf(cap);
    DWORD got = 0;
    NET_DVR_JPEGPARA jp = {0xff, 0};  // 0xff = résolution native du flux

    pthread_mutex_lock(&n.lock);
    if (n.uid < 0) nvrLogin(n);
    bool ok = n.uid >= 0 && NET_DVR_CaptureJPEGPicture_NEW(n.uid, chan, &jp, buf.data(), cap, &got) && got;
    if (!ok && n.uid >= 0) {  // session morte : on rejoue une fois
        NET_DVR_Logout(n.uid);
        n.uid = -1;
        if (nvrLogin(n))
            ok = NET_DVR_CaptureJPEGPicture_NEW(n.uid, chan, &jp, buf.data(), cap, &got) && got;
    }
    pthread_mutex_unlock(&n.lock);

    if (ok) jpeg.assign(buf.data(), got);
    return ok;
}

static Nvr *findNvr(const std::string &id) {
    for (auto &n : g_nvrs) if (n.id == id) return &n;
    return nullptr;
}

static void sendAll(int fd, const char *p, size_t len) {
    while (len) { ssize_t w = send(fd, p, len, 0); if (w <= 0) return; p += w; len -= w; }
}

static void reply(int fd, const char *status, const char *type, const std::string &body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        status, type, body.size());
    sendAll(fd, hdr, n);
    sendAll(fd, body.data(), body.size());
}

static std::string jsonInventory() {
    std::string j = "{\"nvrs\":[";
    for (size_t i = 0; i < g_nvrs.size(); i++) {
        Nvr &n = g_nvrs[i];
        char b[512];
        snprintf(b, sizeof b,
            "%s{\"id\":\"%s\",\"port\":%d,\"model\":\"%s\",\"online\":%s,"
            "\"firstChannel\":%d,\"channels\":%d}",
            i ? "," : "", n.id.c_str(), n.port, n.model.c_str(),
            n.uid >= 0 ? "true" : "false", n.firstChan, n.nChan);
        j += b;
    }
    return j + "]}";
}

static void *serve(void *arg) {
    int fd = (int)(intptr_t)arg;
    char req[2048];
    ssize_t r = recv(fd, req, sizeof(req) - 1, 0);
    if (r <= 0) { close(fd); return nullptr; }
    req[r] = 0;

    char method[8] = {0}, path[512] = {0};
    sscanf(req, "%7s %511s", method, path);

    if (!strcmp(path, "/api/nvrs") || !strcmp(path, "/health")) {
        reply(fd, "200 OK", "application/json", jsonInventory());
    } else if (!strncmp(path, "/snapshot/", 10)) {
        char id[64] = {0}; int chan = 0;
        if (sscanf(path + 10, "%63[^/]/%d", id, &chan) == 2) {
            Nvr *n = findNvr(id);
            std::string jpeg;
            if (n && grab(*n, chan, jpeg))
                reply(fd, "200 OK", "image/jpeg", jpeg);
            else
                reply(fd, "502 Bad Gateway", "text/plain", "capture indisponible");
        } else {
            reply(fd, "400 Bad Request", "text/plain", "format: /snapshot/<nvr>/<canal>");
        }
    } else {
        reply(fd, "404 Not Found", "text/plain", "introuvable");
    }
    close(fd);
    return nullptr;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8090;
    const char *host = getenv("HIK_HOST");
    const char *user = getenv("HIK_USER");
    const char *pass = getenv("HIK_PASS");
    const char *ports = getenv("HIK_PORTS");
    if (!host || !user || !pass) {
        fprintf(stderr, "Requis : HIK_HOST, HIK_USER, HIK_PASS (option HIK_PORTS=8001,8000,7000)\n");
        return 1;
    }

    if (!NET_DVR_Init()) { fprintf(stderr, "NET_DVR_Init echec\n"); return 1; }
    NET_DVR_SetConnectTime(5000, 1);
    NET_DVR_SetReconnect(10000, TRUE);

    char list[256];
    snprintf(list, sizeof list, "%s", ports && *ports ? ports : "8001,8000,7000");
    for (char *tok = strtok(list, ","); tok; tok = strtok(nullptr, ",")) {
        Nvr n;
        n.host = host; n.user = user; n.pass = pass;
        n.port = (WORD)atoi(tok);
        n.id = "nvr" + std::string(tok);
        g_nvrs.push_back(n);
    }
    for (auto &n : g_nvrs) nvrLogin(n);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind(srv, (sockaddr*)&a, sizeof a) || listen(srv, 32)) {
        fprintf(stderr, "bind/listen echec port %d\n", port);
        return 1;
    }
    printf("Service SDK pret sur le port %d\n", port);
    fflush(stdout);

    for (;;) {
        int c = accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        pthread_t t;
        pthread_create(&t, nullptr, serve, (void*)(intptr_t)c);
        pthread_detach(t);
    }
}
