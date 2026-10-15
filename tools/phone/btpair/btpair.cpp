// btpair.cpp - pair the S10e with a nearby classic Bluetooth device by name (D4c R5 second half,
// notes/bt-driver.md). C++/WinRT, ARM64. Prints names and results only - never Bluetooth addresses.
//
//   btpair pair <name-substring> [seconds]   watch unpaired devices, pair with the first match (auto-accept)
//   btpair unpair <name-substring>           remove a paired device
//   btpair list                              list paired Bluetooth devices
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <cstdio>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Devices::Bluetooth;

static bool Matches(hstring const &name, std::wstring const &want)
{
    std::wstring n(name.c_str());
    return !want.empty() && n.find(want) != std::wstring::npos;
}

static const char *PairStatus(DevicePairingResultStatus s)
{
    switch (s)
    {
    case DevicePairingResultStatus::Paired: return "Paired";
    case DevicePairingResultStatus::AlreadyPaired: return "AlreadyPaired";
    case DevicePairingResultStatus::NotReadyToPair: return "NotReadyToPair";
    case DevicePairingResultStatus::AuthenticationTimeout: return "AuthenticationTimeout";
    case DevicePairingResultStatus::AuthenticationFailure: return "AuthenticationFailure";
    case DevicePairingResultStatus::ConnectionRejected: return "ConnectionRejected";
    case DevicePairingResultStatus::RejectedByHandler: return "RejectedByHandler";
    case DevicePairingResultStatus::Failed: return "Failed";
    case DevicePairingResultStatus::OperationAlreadyInProgress: return "OperationAlreadyInProgress";
    case DevicePairingResultStatus::RequiredHandlerNotRegistered: return "RequiredHandlerNotRegistered";
    case DevicePairingResultStatus::AuthenticationNotAllowed: return "AuthenticationNotAllowed";
    case DevicePairingResultStatus::NoSupportedProfiles: return "NoSupportedProfiles";
    case DevicePairingResultStatus::ProtectionLevelCouldNotBeMet: return "ProtectionLevelCouldNotBeMet";
    case DevicePairingResultStatus::AccessDenied: return "AccessDenied";
    default: return "other";
    }
}

static void List()
{
    auto all = DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true)).get();
    printf("paired Bluetooth devices: %u\n", all.Size());
    for (auto const &d : all)
        printf("  \"%ls\"\n", d.Name().c_str());
}

int wmain(int argc, wchar_t **argv)
{
    init_apartment();
    std::wstring mode = argc > 1 ? argv[1] : L"list";
    std::wstring want = argc > 2 ? argv[2] : L"";
    int seconds = argc > 3 ? _wtoi(argv[3]) : 60;

    if (mode == L"list")
    {
        List();
        return 0;
    }

    if (mode == L"unpair")
    {
        auto all = DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true)).get();
        for (auto const &d : all)
            if (Matches(d.Name(), want))
            {
                auto r = d.Pairing().UnpairAsync().get();
                printf("unpair \"%ls\": status %d\n", d.Name().c_str(), (int)r.Status());
            }
        List();
        return 0;
    }

    // pair: watch unpaired classic devices (the watcher runs an inquiry), pair with the first name match
    std::mutex lock;
    DeviceInformation found{ nullptr };
    unsigned seen = 0;
    auto watcher = DeviceInformation::CreateWatcher(BluetoothDevice::GetDeviceSelectorFromPairingState(false));
    watcher.Added([&](DeviceWatcher const &, DeviceInformation const &d) {
        std::lock_guard<std::mutex> g(lock);
        seen++;
        printf("seen \"%ls\"\n", d.Name().c_str());
        fflush(stdout);
        if (!found && Matches(d.Name(), want))
            found = d;
    });
    watcher.Updated([](DeviceWatcher const &, DeviceInformationUpdate const &) {});
    watcher.Start();
    for (int s = 0; s < seconds * 2; s++)
    {
        {
            std::lock_guard<std::mutex> g(lock);
            if (found)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    watcher.Stop();
    if (!found)
    {
        printf("no unpaired device matching \"%ls\" in %d s (%u seen)\n", want.c_str(), seconds, seen);
        return 3;
    }

    printf("pairing with \"%ls\" (can pair: %d)\n", found.Name().c_str(), (int)found.Pairing().CanPair());
    fflush(stdout);
    auto custom = found.Pairing().Custom();
    custom.PairingRequested([](DeviceInformationCustomPairing const &, DevicePairingRequestedEventArgs const &a) {
        printf("pairing request: kind %u pin \"%ls\" -> accept\n", (unsigned)a.PairingKind(), a.Pin().c_str());
        fflush(stdout);
        if (a.PairingKind() == DevicePairingKinds::ProvidePin)
            a.Accept(L"0000");
        else
            a.Accept();
    });
    auto kinds = DevicePairingKinds::ConfirmOnly | DevicePairingKinds::DisplayPin | DevicePairingKinds::ProvidePin |
                 DevicePairingKinds::ConfirmPinMatch;
    auto result = custom.PairAsync(kinds).get();
    printf("pair result: %s (protection level %d)\n", PairStatus(result.Status()), (int)result.ProtectionLevelUsed());
    List();
    return result.Status() == DevicePairingResultStatus::Paired ? 0 : 4;
}
