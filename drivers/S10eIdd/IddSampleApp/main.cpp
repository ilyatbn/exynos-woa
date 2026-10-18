#include <iostream>
#include <vector>

#include <windows.h>
#include <swdevice.h>
#include <conio.h>
#include <stdlib.h>
#include <wrl.h>

VOID WINAPI
CreationCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT hrCreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId
    )
{
    HANDLE hEvent = *(HANDLE*) pContext;

    SetEvent(hEvent);
    UNREFERENCED_PARAMETER(hSwDevice);
    UNREFERENCED_PARAMETER(hrCreateResult);
    UNREFERENCED_PARAMETER(pszDeviceInstanceId);
}

int __cdecl main(int argc, wchar_t *argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    HSWDEVICE hSwDevice;
    SW_DEVICE_CREATE_INFO createInfo = { 0 };
    PCWSTR description = L"Idd Sample Driver";

    // These match the Pnp id's in the inf file so OS will load the driver when the device is created    
    PCWSTR instanceId = L"IddSampleDriver";
    PCWSTR hardwareIds = L"IddSampleDriver\0\0";
    PCWSTR compatibleIds = L"IddSampleDriver\0\0";

    createInfo.cbSize = sizeof(createInfo);
    createInfo.pszzCompatibleIds = compatibleIds;
    createInfo.pszInstanceId = instanceId;
    createInfo.pszzHardwareIds = hardwareIds;
    createInfo.pszDeviceDescription = description;

    createInfo.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                                 SWDeviceCapabilitiesSilentInstall |
                                 SWDeviceCapabilitiesDriverRequired;

    // Create the device
    HRESULT hr = SwDeviceCreate(L"IddSampleDriver",
                                L"HTREE\\ROOT\\0",
                                &createInfo,
                                0,
                                nullptr,
                                CreationCallback,
                                &hEvent,
                                &hSwDevice);
    if (FAILED(hr))
    {
        printf("SwDeviceCreate failed with 0x%lx\n", hr);
        return 1;
    }

    // Wait for callback to signal that the device has been created
    printf("Waiting for device to be created....\n");
    DWORD waitResult = WaitForSingleObject(hEvent, 10*1000);
    if (waitResult != WAIT_OBJECT_0)
    {
        printf("Wait for device creation failed\n");
        return 1;
    }
    printf("Device created\n\n");
    
    // exynos-woa G18: "IddSampleApp SECONDS" keeps the device for SECONDS and exits (started remotely there is no
    // console to press 'x' in); without an argument the sample's key wait below is unchanged.
    // (the sample declares main with wchar_t *argv[], but a narrow main receives char strings - parse them as such)
    DWORD keepSeconds = (argc > 1) ? strtoul(reinterpret_cast<const char *>(argv[1]), nullptr, 10) : 0;
    if (keepSeconds > 0)
    {
        printf("Keeping the device for %lu s\n", keepSeconds);
        Sleep(keepSeconds * 1000);
        SwDeviceClose(hSwDevice);
        return 0;
    }

    // Now wait for user to indicate the device should be stopped
    printf("Press 'x' to exit and destory the software device\n");
    bool bExit = false;
    do
    {
        // Wait for key press
        int key = _getch();

        if (key == 'x' || key == 'X')
        {
            bExit = true;
        }
    }while (!bExit);
    
    // Stop the device, this will cause the sample to be unloaded
    SwDeviceClose(hSwDevice);

    return 0;
}