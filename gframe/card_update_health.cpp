#include "card_update_health.h"
#include "game.h"
#include "data_manager.h"
#include "ocgapi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace ygo {
namespace {
bool healthScriptError = false;
bool IsLowerHex(const char* value, size_t length) {
	if(!value || std::strlen(value) != length)
		return false;
	for(size_t i = 0; i < length; ++i)
		if(!((value[i] >= '0' && value[i] <= '9') ||
			(value[i] >= 'a' && value[i] <= 'f')))
			return false;
	return true;
}

uint32_t HealthScriptMessage(intptr_t duel, uint32_t) {
	healthScriptError = true;
	char message[1024]{};
	get_log_message(duel, message);
	std::fprintf(stderr, "Card update startup script check: %s\n", message);
	return 0;
}

bool LoadSupportScripts(Game& game, bool managed) {
	if(managed && game.managedProbeScript.empty())
		return false;
	const bool preferExpansion = game.gameConf.prefer_expansion_script;
	game.gameConf.prefer_expansion_script = false; // Read the mounted YPK first.
	set_script_reader(DataManager::ScriptReaderEx);
	set_card_reader(DataManager::CardReader);
	set_message_handler(HealthScriptMessage);
	healthScriptError = false;
	const intptr_t duel = create_duel(1);
	bool loaded = false;
	if(duel) {
		loaded = preload_script(duel, "./script/constant.lua") == 1 &&
			preload_script(duel, "./script/utility.lua") == 1 &&
			preload_script(duel, "./script/procedure.lua") == 1;
		if(loaded && managed) {
			// Card scripts expect the core to supply self_code and self_table.
			// Creating a real card exercises that path, including its initial_effect.
			const char* digits = game.managedProbeScript.c_str() + 8;
			char* end = nullptr;
			const unsigned long code = std::strtoul(digits, &end, 10);
			CardData data{};
			loaded = code > 0 && code <= UINT32_MAX && end &&
				!std::strcmp(end, ".lua") &&
				dataManager.GetData(static_cast<uint32_t>(code), &data);
			if(loaded) {
				new_card(duel, static_cast<uint32_t>(code), 0, 0,
					LOCATION_DECK, 0, POS_FACEDOWN_DEFENSE);
				loaded = query_field_count(duel, 0, LOCATION_DECK) == 1;
			}
		}
		loaded = loaded && !healthScriptError;
		end_duel(duel);
	}
	game.gameConf.prefer_expansion_script = preferExpansion;
	return loaded;
}

#ifdef _WIN32
bool Sha256File(const char* path, std::string& digest) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE handle = nullptr;
	std::FILE* file = nullptr;
	bool good = false;
	do {
		if(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
			break;
		ULONG objectLength = 0, hashLength = 0, size = 0;
		if(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectLength), sizeof objectLength, &size, 0) < 0 ||
			BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&hashLength), sizeof hashLength, &size, 0) < 0 ||
			hashLength != 32)
			break;
		std::vector<unsigned char> object(objectLength), hash(hashLength);
		if(BCryptCreateHash(algorithm, &handle, object.data(), objectLength,
			nullptr, 0, 0) < 0)
			break;
		file = std::fopen(path, "rb");
		if(!file)
			break;
		unsigned char buffer[65536];
		size_t read = 0;
		while((read = std::fread(buffer, 1, sizeof buffer, file)) > 0)
			if(BCryptHashData(handle, buffer, static_cast<ULONG>(read), 0) < 0)
				break;
		if(std::ferror(file) || (read != 0))
			break;
		if(BCryptFinishHash(handle, hash.data(), hashLength, 0) < 0)
			break;
		static const char hex[] = "0123456789abcdef";
		digest.clear();
		for(unsigned char byte : hash) {
			digest.push_back(hex[byte >> 4]);
			digest.push_back(hex[byte & 15]);
		}
		good = true;
	} while(false);
	if(file) std::fclose(file);
	if(handle) BCryptDestroyHash(handle);
	if(algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
	return good;
}

bool WriteReceipt(const char* transaction, const char* nonce,
	const std::string& packageHash) {
	const wchar_t* directory = L"card-updates";
	const DWORD attributes = GetFileAttributesW(directory);
	if(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
		(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
		return false;
	const DWORD pid = GetCurrentProcessId();
	std::wstring temporary = L"card-updates\\startup-health-";
	for(const char* p = nonce; *p; ++p)
		temporary.push_back(static_cast<wchar_t>(*p));
	temporary += L".tmp";
	const wchar_t* target = L"card-updates\\startup-health.json";
	const DWORD targetAttributes = GetFileAttributesW(target);
	if(targetAttributes != INVALID_FILE_ATTRIBUTES &&
		(targetAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
		return false;
	char body[512]{};
	const int length = std::snprintf(body, sizeof body,
		"{\"transaction\":\"%s\",\"active_sha256\":\"%s\","
		"\"managed_package_sha256\":\"%s\",\"initialized\":true,"
		"\"database_loaded\":true,\"scripts_loaded\":true,"
		"\"process_id\":%lu,\"launcher_nonce\":\"%s\"}\n",
		transaction, packageHash.c_str(), packageHash.c_str(),
		static_cast<unsigned long>(pid), nonce);
	if(length <= 0 || length >= static_cast<int>(sizeof body))
		return false;
	HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(file == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	const bool saved = WriteFile(file, body, static_cast<DWORD>(length), &written, nullptr) &&
		written == static_cast<DWORD>(length) && FlushFileBuffers(file);
	CloseHandle(file);
	if(!saved || !MoveFileExW(temporary.c_str(), target,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(temporary.c_str());
		return false;
	}
	return true;
}
#endif
} // namespace

bool WriteCardUpdateStartupHealth(Game& game) {
	const char* transaction = std::getenv("YGOPROMCP_CARD_UPDATE_TRANSACTION");
	if(!transaction)
		return true;
#ifdef _WIN32
	const char* expectedHash = std::getenv("YGOPROMCP_CARD_UPDATE_ACTIVE_SHA256");
	const char* nonce = std::getenv("YGOPROMCP_CARD_UPDATE_HEALTH_NONCE");
	if(!IsLowerHex(transaction, 32) || !IsLowerHex(nonce, 32) ||
		!expectedHash || (*expectedHash && !IsLowerHex(expectedHash, 64)))
		return false;
	const bool managed = *expectedHash != '\0';
	if(managed != game.managedArchiveMounted ||
		(managed && (!game.managedReleaseDatabaseLoaded ||
			!game.managedUpdateDatabaseLoaded)))
		return false;
	if(!LoadSupportScripts(game, managed))
		return false;
	std::string loadedHash;
	if(managed && (!Sha256File("expansions/ygopro-super-pre.ypk", loadedHash) ||
		loadedHash != expectedHash))
		return false;
	return WriteReceipt(transaction, nonce, loadedHash);
#else
	(void)game;
	return false;
#endif
}
} // namespace ygo
