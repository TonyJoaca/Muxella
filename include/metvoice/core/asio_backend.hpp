#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <windows.h>

namespace metvoice::core {

struct AsioDriverInfo {
    std::string name;
    std::string clsid_str;
    std::string description;
};

/**
 * @brief Clean-room ASIO 2.0 Host Architecture and Interface.
 * 
 * Enumerates registered ASIO drivers via Windows Registry:
 * HKEY_LOCAL_MACHINE\SOFTWARE\ASIO
 * and defines the callback model for low-latency hardware interfaces.
 */
class AsioHostInterface {
public:
    static std::vector<AsioDriverInfo> enumerate_drivers() {
        std::vector<AsioDriverInfo> drivers;
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char subKeyName[256];
            DWORD index = 0;
            DWORD nameLen = sizeof(subKeyName);

            while (RegEnumKeyExA(hKey, index, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                HKEY hSubKey = nullptr;
                if (RegOpenKeyExA(hKey, subKeyName, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
                    char clsid[128] = {0};
                    char desc[256] = {0};
                    DWORD clsidLen = sizeof(clsid);
                    DWORD descLen = sizeof(desc);

                    RegQueryValueExA(hSubKey, "CLSID", nullptr, nullptr, reinterpret_cast<LPBYTE>(clsid), &clsidLen);
                    RegQueryValueExA(hSubKey, "Description", nullptr, nullptr, reinterpret_cast<LPBYTE>(desc), &descLen);

                    AsioDriverInfo info;
                    info.name = subKeyName;
                    info.clsid_str = clsid;
                    info.description = (descLen > 0) ? desc : subKeyName;
                    drivers.push_back(info);

                    RegCloseKey(hSubKey);
                }
                index++;
                nameLen = sizeof(subKeyName);
            }
            RegCloseKey(hKey);
        }
        return drivers;
    }
};

} // namespace metvoice::core
