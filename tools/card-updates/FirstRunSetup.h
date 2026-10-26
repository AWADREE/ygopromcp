#pragma once
#include <string>

// Called before any game process is spawned. Existing installations are
// preserved; a code-only installation receives a native, cancellable setup.
bool EnsureFirstRunData(const std::wstring& root, bool stdioMode);
