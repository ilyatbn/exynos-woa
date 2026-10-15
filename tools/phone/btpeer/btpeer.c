/*
 * btpeer.c - make THIS PC a Bluetooth peer the S10e can pair with (D4c R5 pairing test, notes/bt-driver.md).
 * Turns on discoverability + incoming connections on the PC's radio and auto-accepts incoming authentication
 * (numeric comparison / passkey notification / legacy PIN "0000") for <seconds>, then turns discoverability off.
 * Prints device names and pairing methods only - never Bluetooth addresses.
 *
 *   btpeer [seconds]      (default 300; run elevated so it can start bthserv)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bluetoothapis.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "bthprops.lib")

static const char *Method(BLUETOOTH_AUTHENTICATION_METHOD m)
{
    switch (m)
    {
    case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY:               return "legacy PIN";
    case BLUETOOTH_AUTHENTICATION_METHOD_OOB:                  return "out of band";
    case BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON:   return "numeric comparison";
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION: return "passkey notification";
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY:              return "passkey entry";
    default:                                                   return "?";
    }
}

static BOOL CALLBACK OnAuth(LPVOID ctx, PBLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS p)
{
    BLUETOOTH_AUTHENTICATE_RESPONSE r;
    DWORD                           e;

    (void)ctx;
    ZeroMemory(&r, sizeof(r));
    r.bthAddressRemote = p->deviceInfo.Address;
    r.authMethod       = p->authenticationMethod;
    r.negativeResponse = FALSE;
    switch (p->authenticationMethod)
    {
    case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY:
        r.pinInfo.pinLength = 4;
        memcpy(r.pinInfo.pin, "0000", 4);
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY:
        r.passkeyInfo.passkey = 0;
        break;
    default:   // numeric comparison / passkey notification: just confirm
        r.numericCompInfo.NumericValue = p->Numeric_Value;
        break;
    }
    e = BluetoothSendAuthenticationResponseEx(NULL, &r);
    printf("pairing request from \"%ls\" (%s, value %06lu): accept -> %lu\n", p->deviceInfo.szName,
           Method(p->authenticationMethod), p->Numeric_Value, e);
    fflush(stdout);
    return TRUE;
}

int main(int argc, char **argv)
{
    int                          seconds = argc > 1 ? atoi(argv[1]) : 300;
    BLUETOOTH_FIND_RADIO_PARAMS  fp = { sizeof(fp) };
    BLUETOOTH_RADIO_INFO         ri = { sizeof(ri) };
    HANDLE                       radio = NULL;
    HBLUETOOTH_RADIO_FIND        f;
    HBLUETOOTH_AUTHENTICATION_REGISTRATION reg = NULL;
    BLUETOOTH_DEVICE_INFO        any = { sizeof(any) };
    DWORD                        e;

    f = BluetoothFindFirstRadio(&fp, &radio);
    if (f == NULL)
    {
        printf("no Bluetooth radio (%lu) - is bthserv running?\n", GetLastError());
        return 1;
    }
    BluetoothFindRadioClose(f);
    BluetoothGetRadioInfo(radio, &ri);
    printf("radio \"%ls\" (manufacturer 0x%04x)\n", ri.szName, ri.manufacturer);

    e = BluetoothRegisterForAuthenticationEx(&any, &reg, OnAuth, NULL);
    printf("auto-accept registration: %lu\n", e);
    printf("incoming connections on: %d\n", BluetoothEnableIncomingConnections(radio, TRUE));
    printf("discoverable on: %d\n", BluetoothEnableDiscovery(radio, TRUE));
    printf("discoverable as \"%ls\" for %d s - pair from the phone now\n", ri.szName, seconds);
    fflush(stdout);

    for (int s = 0; s < seconds; s++)
    {
        if (s % 60 == 0 && s)
        {
            BluetoothEnableDiscovery(radio, TRUE);   // Windows may drop discoverability; keep it on
            printf("%d s\n", s);
            fflush(stdout);
        }
        Sleep(1000);
    }

    printf("discoverable off: %d\n", BluetoothEnableDiscovery(radio, FALSE));
    if (reg)
        BluetoothUnregisterAuthentication(reg);
    CloseHandle(radio);
    return 0;
}
