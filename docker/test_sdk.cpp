#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "HCNetSDK.h"

int main(int argc, char **argv) {
    const char *ip = argc > 1 ? argv[1] : "bstf-rtr-gw.gbm10.com";
    WORD port = argc > 2 ? (WORD)atoi(argv[2]) : 8000;
    const char *user = argc > 3 ? argv[3] : "admin";
    const char *pass = argc > 4 ? argv[4] : "shaker2360";

    if (!NET_DVR_Init()) { printf("Init fail\n"); return 1; }
    NET_DVR_SetLogToFile(3, (char*)"/tmp/sdklog", FALSE);
    NET_DVR_SetConnectTime(10000, 1);

    DWORD v = NET_DVR_GetSDKBuildVersion();
    printf("SDK build %u.%u.%u.%u  ->  %s:%d\n", v>>24, (v>>16)&0xff, (v>>8)&0xff, v&0xff, ip, port);

    NET_DVR_USER_LOGIN_INFO li = {0};
    NET_DVR_DEVICEINFO_V40 dev = {0};
    strncpy(li.sDeviceAddress, ip, NET_DVR_DEV_ADDRESS_MAX_LEN - 1);
    li.wPort = port;
    strncpy(li.sUserName, user, NET_DVR_LOGIN_USERNAME_MAX_LEN - 1);
    strncpy(li.sPassword, pass, NET_DVR_LOGIN_PASSWD_MAX_LEN - 1);
    li.bUseAsynLogin = 0;

    LONG uid = NET_DVR_Login_V40(&li, &dev);
    if (uid < 0) {
        printf("LOGIN ECHEC code=%u\n", NET_DVR_GetLastError());
        NET_DVR_Cleanup();
        return 1;
    }
    const NET_DVR_DEVICEINFO_V30 &d = dev.struDeviceV30;
    printf("LOGIN OK uid=%ld  type=%u  analog=%u@%u  ip=%u@%u  serial=%.48s\n",
           (long)uid, d.wDevType, d.byChanNum, d.byStartChan,
           d.byIPChanNum + 256 * d.byHighDChanNum, d.byStartDChan, d.sSerialNumber);

    // Snapshot du premier canal disponible
    DWORD cap = 4 << 20, got = 0;
    char *buf = (char*)malloc(cap);
    NET_DVR_JPEGPARA jp = {0, 0};
    int chans[8], n = 0;
    if (d.byChanNum) chans[n++] = d.byStartChan;
    if (d.byIPChanNum) chans[n++] = d.byStartDChan;
    if (!n) { chans[n++] = 1; chans[n++] = 33; }

    for (int i = 0; i < n; i++) {
        if (NET_DVR_CaptureJPEGPicture_NEW(uid, chans[i], &jp, buf, cap, &got) && got) {
            char path[64]; snprintf(path, sizeof path, "/out/cap_c%d.jpg", chans[i]);
            FILE *f = fopen(path, "wb"); if (f) { fwrite(buf, 1, got, f); fclose(f); }
            printf("CAPTURE OK canal %d  %u octets  -> %s\n", chans[i], got, path);
            break;
        }
        printf("capture canal %d echec code=%u\n", chans[i], NET_DVR_GetLastError());
    }

    free(buf);
    NET_DVR_Logout(uid);
    NET_DVR_Cleanup();
    return 0;
}
