// Vérifie la config d'enregistrement (dwRecord) du canal.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "HCNetSDK.h"

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : getenv("HIK_HOST");
    WORD port = argc > 2 ? (WORD)atoi(argv[2]) : 8000;
    int chan = argc > 3 ? atoi(argv[3]) : 33;

    if (!NET_DVR_Init()) { printf("Init fail\n"); return 1; }
    NET_DVR_USER_LOGIN_INFO li = {0};
    NET_DVR_DEVICEINFO_V40 dev = {0};
    strncpy(li.sDeviceAddress, host, NET_DVR_DEV_ADDRESS_MAX_LEN - 1);
    li.wPort = port;
    strncpy(li.sUserName, getenv("HIK_USER"), NET_DVR_LOGIN_USERNAME_MAX_LEN - 1);
    strncpy(li.sPassword, getenv("HIK_PASS"), NET_DVR_LOGIN_PASSWD_MAX_LEN - 1);
    LONG uid = NET_DVR_Login_V40(&li, &dev);
    if (uid < 0) { printf("login fail %u\n", NET_DVR_GetLastError()); return 1; }

    NET_DVR_RECORD_V40 rec;
    memset(&rec, 0, sizeof rec);
    rec.dwSize = sizeof rec;
    DWORD ret = 0;
    if (!NET_DVR_GetDVRConfig(uid, NET_DVR_GET_RECORDCFG_V40, chan, &rec, sizeof rec, &ret)) {
        printf("GetDVRConfig RECORDCFG echec code=%u\n", NET_DVR_GetLastError());
    } else {
        printf("canal %d: dwRecord=%u (%s)\n", chan, rec.dwRecord, rec.dwRecord ? "ENREGISTREMENT ACTIF" : "PAS d'enregistrement");
        printf("  streamType=%u (0=main 1=sub)  audio=%u  schedule 7j=%u\n",
               rec.byStreamType, rec.byAudioRec, rec.struRecAllDay[0].byRecordType);
    }
    NET_DVR_Logout(uid);
    NET_DVR_Cleanup();
    return 0;
}
