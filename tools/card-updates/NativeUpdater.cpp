#include "NativeUpdater.h"
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <sqlite3.h>
#include <zlib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <climits>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace cardupdate {
namespace {
using json = nlohmann::json;
constexpr size_t MAX_PACKAGE = 67108864;
constexpr size_t MAX_EXPANDED = 536870912;
const char* CORE_PIN = "3209cd866a55e8501a0ed358d3064d9ae0ae6b30";
const char* SCRIPT_PIN = "cfbb14cdbf7b2095db543f2620eb2e88b0fb9220";
const char* UTILITY_SHA256 = "c9fe3e2cca3754e261ce41b9cac25c7492db3c502f7ff200088cc077024969f0";
const char* BASE_UTILITY_SHA256 = "24ebf20bdea154a4468e89f6c239d09bd100a202524823a73c67200210812665";
const char* BASE_DB_SHA256 = "7f57ae28bb1d0b5987c8159c193c18ff820b1119a513949b1bf3cb5aa36234ef";
const char* BASE_STRINGS_SHA256 = "f1282302f54926094044cfb2e2f9a66a74b7fd642b891d8be7f4e79d8cbdc2e5";
const char* BASE_SCRIPTS_SHA256 = "d5bb8d23d9ebaccde66b64bf7faec277e0c0fe02855c114493aca0f38e91cf7c";
const char* BASE_DB_COMMIT = "ad8b039dfcef7395007932e01cf86e23aa73f0fd";
const char* BASE_NUM_FONT_SHA256 = "b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a";
const char* BASE_TEXT_FONT_SHA256 = "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b";
// Refresh this after the final, separately tested full-audio game build.
const char* TESTED_GAME_SHA256 = "83f8ee216a5587807b1d595d3da618ad875ca9d724d14ba224e3ff0d50d43b98";

void Fail(const std::string& message) { throw std::runtime_error(message); }
bool Hex(const std::string& value, size_t size) {
	if(value.size() != size) return false;
	for(char c : value) if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	return true;
}
std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}
std::string Utf8(const std::wstring& value) {
	const int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if(n <= 0) Fail("Cannot encode a path.");
	std::string result(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], n, nullptr, nullptr);
	result.pop_back();
	return result;
}
std::wstring Wide(const std::string& value) {
	const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.c_str(), -1, nullptr, 0);
	if(n <= 0) Fail("Invalid UTF-8 text.");
	std::wstring result(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.c_str(), -1, &result[0], n);
	result.pop_back();
	return result;
}
std::wstring Join(const std::wstring& a, const std::wstring& b) { return a + L"\\" + b; }
std::wstring Parent(const std::wstring& path) {
	const auto at = path.find_last_of(L"\\/");
	if(at == std::wstring::npos) Fail("Invalid update path.");
	return path.substr(0, at);
}
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void SafePath(const std::wstring& root, const std::wstring& path, bool mayBeMissing = true) {
	if(path.size() <= root.size() || _wcsnicmp(root.c_str(), path.c_str(), root.size()) ||
		path[root.size()] != L'\\') Fail("Update path escapes the game folder.");
	const DWORD rootAttrs = GetFileAttributesW(root.c_str());
	if(rootAttrs == INVALID_FILE_ATTRIBUTES || !(rootAttrs & FILE_ATTRIBUTE_DIRECTORY) ||
		(rootAttrs & FILE_ATTRIBUTE_REPARSE_POINT)) Fail("Unsafe game folder.");
	for(size_t at = root.size() + 1; at <= path.size();) {
		const size_t next = path.find(L'\\', at);
		const auto part = path.substr(0, next);
		const DWORD attrs = GetFileAttributesW(part.c_str());
		if(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
			Fail("Update path traverses a reparse point.");
		if(next == std::wstring::npos) break;
		at = next + 1;
	}
	if(!mayBeMissing && !Exists(path)) Fail("Required update file is missing.");
}
void Directory(const std::wstring& root, const std::wstring& path) {
	SafePath(root, path);
	if(!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
		Fail("Cannot create update folder.");
	SafePath(root, path, false);
	if(!(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY))
		Fail("Update folder path is not a directory.");
}
struct Handle {
	HANDLE value{INVALID_HANDLE_VALUE};
	Handle() = default;
	explicit Handle(HANDLE h) : value(h) {}
	~Handle() { if(value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
	Handle& operator=(Handle&& other) noexcept {
		if(this != &other) {
			if(good()) CloseHandle(value);
			value = other.value;
			other.value = INVALID_HANDLE_VALUE;
		}
		return *this;
	}
	bool good() const { return value && value != INVALID_HANDLE_VALUE; }
};
struct Internet {
	HINTERNET value{};
	explicit Internet(HINTERNET h = nullptr) : value(h) {}
	~Internet() { if(value) WinHttpCloseHandle(value); }
	Internet(const Internet&) = delete;
	Internet& operator=(const Internet&) = delete;
	bool good() const { return value != nullptr; }
};
std::vector<unsigned char> ReadBytes(const std::wstring& root, const std::wstring& path, size_t limit) {
	SafePath(root, path, false);
	Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if(!file.good()) Fail("Cannot read update file.");
	LARGE_INTEGER size{};
	if(!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
		static_cast<unsigned long long>(size.QuadPart) > limit) Fail("Update file exceeds its limit.");
	std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
	size_t offset = 0;
	while(offset < bytes.size()) {
		DWORD got = 0;
		const DWORD ask = static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - offset));
		if(!ReadFile(file.value, bytes.data() + offset, ask, &got, nullptr) || !got)
			Fail("Incomplete update file.");
		offset += got;
	}
	return bytes;
}
std::string ReadText(const std::wstring& root, const std::wstring& path, size_t limit = 65536) {
	const auto bytes = ReadBytes(root, path, limit);
	return std::string(bytes.begin(), bytes.end());
}
json ReadJson(const std::wstring& root, const std::wstring& path) {
	if(!Exists(path)) return nullptr;
	const auto value = json::parse(ReadText(root, path), nullptr, false);
	if(!value.is_object()) Fail("Invalid update metadata; preserving files.");
	return value;
}
std::string Field(const json& object, const char* name) {
	return object.is_object() && object.contains(name) && object[name].is_string() ?
		object[name].get<std::string>() : "";
}
std::string Hash(const unsigned char* data, size_t size, const wchar_t* algorithmName) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::string digest;
	do {
		if(BCryptOpenAlgorithmProvider(&algorithm, algorithmName, nullptr, 0) < 0) break;
		ULONG objectSize = 0, hashSize = 0, read = 0;
		if(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
			sizeof objectSize, &read, 0) < 0 ||
			BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize),
			sizeof hashSize, &read, 0) < 0) break;
		std::vector<unsigned char> object(objectSize), output(hashSize);
		if(BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) break;
		if(size > ULONG_MAX || BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0 ||
			BCryptFinishHash(hash, output.data(), hashSize, 0) < 0) break;
		static const char digits[] = "0123456789abcdef";
		for(unsigned char b : output) { digest.push_back(digits[b >> 4]); digest.push_back(digits[b & 15]); }
	} while(false);
	if(hash) BCryptDestroyHash(hash);
	if(algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
	if(digest.empty()) Fail("Cannot calculate update checksum.");
	return digest;
}
std::string Sha256(const std::vector<unsigned char>& data) {
	return Hash(data.data(), data.size(), BCRYPT_SHA256_ALGORITHM);
}
std::string Sha256File(const std::wstring& root, const std::wstring& path, size_t limit = MAX_PACKAGE) {
	return Sha256(ReadBytes(root, path, limit));
}
std::string GitBlobSha1(const std::vector<unsigned char>& data) {
	const std::string prefix = "blob " + std::to_string(data.size()) + std::string(1, '\0');
	std::vector<unsigned char> buffer(prefix.begin(), prefix.end());
	buffer.insert(buffer.end(), data.begin(), data.end());
	return Hash(buffer.data(), buffer.size(), BCRYPT_SHA1_ALGORITHM);
}
std::string RandomHex() {
	unsigned char bytes[16]{};
	if(BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
		Fail("Cannot create update transaction ID.");
	static const char digits[] = "0123456789abcdef";
	std::string result;
	for(unsigned char b : bytes) { result.push_back(digits[b >> 4]); result.push_back(digits[b & 15]); }
	return result;
}
std::string UtcNow() {
	SYSTEMTIME value{};
	GetSystemTime(&value);
	char result[32]{};
	std::snprintf(result, sizeof result, "%04u-%02u-%02uT%02u:%02u:%02uZ",
		value.wYear, value.wMonth, value.wDay, value.wHour, value.wMinute, value.wSecond);
	return result;
}
void WriteBytes(const std::wstring& root, const std::wstring& path,
	const unsigned char* bytes, size_t length) {
	SafePath(root, path);
	Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL, nullptr));
	if(!file.good()) Fail("Cannot create update file.");
	size_t offset = 0;
	while(offset < length) {
		DWORD written = 0;
		const DWORD ask = static_cast<DWORD>(std::min<size_t>(65536, length - offset));
		if(!WriteFile(file.value, bytes + offset, ask, &written, nullptr) || !written)
			Fail("Cannot write update file.");
		offset += written;
	}
	if(!FlushFileBuffers(file.value)) Fail("Cannot flush update file.");
}
void WriteJson(const std::wstring& root, const std::wstring& path, const json& value) {
	const auto temporary = Join(Parent(path), L".write-" + Wide(RandomHex()));
	const auto text = value.dump() + "\n";
	WriteBytes(root, temporary, reinterpret_cast<const unsigned char*>(text.data()), text.size());
	if(Exists(path)) {
		if(!ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH,
			nullptr, nullptr)) { DeleteFileW(temporary.c_str()); Fail("Cannot replace update metadata."); }
	} else if(!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(temporary.c_str()); Fail("Cannot publish update metadata.");
	}
}
void Remove(const std::wstring& root, const std::wstring& path) {
	if(!Exists(path)) return;
	SafePath(root, path, false);
	if(!DeleteFileW(path.c_str())) Fail("Cannot remove managed update file.");
}
void Copy(const std::wstring& root, const std::wstring& source, const std::wstring& target) {
	SafePath(root, source, false);
	SafePath(root, target);
	if(!CopyFileW(source.c_str(), target.c_str(), TRUE)) Fail("Cannot copy managed update file.");
}
struct Paths {
	std::wstring root, state, versions, expansions, active, installed, pending,
		journal, health, failed, stageLock, activationLock, gameLease,
		baseJournal, baseInstalled;
	explicit Paths(const std::wstring& runtime) : root(runtime),
		state(Join(root, L"card-updates")), versions(Join(state, L"versions")),
		expansions(Join(root, L"expansions")), active(Join(expansions, L"ygopro-super-pre.ypk")),
		installed(Join(state, L"active.json")), pending(Join(state, L"pending.json")),
		journal(Join(state, L"journal.json")), health(Join(state, L"startup-health.json")),
		failed(Join(state, L"failed.json")), stageLock(Join(versions, L".stage.lock")),
		activationLock(Join(state, L"activation.lock")),
		gameLease(Join(state, L"game-live.lock")),
		baseJournal(Join(state, L"base-install.json")),
		baseInstalled(Join(state, L"base-installed.json")) {
		const DWORD attrs = GetFileAttributesW(root.c_str());
		if(attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
			(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) Fail("Unsafe game folder.");
	}
};
void RequireNoRunningGame(const Paths& p) {
	Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if(!snapshot.good()) Fail("Cannot check for running game processes.");
	PROCESSENTRY32W entry{sizeof entry};
	if(!Process32FirstW(snapshot.value, &entry))
		Fail("Cannot enumerate running game processes.");
	const std::wstring expected = Join(p.root, L"YGOPro.exe");
	do {
		if(_wcsicmp(entry.szExeFile, L"YGOPro.exe")) continue;
		Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
		if(!process.good()) Fail("A game process cannot be inspected; close games before changing card data.");
		wchar_t path[32768]{};
		DWORD length = 32768;
		if(!QueryFullProcessImageNameW(process.value, 0, path, &length))
			Fail("A game process path cannot be inspected; close games before changing card data.");
		if(!_wcsicmp(path, expected.c_str()))
			Fail("Close the running game before changing card data.");
	} while(Process32NextW(snapshot.value, &entry));
}
Handle Lock(const Paths& p, const std::wstring& path) {
	SafePath(p.root, path);
	Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	if(!file.good()) Fail("Another update operation is running.");
	return file;
}
Handle LockGameMutation(const Paths& p) {
	SafePath(p.root, p.gameLease);
	Handle file(CreateFileW(p.gameLease.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if(!file.good()) Fail("Game startup or active play holds the card-data lease; close the game before changing card data.");
	return file;
}

uint16_t Le16(const std::vector<unsigned char>& bytes, size_t at) {
	if(at > bytes.size() || bytes.size() - at < 2) Fail("Truncated YPK archive.");
	return static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}
uint32_t Le32(const std::vector<unsigned char>& bytes, size_t at) {
	if(at > bytes.size() || bytes.size() - at < 4) Fail("Truncated YPK archive.");
	return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
		(static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}
struct ZipEntry {
	std::string name;
	uint32_t offset{}, compressed{}, expanded{}, crc{};
	uint16_t method{}, flags{};
};
bool SafeZipName(const std::string& name) {
	if(name.empty() || name[0] == '/' || name.find('\\') != std::string::npos) return false;
	size_t at = 0;
	while(at < name.size()) {
		const auto end = name.find('/', at);
		const auto part = name.substr(at, end == std::string::npos ? end : end - at);
		if(part == "." || part == ".." || part.empty())
			return end == name.size() - 1 && at != 0 && part.empty();
		if(end == std::string::npos) break;
		at = end + 1;
	}
	for(unsigned char c : name)
		if(!(std::isalnum(c) || c == '.' || c == '_' || c == '/' || c == ' ' || c == '-'))
			return false;
	return true;
}
std::vector<ZipEntry> ZipIndex(const std::vector<unsigned char>& bytes,
	unsigned minimum = 50, unsigned maximum = 5000) {
	if(bytes.size() < 22 || bytes.size() > MAX_PACKAGE) Fail("YPK archive size is invalid.");
	size_t eocd = std::string::npos;
	const size_t lower = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
	for(size_t at = bytes.size() - 22;; --at) {
		if(Le32(bytes, at) == 0x06054b50) { eocd = at; break; }
		if(at == lower) break;
	}
	if(eocd == std::string::npos || Le16(bytes, eocd + 4) || Le16(bytes, eocd + 6) ||
		Le16(bytes, eocd + 8) != Le16(bytes, eocd + 10) ||
		Le16(bytes, eocd + 10) == 0xffff ||
		eocd + 22 + Le16(bytes, eocd + 20) != bytes.size())
		Fail("Unsupported YPK ZIP directory.");
	const uint16_t count = Le16(bytes, eocd + 10);
	const uint32_t directorySize = Le32(bytes, eocd + 12);
	const uint32_t directoryOffset = Le32(bytes, eocd + 16);
	if(count < minimum || count > maximum || directoryOffset == 0xffffffff ||
		directorySize == 0xffffffff ||
		static_cast<uint64_t>(directoryOffset) + directorySize > eocd)
		Fail("Unexpected YPK ZIP directory bounds.");
	std::vector<ZipEntry> entries;
	std::set<std::string> seen;
	size_t at = directoryOffset;
	uint64_t expandedTotal = 0;
	for(unsigned i = 0; i < count; ++i) {
		if(Le32(bytes, at) != 0x02014b50 || at > bytes.size() || bytes.size() - at < 46)
			Fail("Malformed YPK ZIP entry.");
		ZipEntry entry;
		entry.flags = Le16(bytes, at + 8);
		entry.method = Le16(bytes, at + 10);
		entry.crc = Le32(bytes, at + 16);
		entry.compressed = Le32(bytes, at + 20);
		entry.expanded = Le32(bytes, at + 24);
		const uint16_t nameSize = Le16(bytes, at + 28);
		const uint16_t extraSize = Le16(bytes, at + 30);
		const uint16_t commentSize = Le16(bytes, at + 32);
		entry.offset = Le32(bytes, at + 42);
		const uint32_t external = Le32(bytes, at + 38);
		if(!nameSize || static_cast<uint64_t>(at) + 46 + nameSize + extraSize + commentSize > bytes.size() ||
			entry.offset == 0xffffffff || entry.compressed == 0xffffffff ||
			entry.expanded > MAX_PACKAGE || (entry.flags & 1) ||
			(entry.method != 0 && entry.method != 8) ||
			((external >> 16) & 0170000) == 0120000)
			Fail("Unsupported or unsafe YPK ZIP member.");
		entry.name.assign(reinterpret_cast<const char*>(bytes.data() + at + 46), nameSize);
		if(!SafeZipName(entry.name) || !seen.insert(Lower(entry.name)).second)
			Fail("Unsafe or duplicate YPK ZIP member.");
		expandedTotal += entry.expanded;
		if(expandedTotal > MAX_EXPANDED) Fail("YPK expands beyond 512 MiB.");
		entries.push_back(entry);
		at += 46 + nameSize + extraSize + commentSize;
	}
	if(at != static_cast<size_t>(directoryOffset) + directorySize)
		Fail("YPK ZIP directory has unexpected data.");
	return entries;
}
std::vector<unsigned char> ZipPayload(const std::vector<unsigned char>& archive, const ZipEntry& entry) {
	const size_t at = entry.offset;
	if(Le32(archive, at) != 0x04034b50 || at > archive.size() || archive.size() - at < 30)
		Fail("Malformed YPK member header.");
	const uint16_t nameSize = Le16(archive, at + 26), extraSize = Le16(archive, at + 28);
	if(Le16(archive, at + 6) != entry.flags || Le16(archive, at + 8) != entry.method ||
		nameSize != entry.name.size() ||
		static_cast<uint64_t>(at) + 30 + nameSize + extraSize + entry.compressed > archive.size())
		Fail("YPK member header differs from directory.");
	const size_t payloadAt = at + 30 + nameSize + extraSize;
	if(std::memcmp(archive.data() + at + 30, entry.name.data(), nameSize))
		Fail("YPK member name differs from directory.");
	std::vector<unsigned char> output(entry.expanded);
	if(entry.method == 0) {
		if(entry.compressed != entry.expanded) Fail("Stored YPK member size differs.");
		std::copy(archive.begin() + payloadAt, archive.begin() + payloadAt + entry.compressed,
			output.begin());
	} else {
		z_stream stream{};
		if(inflateInit2(&stream, -MAX_WBITS) != Z_OK) Fail("Cannot initialize YPK decompression.");
		unsigned char dummy{};
		stream.next_in = const_cast<Bytef*>(archive.data() + payloadAt);
		stream.avail_in = entry.compressed;
		stream.next_out = output.empty() ? &dummy : output.data();
		stream.avail_out = output.empty() ? 1 : entry.expanded;
		const int code = inflate(&stream, Z_FINISH);
		const bool valid = code == Z_STREAM_END && stream.total_in == entry.compressed &&
			stream.total_out == entry.expanded;
		inflateEnd(&stream);
		if(!valid) Fail("Invalid YPK compressed member.");
	}
	const uLong crc = crc32(0, output.empty() ? Z_NULL : output.data(),
		static_cast<uInt>(output.size()));
	if(crc != entry.crc) Fail("YPK member CRC mismatch.");
	return output;
}
int SqliteRows(const std::vector<unsigned char>& bytes) {
	if(bytes.size() < 100 || std::memcmp(bytes.data(), "SQLite format 3\0", 16))
		Fail("CDB header is invalid.");
	sqlite3* db = nullptr;
	if(sqlite3_open(":memory:", &db) != SQLITE_OK) { if(db) sqlite3_close(db); Fail("Cannot validate CDB."); }
	unsigned char* copy = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
	if(!copy) { sqlite3_close(db); Fail("Cannot allocate CDB validation memory."); }
	std::memcpy(copy, bytes.data(), bytes.size());
	// Some reviewed CDBs have WAL read/write header flags but no sidecar files.
	// In-memory, read-only validation has no WAL file to open. Normalize only
	// this private validation copy; the downloaded/staged bytes and their pinned
	// SHA-256 remain untouched.
	if(copy[18] == 2 && copy[19] == 2) { copy[18] = 1; copy[19] = 1; }
	if(sqlite3_deserialize(db, "main", copy, static_cast<sqlite3_int64>(bytes.size()),
		static_cast<sqlite3_int64>(bytes.size()), SQLITE_DESERIALIZE_FREEONCLOSE |
		SQLITE_DESERIALIZE_READONLY) != SQLITE_OK) {
		sqlite3_free(copy); sqlite3_close(db); Fail("CDB is not a readable SQLite database.");
	}
	struct DbClose { sqlite3* db; ~DbClose() { sqlite3_close(db); } } close{db};
	const ULONGLONG deadline = GetTickCount64() + 10000;
	sqlite3_progress_handler(db, 1000, [](void* value) -> int {
		return GetTickCount64() > *static_cast<ULONGLONG*>(value) ? 1 : 0;
	}, const_cast<ULONGLONG*>(&deadline));
	auto text = [db](const char* sql) -> std::string {
		sqlite3_stmt* statement = nullptr;
		if(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
			Fail("CDB query preparation failed.");
		struct Finalize { sqlite3_stmt* s; ~Finalize() { sqlite3_finalize(s); } } finish{statement};
		if(sqlite3_step(statement) != SQLITE_ROW) Fail("CDB query failed.");
		const unsigned char* value = sqlite3_column_text(statement, 0);
		return value ? reinterpret_cast<const char*>(value) : "";
	};
	if(text("PRAGMA quick_check(1)") != "ok") Fail("CDB quick_check failed.");
	auto columns = [db](const char* table) {
		std::set<std::string> names;
		const std::string sql = std::string("PRAGMA table_info(") + table + ")";
		sqlite3_stmt* statement = nullptr;
		if(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
			Fail("CDB schema query failed.");
		int step = SQLITE_OK;
		while((step = sqlite3_step(statement)) == SQLITE_ROW) {
			const unsigned char* value = sqlite3_column_text(statement, 1);
			if(value) names.insert(reinterpret_cast<const char*>(value));
		}
		sqlite3_finalize(statement);
		if(step != SQLITE_DONE) Fail("CDB schema query did not complete.");
		return names;
	};
	const auto data = columns("datas"), descriptions = columns("texts");
	for(const char* name : {"id","ot","alias","setcode","type","atk","def","level",
		"race","attribute","category"})
		if(!data.count(name)) Fail("CDB datas schema is incomplete.");
	for(const char* name : {"id","name","desc"})
		if(!descriptions.count(name)) Fail("CDB texts schema is incomplete.");
	const int rows = std::stoi(text("SELECT count(*) FROM datas"));
	if(rows < 1 || rows > 200000 ||
		std::stoi(text("SELECT count(*) FROM texts")) != rows ||
		std::stoi(text("SELECT count(*) FROM datas LEFT JOIN texts ON datas.id=texts.id WHERE texts.id IS NULL")) != 0)
		Fail("CDB datas/texts rows do not match.");
	return rows;
}
json ValidatePackage(const std::vector<unsigned char>& bytes) {
	const auto entries = ZipIndex(bytes);
	std::map<std::string, std::vector<unsigned char>> databases;
	int scripts = 0;
	uint64_t expanded = 0;
	bool strings = false, constant = false, procedure = false;
	for(const auto& entry : entries) {
		const auto payload = ZipPayload(bytes, entry); // All CRCs and deflate streams are checked.
		expanded += entry.expanded;
		if(entry.name == "test-release.cdb" || entry.name == "test-update.cdb")
			databases.emplace(entry.name, payload);
		if(entry.name == "test-strings.conf" && !payload.empty()) strings = true;
		if(entry.name == "script/constant.lua" && !payload.empty()) constant = true;
		if(entry.name == "script/procedure.lua" && !payload.empty()) procedure = true;
		if(entry.name.size() > 12 && entry.name.compare(0, 8, "script/c") == 0 &&
			entry.name.compare(entry.name.size() - 4, 4, ".lua") == 0) {
			bool numeric = true;
			for(size_t i = 8; i < entry.name.size() - 4; ++i)
				if(!std::isdigit(static_cast<unsigned char>(entry.name[i]))) numeric = false;
			if(numeric) ++scripts;
		}
	}
	if(databases.size() != 2 || !strings || !constant || !procedure || scripts < 50)
		Fail("YPK lacks required card data or scripts.");
	return json{{"scripts",scripts},{"release_rows",SqliteRows(databases.at("test-release.cdb"))},
		{"update_rows",SqliteRows(databases.at("test-update.cdb"))},
		{"entries",entries.size()},{"expanded_bytes",expanded}};
}

std::vector<unsigned char> HttpGet(const wchar_t* host, const std::wstring& path,
	size_t limit, int seconds) {
	Internet session(WinHttpOpen(L"YGOPROMCP-CardData-Update/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0));
	if(!session.good()) Fail("Cannot initialize secure update connection.");
	if(!WinHttpSetTimeouts(session.value, 10000, 10000, seconds * 1000, seconds * 1000))
		Fail("Cannot set update network timeout.");
	Internet connection(WinHttpConnect(session.value, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
	if(!connection.good()) Fail("Cannot connect to update provider.");
	Internet request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if(!request.good()) Fail("Cannot prepare update request.");
	DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
	if(!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY,
		&redirects, sizeof redirects) ||
		!WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY,
		&autologon, sizeof autologon))
		Fail("Cannot enforce update request policy.");
	const wchar_t* headers = L"Accept: application/vnd.github+json\r\n"
		L"X-GitHub-Api-Version: 2022-11-28\r\n";
	if(!WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1),
		WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request.value, nullptr))
		Fail("Update provider request failed.");
	DWORD code = 0, size = sizeof code;
	if(!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE |
		WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
		WINHTTP_NO_HEADER_INDEX) || code != 200)
		Fail("Update provider returned HTTP " + std::to_string(code) + ".");
	std::vector<unsigned char> result;
	unsigned char buffer[65536];
	for(;;) {
		DWORD got = 0;
		if(!WinHttpReadData(request.value, buffer, sizeof buffer, &got))
			Fail("Update provider response failed.");
		if(!got) break;
		if(result.size() + got > limit) Fail("Update response exceeds its limit.");
		result.insert(result.end(), buffer, buffer + got);
	}
	return result;
}
json JsonBytes(const std::vector<unsigned char>& bytes) {
	const auto result = json::parse(bytes.begin(), bytes.end(), nullptr, false);
	if(result.is_discarded()) Fail("Update provider returned invalid JSON.");
	return result;
}
json Feed() {
	const auto commits = JsonBytes(HttpGet(L"api.github.com",
		L"/repos/ElderLich/Pre-Release-Translations/commits?path=EN%2Fygopro-super-pre.ypk&per_page=1",
		1048576, 30));
	if(!commits.is_array() || commits.size() != 1 || !commits[0].is_object())
		Fail("Cannot establish the English package revision.");
	const std::string revision = Field(commits[0], "sha");
	if(!Hex(revision, 40)) Fail("Invalid English package revision.");
	const auto tree = JsonBytes(HttpGet(L"api.github.com",
		L"/repos/ElderLich/Pre-Release-Translations/git/trees/" + Wide(revision) + L"?recursive=1",
		8388608, 30));
	if(!tree.is_object() || tree.value("truncated", true) ||
		!tree.contains("tree") || !tree["tree"].is_array())
		Fail("Provider tree was truncated or invalid.");
	std::string blob;
	uint64_t bytes = 0;
	int matches = 0;
	for(const auto& entry : tree["tree"]) {
		if(!entry.is_object() || Field(entry, "path") != "EN/ygopro-super-pre.ypk" ||
			Field(entry, "type") != "blob") continue;
		++matches;
		blob = Field(entry, "sha");
		if(!entry.contains("size") || !entry["size"].is_number_unsigned())
			Fail("Provider package size is missing.");
		bytes = entry["size"].get<uint64_t>();
	}
	if(matches != 1 || !Hex(blob, 40) || bytes < 1 || bytes > MAX_PACKAGE)
		Fail("No valid English package blob at provider revision.");
	std::string published;
	try { published = commits[0].at("commit").at("committer").at("date").get<std::string>(); }
	catch(...) { Fail("Provider publication date is missing."); }
	if(published.size() < 20 || published[19] != 'Z')
		Fail("Provider publication date is invalid.");
	std::tm tm{};
	if(std::sscanf(published.c_str(), "%d-%d-%dT%d:%d:%dZ", &tm.tm_year,
		&tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
		Fail("Provider publication date is invalid.");
	tm.tm_year -= 1900; tm.tm_mon -= 1;
	const time_t when = _mkgmtime(&tm), now = std::time(nullptr);
	const bool stale = when < 0 || now < when || std::difftime(now, when) > 14 * 86400.0;
	return json{{"provider","ElderLich/Pre-Release-Translations"},
		{"channel","prerelease-English"},{"revision",revision},{"blob_sha1",blob},
		{"bytes",bytes},{"published_utc",published},{"checked_utc",UtcNow()},
		{"stale",stale},{"scope","Available MyCard SuperPre English cards; not a guarantee of every announced card."}};
}
json CoreProof(const Paths& p, bool requireUtility = true) {
	json result{{"compatible",false},{"reason",""},{"core_commit",""},
		{"base_script_commit",""},{"exe_sha256",""},{"utility_sha256",""}};
	try {
		const auto exe = Join(p.root, L"YGOPro.exe");
		const auto utility = Join(p.root, L"script\\utility.lua");
		const auto manifest = ReadJson(p.root, Join(p.root, L"build-info.json"));
		if(!manifest.is_object()) Fail("Build manifest is missing.");
		result["core_commit"] = Field(manifest, "core_commit");
		result["base_script_commit"] = Field(manifest, "base_script_commit");
		if(Field(manifest, "core_commit") != CORE_PIN ||
			Field(manifest, "base_script_commit") != SCRIPT_PIN)
			Fail("Build manifest core/base-script pins do not match the tested pair.");
		result["exe_sha256"] = Sha256File(p.root, exe, 67108864);
		if(Field(manifest, "exe_sha256") != result["exe_sha256"] ||
			Field(result, "exe_sha256") != TESTED_GAME_SHA256)
			Fail("Installed executable is not an allowlisted tested build.");
		if(requireUtility) {
			result["utility_sha256"] = Sha256File(p.root, utility, 1048576);
			if(Field(result, "utility_sha256") != UTILITY_SHA256 &&
				Field(result, "utility_sha256") != BASE_UTILITY_SHA256)
				Fail("Installed base utility.lua differs from tested source.");
		}
		const std::wstring output = Join(p.state, L".probe-out-" + Wide(RandomHex()));
		const std::wstring errors = Join(p.state, L".probe-err-" + Wide(RandomHex()));
		Directory(p.root, p.state);
		struct ProbeCleanup {
			std::wstring output, errors;
			~ProbeCleanup() { DeleteFileW(output.c_str()); DeleteFileW(errors.c_str()); }
		} cleanup{output, errors};
		SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
		Handle childOut(CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
			&sa, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
		Handle childErr(CreateFileW(errors.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
			&sa, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
		Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			&sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if(!childOut.good() || !childErr.good() || !input.good()) Fail("Cannot capture linked-core proof.");
		STARTUPINFOW startup{sizeof startup};
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = input.value; startup.hStdOutput = childOut.value;
		startup.hStdError = childErr.value;
		PROCESS_INFORMATION child{};
		std::wstring command = L"\"" + exe + L"\" --card-update-probe";
		if(!CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, p.root.c_str(), &startup, &child))
			Fail("Cannot run linked-core proof.");
		const DWORD wait = WaitForSingleObject(child.hProcess, 5000);
		if(wait == WAIT_TIMEOUT) { TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, 5000); }
		DWORD code = 1; GetExitCodeProcess(child.hProcess, &code);
		CloseHandle(child.hThread); CloseHandle(child.hProcess);
		// Child output handles must close before reading, particularly on Windows.
		CloseHandle(childOut.value); childOut.value = INVALID_HANDLE_VALUE;
		CloseHandle(childErr.value); childErr.value = INVALID_HANDLE_VALUE;
		if(wait != WAIT_OBJECT_0 || code) Fail("Linked-core proof failed.");
		const std::string stdoutText = ReadText(p.root, output, 4096);
		const std::string stderrText = ReadText(p.root, errors, 4096);
		const std::string marker = std::string("YGOPROMCP_CARD_UPDATE_CORE_V1 ") + CORE_PIN;
		if(!stdoutText.empty() || (stderrText != marker + "\n" && stderrText != marker + "\r\n"))
			Fail("Installed executable did not prove the linked core API.");
		result["compatible"] = true;
		result["reason"] = requireUtility ?
			"Allowlisted executable hash, linked-core API and base utility verified." :
			"Allowlisted executable hash and linked-core API verified; base scripts are pending.";
	} catch(const std::exception& ex) { result["reason"] = ex.what(); }
	return result;
}

std::string ActiveHash(const Paths& p) {
	return Exists(p.active) ? Sha256File(p.root, p.active) : "";
}
json Staged(const Paths& p, const std::string& revision) {
	if(!Hex(revision, 40)) Fail("Invalid staged revision.");
	const auto folder = Join(p.versions, Wide(revision));
	const auto package = Join(folder, L"package.ypk");
	const auto manifest = ReadJson(p.root, Join(folder, L"manifest.json"));
	if(!manifest.is_object() || !Exists(package)) Fail("Staged package is missing.");
	if(Field(manifest, "revision") != revision || Field(manifest, "core_pin") != CORE_PIN ||
		Field(manifest, "base_script_pin") != SCRIPT_PIN ||
		!Hex(Field(manifest, "sha256"), 64) ||
		Sha256File(p.root, package) != Field(manifest, "sha256"))
		Fail("Staged package bytes or compatibility metadata changed.");
	return manifest;
}
void Conflicts(const Paths& p) {
	if(!Exists(p.expansions)) return;
	SafePath(p.root, p.expansions, false);
	WIN32_FIND_DATAW entry{};
	HANDLE find = FindFirstFileW(Join(p.expansions, L"*").c_str(), &entry);
	if(find == INVALID_HANDLE_VALUE) return;
	try {
		do {
			if(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			const std::wstring name = entry.cFileName;
			if(_wcsicmp(name.c_str(), L"ygopro-super-pre.ypk") == 0) continue;
			const auto path = Join(p.expansions, name);
			SafePath(p.root, path, false);
			const auto dot = name.find_last_of(L'.');
			const auto extension = dot == std::wstring::npos ? L"" : name.substr(dot);
			if(_wcsicmp(extension.c_str(), L".cdb") == 0 ||
				_wcsicmp(extension.c_str(), L".conf") == 0)
				Fail("Other expansion data may override managed cards; preserving files.");
			if(_wcsicmp(extension.c_str(), L".ypk") != 0 &&
				_wcsicmp(extension.c_str(), L".zip") != 0) continue;
			const auto archive = ReadBytes(p.root, path, MAX_PACKAGE);
			for(const auto& member : ZipIndex(archive)) {
				const auto lower = Lower(member.name);
				const auto slash = lower.find_last_of('/');
				const auto basename = slash == std::string::npos ? lower : lower.substr(slash + 1);
				if((basename.size() >= 4 && basename.compare(basename.size() - 4, 4, ".cdb") == 0) ||
					(basename.size() >= 5 && basename.compare(basename.size() - 5, 5, ".conf") == 0) ||
					(lower.compare(0, 7, "script/") == 0 &&
					 (basename == "constant.lua" || basename == "procedure.lua" ||
					  basename == "utility.lua" || (basename.size() >= 6 &&
					  basename[0] == 'c' && basename.compare(basename.size() - 4, 4, ".lua") == 0))))
					Fail("Other expansion may override managed card data or scripts; preserving files.");
			}
		} while(FindNextFileW(find, &entry));
	} catch(...) { FindClose(find); throw; }
	FindClose(find);
}
json Status(const Paths& p) {
	const auto installed = ReadJson(p.root, p.installed);
	const auto pending = ReadJson(p.root, p.pending);
	const auto journal = ReadJson(p.root, p.journal);
	const auto active = ActiveHash(p);
	const bool healthy = installed.is_null() ? active.empty() :
		active == Field(installed, "sha256");
	std::string phase;
	if(!journal.is_null() && active == Field(journal, "new_sha256")) phase = "startup-pending";
	else if(!journal.is_null()) phase = "recovery-needed";
	else if(!healthy) phase = "external-change";
	else if(!pending.is_null()) phase = "restart-required";
	else if(!installed.is_null()) phase = "installed";
	else phase = "not-checked";
	return json{{"state",phase},{"core",CoreProof(p)},
		{"installed_revision",Field(installed,"revision")},
		{"pending_revision",Field(pending,"revision")},
		{"active_sha256",active},{"transaction",Field(journal,"transaction")},
		{"can_rollback",!installed.is_null() && journal.is_null()}};
}
void CheckHealthyActive(const Paths& p, const json& installed) {
	const auto active = ActiveHash(p);
	if((installed.is_null() && !active.empty()) ||
		(!installed.is_null() && active != Field(installed, "sha256")))
		Fail("Active card data changed outside updates. No update was scheduled; preserve the package and resolve the change first.");
}
void Recover(const Paths& p) {
	const auto journal = ReadJson(p.root, p.journal);
	if(journal.is_null()) return;
	RequireNoRunningGame(p);
	const auto transaction = Field(journal, "transaction");
	const auto newHash = Field(journal, "new_sha256");
	const auto oldHash = Field(journal, "old_sha256");
	if(!Hex(transaction, 32) || (!newHash.empty() && !Hex(newHash, 64)))
		Fail("Invalid activation journal; preserving files.");
	const auto backup = Join(p.state, L"backup-" + Wide(transaction) + L".ypk");
	const auto prepared = Join(p.expansions, L".superpre-new-" + Wide(transaction));
	SafePath(p.root, backup); SafePath(p.root, prepared);
	const bool hadActive = journal.value("had_active", false);
	if(hadActive) {
		if(!Hex(oldHash, 64)) Fail("Journal lacks previous package hash.");
		if(Exists(backup)) {
			if(Sha256File(p.root, backup) != oldHash)
				Fail("Activation backup hash mismatch; preserving it.");
			if(Exists(p.active)) {
				const auto current = ActiveHash(p);
				if(current != newHash && current != oldHash)
					Fail("Unknown active package during recovery; preserving it.");
				const auto restore = Join(p.expansions, L".superpre-restore-" + Wide(transaction));
				Copy(p.root, backup, restore);
				if(!ReplaceFileW(p.active.c_str(), restore.c_str(), nullptr,
					REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
					Fail("Cannot restore previous card package.");
			} else if(!MoveFileExW(backup.c_str(), p.active.c_str(), MOVEFILE_WRITE_THROUGH))
				Fail("Cannot restore previous card package.");
			Remove(p.root, backup);
		} else if(ActiveHash(p) != oldHash)
			Fail("Previous card package backup is missing; preserving files.");
	} else if(Exists(p.active)) {
		if(ActiveHash(p) != newHash)
			Fail("Unknown active package during recovery; preserving it.");
		Remove(p.root, p.active);
	}
	if(journal.contains("old_state") && journal["old_state"].is_object())
		WriteJson(p.root, p.installed, journal["old_state"]);
	else Remove(p.root, p.installed);
	Remove(p.root, prepared);
	Remove(p.root, p.health);
	Remove(p.root, p.journal);
}
json Schedule(const Paths& p, const std::string& action, const std::string& requested) {
	if(Exists(p.journal)) Fail("Activation recovery is required first.");
	if(Exists(p.pending)) Fail("An update is already awaiting restart.");
	const auto installed = ReadJson(p.root, p.installed);
	CheckHealthyActive(p, installed);
	std::string revision = requested;
	if(action == "Rollback") {
		if(installed.is_null()) Fail("No managed installation to restore.");
		revision = Field(installed, "previous_revision");
		if(revision.empty()) revision = "none";
	}
	json manifest;
	if(revision == "none") {
		if(action != "Rollback") Fail("Only rollback can restore no overlay.");
	} else {
		manifest = Staged(p, revision);
		Conflicts(p);
	}
	WriteJson(p.root, p.pending, json{{"revision",revision},
		{"sha256",revision == "none" ? "" : Field(manifest,"sha256")},
		{"transaction",RandomHex()}});
	return json{{"state","restart-required"},{"revision",revision}};
}
json Activate(const Paths& p) {
	if(Exists(p.journal) || Exists(p.pending)) RequireNoRunningGame(p);
	if(Exists(p.journal)) {
		const auto old = ReadJson(p.root, p.journal);
		const auto transaction = Field(old, "transaction");
		Recover(p);
		Remove(p.root, p.pending);
		WriteJson(p.root, p.failed, json{{"transaction",transaction},
			{"reason","Previous startup was not confirmed; prior package restored."}});
		return json{{"state","rolled-back-after-interruption"},{"transaction",transaction}};
	}
	const auto pending = ReadJson(p.root, p.pending);
	if(pending.is_null()) return json{{"state","no-pending-update"}};
	const auto transaction = Field(pending, "transaction");
	const auto revision = Field(pending, "revision");
	if(!Hex(transaction, 32)) Fail("Invalid pending transaction.");
	const auto installed = ReadJson(p.root, p.installed);
	CheckHealthyActive(p, installed);
	std::string newHash;
	std::wstring stagedFile;
	if(revision == "none") {
		if(installed.is_null()) Fail("No package to restore.");
	} else {
		const auto stage = Staged(p, revision);
		newHash = Field(stage, "sha256");
		if(Field(pending, "sha256") != newHash) Fail("Pending package hash mismatch.");
		Conflicts(p);
		stagedFile = Join(Join(p.versions, Wide(revision)), L"package.ypk");
	}
	Directory(p.root, p.expansions);
	const auto oldHash = ActiveHash(p);
	const auto backup = Join(p.state, L"backup-" + Wide(transaction) + L".ypk");
	const auto prepared = Join(p.expansions, L".superpre-new-" + Wide(transaction));
	SafePath(p.root, backup); SafePath(p.root, prepared);
	if(!newHash.empty()) {
		Copy(p.root, stagedFile, prepared);
		if(Sha256File(p.root, prepared) != newHash) Fail("Prepared package hash mismatch.");
	}
	Remove(p.root, p.health);
	WriteJson(p.root, p.journal, json{{"transaction",transaction},
		{"had_active",!oldHash.empty()},{"old_sha256",oldHash},
		{"new_sha256",newHash},{"old_state",installed}});
	try {
		if(newHash.empty()) {
			if(!MoveFileExW(p.active.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH))
				Fail("Cannot prepare package rollback.");
		} else if(!oldHash.empty()) {
			if(!ReplaceFileW(p.active.c_str(), prepared.c_str(), backup.c_str(),
				REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) Fail("Cannot activate card package.");
		} else if(!MoveFileExW(prepared.c_str(), p.active.c_str(), MOVEFILE_WRITE_THROUGH))
			Fail("Cannot activate card package.");
		if(ActiveHash(p) != newHash) Fail("Provisional package hash mismatch.");
		return json{{"state","startup-pending"},{"revision",revision},
			{"transaction",transaction},{"active_sha256",newHash}};
	} catch(...) {
		Recover(p);
		Remove(p.root, p.pending);
		WriteJson(p.root, p.failed, json{{"transaction",transaction},
			{"reason","Activation failed; prior package restored."}});
		throw;
	}
}
json Confirm(const Paths& p, const std::string& transaction,
	const std::string& nonce, DWORD childPid, HANDLE childProcess) {
	if(!Hex(nonce, 32) || !childProcess || childProcess == INVALID_HANDLE_VALUE ||
		!childPid || GetProcessId(childProcess) != childPid ||
		WaitForSingleObject(childProcess, 0) != WAIT_TIMEOUT)
		Fail("Launcher-held game process proof is missing or exited.");
	wchar_t path[32768]{};
	DWORD length = 32768;
	if(!QueryFullProcessImageNameW(childProcess, 0, path, &length) ||
		_wcsicmp(path, Join(p.root, L"YGOPro.exe").c_str()))
		Fail("Health process is not this game executable.");
	const auto journal = ReadJson(p.root, p.journal), pending = ReadJson(p.root, p.pending);
	const auto health = ReadJson(p.root, p.health);
	if(journal.is_null() || pending.is_null() || health.is_null() ||
		transaction != Field(journal,"transaction") || transaction != Field(pending,"transaction") ||
		transaction != Field(health,"transaction") ||
		nonce != Field(health,"launcher_nonce") ||
		!health.contains("process_id") || !health["process_id"].is_number_unsigned() ||
		health["process_id"].get<uint64_t>() != childPid ||
		Field(health,"active_sha256") != Field(journal,"new_sha256") ||
		Field(health,"managed_package_sha256") != Field(journal,"new_sha256") ||
		!health.value("initialized",false) || !health.value("database_loaded",false) ||
		!health.value("scripts_loaded",false) || ActiveHash(p) != Field(journal,"new_sha256"))
		Fail("Startup health receipt or active package does not match transaction.");
	const auto revision = Field(pending, "revision");
	if(Field(journal,"new_sha256").empty()) Remove(p.root, p.installed);
	else WriteJson(p.root, p.installed, json{{"revision",revision},
		{"sha256",Field(journal,"new_sha256")},
		{"previous_revision",Field(journal.value("old_state",json::object()),"revision")},
		{"activated_utc",UtcNow()}});
	Remove(p.root, p.pending); Remove(p.root, p.journal); Remove(p.root, p.health);
	Remove(p.root, Join(p.state, L"backup-" + Wide(transaction) + L".ypk"));
	Remove(p.root, p.failed);
	return json{{"state","installed"},{"revision",revision},{"transaction",transaction}};
}
json Abort(const Paths& p, const std::string& transaction) {
	RequireNoRunningGame(p);
	const auto journal = ReadJson(p.root, p.journal);
	if(journal.is_null() || transaction != Field(journal,"transaction"))
		Fail("No matching provisional activation.");
	Recover(p); Remove(p.root, p.pending);
	WriteJson(p.root, p.failed, json{{"transaction",transaction},
		{"reason","Launcher reported startup failure; prior package restored."}});
	return json{{"state","rolled-back"},{"transaction",transaction}};
}

int CleanupIncoming(const Paths& p) {
	if(!Exists(p.versions)) return 0;
	SafePath(p.root, p.versions, false);
	WIN32_FIND_DATAW entry{};
	HANDLE find = FindFirstFileW(Join(p.versions, L".incoming-*").c_str(), &entry);
	if(find == INVALID_HANDLE_VALUE) return 0;
	int removed = 0;
	try {
		do {
			const std::wstring name = entry.cFileName;
			if(name.size() != 42 || name.compare(0, 10, L".incoming-") != 0 ||
				!Hex(Utf8(name.substr(10)), 32) ||
				!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
				(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				Fail("Unknown staging directory; preserving it.");
			const auto folder = Join(p.versions, name);
			WIN32_FIND_DATAW item{};
			HANDLE children = FindFirstFileW(Join(folder, L"*").c_str(), &item);
			if(children != INVALID_HANDLE_VALUE) {
				try {
					do {
						const std::wstring childName = item.cFileName;
						if(childName == L"." || childName == L"..") continue;
						if((item.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
							(childName != L"package.ypk" && childName != L"manifest.json"))
							Fail("Unknown staging file; preserving it.");
						Remove(p.root, Join(folder, childName));
					} while(FindNextFileW(children, &item));
				} catch(...) { FindClose(children); throw; }
				FindClose(children);
			}
			SafePath(p.root, folder, false);
			if(!RemoveDirectoryW(folder.c_str())) Fail("Cannot remove incomplete staging folder.");
			++removed;
		} while(FindNextFileW(find, &entry));
	} catch(...) { FindClose(find); throw; }
	FindClose(find);
	return removed;
}
json Download(const Paths& p, const std::string& revision) {
	if(!Hex(revision, 40)) Fail("Checked update revision is invalid.");
	const auto core = CoreProof(p);
	if(!core.value("compatible", false))
		Fail("This client lacks the exact tested core/base-script build pins.");
	Directory(p.root, p.state); Directory(p.root, p.versions);
	auto lock = Lock(p, p.stageLock);
	CleanupIncoming(p);
	const auto feed = Feed();
	if(revision != Field(feed, "revision")) Fail("Candidate revision changed; check again.");
	const auto target = Join(p.versions, Wide(revision));
	SafePath(p.root, target);
	if(Exists(target)) Fail("Revision is already staged.");
	const auto incoming = Join(p.versions, L".incoming-" + Wide(RandomHex()));
	Directory(p.root, incoming);
	try {
		const auto bytes = HttpGet(L"raw.githubusercontent.com",
			L"/ElderLich/Pre-Release-Translations/" + Wide(revision) +
			L"/EN/ygopro-super-pre.ypk", MAX_PACKAGE, 120);
		if(bytes.size() != feed.at("bytes").get<uint64_t>())
			Fail("Download length differs from pinned Git blob.");
		if(GitBlobSha1(bytes) != Field(feed,"blob_sha1"))
			Fail("Download differs from pinned Git blob.");
		const auto details = ValidatePackage(bytes);
		const auto package = Join(incoming, L"package.ypk");
		WriteBytes(p.root, package, bytes.data(), bytes.size());
		json manifest{{"provider",Field(feed,"provider")},{"channel",Field(feed,"channel")},
			{"revision",revision},{"git_blob_sha1",Field(feed,"blob_sha1")},
			{"sha256",Sha256(bytes)},{"bytes",bytes.size()},
			{"published_utc",Field(feed,"published_utc")},
			{"checked_utc",Field(feed,"checked_utc")},
			{"core_pin",CORE_PIN},{"base_script_pin",SCRIPT_PIN},{"package",details}};
		WriteJson(p.root, Join(incoming, L"manifest.json"), manifest);
		if(!MoveFileExW(incoming.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
			Fail("Cannot publish verified staging folder.");
		return json{{"state","ready-to-apply"},{"manifest",manifest}};
	} catch(...) { CleanupIncoming(p); throw; }
}
std::wstring BaseStage(const Paths& p) { return Join(p.state, L"base-stage"); }
bool PresentationReady(const Paths& p) {
	const auto textures = Join(p.root,L"textures");
	const auto fonts = Join(p.root,L"fonts");
	if(!Exists(textures) || !Exists(fonts)) return false;
	SafePath(p.root,textures,false); SafePath(p.root,fonts,false);
	for(const wchar_t* name : {L"act.png",L"attack.png",L"bg.jpg",L"bg_deck.jpg",
		L"bg_menu.jpg",L"chain.png",L"chaintarget.png",L"equip.png",L"f1.jpg",
		L"f2.jpg",L"f3.jpg",L"field-transparent2.png",L"field-transparent3.png",
		L"field.png",L"field2.png",L"field3.png",L"lim.png",L"lp.png",
		L"lpf.png",L"mask.png",L"negated.png",L"number.png",L"ot.png",
		L"target.png",L"unknown.jpg"}) {
		const auto path = Join(textures,name);
		if(!Exists(path)) return false;
		SafePath(p.root,path,false);
		if(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) return false;
	}
	for(const wchar_t* stem : {L"cover",L"cover2"}) {
		const auto png = Join(textures,std::wstring(stem) + L".png");
		const auto jpg = Join(textures,std::wstring(stem) + L".jpg");
		const auto path = Exists(png) ? png : jpg;
		if(!Exists(path)) return false;
		SafePath(p.root,path,false);
		if(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) return false;
	}
	try {
		return Sha256File(p.root,Join(fonts,L"numFont.otf"), 20000000) == BASE_NUM_FONT_SHA256 &&
			Sha256File(p.root,Join(fonts,L"textFont.otf"), 20000000) == BASE_TEXT_FONT_SHA256;
	} catch(const std::exception&) { return false; }
}
bool ManagedScriptsValid(const Paths& p);
bool ExistingDataCandidate(const Paths& p) {
	if(!PresentationReady(p)) return false;
	try {
		const auto database = ReadBytes(p.root, Join(p.root,L"cards.cdb"), 67108864);
		if(SqliteRows(database) < 1) return false;
		const auto strings = ReadBytes(p.root, Join(p.root,L"strings.conf"), 1048576);
		if(strings.empty()) return false;
		for(const wchar_t* name : {L"utility.lua",L"constant.lua",L"procedure.lua"}) {
			const auto path = Join(Join(p.root,L"script"),name);
			if(!Exists(path) || ReadBytes(p.root,path,1048576).empty()) return false;
		}
		return true;
	} catch(const std::exception&) { return false; }
}
json BaseStatus(const Paths& p) {
	const auto core = CoreProof(p, false);
	if(Exists(p.baseJournal))
		return json{{"state","base-recovery-needed"},{"playable",false},{"core",core}};
	const bool database = Exists(Join(p.root, L"cards.cdb"));
	const bool strings = Exists(Join(p.root, L"strings.conf"));
	const bool scripts = Exists(Join(p.root, L"script"));
	if(database && strings && scripts && Exists(p.baseInstalled)) {
		try {
			const auto receipt = ReadJson(p.root, p.baseInstalled);
			if(Field(receipt,"database_sha256") == BASE_DB_SHA256 &&
				Field(receipt,"strings_sha256") == BASE_STRINGS_SHA256 &&
				Field(receipt,"scripts_sha256") == BASE_SCRIPTS_SHA256 &&
				Sha256File(p.root, Join(p.root,L"cards.cdb"), 8076288) == BASE_DB_SHA256 &&
				Sha256File(p.root, Join(p.root,L"strings.conf"), 37759) == BASE_STRINGS_SHA256 &&
				ManagedScriptsValid(p))
				return PresentationReady(p) ?
					json{{"state",core.value("compatible",false) ? "base-ready" : "incompatible-core"},
						{"playable",core.value("compatible",false)},
						{"script_integrity","pinned-archive-crc32"},{"core",core}} :
					json{{"state","base-installed-needs-presentation"},{"playable",false},
						{"needs_presentation_assets",true},
						{"script_integrity","pinned-archive-crc32"},{"core",core}};
		} catch(const std::exception&) { /* Preserve externally changed data. */ }
	}
	if(Exists(p.baseInstalled))
		return json{{"state","managed-base-changed"},{"playable",false},{"core",core}};
	if(database || strings || scripts)
		return json{{"state",database && strings && scripts &&
			core.value("compatible",false) && ExistingDataCandidate(p) ?
			"existing-data-unverified" : "incomplete-existing-data"},
			{"playable",false},{"core",core}};
	const auto stage = BaseStage(p);
	if(!Exists(stage)) return json{{"state","base-missing"},{"playable",false},{"core",core}};
	SafePath(p.root, stage, false);
	try {
	const auto manifest = ReadJson(p.root, Join(stage, L"manifest.json"));
	if(manifest.is_object() && Field(manifest,"database_sha256") == BASE_DB_SHA256 &&
		Field(manifest,"strings_sha256") == BASE_STRINGS_SHA256 &&
		Field(manifest,"scripts_sha256") == BASE_SCRIPTS_SHA256 &&
		Sha256File(p.root, Join(stage,L"cards.cdb"), 8076288) == BASE_DB_SHA256 &&
		Sha256File(p.root, Join(stage,L"strings.conf"), 37759) == BASE_STRINGS_SHA256 &&
		Sha256File(p.root, Join(stage,L"scripts.zip"), 14077251) == BASE_SCRIPTS_SHA256)
		return json{{"state","verified-base-staged"},{"playable",false},
			{"needs_presentation_assets",true},{"core",core},{"manifest",manifest}};
	} catch(const std::exception&) { /* Incomplete stage remains for explicit cleanup. */ }
	return json{{"state","incomplete-base-stage"},{"playable",false},{"core",core}};
}
json BasePrepare(const Paths& p) {
	const auto prior = BaseStatus(p);
	if(Field(prior,"state") != "base-missing")
		Fail("Base data already exists or staging needs attention; preserving all existing files.");
	if(!prior["core"].value("compatible",false))
		Fail("This executable is not the tested base-data build.");
	const std::wstring databasePath = L"/mycard/ygopro-database/" + Wide(BASE_DB_COMMIT) +
		L"/locales/en-US/cards.cdb";
	const std::wstring stringsPath = L"/mycard/ygopro-database/" + Wide(BASE_DB_COMMIT) +
		L"/locales/en-US/strings.conf";
	const std::wstring scriptsPath = L"/Fluorohydride/ygopro-scripts/zip/" + Wide(SCRIPT_PIN);
	const auto database = HttpGet(L"raw.githubusercontent.com", databasePath, 8076288, 90);
	if(database.size() != 8076288 || Sha256(database) != BASE_DB_SHA256 ||
		SqliteRows(database) != 15019)
		Fail("Pinned English card database failed validation.");
	const auto strings = HttpGet(L"raw.githubusercontent.com", stringsPath, 37759, 90);
	if(strings.size() != 37759 || Sha256(strings) != BASE_STRINGS_SHA256)
		Fail("Pinned English strings failed validation.");
	const auto scripts = HttpGet(L"codeload.github.com", scriptsPath, 14077251, 120);
	if(scripts.size() != 14077251 || Sha256(scripts) != BASE_SCRIPTS_SHA256)
		Fail("Pinned base scripts archive failed validation.");
	const std::string prefix = std::string("ygopro-scripts-") + SCRIPT_PIN + "/";
	unsigned files = 0, cards = 0;
	uint64_t expanded = 0;
	bool constant = false, procedure = false, utility = false, license = false;
	for(const auto& entry : ZipIndex(scripts, 12813, 13000)) {
		if(entry.name == prefix) continue;
		if(entry.name.compare(0, prefix.size(), prefix) != 0)
			Fail("Base scripts ZIP contains an unexpected root.");
		const auto name = entry.name.substr(prefix.size());
		bool card = name.size() >= 6 && name.front() == 'c' &&
			name.compare(name.size() - 4, 4, ".lua") == 0;
		if(card) for(size_t i = 1; i < name.size() - 4; ++i)
			if(!std::isdigit(static_cast<unsigned char>(name[i]))) card = false;
		if(!(card || name == "constant.lua" || name == "procedure.lua" ||
			name == "utility.lua" || name == "LICENSE" || name == ".gitignore"))
			Fail("Base scripts ZIP contains an unexpected file.");
		const auto payload = ZipPayload(scripts, entry);
		if(payload.empty() && name != ".gitignore")
			Fail("Base scripts ZIP contains an empty required file.");
		++files; expanded += payload.size();
		if(card) ++cards;
		if(name == "constant.lua") constant = true;
		if(name == "procedure.lua") procedure = true;
		if(name == "utility.lua") utility = true;
		if(name == "LICENSE") license = true;
	}
	if(files != 12813 || cards != 12808 || expanded != 29329459 ||
		!constant || !procedure || !utility || !license)
		Fail("Base scripts ZIP does not match the reviewed file inventory.");
	const auto stage = BaseStage(p);
	Directory(p.root, p.state);
	SafePath(p.root, stage);
	if(!CreateDirectoryW(stage.c_str(), nullptr))
		Fail("Base staging appeared during preparation; preserving it.");
	SafePath(p.root, stage, false);
	WriteJson(p.root, Join(stage,L"ownership.json"), json{
		{"format","ygopromcp-base-stage-v1"},{"transaction",RandomHex()},
		{"database_sha256",BASE_DB_SHA256},{"strings_sha256",BASE_STRINGS_SHA256},
		{"scripts_sha256",BASE_SCRIPTS_SHA256}});
	WriteBytes(p.root, Join(stage,L"cards.cdb"), database.data(), database.size());
	WriteBytes(p.root, Join(stage,L"strings.conf"), strings.data(), strings.size());
	WriteBytes(p.root, Join(stage,L"scripts.zip"), scripts.data(), scripts.size());
	const json manifest{{"database_commit",BASE_DB_COMMIT},{"scripts_commit",SCRIPT_PIN},
		{"core_commit",CORE_PIN},{"database_sha256",BASE_DB_SHA256},
		{"strings_sha256",BASE_STRINGS_SHA256},{"scripts_sha256",BASE_SCRIPTS_SHA256},
		{"database_rows",15019},{"scripts_files",files},{"checked_utc",UtcNow()},
		{"stage_only",true}};
	WriteJson(p.root, Join(stage,L"manifest.json"), manifest);
	return json{{"state","verified-base-staged"},{"playable",false},
		{"needs_presentation_assets",true},{"manifest",manifest}};
}
json BaseCleanup(const Paths& p) {
	if(Exists(p.baseJournal)) Fail("Base installation recovery is required before clearing staging.");
	if(Exists(p.baseInstalled)) Fail("Installed base integrity needs the verified staging archive; preserving it.");
	const auto stage = BaseStage(p);
	if(!Exists(stage)) return json{{"state","base-stage-absent"}};
	SafePath(p.root, stage, false);
	WIN32_FIND_DATAW entry{};
	const HANDLE find = FindFirstFileW(Join(stage,L"*").c_str(), &entry);
	if(find == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
		Fail("Cannot inspect base staging folder.");
	struct FindGuard { HANDLE handle; ~FindGuard() { if(handle != INVALID_HANDLE_VALUE) FindClose(handle); } } guard{find};
	bool hasFiles = false;
	if(find != INVALID_HANDLE_VALUE) do {
		const std::wstring name = entry.cFileName;
		if(name == L"." || name == L"..") continue;
		hasFiles = true;
		if((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
			(name != L"cards.cdb" && name != L"strings.conf" &&
			 name != L"scripts.zip" && name != L"manifest.json" &&
			 name != L"ownership.json"))
			Fail("Unknown base staging content; preserving it.");
	} while(FindNextFileW(find, &entry));
	if(find != INVALID_HANDLE_VALUE && GetLastError() != ERROR_NO_MORE_FILES)
		Fail("Cannot finish inspecting base staging folder.");
	// An empty directory is safe to clear after an interrupted creation. Once
	// files exist, require our stage marker and exact known content before any
	// removal. A partial or externally changed file remains for manual review.
	if(hasFiles) {
		const auto owner = ReadJson(p.root,Join(stage,L"ownership.json"));
		if(Field(owner,"format") != "ygopromcp-base-stage-v1" ||
			!Hex(Field(owner,"transaction"),32) ||
			Field(owner,"database_sha256") != BASE_DB_SHA256 ||
			Field(owner,"strings_sha256") != BASE_STRINGS_SHA256 ||
			Field(owner,"scripts_sha256") != BASE_SCRIPTS_SHA256)
			Fail("Base staging ownership is unverified; preserving files.");
		for(const auto& item : {std::make_pair(L"cards.cdb",BASE_DB_SHA256),
			std::make_pair(L"strings.conf",BASE_STRINGS_SHA256),
			std::make_pair(L"scripts.zip",BASE_SCRIPTS_SHA256)}) {
			const auto file = Join(stage,item.first);
			if(Exists(file) && Sha256File(p.root,file) != item.second)
				Fail("Base staging content changed; preserving files.");
		}
		if(Exists(Join(stage,L"manifest.json"))) {
			const auto manifest = ReadJson(p.root,Join(stage,L"manifest.json"));
			if(Field(manifest,"database_sha256") != BASE_DB_SHA256 ||
				Field(manifest,"strings_sha256") != BASE_STRINGS_SHA256 ||
				Field(manifest,"scripts_sha256") != BASE_SCRIPTS_SHA256 ||
				!manifest.value("stage_only",false))
				Fail("Base staging manifest changed; preserving files.");
		}
	}
	for(const wchar_t* name : {L"manifest.json",L"scripts.zip",L"strings.conf",L"cards.cdb"})
		Remove(p.root, Join(stage,name));
	Remove(p.root, Join(stage,L"ownership.json"));
	if(!RemoveDirectoryW(stage.c_str())) Fail("Cannot remove base staging folder.");
	return json{{"state","base-stage-cleared"}};
}
std::map<std::string,std::string> BaseScriptHashes(const std::vector<unsigned char>& archive) {
	const std::string prefix = std::string("ygopro-scripts-") + SCRIPT_PIN + "/";
	std::map<std::string,std::string> expected;
	for(const auto& entry : ZipIndex(archive, 12813, 13000)) {
		if(entry.name == prefix) continue;
		if(entry.name.compare(0, prefix.size(), prefix) != 0)
			Fail("Unexpected base script ZIP root.");
		const auto name = entry.name.substr(prefix.size());
		if(name == ".gitignore") continue;
		if(name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
			Fail("Unexpected nested base script path.");
		expected.emplace(Lower(name), Sha256(ZipPayload(archive, entry)));
	}
	if(expected.size() != 12812) Fail("Base script inventory is incomplete.");
	return expected;
}
bool ScriptFileMatches(const std::wstring& path, uint32_t size, uint32_t crc) {
	Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	BY_HANDLE_FILE_INFORMATION info{};
	if(!file.good() || !GetFileInformationByHandle(file.value,&info) ||
		(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
		info.nFileSizeHigh || info.nFileSizeLow != size || size > 1048576) return false;
	std::vector<unsigned char> bytes(size);
	size_t offset = 0;
	while(offset < bytes.size()) {
		DWORD got = 0;
		if(!ReadFile(file.value,bytes.data()+offset,
			static_cast<DWORD>(bytes.size()-offset),&got,nullptr) || !got) return false;
		offset += got;
	}
	return crc32(crc32(0,Z_NULL,0),bytes.data(),static_cast<uInt>(bytes.size())) == crc;
}
bool ManagedScriptsValid(const Paths& p) {
	const auto archivePath = Join(BaseStage(p),L"scripts.zip");
	const auto archive = ReadBytes(p.root,archivePath,14077251);
	if(Sha256(archive) != BASE_SCRIPTS_SHA256) return false;
	const std::string prefix = std::string("ygopro-scripts-") + SCRIPT_PIN + "/";
	std::map<std::string,std::pair<uint32_t,uint32_t>> expected;
	for(const auto& entry : ZipIndex(archive,12813,13000)) {
		if(entry.name == prefix) continue;
		if(entry.name.compare(0,prefix.size(),prefix) != 0) return false;
		const auto name = entry.name.substr(prefix.size());
		if(name == ".gitignore") continue;
		if(name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
			return false;
		expected.emplace(Lower(name),std::make_pair(entry.expanded,entry.crc));
	}
	if(expected.size() != 12812) return false;
	const auto scriptDir = Join(p.root,L"script");
	SafePath(p.root,scriptDir,false);
	const DWORD attrs = GetFileAttributesW(scriptDir.c_str());
	if(attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
	WIN32_FIND_DATAW entry{};
	const HANDLE find = FindFirstFileW(Join(scriptDir,L"*").c_str(),&entry);
	if(find == INVALID_HANDLE_VALUE) return false;
	struct FindGuard { HANDLE handle; ~FindGuard() { FindClose(handle); } } guard{find};
	struct ScriptCheck { std::wstring path; uint32_t size, crc; };
	std::vector<ScriptCheck> checks;
	checks.reserve(expected.size());
	do {
		const std::wstring name = entry.cFileName;
		if(name == L"." || name == L"..") continue;
		if(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
		const auto match = expected.find(Lower(Utf8(name)));
		if(match == expected.end()) return false;
		if(entry.nFileSizeHigh || entry.nFileSizeLow != match->second.first) return false;
		checks.push_back({Join(scriptDir,name),match->second.first,match->second.second});
	} while(FindNextFileW(find,&entry));
	if(GetLastError() != ERROR_NO_MORE_FILES || checks.size() != expected.size()) return false;
	// A fresh copy can make thousands of tiny files cold to the filesystem.
	// Bounded parallel reads keep the complete CRC check without making the
	// first launch appear stuck on one synchronous file at a time.
	(void)crc32(0,Z_NULL,0);
	std::atomic<size_t> next{0};
	std::atomic<bool> valid{true};
	std::vector<std::thread> workers;
	try {
		for(unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
			try {
				while(valid.load(std::memory_order_relaxed)) {
					const size_t index = next.fetch_add(1,std::memory_order_relaxed);
					if(index >= checks.size()) break;
					const auto& check = checks[index];
					if(!ScriptFileMatches(check.path,check.size,check.crc))
						valid.store(false,std::memory_order_relaxed);
				}
			} catch(...) { valid.store(false,std::memory_order_relaxed); }
		});
	} catch(...) {
		valid.store(false,std::memory_order_relaxed);
		for(auto& worker : workers) worker.join();
		throw;
	}
	for(auto& worker : workers) worker.join();
	return valid.load(std::memory_order_relaxed);
}
json BaseRecover(const Paths& p) {
	const auto journal = ReadJson(p.root, p.baseJournal);
	if(journal.is_null()) return json{{"state","base-recovery-not-needed"}};
	const auto tx = Field(journal,"transaction");
	if(!Hex(tx, 32) || Field(journal,"scripts_sha256") != BASE_SCRIPTS_SHA256)
		Fail("Unknown base installation journal; preserving files.");
	const auto archivePath = Join(BaseStage(p),L"scripts.zip");
	if(Sha256File(p.root, archivePath, 14077251) != BASE_SCRIPTS_SHA256)
		Fail("Base staging differs from the installation journal; preserving files.");
	const auto archive = ReadBytes(p.root, archivePath, 14077251);
	const auto expected = BaseScriptHashes(archive);
	const auto scriptDir = Join(p.root,L"script");
	std::vector<std::wstring> installedScripts;
	if(Exists(scriptDir)) {
		SafePath(p.root, scriptDir, false);
		if(!(GetFileAttributesW(scriptDir.c_str()) & FILE_ATTRIBUTE_DIRECTORY))
			Fail("Base script destination changed; preserving files.");
		WIN32_FIND_DATAW entry{};
		const HANDLE find = FindFirstFileW(Join(scriptDir,L"*").c_str(), &entry);
		if(find == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
			Fail("Cannot inspect base scripts during recovery.");
		struct FindGuard { HANDLE handle; ~FindGuard() { if(handle != INVALID_HANDLE_VALUE) FindClose(handle); } } guard{find};
		if(find != INVALID_HANDLE_VALUE) do {
			const std::wstring name = entry.cFileName;
			if(name == L"." || name == L"..") continue;
			if(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
				Fail("Unknown base script directory content; preserving files.");
			const auto actual = Join(scriptDir,name);
			const auto known = expected.find(Lower(Utf8(name)));
			if(known == expected.end() || Sha256File(p.root, actual, 1048576) != known->second)
				Fail("Base script changed during interrupted install; preserving files.");
			installedScripts.push_back(actual);
		} while(FindNextFileW(find, &entry));
		if(find != INVALID_HANDLE_VALUE && GetLastError() != ERROR_NO_MORE_FILES)
			Fail("Cannot finish inspecting base scripts.");
	}
	const auto db = Join(p.root,L"cards.cdb"), strings = Join(p.root,L"strings.conf");
	if((Exists(db) && Sha256File(p.root,db,8076288) != BASE_DB_SHA256) ||
		(Exists(strings) && Sha256File(p.root,strings,37759) != BASE_STRINGS_SHA256))
		Fail("Base files changed during interrupted install; preserving files.");
	const auto receipt = ReadJson(p.root,p.baseInstalled);
	if(!receipt.is_null() && Field(receipt,"transaction") != tx)
		Fail("Base install receipt differs from journal; preserving files.");
	for(const auto& file : installedScripts) Remove(p.root,file);
	if(Exists(scriptDir) && !RemoveDirectoryW(scriptDir.c_str()))
		Fail("Cannot clear interrupted base script directory.");
	Remove(p.root,db); Remove(p.root,strings);
	Remove(p.root,Join(p.state,L".base-part-" + Wide(tx)));
	Remove(p.root,p.baseInstalled); Remove(p.root,p.baseJournal);
	return json{{"state","base-recovered"},{"transaction",tx}};
}
json BaseInstall(const Paths& p) {
	const auto status = BaseStatus(p);
	if(Field(status,"state") != "verified-base-staged" ||
		!status["core"].value("compatible",false))
		Fail("Verified base staging and the tested executable are required.");
	RequireNoRunningGame(p);
	const auto archive = ReadBytes(p.root, Join(BaseStage(p),L"scripts.zip"), 14077251);
	const auto expected = BaseScriptHashes(archive);
	if(Exists(Join(p.root,L"cards.cdb")) || Exists(Join(p.root,L"strings.conf")) ||
		Exists(Join(p.root,L"script")) || Exists(p.baseJournal))
		Fail("Existing game data or recovery state will not be replaced.");
	const auto tx = RandomHex();
	const auto part = Join(p.state,L".base-part-" + Wide(tx));
	WriteJson(p.root,p.baseJournal,json{{"transaction",tx},
		{"scripts_sha256",BASE_SCRIPTS_SHA256},{"started_utc",UtcNow()}});
	try {
		const auto scriptDir = Join(p.root,L"script");
		Directory(p.root,scriptDir);
		const std::string prefix = std::string("ygopro-scripts-") + SCRIPT_PIN + "/";
		unsigned written = 0;
		for(const auto& entry : ZipIndex(archive, 12813, 13000)) {
			if(entry.name == prefix) continue;
			const auto name = entry.name.substr(prefix.size());
			if(name == ".gitignore") continue;
			if(expected.find(Lower(name)) == expected.end())
				Fail("Unexpected base script while installing.");
			const auto payload = ZipPayload(archive,entry);
			WriteBytes(p.root,part,payload.data(),payload.size());
			const auto target = Join(scriptDir,Wide(name));
			SafePath(p.root,target);
			if(!MoveFileExW(part.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))
				Fail("Cannot install a base script without replacement.");
			++written;
		}
		if(written != expected.size()) Fail("Base script install count differs.");
		for(const wchar_t* name : {L"cards.cdb",L"strings.conf"}) {
			Copy(p.root,Join(BaseStage(p),name),part);
			const auto target = Join(p.root,name);
			SafePath(p.root,target);
			if(!MoveFileExW(part.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))
				Fail("Cannot install base data without replacement.");
		}
		WriteJson(p.root,p.baseInstalled,json{{"transaction",tx},
			{"database_sha256",BASE_DB_SHA256},{"strings_sha256",BASE_STRINGS_SHA256},
			{"scripts_sha256",BASE_SCRIPTS_SHA256},{"installed_utc",UtcNow()}});
		Remove(p.root,p.baseJournal);
		return json{{"state","base-installed-needs-presentation"},{"playable",false},
			{"needs_presentation_assets",true},{"scripts_installed",written}};
	} catch(...) {
		try { BaseRecover(p); } catch(const std::exception&) { /* Preserve evidence for a later retry. */ }
		throw;
	}
}
json RunAction(const Paths& p, const std::string& action, const std::string& argument) {
	if(action == "BaseStatus") return BaseStatus(p);
	if(action == "BasePrepare" || action == "BaseCleanup") {
		Directory(p.root, p.state); Directory(p.root, p.versions);
		auto lock = Lock(p, p.stageLock);
		return action == "BasePrepare" ? BasePrepare(p) : BaseCleanup(p);
	}
	if(action == "BaseInstall" || action == "BaseRecover") {
		Directory(p.root,p.state); Directory(p.root,p.versions);
		auto activation = Lock(p,p.activationLock);
		auto game = LockGameMutation(p);
		auto stage = Lock(p,p.stageLock);
		RequireNoRunningGame(p);
		return action == "BaseInstall" ? BaseInstall(p) : BaseRecover(p);
	}
	if(action == "Status") return Status(p);
	if(action == "Check") {
		const auto feed = Feed(), core = CoreProof(p);
		return json{{"state",core.value("compatible",false) ? "candidate" : "incompatible"},
			{"feed",feed},{"core",core}};
	}
	if(action == "ValidateLocal") {
		const auto archive = Join(p.state, L"local-test.ypk");
		const auto bytes = ReadBytes(p.root, archive, MAX_PACKAGE);
		return json{{"state","validated-local-only"},{"package",ValidatePackage(bytes)},
			{"sha256",Sha256(bytes)}};
	}
	if(action == "Download") return Download(p, argument);
	if(action == "Cleanup") {
		Directory(p.root, p.state); Directory(p.root, p.versions);
		auto lock = Lock(p, p.stageLock);
		return json{{"state","staging-clean"},{"removed",CleanupIncoming(p)}};
	}
	Directory(p.root, p.state);
	auto lock = Lock(p, p.activationLock);
	// A game takes a shared read lease before initialization and retains it
	// until exit. This exclusive handle closes the gap between the process
	// snapshot and a direct game startup during active-package mutation.
	Handle gameExclusion;
	if((action == "Activate" && (Exists(p.journal) || Exists(p.pending))) ||
		action == "AbortStartup")
		gameExclusion = LockGameMutation(p);
	if(action == "Cancel") {
		if(Exists(p.journal)) Fail("Activation recovery is required first.");
		Remove(p.root, p.pending);
		return json{{"state","canceled"}};
	}
	if(action == "AbortStartup") return Abort(p, argument);
	if(action == "Activate" && Exists(p.journal)) return Activate(p);
	const auto core = CoreProof(p);
	if(!core.value("compatible",false))
		Fail("This client lacks the exact tested core/base-script build pins.");
	if(action == "Schedule" || action == "Rollback") return Schedule(p, action, argument);
	if(action == "Activate") return Activate(p);
	Fail("Unknown native update action.");
	return nullptr;
}
} // namespace

std::string Run(const std::wstring& runtime, const std::string& action,
	const std::string& argument) {
	const Paths paths(runtime);
	return RunAction(paths, action, argument).dump();
}
std::string ConfirmSpawnedGame(const std::wstring& runtime,
	const std::string& transaction, const std::string& nonce,
	DWORD childPid, HANDLE childProcess) {
	const Paths paths(runtime);
	Directory(paths.root, paths.state);
	auto lock = Lock(paths, paths.activationLock);
	const auto core = CoreProof(paths);
	if(!core.value("compatible", false))
		Fail("This client lacks the exact tested core/base-script build pins.");
	return Confirm(paths, transaction, nonce, childPid, childProcess).dump();
}
} // namespace cardupdate
