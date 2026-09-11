// Liste les enregistrements via NET_DVR_FindFile_V40 (paire correcte avec FindNextFile_V40).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "HCNetSDK.h"

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : getenv("HIK_HOST");
    WORD port = argc > 2 ? (WORD)atoi(argv[2]) : 8000;
    int chan = argc > 3 ? atoi(argv[3]) : 33;
    int days = argc > 4 ? atoi(argv[4]) : 2;

    if (!NET_DVR_Init()) { printf("Init fail\n"); return 1; }
    NET_DVR_USER_LOGIN_INFO li = {0};
    NET_DVR_DEVICEINFO_V40 dev = {0};
    strncpy(li.sDeviceAddress, host, NET_DVR_DEV_ADDRESS_MAX_LEN - 1);
    li.wPort = port;
    strncpy(li.sUserName, getenv("HIK_USER"), NET_DVR_LOGIN_USERNAME_MAX_LEN - 1);
    strncpy(li.sPassword, getenv("HIK_PASS"), NET_DVR_LOGIN_PASSWD_MAX_LEN - 1);
    LONG uid = NET_DVR_Login_V40(&li, &dev);
    if (uid < 0) { printf("login fail %u\n", NET_DVR_GetLastError()); return 1; }

    time_t now = time(nullptr), from = now - days * 86400;
    struct tm *t0 = localtime(&from), *t1 = localtime(&now);

    NET_DVR_FILECOND_V40 cond;
    memset(&cond, 0, sizeof cond);
    cond.lChannel = chan;
    cond.dwFileType = 0xff;   // tous types
    cond.struStartTime.dwYear  = t0->tm_year + 1900;
    cond.struStartTime.dwMonth = t0->tm_mon + 1;
    cond.struStartTime.dwDay   = t0->tm_mday;
    cond.struStartTime.dwHour  = t0->tm_hour;
    cond.struStartTime.dwMinute= t0->tm_min;
    cond.struStartTime.dwSecond= t0->tm_sec;
    cond.struStopTime.dwYear  = t1->tm_year + 1900;
    cond.struStopTime.dwMonth = t1->tm_mon + 1;
    cond.struStopTime.dwDay   = t1->tm_mday;
    cond.struStopTime.dwHour  = t1->tm_hour;
    cond.struStopTime.dwMinute= t1->tm_min;
    cond.struStopTime.dwSecond= t1->tm_sec;

    LONG h = NET_DVR_FindFile_V40(uid, &cond);
    if (h < 0) { printf("FindFile_V40 echec code=%u\n", NET_DVR_GetLastError()); return 1; }

    NET_DVR_FINDDATA_V40 fd;
    int count = 0;
    for (;;) {
        int r = NET_DVR_FindNextFile_V40(h, &fd);
        if (r == -1) break;             // fin de liste
        if (r == NET_DVR_ISFINDING) continue;  // encore en recherche
        printf("  %04d-%02d-%02d %02d:%02d:%02d -> %02d:%02d:%02d  taille=%u  type=%u  nom=%.100s\n",
               fd.struStartTime.dwYear, fd.struStartTime.dwMonth, fd.struStartTime.dwDay,
               fd.struStartTime.dwHour, fd.struStartTime.dwMinute, fd.struStartTime.dwSecond,
               fd.struStopTime.dwHour, fd.struStopTime.dwMinute, fd.struStopTime.dwSecond,
               fd.dwFileSize, fd.byFileType, fd.sFileName);
        if (++count >= 30) break;
    }
    printf("total: %d fichiers\n", count);
    NET_DVR_FindClose_V30(h);
    NET_DVR_Logout(uid);
    NET_DVR_Cleanup();
    return 0;
}
