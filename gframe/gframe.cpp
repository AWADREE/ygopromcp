#include "config.h"
#include "mcp.h"
#include "game.h"
#include "data_manager.h"
#include "ocgapi.h"
#include "card_update_health.h"
#include "card_update_ui.h"
#include <cstdio>
#include <cstring>
#include <event2/thread.h>
#include <clocale>
#include <memory>
#include <cstdlib>
#ifdef _WIN32
#include <WinSock2.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#endif

#if defined(_WIN32) && (!defined(WDK_NTDDI_VERSION) || (WDK_NTDDI_VERSION < 0x0A000005)) // Redstone 4, Version 1803, Build 17134.
#error "This program requires the Windows 10 SDK version 1803 or above to compile on Windows. Otherwise, non-ASCII characters will not be displayed or processed correctly."
#endif


void ClickButton(irr::gui::IGUIElement* btn) {
	irr::SEvent event;
	event.EventType = irr::EET_GUI_EVENT;
	event.GUIEvent.EventType = irr::gui::EGET_BUTTON_CLICKED;
	event.GUIEvent.Caller = btn;
	ygo::mainGame->device->postEventFromUser(event);
}

namespace {
#ifdef _WIN32
class GameDataReadLease {
	HANDLE file{INVALID_HANDLE_VALUE};
public:
	GameDataReadLease() = default;
	GameDataReadLease(const GameDataReadLease&) = delete;
	GameDataReadLease& operator=(const GameDataReadLease&) = delete;
	~GameDataReadLease() { if(file != INVALID_HANDLE_VALUE) CloseHandle(file); }
	bool Acquire() {
		wchar_t image[32768]{};
		const DWORD size = GetModuleFileNameW(nullptr, image, 32768);
		if(!size || size >= 32768) return false;
		wchar_t* separator = std::wcsrchr(image, L'\\');
		if(!separator) return false;
		*separator = L'\0';
		const std::wstring root(image), state = root + L"\\card-updates";
		const DWORD rootAttrs = GetFileAttributesW(root.c_str());
		if(rootAttrs == INVALID_FILE_ATTRIBUTES || !(rootAttrs & FILE_ATTRIBUTE_DIRECTORY) ||
			(rootAttrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
		if(!CreateDirectoryW(state.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
			return false;
		const DWORD stateAttrs = GetFileAttributesW(state.c_str());
		if(stateAttrs == INVALID_FILE_ATTRIBUTES || !(stateAttrs & FILE_ATTRIBUTE_DIRECTORY) ||
			(stateAttrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
		const auto path = state + L"\\game-live.lock";
		const DWORD attrs = GetFileAttributesW(path.c_str());
		if(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
		file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if(file == INVALID_HANDLE_VALUE) return false;
		const DWORD journalAttrs = GetFileAttributesW((state + L"\\base-install.json").c_str());
		return journalAttrs == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
	}
};
#endif
byte* CardUpdateProbeScript(const char* name, int* length) {
    static byte empty[] = "";
    static byte check[] =
        "assert(type(Card.GetCardType)=='function' and "
        "type(Card.IsCardType)=='function' and "
        "type(Card.IsAllCardTypes)=='function')";
    if(std::strcmp(name, "./script/__card_update_probe.lua") == 0) {
        *length = sizeof(check) - 1;
        return check;
    }
    *length = 0;
    return empty;
}
uint32_t CardUpdateProbeMessage(intptr_t duel, uint32_t) {
    char message[1024]{};
    get_log_message(duel, message);
    std::fprintf(stderr, "card-update-probe-error: %s\n", message);
    return 0;
}
int RunCardUpdateProbe() {
    set_script_reader(CardUpdateProbeScript);
    set_message_handler(CardUpdateProbeMessage);
    const intptr_t duel = create_duel(1);
    const bool supported = preload_script(duel, "./script/__card_update_probe.lua") == 1;
    end_duel(duel);
    if(!supported)
        return EXIT_FAILURE;
    std::fputs("YGOPROMCP_CARD_UPDATE_CORE_V1 3209cd866a55e8501a0ed358d3064d9ae0ae6b30\n", stderr);
    return EXIT_SUCCESS;
}
}
static int mymain(int wargc, const wchar_t* const wargv[]) {
    if(wargc == 2 && std::wcscmp(wargv[1], L"--card-update-probe") == 0)
        return RunCardUpdateProbe();
#ifdef _WIN32
	GameDataReadLease gameDataLease;
	if(!gameDataLease.Acquire()) {
		std::fputs("Game data is being updated or its startup lock is unavailable.\n", stderr);
		return EXIT_FAILURE;
	}
#ifdef YGOPROMCP_HTTP_PROBE
	// Test-only gate: prove a direct game holds the data lease even before
	// initialization. The production build has no marker or delay.
	if(const char* marker = std::getenv("YGOPROMCP_HTTP_PROBE_EARLY_LEASE_MARKER")) {
		if(FILE* file = std::fopen(marker, "wb")) {
			std::fputs("lease-acquired\n", file);
			std::fclose(file);
		}
		Sleep(5000);
	}
#endif
#endif
#ifdef __APPLE__
	ygo::Game::FixMacOSBundleWorkingDirectory();
#endif //__APPLE__
#ifdef _WIN32
	if (wargc == 2 && (ygo::IsExtension(wargv[1], L".ydk") || ygo::IsExtension(wargv[1], L".yrp"))) { // open file from explorer
		wchar_t exepath[MAX_PATH];
		GetModuleFileNameW(nullptr, exepath, MAX_PATH);
		wchar_t* p = std::wcsrchr(exepath, L'\\');
		if (p) {
			*p = 0;
			SetCurrentDirectoryW(exepath);
		}
	}
#endif //_WIN32
#ifdef _WIN32
	WORD wVersionRequested;
	WSADATA wsaData;
	wVersionRequested = MAKEWORD(2, 2);
	WSAStartup(wVersionRequested, &wsaData);
	evthread_use_windows_threads();
#else
	evthread_use_pthreads();
	signal(SIGCHLD, SIG_IGN);
#endif //_WIN32
	// The MCP bridge owns stdout from the very start (before Initialize
	// prints the irrlicht banner), so the JSON-RPC channel never sees app
	// output. Always on; no flags.
	ygo::MCP_Init();

	ygo::Game _game;
	ygo::mainGame = &_game;
	if(!ygo::mainGame->Initialize()) {
		ygo::MCP_Shutdown();
#ifdef _WIN32
		WSACleanup();
#endif
		return EXIT_FAILURE;
	}
	if(!ygo::WriteCardUpdateStartupHealth(*ygo::mainGame)) {
		std::fputs("Card update startup health validation failed.\n", stderr);
		ygo::MCP_Shutdown();
#ifdef _WIN32
		WSACleanup();
#endif
		return EXIT_FAILURE;
	}
#ifdef YGOPROMCP_HTTP_PROBE
	// Built only by the offline probe target. Exercise the production menu
	// receiver, not a separate listener startup path.
	if(const char* port = std::getenv("YGOPROMCP_HTTP_PROBE_PORT")) {
		std::wstring wide;
		for(const char* p = port; *p; ++p) wide.push_back(static_cast<wchar_t>(*p));
		ygo::mainGame->ebMcpPort->setText(wide.c_str());
		ClickButton(ygo::mainGame->btnMcpOpen);
		ClickButton(ygo::mainGame->btnMcpToggle);
	}
#endif

	bool keep_on_return = false;
	bool deckCategorySpecified = false;
	for(int i = 1; i < wargc; ++i) {
		if (wargc == 2) {
			if (ygo::IsExtension(wargv[1], L".ydk")) {
				ygo::mainGame->open_file = true;
				BufferIO::CopyWideString(wargv[1], ygo::mainGame->open_file_name);
				ygo::mainGame->exit_on_return = true;
				ClickButton(ygo::mainGame->btnDeckEdit);
				break;
			}
			if (ygo::IsExtension(wargv[1], L".yrp")) {
				ygo::mainGame->open_file = true;
				BufferIO::CopyWideString(wargv[1], ygo::mainGame->open_file_name);
				ygo::mainGame->exit_on_return = true;
				ClickButton(ygo::mainGame->btnReplayMode);
				ClickButton(ygo::mainGame->btnLoadReplay);
				break;
			}
		}
		if(wargv[i][0] == L'-' && wargv[i][1] == L'e' && wargv[i][2] != L'\0') {
			char file[1024];
			BufferIO::EncodeUTF8(wargv[i] + 2, file);
			ygo::dataManager.LoadDB(file);
			continue;
		}
		if(!std::wcscmp(wargv[i], L"-e")) { // extra database
			++i;
			if(i < wargc) {
				char file[1024];
				BufferIO::EncodeUTF8(wargv[i], file);
				ygo::dataManager.LoadDB(file);
			}
			continue;
		} else if(!std::wcscmp(wargv[i], L"-n")) { // nickName
			++i;
			if(i < wargc)
				ygo::mainGame->ebNickName->setText(wargv[i]);
			continue;
		} else if(!std::wcscmp(wargv[i], L"-h")) { // Host address
			++i;
			if(i < wargc)
				ygo::mainGame->ebJoinHost->setText(wargv[i]);
			continue;
		} else if(!std::wcscmp(wargv[i], L"-p")) { // host Port
			++i;
			if(i < wargc)
				ygo::mainGame->ebJoinPort->setText(wargv[i]);
			continue;
		} else if(!std::wcscmp(wargv[i], L"-w")) { // host passWord
			++i;
			if(i < wargc)
				ygo::mainGame->ebJoinPass->setText(wargv[i]);
			continue;
		} else if(!std::wcscmp(wargv[i], L"-k")) { // Keep on return
			ygo::mainGame->exit_on_return = false;
			keep_on_return = true;
		} else if(!std::wcscmp(wargv[i], L"--deck-category")) {
			++i;
			if(i < wargc) {
				deckCategorySpecified = true;
				BufferIO::CopyWideString(wargv[i], ygo::mainGame->gameConf.lastcategory);
			}
		} else if(!std::wcscmp(wargv[i], L"-d")) { // Deck
			++i;
			if(!deckCategorySpecified)
				ygo::mainGame->gameConf.lastcategory[0] = 0;
			if(i + 1 < wargc) { // select deck
				BufferIO::CopyWideString(wargv[i], ygo::mainGame->gameConf.lastdeck);
				continue;
			} else { // open deck
				ygo::mainGame->exit_on_return = !keep_on_return;
				if(i < wargc) {
					ygo::mainGame->open_file = true;
					if(deckCategorySpecified) {
#ifdef _WIN32
						myswprintf(ygo::mainGame->open_file_name, L"%ls\\%ls", ygo::mainGame->gameConf.lastcategory, wargv[i]);
#else
						myswprintf(ygo::mainGame->open_file_name, L"%ls/%ls", ygo::mainGame->gameConf.lastcategory, wargv[i]);
#endif
					} else {
						BufferIO::CopyWideString(wargv[i], ygo::mainGame->open_file_name);
					}
				}
				ClickButton(ygo::mainGame->btnDeckEdit);
				break;
			}
		} else if(!std::wcscmp(wargv[i], L"-c")) { // Create host
			ygo::mainGame->exit_on_return = !keep_on_return;
			ygo::mainGame->HideElement(ygo::mainGame->wMainMenu);
			ClickButton(ygo::mainGame->btnHostConfirm);
			break;
		} else if(!std::wcscmp(wargv[i], L"-j")) { // Join host
			ygo::mainGame->exit_on_return = !keep_on_return;
			ygo::mainGame->HideElement(ygo::mainGame->wMainMenu);
			ClickButton(ygo::mainGame->btnJoinHost);
			break;
		} else if(!std::wcscmp(wargv[i], L"-r")) { // Replay
			ygo::mainGame->exit_on_return = !keep_on_return;
			++i;
			if(i < wargc) {
				ygo::mainGame->open_file = true;
				BufferIO::CopyWideString(wargv[i], ygo::mainGame->open_file_name);
			}
			ClickButton(ygo::mainGame->btnReplayMode);
			if(ygo::mainGame->open_file)
				ClickButton(ygo::mainGame->btnLoadReplay);
			break;
		} else if(!std::wcscmp(wargv[i], L"-s")) { // Single
			ygo::mainGame->exit_on_return = !keep_on_return;
			++i;
			if(i < wargc) {
				ygo::mainGame->open_file = true;
				BufferIO::CopyWideString(wargv[i], ygo::mainGame->open_file_name);
			}
			ClickButton(ygo::mainGame->btnSingleMode);
			if(ygo::mainGame->open_file)
				ClickButton(ygo::mainGame->btnLoadSinglePlay);
			break;
		}
	}
	ygo::mainGame->MainLoop();
	ygo::CardUpdateUiShutdown();
	ygo::MCP_Shutdown();
#ifdef _WIN32
	WSACleanup();
#else

#endif //_WIN32
	return EXIT_SUCCESS;
}

#ifdef _WIN32

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	std::setlocale(LC_CTYPE, ".UTF-8");
	int wargc = 0;
	std::unique_ptr<wchar_t*[], void(*)(wchar_t**)> wargv(CommandLineToArgvW(GetCommandLineW(), &wargc), [](wchar_t** wargv) {
		LocalFree(wargv);
	});
	if(!wargv)
		return EXIT_FAILURE;
	return mymain(wargc, wargv.get());
}

#else

int main(int argc, char* argv[]) {
#ifdef __APPLE__
	std::setlocale(LC_CTYPE, "UTF-8");
#else
	std::setlocale(LC_CTYPE, "");
#endif
	std::vector<std::wstring> wide_arguments;
	wide_arguments.reserve(argc);
	for(int i = 0; i < argc; ++i)
		wide_arguments.emplace_back(BufferIO::DecodeUTF8String(argv[i]));
	std::vector<const wchar_t*> wargv;
	wargv.reserve(argc);
	for(const auto& argument : wide_arguments)
		wargv.emplace_back(argument.c_str());
	return mymain(argc, wargv.data());
}

#endif //_WIN32
