#include "../tools/card-updates/WindowsPowerShellEnvironment.h"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
	if(argc != 3) return 2;
	const bool safe = SafeUpdateScript(argv[1], argv[2]);
	const auto environment = WindowsPowerShellEnvironment();
	if(environment.empty()) return 3;
	std::wcout << (safe ? L"verified" : L"rejected") << L"\n";
	return safe ? 0 : 1;
}
