// Service SDK Hikvision : maintient les sessions NVR et sert les snapshots en HTTP.
//   GET /api/nvrs            -> inventaire JSON (NVR, canaux)
//   GET /snapshot/<nvr>/<ch> -> JPEG du canal
//   GET /health              -> état des sessions
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <map>
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

// Parse "YYYYMMDDHHMMSS" en NET_DVR_TIME. Retourne false si invalide.
static bool parseTime(const std::string &s, NET_DVR_TIME &t) {
    if (s.size() != 14) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    t.dwYear   = atoi(s.substr(0, 4).c_str());
    t.dwMonth  = atoi(s.substr(4, 2).c_str());
    t.dwDay    = atoi(s.substr(6, 2).c_str());
    t.dwHour   = atoi(s.substr(8, 2).c_str());
    t.dwMinute = atoi(s.substr(10, 2).c_str());
    t.dwSecond = atoi(s.substr(12, 2).c_str());
    return t.dwYear >= 2020 && t.dwMonth >= 1 && t.dwMonth <= 12 &&
           t.dwDay >= 1 && t.dwDay <= 31 && t.dwHour <= 23 &&
           t.dwMinute <= 59 && t.dwSecond <= 59;
}

// Extrait un paramètre de query string. Retourne false si absent.
static bool queryParam(const char *req, const char *key, std::string &out) {
    const char *q = strchr(req, '?');
    if (!q) return false;
    q++;
    char needle[64];
    snprintf(needle, sizeof needle, "%s=", key);
    const char *p = strstr(q, needle);
    if (!p) return false;
    p += strlen(needle);
    out.clear();
    while (*p && *p != '&' && *p != ' ' && *p != '\r' && *p != '\n') out += *p++;
    return !out.empty();
}

