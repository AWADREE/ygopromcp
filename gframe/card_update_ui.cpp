#include "card_update_ui.h"
#include "game.h"
#include "mcp.h"
#include "bufferio.h"
#include "../vendor/nlohmann/json.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>

namespace ygo {
namespace {
using json = nlohmann::json;
struct Result { bool ok{}; bool canceled{}; std::string body; std::string error; std::string stagedRevision; };
struct UiState {
	std::thread worker;
	std::mutex mutex;
	std::atomic<bool> cancel{false}, ready{false};
	bool busy{};
	CardUpdateAction action{CardUpdateAction::Status};
	Result result;
	std::string checkedRevision, stagedRevision;
	bool canRollback{}, scheduled{}, compatible{}, activationHealthy{};
} state;

bool LowerHex(const std::string& value, size_t size) {
	if(value.size() != size) return false;
	for(char c : value)
		if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	return true;
}
std::wstring RuntimeRoot() {
	DWORD size = GetCurrentDirectoryW(0, nullptr);
	std::wstring root(size, L'\0');
	if(!size || GetCurrentDirectoryW(size, &root[0]) != size - 1) return {};
	root.resize(size - 1);
	return root;
}
std::string ReadBounded(const std::wstring& path, bool& overflow) {
	std::string value;
	if(FILE* file = _wfopen(path.c_str(), L"rb")) {
		char buffer[4096];
		size_t n = 0;
		while((n = std::fread(buffer, 1, sizeof buffer, file)) != 0) {
			if(value.size() + n > 131072) { overflow = true; break; }
			value.append(buffer, n);
		}
		std::fclose(file);
	}
	return value;
}
bool RegularLocalPath(const std::wstring& path, bool directory) {
	const DWORD attrs = GetFileAttributesW(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) &&
		((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}
std::string FileSha256(const std::wstring& path) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE handle = nullptr;
	std::string digest;
	do {
		if(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) break;
		ULONG objectSize = 0, hashSize = 0, size = 0;
		if(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
			sizeof objectSize, &size, 0) < 0 ||
			BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize),
			sizeof hashSize, &size, 0) < 0 || hashSize != 32) break;
		std::vector<unsigned char> object(objectSize), hash(hashSize);
		if(BCryptCreateHash(algorithm, &handle, object.data(), objectSize, nullptr, 0, 0) < 0) break;
		FILE* file = _wfopen(path.c_str(), L"rb");
		if(!file) break;
		unsigned char buffer[65536];
		bool okay = true;
		for(size_t n; (n = std::fread(buffer, 1, sizeof buffer, file)) != 0;)
			if(BCryptHashData(handle, buffer, static_cast<ULONG>(n), 0) < 0) { okay = false; break; }
		if(std::ferror(file)) okay = false;
		std::fclose(file);
		if(!okay || BCryptFinishHash(handle, hash.data(), hashSize, 0) < 0) break;
		static const char hex[] = "0123456789abcdef";
		for(unsigned char byte : hash) { digest.push_back(hex[byte >> 4]); digest.push_back(hex[byte & 15]); }
	} while(false);
	if(handle) BCryptDestroyHash(handle);
	if(algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
	return digest;
}
std::string DiscoverStaged(const std::wstring& root) {
	const auto versions = root + L"\\card-updates\\versions";
	if(!RegularLocalPath(root + L"\\card-updates", true) ||
		!RegularLocalPath(versions, true)) return {};
	bool overflow = false;
	const auto buildPath = root + L"\\build-info.json";
	if(!RegularLocalPath(buildPath, false)) return {};
	const auto build = json::parse(ReadBounded(buildPath, overflow), nullptr, false);
	if(overflow || !build.is_object() || !build.contains("core_commit") ||
		!build["core_commit"].is_string() || !build.contains("base_script_commit") ||
		!build["base_script_commit"].is_string()) return {};
	const std::string core = build["core_commit"], scripts = build["base_script_commit"];
	if(!LowerHex(core, 40) || !LowerHex(scripts, 40)) return {};
	WIN32_FIND_DATAW entry{};
	HANDLE find = FindFirstFileW((versions + L"\\*").c_str(), &entry);
	if(find == INVALID_HANDLE_VALUE) return {};
	std::string best, bestDate;
	do {
		const std::wstring name = entry.cFileName;
		if(name.size() != 40 || !(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
			(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
		const std::string revision(name.begin(), name.end());
		if(!LowerHex(revision, 40)) continue;
		const auto dir = versions + L"\\" + name;
		const auto manifestPath = dir + L"\\manifest.json";
		const auto packagePath = dir + L"\\package.ypk";
		if(!RegularLocalPath(manifestPath, false) || !RegularLocalPath(packagePath, false)) continue;
		overflow = false;
		const auto manifest = json::parse(ReadBounded(manifestPath, overflow), nullptr, false);
		if(overflow || !manifest.is_object() || !manifest.contains("revision") ||
			!manifest["revision"].is_string() || manifest["revision"] != revision ||
			!manifest.contains("core_pin") || !manifest["core_pin"].is_string() ||
			manifest["core_pin"] != core || !manifest.contains("base_script_pin") ||
			!manifest["base_script_pin"].is_string() || manifest["base_script_pin"] != scripts ||
			!manifest.contains("sha256") || !manifest["sha256"].is_string()) continue;
		const std::string expected = manifest["sha256"];
		if(!LowerHex(expected, 64) || FileSha256(packagePath) != expected) continue;
		const std::string date = manifest.contains("checked_utc") && manifest["checked_utc"].is_string() ?
			manifest["checked_utc"].get<std::string>() : "";
		if(best.empty() || date > bestDate) { best = revision; bestDate = date; }
	} while(FindNextFileW(find, &entry));
	FindClose(find);
	return best;
}
Result RunBackend(CardUpdateAction action, const std::string& revision,
	std::atomic<bool>& cancel) {
	Result result;
	const auto root = RuntimeRoot();
	const bool stage = action == CardUpdateAction::Check ||
		action == CardUpdateAction::Download || action == CardUpdateAction::Cleanup;
	const auto executable = root + L"\\YGOProUpdateLauncher.exe";
	const DWORD exeAttrs = GetFileAttributesW(executable.c_str());
	if(root.empty() || exeAttrs == INVALID_FILE_ATTRIBUTES ||
		(exeAttrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
		result.error = "Native update component is missing from this game folder.";
		return result;
	}
	if((action == CardUpdateAction::Download || action == CardUpdateAction::Schedule) &&
		!LowerHex(revision, 40)) {
		result.error = "Checked update revision is invalid.";
		return result;
	}
	const wchar_t* name = L"Status";
	switch(action) {
	case CardUpdateAction::Check: name = L"Check"; break;
	case CardUpdateAction::Download: name = L"Download"; break;
	case CardUpdateAction::Schedule: name = L"Schedule"; break;
	case CardUpdateAction::Rollback: name = L"Rollback"; break;
	case CardUpdateAction::Cancel: name = L"Cancel"; break;
	case CardUpdateAction::Cleanup: name = L"Cleanup"; break;
	default: break;
	}
	std::wstring folder = root + L"\\card-updates";
	if(!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
		result.error = "Cannot create the update state folder.";
		return result;
	}
	const DWORD attrs = GetFileAttributesW(folder.c_str());
	if(attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
		(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
		result.error = "Unsafe update state folder.";
		return result;
	}
	wchar_t outName[MAX_PATH]{}, errName[MAX_PATH]{};
	if(!GetTempFileNameW(folder.c_str(), L"uio", 0, outName) ||
		!GetTempFileNameW(folder.c_str(), L"uie", 0, errName)) {
		result.error = "Cannot create update output files.";
		return result;
	}
	SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
	HANDLE output = CreateFileW(outName, GENERIC_WRITE, FILE_SHARE_READ, &sa,
		CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	HANDLE errors = CreateFileW(errName, GENERIC_WRITE, FILE_SHARE_READ, &sa,
		CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(output == INVALID_HANDLE_VALUE || errors == INVALID_HANDLE_VALUE ||
		input == INVALID_HANDLE_VALUE) {
		result.error = "Cannot capture update process output.";
	} else {
		std::wstring command = L"\"" + executable + L"\" --update-action " + name;
		if(!revision.empty())
			command += L" " + std::wstring(revision.begin(), revision.end());
		STARTUPINFOW startup{sizeof startup};
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = input;
		startup.hStdOutput = output;
		startup.hStdError = errors;
		PROCESS_INFORMATION child{};
		if(!CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &child)) {
			result.error = "Cannot start the native update component.";
		} else {
			const DWORD limit = action == CardUpdateAction::Download ? 180000 :
				(action == CardUpdateAction::Check ? 60000 : 30000);
			const ULONGLONG deadline = GetTickCount64() + limit;
			bool timedOut = false;
			while(WaitForSingleObject(child.hProcess, 100) == WAIT_TIMEOUT) {
				if((stage && cancel.load()) || GetTickCount64() >= deadline) {
					timedOut = !cancel.load();
					TerminateProcess(child.hProcess, 1); // This UI owns only this child.
					WaitForSingleObject(child.hProcess, 5000);
					break;
				}
			}
			DWORD exitCode = 1;
			GetExitCodeProcess(child.hProcess, &exitCode);
			result.canceled = stage && cancel.load();
			if(timedOut) result.error = "Update helper timed out.";
			else if(result.canceled) result.error = "Canceled.";
			else result.ok = exitCode == 0;
			CloseHandle(child.hThread);
			CloseHandle(child.hProcess);
		}
	}
	if(input != INVALID_HANDLE_VALUE) CloseHandle(input);
	if(output != INVALID_HANDLE_VALUE) CloseHandle(output);
	if(errors != INVALID_HANDLE_VALUE) CloseHandle(errors);
	bool overflow = false;
	result.body = ReadBounded(outName, overflow);
	const auto stderrText = ReadBounded(errName, overflow);
	DeleteFileW(outName);
	DeleteFileW(errName);
	if(overflow) { result.ok = false; result.error = "Update helper output exceeded its limit."; }
	if(!result.ok && result.error.empty()) {
		const auto first = stderrText.find_first_of("\r\n");
		result.error = stderrText.empty() ? "Native update component failed." :
			stderrText.substr(0, std::min<size_t>(first, 120));
	}
	return result;
}
std::wstring Wide(const std::string& s) { return BufferIO::DecodeUTF8String(s); }
std::string String(const json& j, const char* key) {
	return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : "";
}
void Controls(Game& game) {
	const bool idle = !state.busy;
	game.btnUpdateCheck->setEnabled(idle);
	game.btnUpdateDownload->setEnabled(idle && state.compatible && LowerHex(state.checkedRevision, 40));
	game.btnUpdateApply->setEnabled(idle && state.compatible && state.activationHealthy &&
		LowerHex(state.stagedRevision, 40) &&
		!MCP_HTTP_IsRunning() && !game.dInfo.isStarted);
	game.btnUpdateRestore->setEnabled(idle && state.compatible && state.activationHealthy &&
		state.canRollback &&
		!MCP_HTTP_IsRunning() && !game.dInfo.isStarted);
	game.btnUpdateCancel->setEnabled((state.busy &&
		(state.action == CardUpdateAction::Check || state.action == CardUpdateAction::Download)) ||
		(idle && state.scheduled));
	game.btnUpdateCancel->setText(state.busy ? L"Cancel operation" : L"Cancel scheduled");
}
void Start(Game& game, CardUpdateAction action, const std::string& revision = {}) {
	if(state.busy) return;
	if(state.worker.joinable()) state.worker.join();
	state.cancel = false;
	state.ready = false;
	state.busy = true;
	state.action = action;
	const wchar_t* label = L"Reading update status...";
	if(action == CardUpdateAction::Check) label = L"Checking provider...";
	if(action == CardUpdateAction::Download) label = L"Downloading and validating...";
	if(action == CardUpdateAction::Schedule) label = L"Scheduling for restart...";
	if(action == CardUpdateAction::Rollback) label = L"Preparing restore for restart...";
	game.stUpdateStatus->setText(label);
	Controls(game);
	state.worker = std::thread([action, revision] {
		Result result = RunBackend(action, revision, state.cancel);
		if(result.ok && action == CardUpdateAction::Status)
			result.stagedRevision = DiscoverStaged(RuntimeRoot());
		if(result.canceled) {
			std::atomic<bool> noCancel{false};
			const Result clean = RunBackend(CardUpdateAction::Cleanup, {}, noCancel);
			if(!clean.ok) result.error = "Canceled; staging cleanup failed: " + clean.error;
		}
		{
			std::lock_guard<std::mutex> guard(state.mutex);
			state.result = std::move(result);
		}
		state.ready = true;
	});
}
} // namespace

bool CardUpdateUiAvailable() {
	const auto root = RuntimeRoot();
	if(root.empty()) return false;
	const DWORD attrs = GetFileAttributesW((root + L"\\YGOProUpdateLauncher.exe").c_str());
	return attrs != INVALID_FILE_ATTRIBUTES &&
		!(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
void CardUpdateUiOpen(Game& game) { Start(game, CardUpdateAction::Status); }
void CardUpdateUiAction(Game& game, CardUpdateAction action) {
	if(action == CardUpdateAction::Cancel && state.busy) {
		if(state.action == CardUpdateAction::Check || state.action == CardUpdateAction::Download)
			state.cancel = true;
		return;
	}
	if(state.busy) return;
	if(action == CardUpdateAction::Schedule || action == CardUpdateAction::Rollback)
		if(!state.activationHealthy) {
			game.stUpdateStatus->setText(L"Read a healthy update status before changing data.");
			return;
		}
	if(action == CardUpdateAction::Schedule || action == CardUpdateAction::Rollback)
		if(MCP_HTTP_IsRunning() || game.dInfo.isStarted) {
			game.stUpdateStatus->setText(L"Stop MCP and leave the duel before changing data.");
			return;
		}
	if(action == CardUpdateAction::Download && !LowerHex(state.checkedRevision, 40)) return;
	if(action == CardUpdateAction::Schedule && !LowerHex(state.stagedRevision, 40)) return;
	const std::string revision = action == CardUpdateAction::Download ? state.checkedRevision :
		(action == CardUpdateAction::Schedule ? state.stagedRevision : "");
	Start(game, action, revision);
}
void CardUpdateUiPoll(Game& game) {
	if(!state.ready.exchange(false)) { if(state.busy) Controls(game); return; }
	if(state.worker.joinable()) state.worker.join();
	Result result;
	{
		std::lock_guard<std::mutex> guard(state.mutex);
		result = std::move(state.result);
	}
	state.busy = false;
	if(!result.ok) {
		if(state.action == CardUpdateAction::Check) state.checkedRevision.clear();
		if(state.action == CardUpdateAction::Schedule || state.action == CardUpdateAction::Rollback)
			state.activationHealthy = false;
		if(state.action == CardUpdateAction::Status) {
			state.checkedRevision.clear(); state.stagedRevision.clear();
			state.compatible = false; state.activationHealthy = false;
			state.canRollback = false; state.scheduled = false;
		}
		game.stUpdateStatus->setText(result.canceled ? L"Canceled" : L"Update error");
		game.stUpdateDetail->setText(Wide(result.error).c_str());
		Controls(game);
		return;
	}
	const auto response = json::parse(result.body, nullptr, false);
	if(response.is_discarded() || !response.is_object()) {
		if(state.action == CardUpdateAction::Check) state.checkedRevision.clear();
		if(state.action == CardUpdateAction::Status) {
			state.checkedRevision.clear(); state.stagedRevision.clear();
			state.compatible = false; state.activationHealthy = false;
		}
		game.stUpdateStatus->setText(L"Update response was invalid");
		Controls(game);
		return;
	}
	const std::string phase = String(response, "state");
	if(phase.empty()) {
		if(state.action == CardUpdateAction::Check) state.checkedRevision.clear();
		if(state.action == CardUpdateAction::Status) {
			state.checkedRevision.clear(); state.stagedRevision.clear();
			state.compatible = false; state.activationHealthy = false;
		}
		game.stUpdateStatus->setText(L"Update response was incomplete");
		Controls(game);
		return;
	}
	if(state.action == CardUpdateAction::Status || state.action == CardUpdateAction::Check) {
		state.compatible = response.contains("core") && response["core"].is_object() &&
			response["core"].contains("compatible") && response["core"]["compatible"].is_boolean() &&
			response["core"]["compatible"].get<bool>();
		if(!state.compatible) state.stagedRevision.clear();
	}
	if(state.action == CardUpdateAction::Check) {
		state.checkedRevision.clear();
		if(phase == "incompatible" && response.contains("core") &&
			response["core"].is_object())
			game.stUpdateDetail->setText(Wide(String(response["core"], "reason")).c_str());
		if(response.contains("feed") && response["feed"].is_object()) {
			const auto& feed = response["feed"];
			const auto revision = String(feed, "revision");
			if(phase == "candidate" && LowerHex(revision, 40)) state.checkedRevision = revision;
			game.stUpdateProvider->setText(Wide("Provider: " + String(feed, "provider") +
				" / " + String(feed, "channel")).c_str());
			if(phase == "candidate")
				game.stUpdateDetail->setText(Wide("Revision: " + revision.substr(0, 12) +
					"  |  scope: English prerelease overlay").c_str());
		}
	} else if(state.action == CardUpdateAction::Download) {
		state.stagedRevision.clear();
		if(phase == "ready-to-apply" && response.contains("manifest") &&
			response["manifest"].is_object()) {
			const auto revision = String(response["manifest"], "revision");
			if(LowerHex(revision, 40)) state.stagedRevision = revision;
		}
	} else if(state.action == CardUpdateAction::Status) {
		state.checkedRevision.clear();
		state.activationHealthy = phase == "not-checked" || phase == "installed";
		state.stagedRevision = state.compatible ? result.stagedRevision : "";
		if(state.stagedRevision == String(response, "installed_revision") ||
			phase == "restart-required" || phase == "startup-pending")
			state.stagedRevision.clear();
		state.canRollback = response.contains("can_rollback") &&
			response["can_rollback"].is_boolean() &&
			response["can_rollback"].get<bool>();
		state.scheduled = phase == "restart-required";
		if(response.contains("core") && response["core"].is_object() &&
			response["core"].contains("compatible") &&
			response["core"]["compatible"].is_boolean() &&
			!response["core"]["compatible"].get<bool>())
			game.stUpdateDetail->setText(Wide(String(response["core"], "reason")).c_str());
	} else if(state.action == CardUpdateAction::Schedule ||
		state.action == CardUpdateAction::Rollback) {
		state.activationHealthy = false;
		state.scheduled = phase == "restart-required";
		state.stagedRevision.clear();
	} else if(state.action == CardUpdateAction::Cancel) {
		state.scheduled = false;
		state.activationHealthy = false;
		Start(game, CardUpdateAction::Status);
		return;
	}
	std::wstring label = Wide(phase);
	if(phase == "restart-required") {
		const bool restoring = state.action == CardUpdateAction::Rollback ||
			String(response, "pending_revision") == "none";
		label = restoring ? L"Restart to restore previous card data" :
			L"Restart to install verified card data";
		game.stUpdateDetail->setText(restoring ?
			L"Current card data stays in place until the next launch." :
			L"The verified update will be applied on the next launch.");
	}
	if(phase == "installed") {
		label = L"Card data update installed";
		const auto revision = String(response, "installed_revision");
		if(LowerHex(revision, 40))
			game.stUpdateDetail->setText(Wide("Revision: " + revision.substr(0, 12)).c_str());
	}
	if(phase == "ready-to-apply") label = L"Verified download ready to apply";
	if(phase == "candidate") label = L"Compatible prerelease update found";
	if(phase == "incompatible") label = L"This build is not compatible with updates";
	if(phase == "external-change") {
		label = L"Active card data changed outside updates";
		game.stUpdateDetail->setText(L"No files were changed. Preserve the package and restore a verified installation before retrying.");
	}
	if(phase == "recovery-needed") {
		label = L"Update recovery needs attention";
		game.stUpdateDetail->setText(L"No new update will be scheduled. Preserve the current files and review the local update state.");
	}
	game.stUpdateStatus->setText(label.c_str());
	Controls(game);
}
void CardUpdateUiShutdown() {
	if(state.busy && (state.action == CardUpdateAction::Check ||
		state.action == CardUpdateAction::Download)) state.cancel = true;
	if(state.worker.joinable()) state.worker.join();
}
} // namespace ygo

#else
namespace ygo {
bool CardUpdateUiAvailable() { return false; }
void CardUpdateUiOpen(Game&) {}
void CardUpdateUiAction(Game&, CardUpdateAction) {}
void CardUpdateUiPoll(Game&) {}
void CardUpdateUiShutdown() {}
}
#endif
