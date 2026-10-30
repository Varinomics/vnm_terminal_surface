#pragma once

#include <QString>

namespace vnm_terminal::test_helpers {

// Package registration, rather than PATH, distinguishes the MSIX executable
// from MSI PowerShell. Return 77 only when no package is registered; discovery
// errors must not turn an installed-but-broken fixture into missing coverage.
int find_store_powershell(QString& package, QString& executable);

} // namespace vnm_terminal::test_helpers
