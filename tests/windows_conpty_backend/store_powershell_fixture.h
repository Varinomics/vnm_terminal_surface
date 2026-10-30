#pragma once

#include "helpers/test_check.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <appmodel.h>
#include <iostream>
#include <vector>

namespace vnm_terminal::test_helpers {

// Package registration, rather than PATH, distinguishes the MSIX executable
// from MSI PowerShell. Return 77 only when no package is registered; discovery
// errors must not turn an installed-but-broken fixture into missing coverage.
inline int find_store_powershell(QString& package, QString& executable)
{
    package.clear();
    executable.clear();
    UINT32 count = 0, buffer_length = 0;
    constexpr wchar_t family[] = L"Microsoft.PowerShell_8wekyb3d8bbwe";
    LONG status = GetPackagesByPackageFamily(family, &count, nullptr, &buffer_length, nullptr);
    if (status == ERROR_SUCCESS && count == 0) {
        std::cout << "SKIP: Microsoft Store PowerShell is not registered for this user.\n";
        return 77;
    }
    if (!check(status == ERROR_INSUFFICIENT_BUFFER, "Store PowerShell registration is queryable")) {
        std::cerr << "GetPackagesByPackageFamily: " << status << '\n';
        return 1;
    }
    std::vector<PWSTR>   names(count);
    std::vector<wchar_t> buffer(buffer_length);
    status = GetPackagesByPackageFamily(family, &count, names.data(), &buffer_length, buffer.data());
    if (!check(status == ERROR_SUCCESS, "Store PowerShell registrations are readable")) {
        std::cerr << "GetPackagesByPackageFamily: " << status << '\n';
        return 1;
    }

    for (UINT32 index = 0; index < count; ++index) {
        UINT32 path_length = 0;
        status = GetPackagePathByFullName(names[index], &path_length, nullptr);
        if (!check(status == ERROR_INSUFFICIENT_BUFFER, "registered package path is queryable")) {
            std::cerr << "GetPackagePathByFullName: " << status << '\n';
            return 1;
        }
        std::vector<wchar_t> path(path_length);
        status = GetPackagePathByFullName(names[index], &path_length, path.data());
        if (!check(status == ERROR_SUCCESS, "registered package path is readable")) {
            std::cerr << "GetPackagePathByFullName: " << status << '\n';
            return 1;
        }
        const QString candidate = QDir(QString::fromWCharArray(path.data())).filePath(QStringLiteral("pwsh.exe"));
        if (QFileInfo(candidate).isFile()) {
            package    = QString::fromWCharArray(names[index]);
            executable = candidate;
            break;
        }
    }
    if (!check(!executable.isEmpty(), "a registered Store PowerShell package contains pwsh.exe")) {
        return 1;
    }
    return 0;
}

} // namespace vnm_terminal::test_helpers
