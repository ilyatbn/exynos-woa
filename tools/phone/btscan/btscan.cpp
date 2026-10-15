// btscan.exe - D4c rung R5 probe for the phone's Windows (notes/bt-driver.md). C++/WinRT, ARM64 console.
// 1) BluetoothLEAdvertisementWatcher (active scan) for N seconds: number of advertisements and distinct advertisers,
//    with their local names when they send one. 2) DeviceWatcher for unpaired classic devices for N seconds: names.
// Prints NO device addresses (repo rule: never record MAC/IP addresses) - distinct advertisers are counted by address
// in memory only.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <chrono>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Enumeration;

int main(int argc, char **argv)
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 20;
    init_apartment();

    // 1) LE advertisements
    std::mutex lock;
    std::map<uint64_t, std::wstring> advertisers;
    long adverts = 0;
    short bestRssi = -127;
    BluetoothLEAdvertisementWatcher le;
    le.ScanningMode(BluetoothLEScanningMode::Active);
    le.Received([&](BluetoothLEAdvertisementWatcher const &, BluetoothLEAdvertisementReceivedEventArgs const &a) {
        std::lock_guard<std::mutex> g(lock);
        adverts++;
        if (a.RawSignalStrengthInDBm() > bestRssi) bestRssi = a.RawSignalStrengthInDBm();
        auto &name = advertisers[a.BluetoothAddress()];
        if (name.empty() && !a.Advertisement().LocalName().empty()) name = a.Advertisement().LocalName().c_str();
    });
    le.Start();
    printf("=== LE scan %d s (status %d)\n", seconds, (int)le.Status());
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    le.Stop();
    {
        std::lock_guard<std::mutex> g(lock);
        printf("LE: %ld advertisements from %zu distinct advertisers, best RSSI %d dBm\n", adverts, advertisers.size(), bestRssi);
        for (auto const &kv : advertisers)
            if (!kv.second.empty()) printf("  name: %ls\n", kv.second.c_str());
    }

    // 2) classic unpaired devices
    std::set<std::wstring> classic;
    auto watcher = DeviceInformation::CreateWatcher(BluetoothDevice::GetDeviceSelectorFromPairingState(false));
    watcher.Added([&](DeviceWatcher const &, DeviceInformation const &d) {
        std::lock_guard<std::mutex> g(lock);
        classic.insert(std::wstring(d.Name().c_str()));
    });
    watcher.Updated([](DeviceWatcher const &, DeviceInformationUpdate const &) {});
    watcher.Start();
    printf("=== classic DeviceWatcher %d s\n", seconds);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    watcher.Stop();
    {
        std::lock_guard<std::mutex> g(lock);
        printf("classic: %zu devices\n", classic.size());
        for (auto const &n : classic) printf("  name: %ls\n", n.c_str());
    }
    printf("=== done\n");
    return 0;
}