// Exporte un segment vidéo [start, end) vers /clips/<nvr>_<canal>_<start>-<end>.mp4
static bool exportClip(Nvr &n, int chan, const std::string &start, const std::string &end,
                       std::string &savedPath) {
    NET_DVR_TIME t0, t1;
    if (!parseTime(start, t0) || !parseTime(end, t1)) return false;

    char file[256];
    snprintf(file, sizeof file, "/clips/%s_ch%d_%s_%s.mp4",
             n.id.c_str(), chan, start.c_str(), end.c_str());

    NET_DVR_PLAYCOND cond;
    memset(&cond, 0, sizeof cond);
    cond.dwChannel = chan;
    cond.struStartTime = t0;
    cond.struStopTime = t1;
    cond.byStreamType = 0;   // flux principal
    cond.byDownload = 1;     // mode téléchargement

    pthread_mutex_lock(&n.lock);
    if (n.uid < 0) nvrLogin(n);
    LONG h = n.uid >= 0 ? NET_DVR_GetFileByTime_V40(n.uid, file, &cond) : -1;
    pthread_mutex_unlock(&n.lock);

    if (h < 0) {
        printf("[%s] export clip ch%d echec code=%u\n", n.id.c_str(), chan, NET_DVR_GetLastError());
        fflush(stdout);
        return false;
    }
    // Démarrer le téléchargement (indispensable, comme dans le demo officiel)
    NET_DVR_PlayBackControl(h, NET_DVR_PLAYSTART, 0, nullptr);
    for (int i = 0; i < 600; i++) {
        usleep(500000);
        LONG pos = NET_DVR_GetDownloadPos(h);
        if (pos == 100 || pos < 0) break;
    }
    NET_DVR_StopGetFile(h);
    savedPath = file;
    printf("[%s] clip exporté ch%d : %s\n", n.id.c_str(), chan, file);
    fflush(stdout);
    return true;
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

// ---------- Live (flux continu) ----------
// Le callback standard fournit un flux H.264 brut. On le pipe dans ffmpeg
// (sous-processus) qui le décode et le ré-encode en MJPEG, puis on encapsule
// chaque image JPEG en multipart/x-mixed-replace pour le navigateur.

static std::map<LONG, int> g_live_pipes;         // realPlayHandle -> fd d'écriture ffmpeg
static std::map<LONG, int> g_live_started;       // realPlayHandle -> en-tête IMKH déjà retiré
static pthread_mutex_t g_live_mutex = PTHREAD_MUTEX_INITIALIZER;

// Le flux privé Hikvision commence par un en-tête propriétaire "IMKH" suivi du
// MPEG-PS standard (start code 00 00 01 ba). On retire l'en-tête une fois, puis
// on laisse passer le flux PS brut vers ffmpeg.
static void CALLBACK stdDataCallback(LONG h, DWORD, BYTE *buf, DWORD size, DWORD) {
    pthread_mutex_lock(&g_live_mutex);
    auto it = g_live_pipes.find(h);
    int fd = it != g_live_pipes.end() ? it->second : -1;
    int &started = g_live_started[h];
    pthread_mutex_unlock(&g_live_mutex);
    if (fd < 0) return;

    if (!started) {
        // cherche le start code PS (00 00 01 ba) pour sauter l'en-tête IMKH
        for (DWORD i = 0; i + 4 <= size; i++) {
            if (buf[i] == 0x00 && buf[i+1] == 0x00 && buf[i+2] == 0x01 &&
                (buf[i+3] == 0xba || buf[i+3] == 0xbb)) {
                buf += i; size -= i;
                started = 1;
                break;
            }
        }
        if (!started) return;  // pas encore trouvé, on attend la suite
    }
    write(fd, buf, size);
}

static void serveLive(int cfd, Nvr &n, int chan) {
    NET_DVR_PREVIEWINFO pi;
    memset(&pi, 0, sizeof pi);
    pi.lChannel = chan;
    pi.dwStreamType = 1;     // sous-flux (plus léger)
    pi.dwLinkMode = 0;       // TCP
    pi.hPlayWnd = 0;
    pi.bBlocked = 0;         // non bloquant
    pi.byProtoType = 0;      // protocole privé (passe par le port SDK, pas RTSP)
    pi.byDataType = 0;

    pthread_mutex_lock(&n.lock);
    if (n.uid < 0) nvrLogin(n);
    LONG h = n.uid >= 0 ? NET_DVR_RealPlay_V40(n.uid, &pi, nullptr, nullptr) : -1;
    pthread_mutex_unlock(&n.lock);
    if (h < 0) {
        reply(cfd, "502 Bad Gateway", "text/plain", "flux live indisponible (code SDK)");
        close(cfd);
        return;
    }
    printf("[%s] live ch%d demarre handle=%ld\n", n.id.c_str(), chan, (long)h);
    fflush(stdout);

    int toff[2], fromff[2];
    if (pipe(toff) || pipe(fromff)) { NET_DVR_StopRealPlay(h); reply(cfd, "500", "text/plain", "pipe"); close(cfd); return; }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(toff[0], 0); close(toff[0]); close(toff[1]);
        dup2(fromff[1], 1); close(fromff[0]); close(fromff[1]);
        // Le flux privé Hikvision est un MPEG-PS (program stream) : ffmpeg le décode.
        execlp("ffmpeg", "ffmpeg", "-loglevel", "error",
               "-f", "mpeg", "-i", "pipe:0", "-an",
               "-c:v", "mjpeg", "-q:v", "8",
               "-f", "image2pipe", "pipe:1", (char*)nullptr);
        _exit(127);
    }
    close(toff[0]); close(fromff[1]);

    pthread_mutex_lock(&g_live_mutex);
    g_live_pipes[h] = toff[1];
    pthread_mutex_unlock(&g_live_mutex);
    NET_DVR_SetRealDataCallBack(h, stdDataCallback, 0);

    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                      "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    sendAll(cfd, hdr, strlen(hdr));

    std::string acc;
    char buf[65536];
    for (;;) {
        ssize_t r = read(fromff[0], buf, sizeof buf);
        if (r <= 0) break;
        acc.append(buf, r);
        size_t s;
        while ((s = acc.find("\xff\xd8")) != std::string::npos) {
            size_t e = acc.find("\xff\xd9", s + 2);
            if (e == std::string::npos) break;
            std::string jpeg = acc.substr(s, e + 2 - s);
            acc.erase(0, e + 2);
            char len[32];
            int ln = snprintf(len, sizeof len, "%zu", jpeg.size());
            std::string frame = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ";
            frame += len; frame += "\r\n\r\n"; frame += jpeg; frame += "\r\n";
            if (send(cfd, frame.data(), frame.size(), 0) <= 0) { r = 0; break; }
        }
        if (r <= 0) break;
    }

    pthread_mutex_lock(&g_live_mutex);
    g_live_pipes.erase(h);
    g_live_started.erase(h);
    pthread_mutex_unlock(&g_live_mutex);
    NET_DVR_StopRealPlay(h);
    close(toff[1]); close(fromff[0]);
    kill(pid, SIGKILL); waitpid(pid, nullptr, 0);
    close(cfd);
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
    } else if (!strncmp(path, "/live/", 6)) {
        char id[64] = {0}; int chan = 0;
        if (sscanf(path + 6, "%63[^/]/%d", id, &chan) == 2) {
            Nvr *n = findNvr(id);
            if (n) serveLive(fd, *n, chan);   // serveLive ferme le socket lui-même
            else { reply(fd, "404 Not Found", "text/plain", "nvr inconnu"); close(fd); }
            return nullptr;
        } else {
            reply(fd, "400 Bad Request", "text/plain", "format: /live/<nvr>/<canal>");
        }
    } else if (!strncmp(path, "/clip/", 6)) {
        char id[64] = {0}; int chan = 0;
        if (sscanf(path + 6, "%63[^/]/%d", id, &chan) == 2) {
            std::string start, end;
            if (!queryParam(req, "start", start) || !queryParam(req, "end", end)) {
                reply(fd, "400 Bad Request", "text/plain",
                      "requis: ?start=YYYYMMDDHHMMSS&end=YYYYMMDDHHMMSS");
            } else {
                Nvr *n = findNvr(id);
                std::string saved;
                if (n && exportClip(*n, chan, start, end, saved))
                    reply(fd, "200 OK", "application/json",
                          "{\"status\":\"ok\",\"file\":\"" + saved + "\"}");
                else
                    reply(fd, "502 Bad Gateway", "text/plain", "export impossible (plage vide ou NVR indisponible)");
            }
        } else {
            reply(fd, "400 Bad Request", "text/plain", "format: /clip/<nvr>/<canal>?start=..&end=..");
        }
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
