// Windows launcher for the native, journalled card-data updater.
// It does not parse MCP traffic: the game inherits the caller's raw handles.
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "NativeUpdater.h"
#include "FirstRunSetup.h"

namespace {
using json = nlohmann::json;
std::wstring root;
std::wstring exe;
std::wstring updateDir;
bool mcp = false;

void Fail(const std::string& message) { throw std::runtime_error(message); }
std::string U8(const std::wstring& value) {
	const int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if(n <= 0) return {};
	std::string result(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], n, nullptr, nullptr);
	result.pop_back();
	return result;
}
std::wstring W(const std::string& value) {
	const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.c_str(), -1, nullptr, 0);
	if(n <= 0) return L"Update failed.";
	std::wstring result(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.c_str(), -1, &result[0], n);
	result.pop_back();
	return result;
}
void Error(const std::string& message) {
	const std::string line = "YGOPro update startup: " + message + "\r\n";
	const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
	DWORD count = 0;
	if(handle != INVALID_HANDLE_VALUE && handle)
		WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &count, nullptr);
	if(!mcp) MessageBoxW(nullptr, W(message).c_str(), L"YGOPro startup", MB_OK | MB_ICONERROR);
}
bool Hex(const std::string& value, size_t n) {
	if(value.size() != n) return false;
	for(char c : value) if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	return true;
}
std::string Field(const json& object, const char* key) {
	return object.is_object() && object.contains(key) && object[key].is_string() ?
		object[key].get<std::string>() : "";
}
std::wstring RootFromExecutable() {
	std::vector<wchar_t> buffer(32768);
	const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if(!n || n >= buffer.size()) Fail("Cannot locate the launcher folder.");
	std::wstring path(buffer.data(), n);
	const auto slash = path.find_last_of(L"\\/");
	if(slash == std::wstring::npos) Fail("Cannot locate the launcher folder.");
	return path.substr(0, slash);
}
std::string ReadBounded(const std::wstring& path, size_t limit) {
	FILE* file = _wfopen(path.c_str(), L"rb");
	if(!file) Fail("Cannot read update helper output.");
	std::string result;
	char buffer[4096];
	size_t n = 0;
	while((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
		if(result.size() + n > limit) { std::fclose(file); Fail("Update helper output exceeded its limit."); }
		result.append(buffer, n);
	}
	std::fclose(file);
	return result;
}
json RunBackend(const wchar_t* action, const std::string& transaction = {}) {
	if(!transaction.empty() && !Hex(transaction, 32)) Fail("Invalid update transaction.");
	const auto response = json::parse(cardupdate::Run(root, U8(action), transaction), nullptr, false);
	if(!response.is_object() || Field(response, "state").empty())
		Fail("Native update engine returned invalid JSON.");
	return response;
}
std::string Nonce() {
	unsigned char bytes[16]{};
	if(BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
		Fail("Cannot generate a startup nonce.");
	static const char hex[] = "0123456789abcdef";
	std::string value;
	for(unsigned char byte : bytes) { value.push_back(hex[byte >> 4]); value.push_back(hex[byte & 15]); }
	return value;
}
bool GameAlreadyRunning() {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(snapshot == INVALID_HANDLE_VALUE) Fail("Cannot check running game processes.");
	PROCESSENTRY32W entry{sizeof entry};
	bool found = false;
	if(Process32FirstW(snapshot, &entry)) do {
		if(_wcsicmp(entry.szExeFile, L"YGOPro.exe")) continue;
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
		if(!process) { found = true; break; }
		wchar_t path[32768]{};
		DWORD count = 32768;
		if(QueryFullProcessImageNameW(process, 0, path, &count) && !_wcsicmp(path, exe.c_str()))
			found = true;
		CloseHandle(process);
		if(found) break;
	} while(Process32NextW(snapshot, &entry));
	CloseHandle(snapshot);
	return found;
}
bool HealthMatches(const std::string& tx, const std::string& hash,
	const std::string& nonce, DWORD pid) {
	const std::wstring path = updateDir + L"\\startup-health.json";
	if(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
	const auto receipt = json::parse(ReadBounded(path, 65536), nullptr, false);
	return receipt.is_object() && Field(receipt, "transaction") == tx &&
		Field(receipt, "launcher_nonce") == nonce &&
		Field(receipt, "active_sha256") == hash &&
		Field(receipt, "managed_package_sha256") == hash &&
		receipt.contains("process_id") && receipt["process_id"].is_number_unsigned() &&
		receipt["process_id"].get<DWORD>() == pid &&
		receipt.value("initialized", false) && receipt.value("database_loaded", false) &&
		receipt.value("scripts_loaded", false);
}
int Launch() {
	root = RootFromExecutable();
	exe = root + L"\\YGOPro.exe";
	updateDir = root + L"\\card-updates";
	if(GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) Fail("YGOPro.exe is missing.");
	if(!EnsureFirstRunData(root, mcp)) return 1;
	if(!CreateDirectoryW(updateDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
		Fail("Cannot create update state folder.");
	const DWORD attrs = GetFileAttributesW(updateDir.c_str());
	if(attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
		(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) Fail("Unsafe update state folder.");
	const std::wstring lockPath = updateDir + L"\\.launch-lock";
	HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(lock == INVALID_HANDLE_VALUE) Fail("Another game launch is preparing an update.");
	std::string tx, hash, nonce;
	HANDLE game = nullptr;
	bool confirmed = false;
	try {
		const auto status = RunBackend(L"Status");
		const std::string phase = Field(status, "state");
		if(phase == "external-change" || phase == "recovery-needed")
			Fail("Update state needs attention in Settings: " + phase);
		const bool compatible = status.contains("core") && status["core"].is_object() &&
			status["core"].contains("compatible") && status["core"]["compatible"].is_boolean() &&
			status["core"]["compatible"].get<bool>();
		if(!compatible && (phase == "restart-required" || phase == "startup-pending"))
			Fail("This game build cannot safely apply the scheduled update.");
		if(compatible) {
			if((phase == "restart-required" || phase == "startup-pending") && GameAlreadyRunning())
				Fail("Close the running game before applying an update.");
			const auto activation = RunBackend(L"Activate");
			const auto result = Field(activation, "state");
			if(result == "startup-pending") {
				tx = Field(activation, "transaction");
				hash = Field(activation, "active_sha256");
				if(!Hex(tx, 32) || (!hash.empty() && !Hex(hash, 64)))
					Fail("Activation returned an invalid transaction.");
				nonce = Nonce();
			} else if(result != "no-pending-update" && result != "rolled-back-after-interruption")
				Fail("Unexpected activation result: " + result);
		}
		SetEnvironmentVariableW(L"YGOPROMCP_CARD_UPDATE_TRANSACTION", tx.empty() ? nullptr : W(tx).c_str());
		SetEnvironmentVariableW(L"YGOPROMCP_CARD_UPDATE_ACTIVE_SHA256", tx.empty() ? nullptr : W(hash).c_str());
		SetEnvironmentVariableW(L"YGOPROMCP_CARD_UPDATE_HEALTH_NONCE", tx.empty() ? nullptr : W(nonce).c_str());
		std::wstring command = L"\"" + exe + L"\"";
		STARTUPINFOW startup{sizeof startup};
		BOOL inherit = FALSE;
		if(mcp) {
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
			startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
			inherit = TRUE;
		}
		PROCESS_INFORMATION child{};
		const bool started = CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr,
			inherit, 0, nullptr, root.c_str(), &startup, &child);
		SetEnvironmentVariableW(L"YGOPROMCP_CARD_UPDATE_TRANSACTION", nullptr);
		SetEnvironmentVariableW(L"YGOPROMCP_CARD_UPDATE_ACTIVE_SHA256", nullptr);
		SetEnvironmentVariableW(L"YGOPROMCP_CARD_UPDATE_HEALTH_NONCE", nullptr);
		if(!started) Fail("Cannot start YGOPro.exe.");
		game = child.hProcess;
		CloseHandle(child.hThread);
		if(!tx.empty()) {
			const ULONGLONG deadline = GetTickCount64() + 30000;
			bool healthy = false;
			while(GetTickCount64() < deadline) {
				if(WaitForSingleObject(game, 100) == WAIT_OBJECT_0)
					Fail("Game exited before update health confirmation.");
				if(HealthMatches(tx, hash, nonce, child.dwProcessId)) { healthy = true; break; }
			}
			if(!healthy) Fail("Game startup did not prove the update within 30 seconds.");
			const auto confirmation = json::parse(cardupdate::ConfirmSpawnedGame(
				root, tx, nonce, child.dwProcessId, game), nullptr, false);
			if(!confirmation.is_object() || Field(confirmation, "state") != "installed")
				Fail("Update confirmation failed.");
			confirmed = true;
		}
		CloseHandle(lock); lock = nullptr;
		if(!mcp) { CloseHandle(game); game = nullptr; return 0; }
		WaitForSingleObject(game, INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(game, &code);
		CloseHandle(game); game = nullptr;
		return static_cast<int>(code);
	} catch(...) {
		if(game) { TerminateProcess(game, 1); WaitForSingleObject(game, 5000); CloseHandle(game); }
		if(!tx.empty() && !confirmed) {
			try { RunBackend(L"AbortStartup", tx); }
			catch(...) { Error("Prior update recovery needs attention in Settings."); }
		}
		if(lock) CloseHandle(lock);
		throw;
	}
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	int count = 0;
	wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
	if(!args) { Error("Cannot parse launcher arguments."); return 1; }
	if(count == 2 && wcscmp(args[1], L"--self-check") == 0) {
		LocalFree(args);
		const char message[] = "YGOPro update launcher loader: OK\r\n";
		const HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
		DWORD written = 0;
		if(output && output != INVALID_HANDLE_VALUE)
			WriteFile(output, message, sizeof message - 1, &written, nullptr);
		return 0; // Before filesystem, update engine, or game process access.
	}
	if((count == 3 || count == 4) && wcscmp(args[1], L"--update-action") == 0) {
		try {
			const std::wstring folder = RootFromExecutable();
			const std::string result = cardupdate::Run(folder, U8(args[2]), count == 4 ? U8(args[3]) : "");
			const std::string line = result + "\n";
			DWORD written = 0;
			const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
			if(!output || output == INVALID_HANDLE_VALUE ||
				!WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
				written != line.size()) Fail("Cannot write native update result.");
			LocalFree(args);
			return 0;
		} catch(const std::exception& ex) {
			const std::string line = std::string("Native card update: ") + ex.what() + "\r\n";
			DWORD written = 0;
			const HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
			if(output && output != INVALID_HANDLE_VALUE)
				WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
			LocalFree(args);
			return 1;
		}
	}
	if(count == 2 && wcscmp(args[1], L"--mcp") == 0) mcp = true;
	else if(count != 1) {
		LocalFree(args);
		Error("Use YGOProUpdateLauncher.exe, --mcp, --self-check, or --update-action.");
		return 1;
	}
	LocalFree(args);
	try { return Launch(); }
	catch(const std::exception& ex) { Error(ex.what()); return 1; }
}
