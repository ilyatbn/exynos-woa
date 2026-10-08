/* SPDX-License-Identifier: GPL-2.0-only */
static void update_touch_driver(const std::wstring &volume)
{
    const auto system = volume + L"Windows\\System32\\";
    require(hash_file(system + L"ntoskrnl.exe", 16 * 1024 * 1024) ==
        "b3e83f4a54a09f439b7e51e686dfcfb0273717594bba21fefe8bb05f69f321bc" &&
        hash_file(system + L"winload.efi", 4 * 1024 * 1024) ==
        "86f882adaeec3db25f8add7d4d3331d7d4c09227c3dbfd114519bdda24961074",
        "Offline Windows kernel/loader differs from the captured phone");
    const auto store = system + L"DriverStore\\FileRepository\\";
    const auto old = store + L"s6sy761touch.inf_arm64_5967a6a5010c79b6\\";
    const wchar_t *names[] = {L"S6SY761TouchBeyond0.sys", L"S6SY761TouchBeyond0.inf", L"S6SY761TouchBeyond0.cat"};
    const char *before[] = {FT_TOUCH_OLD_SYS_SHA, FT_TOUCH_OLD_INF_SHA, FT_TOUCH_OLD_CAT_SHA};
    const char *after[] = {FT_TOUCH_SYS_SHA, FT_TOUCH_INF_SHA, FT_TOUCH_CAT_SHA};
    const auto package = std::wstring(UpdateRoot) + L"\\Touch\\";
    for (unsigned i = 0; i != std::size(names); ++i) {
        require(hash_file(old + names[i], 1024 * 1024) == before[i],
            "Installed retained touch package changed");
        require(hash_file(package + names[i], 1024 * 1024) == after[i],
            "Signed touch update package changed");
        const auto backup = update_evidence_directory + L"\\before-" + names[i];
        require(CopyFileW((old + names[i]).c_str(), backup.c_str(), TRUE) &&
            hash_file(backup, 1024 * 1024) == before[i], "Native retained driver backup failed");
    }
    emit("\"type\":\"touch-driver-backup\",\"originalPackageRetained\":true");
    const auto scratch = std::wstring(UpdateRoot) + L"\\Scratch";
    require(CreateDirectoryW(scratch.c_str(), nullptr), "Create RAM-only servicing scratch failed");
    const auto options = L"/English /Image:" + volume + L" /Add-Driver /Driver:\"" +
        package + L"S6SY761TouchBeyond0.inf\" /ScratchDir:\"" + scratch +
        L"\" /LogPath:\"" + update_evidence_directory + L"\\dism-servicing.log\"";
    update_child(L"X:\\Windows\\System32\\Dism.exe", options, 0, 240000, L"touch-add-driver.log");
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW((store + L"s6sy761touch.inf_arm64_*").c_str(), &data);
    require(found != INVALID_HANDLE_VALUE, "Serviced touch package enumeration failed");
    struct FindGuard { HANDLE value; ~FindGuard() { FindClose(value); } } guard{found};
    unsigned matches = 0;
    do {
        require((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Unexpected touch store entry");
        const auto candidate = store + data.cFileName + L"\\";
        if (hash_file(candidate + names[0], 1024 * 1024) != after[0]) continue;
        for (unsigned i = 0; i != std::size(names); ++i)
            require(hash_file(candidate + names[i], 1024 * 1024) == after[i],
                "Serviced touch package content differs");
        ++matches;
    } while (FindNextFileW(found, &data));
    require(GetLastError() == ERROR_NO_MORE_FILES && matches == 1,
        "Exactly one matching updated touch package must be serviced");
    for (unsigned i = 0; i != std::size(names); ++i)
        require(hash_file(old + names[i], 1024 * 1024) == before[i],
            "Original rollback package was modified");
    emit("\"type\":\"touch-driver-serviced\",\"newSysSha256\":\"" FT_TOUCH_SYS_SHA
        "\",\"version\":\"6.0.5058.3\",\"originalPackageRetained\":true,\"runtimeVerified\":false");
}
