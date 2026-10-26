#pragma once

#include <string>
#include <windows.h>

namespace cardupdate {

// The result schema and transaction files are compatible with the reviewed
// SuperPre backend. The caller prints exactly one JSON result on success.
std::string Run(const std::wstring& runtime, const std::string& action,
	const std::string& argument = {});

// Not exposed by --update-action. The launcher retains the exact child process
// handle and nonce until the game's own health receipt has been checked.
std::string ConfirmSpawnedGame(const std::wstring& runtime,
	const std::string& transaction, const std::string& nonce,
	DWORD childPid, HANDLE childProcess);

}
