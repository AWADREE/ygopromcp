#include "config.h"
#include "mcp.h"
#include "card_update_ui.h"
#include "game.h"
#include "field_projection.h"
#include "CGUITTFont.h"
#include "file_system.h"
#include "image_manager.h"
#include "data_manager.h"
#include "deck_manager.h"
#include "sound_manager.h"
#include "materials.h"
#include "client_card.h"
#include "duelclient.h"
#include "netserver.h"
#include "single_mode.h"
#include <thread>
#include <chrono>
#include <cstdlib>
#ifdef YGOPROMCP_HTTP_PROBE
void ClickButton(irr::gui::IGUIElement* btn);
#endif
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#else
#include <spawn.h>
#ifdef __APPLE__
#include <crt_externs.h>
#define GetEnviron() (*_NSGetEnviron())
#else
extern char **environ;
#define GetEnviron() environ
#endif
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#if defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || \
	defined(__x86_64__) || defined(_M_X64) || defined(__x86_64) || defined(_M_AMD64)
	#include <immintrin.h>
	#define CPU_PAUSE() _mm_pause()
#elif defined(_M_ARM) || defined(_M_ARM64) || defined(__arm__) || defined(__aarch64__)
    #if defined(_MSC_VER)
        #include <intrin.h>
        #define CPU_PAUSE() __yield()
    #else
        #define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
    #endif
#else
	#define CPU_PAUSE() ((void)0)
#endif

namespace ygo {

Game* mainGame;

void DuelInfo::Clear() {
	isStarted = false;
	isInDuel = false;
	isFinished = false;
	isReplay = false;
	isReplaySkiping = false;
	isFirst = false;
	isTag = false;
	isSingleMode = false;
	is_shuffling = false;
	tag_player[0] = false;
	tag_player[1] = false;
	isReplaySwapped = false;
	lp[0] = 0;
	lp[1] = 0;
	start_lp = 0;
	duel_rule = 0;
	turn = 0;
	curMsg = 0;
	hostname[0] = 0;
	clientname[0] = 0;
	hostname_tag[0] = 0;
	clientname_tag[0] = 0;
	strLP[0][0] = 0;
	strLP[1][0] = 0;
	player_type = 0;
	time_player = 0;
	time_limit = 0;
	time_left[0] = 0;
	time_left[1] = 0;
}

bool Game::Initialize() {
	LoadConfig("system.conf");
	LoadConfig("load-once.conf");
	irr::SIrrlichtCreationParameters params{};
	params.AntiAlias = gameConf.antialias;
	if(gameConf.use_d3d)
		params.DriverType = irr::video::EDT_DIRECT3D9;
	else
		params.DriverType = irr::video::EDT_OPENGL;
	params.WindowSize = irr::core::dimension2d<irr::u32>(gameConf.window_width, gameConf.window_height);
	device = irr::createDeviceEx(params);
#ifdef __APPLE__
	FixMacOSBundleWorkingDirectory();
#endif
	if(!device) {
		ErrorLog("Failed to create Irrlicht Engine device!");
		return false;
	}
#ifndef _DEBUG
	device->getLogger()->setLogLevel(irr::ELOG_LEVEL::ELL_ERROR);
#endif
	deckManager.LoadLFList();
	if(gameConf.default_lflist >= (int)deckManager._lfList.size() || gameConf.default_lflist < 0)
		gameConf.default_lflist = 0;
	driver = device->getVideoDriver();
	driver->setTextureCreationFlag(irr::video::ETCF_CREATE_MIP_MAPS, false);
	driver->setTextureCreationFlag(irr::video::ETCF_OPTIMIZED_FOR_QUALITY, true);
	imageManager.SetDevice(device);
	if(!imageManager.Initial()) {
		ErrorLog("Failed to load textures!");
		return false;
	}
	dataManager.IrrFileSystem = device->getFileSystem();
	if(!dataManager.LoadDB("cards.cdb")) {
		std::string errmsg = "Failed to load card database (cards.cdb)! ";
		errmsg.append(dataManager.errmsg);
		ErrorLog(errmsg.c_str());
		return false;
	}
	if(!dataManager.LoadStrings("strings.conf")) {
		ErrorLog("Failed to load strings!");
		return false;
	}
	LoadExpansions();
	env = device->getGUIEnvironment();
	numFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.numfont, 16);
	if(!numFont) {
		const char* numFontPaths[] = {
			"./fonts/numFont.ttf",
			"./fonts/numFont.ttc",
			"./fonts/numFont.otf",
			"C:/Windows/Fonts/arialbd.ttf",
			"/usr/share/fonts/truetype/DroidSansFallbackFull.ttf",
			"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
			"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc",
			"/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
			"/System/Library/Fonts/SFNSTextCondensed-Bold.otf",
			"/System/Library/Fonts/SFNS.ttf",
		};
		for(auto path : numFontPaths) {
			BufferIO::CopyString(path, gameConf.numfont);
			numFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.numfont, 16);
			if(numFont)
				break;
		}
	}
	textFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.textfont, gameConf.textfontsize);
	if(!textFont) {
		const char* textFontPaths[] = {
			"./fonts/textFont.ttf",
			"./fonts/textFont.ttc",
			"./fonts/textFont.otf",
			"C:/Windows/Fonts/msyh.ttc",
			"C:/Windows/Fonts/msyh.ttf",
			"C:/Windows/Fonts/simsun.ttc",
			"C:/Windows/Fonts/YuGothM.ttc",
			"C:/Windows/Fonts/meiryo.ttc",
			"C:/Windows/Fonts/msgothic.ttc",
			"/usr/share/fonts/truetype/DroidSansFallbackFull.ttf",
			"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
			"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
			"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
			"/System/Library/Fonts/PingFang.ttc",
			"/System/Library/Fonts/STHeiti Medium.ttc",
		};
		for(auto path : textFontPaths) {
			BufferIO::CopyString(path, gameConf.textfont);
			textFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.textfont, gameConf.textfontsize);
			if(textFont)
				break;
		}
	}
	if(!numFont || !textFont) {
		wchar_t fpath[1024]{};
		FileSystem::TraversalDir(L"./fonts", [&fpath](const wchar_t* name, bool isdir) {
			if(!isdir && (IsExtension(name, L".ttf") || IsExtension(name, L".ttc") || IsExtension(name, L".otf"))) {
				myswprintf(fpath, L"./fonts/%ls", name);
			}
		});
		if(fpath[0] == 0) {
			ErrorLog("No fonts found! Please place appropriate font file in the fonts directory, or edit system.conf manually.");
			return false;
		}
		if(!numFont) {
			BufferIO::EncodeUTF8(fpath, gameConf.numfont);
			numFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.numfont, 16);
		}
		if(!textFont) {
			BufferIO::EncodeUTF8(fpath, gameConf.textfont);
			textFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.textfont, gameConf.textfontsize);
		}
	}
	if(!numFont || !textFont) {
		ErrorLog("Failed to load font(s)!");
		return false;
	}
	adFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.numfont, 12);
	lpcFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.numfont, 48);
	guiFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.textfont, gameConf.textfontsize);
	smgr = device->getSceneManager();
	device->setWindowCaption(L"YGOPro");
	device->setResizable(true);
	if(gameConf.window_maximized)
		device->maximizeWindow();
#ifdef _WIN32
	irr::video::SExposedVideoData exposedData = driver->getExposedVideoData();
	if(gameConf.use_d3d)
		hWnd = reinterpret_cast<HWND>(exposedData.D3D9.HWnd);
	else
		hWnd = reinterpret_cast<HWND>(exposedData.OpenGLWin32.HWnd);
#endif
	SetWindowsIcon();
	//main menu
	wchar_t strbuf[256];
	myswprintf(strbuf, L"YGOPro Version:%X.0%X.%X", (PRO_VERSION & 0xf000U) >> 12, (PRO_VERSION & 0x0ff0U) >> 4, PRO_VERSION & 0x000fU);
	wMainMenu = env->addWindow(irr::core::rect<irr::s32>(370, 165, 650, 475), false, strbuf);
	wMainMenu->getCloseButton()->setVisible(false);
	btnLanMode = env->addButton(irr::core::rect<irr::s32>(10, 30, 270, 60), wMainMenu, BUTTON_LAN_MODE, dataManager.GetSysString(1200));
	btnSingleMode = env->addButton(irr::core::rect<irr::s32>(10, 65, 270, 95), wMainMenu, BUTTON_SINGLE_MODE, dataManager.GetSysString(1201));
	btnReplayMode = env->addButton(irr::core::rect<irr::s32>(10, 100, 270, 130), wMainMenu, BUTTON_REPLAY_MODE, dataManager.GetSysString(1202));
//	btnTestMode = env->addButton(irr::core::rect<irr::s32>(10, 135, 270, 165), wMainMenu, BUTTON_TEST_MODE, dataManager.GetSysString(1203));
	btnDeckEdit = env->addButton(irr::core::rect<irr::s32>(10, 135, 270, 165), wMainMenu, BUTTON_DECK_EDIT, dataManager.GetSysString(1204));
	btnSettingsOpen = env->addButton(irr::core::rect<irr::s32>(10, 170, 270, 200), wMainMenu, BUTTON_SETTINGS_OPEN, L"Settings");
	btnMcpOpen = env->addButton(irr::core::rect<irr::s32>(10, 205, 270, 235), wMainMenu, BUTTON_MCP_OPEN, L"Local MCP");
	btnModeExit = env->addButton(irr::core::rect<irr::s32>(10, 240, 270, 270), wMainMenu, BUTTON_MODE_EXIT, dataManager.GetSysString(1210));
	stMcpHome = env->addStaticText(L"MCP: stopped", irr::core::rect<irr::s32>(10, 278, 270, 301), false, false, wMainMenu);
	wMcpPanel = env->addWindow(irr::core::rect<irr::s32>(250, 155, 774, 475), false, L"Local MCP");
	wMcpPanel->getCloseButton()->setVisible(false);
	wMcpPanel->setVisible(false);
	stMcpLabel = env->addStaticText(L"Only this PC can connect. Enter a free localhost port:",
		irr::core::rect<irr::s32>(22, 42, 492, 68), false, false, wMcpPanel);
	ebMcpPort = env->addEditBox(std::to_wstring(gameConf.mcp_port).c_str(),
		irr::core::rect<irr::s32>(22, 76, 142, 110), true, wMcpPanel);
	btnMcpToggle = env->addButton(irr::core::rect<irr::s32>(158, 76, 492, 110),
		wMcpPanel, BUTTON_MCP_TOGGLE, L"Enable MCP");
	stMcpStatus = env->addStaticText(L"Stopped", irr::core::rect<irr::s32>(22, 126, 492, 156),
		false, false, wMcpPanel);
	stMcpEndpoint = env->addStaticText(L"Enable to get a local connection URL",
		irr::core::rect<irr::s32>(22, 170, 492, 200), false, false, wMcpPanel);
	btnMcpCopy = env->addButton(irr::core::rect<irr::s32>(22, 216, 320, 254),
		wMcpPanel, BUTTON_MCP_COPY, L"Copy connection URL");
	btnMcpCopy->setEnabled(false);
	btnMcpClose = env->addButton(irr::core::rect<irr::s32>(348, 216, 492, 254),
		wMcpPanel, BUTTON_MCP_CLOSE, L"Back");
	wHomeSettings = env->addWindow(irr::core::rect<irr::s32>(300, 180, 724, 420), false, L"Settings");
	wHomeSettings->getCloseButton()->setVisible(false);
	wHomeSettings->setVisible(false);
	env->addStaticText(L"Theme", irr::core::rect<irr::s32>(22, 45, 380, 73), false, false, wHomeSettings);
	cbHomeTheme = env->addComboBox(irr::core::rect<irr::s32>(22, 82, 390, 120), wHomeSettings, COMBOBOX_HOME_THEME);
	cbHomeTheme->addItem(L"Classic");
	if(themeManager.IsAvailable("gilded")) cbHomeTheme->addItem(L"Gilded Duel Terminal");
	btnSettingsClose = env->addButton(irr::core::rect<irr::s32>(255, 165, 390, 205),
		wHomeSettings, BUTTON_SETTINGS_CLOSE, L"Back");
	btnGameDataOpen = env->addButton(irr::core::rect<irr::s32>(22, 165, 238, 205),
		wHomeSettings, BUTTON_GAME_DATA_OPEN, L"Game Data & Updates");
	// Base setup runs in the packaged launcher before this client initializes.
	btnGameDataOpen->setVisible(CardUpdateUiAvailable());
	wGameDataSettings = env->addWindow(irr::core::rect<irr::s32>(232, 105, 792, 535),
		false, L"Game Data & Updates");
	wGameDataSettings->getCloseButton()->setVisible(false);
	wGameDataSettings->setVisible(false);
	env->addStaticText(L"Base game data", irr::core::rect<irr::s32>(22, 35, 530, 59),
		false, false, wGameDataSettings);
	stGameDataBaseStatus = env->addStaticText(L"Base data loaded at startup. The launcher handles first-run setup and recovery.",
		irr::core::rect<irr::s32>(22, 66, 530, 101), false, true, wGameDataSettings);
	btnGameDataImport = env->addButton(irr::core::rect<irr::s32>(22, 108, 234, 145),
		wGameDataSettings, -1, L"Import existing data");
	btnGameDataImport->setVisible(false);
	env->addStaticText(L"English prerelease overlay", irr::core::rect<irr::s32>(22, 172, 530, 197),
		false, false, wGameDataSettings);
	stUpdateProvider = env->addStaticText(L"Provider: not checked",
		irr::core::rect<irr::s32>(22, 205, 530, 228), false, false, wGameDataSettings);
	stUpdateStatus = env->addStaticText(L"Updates are unavailable in this build.",
		irr::core::rect<irr::s32>(22, 232, 530, 257), false, false, wGameDataSettings);
	stUpdateDetail = env->addStaticText(L"A verified core and data service are required.",
		irr::core::rect<irr::s32>(22, 262, 530, 304), false, true, wGameDataSettings);
	btnUpdateCheck = env->addButton(irr::core::rect<irr::s32>(22, 310, 150, 346),
		wGameDataSettings, BUTTON_UPDATE_CHECK, L"Check");
	btnUpdateDownload = env->addButton(irr::core::rect<irr::s32>(162, 310, 290, 346),
		wGameDataSettings, BUTTON_UPDATE_DOWNLOAD, L"Download");
	btnUpdateApply = env->addButton(irr::core::rect<irr::s32>(302, 310, 530, 346),
		wGameDataSettings, BUTTON_UPDATE_APPLY, L"Apply on next launch");
	btnUpdateRestore = env->addButton(irr::core::rect<irr::s32>(22, 355, 195, 392),
		wGameDataSettings, BUTTON_UPDATE_RESTORE, L"Restore previous");
	btnUpdateCancel = env->addButton(irr::core::rect<irr::s32>(207, 355, 343, 392),
		wGameDataSettings, BUTTON_UPDATE_CANCEL, L"Cancel");
	btnGameDataClose = env->addButton(irr::core::rect<irr::s32>(394, 355, 530, 392),
		wGameDataSettings, BUTTON_GAME_DATA_CLOSE, L"Back");
	for(auto* action : {btnGameDataImport, btnUpdateCheck, btnUpdateDownload,
			btnUpdateApply, btnUpdateRestore, btnUpdateCancel})
		action->setEnabled(false);
	//lan mode
	wLanWindow = env->addWindow(irr::core::rect<irr::s32>(220, 100, 800, 520), false, dataManager.GetSysString(1200));
	wLanWindow->getCloseButton()->setVisible(false);
	wLanWindow->setVisible(false);
	stLanNickname = env->addStaticText(dataManager.GetSysString(1220), irr::core::rect<irr::s32>(10, 30, 220, 50), false, false, wLanWindow);
	ebNickName = env->addEditBox(gameConf.nickname, irr::core::rect<irr::s32>(110, 25, 450, 50), true, wLanWindow);
	ebNickName->setTextAlignment(irr::gui::EGUIA_UPPERLEFT, irr::gui::EGUIA_CENTER);
	lstHostList = env->addListBox(irr::core::rect<irr::s32>(10, 60, 570, 320), wLanWindow, LISTBOX_LAN_HOST, true);
	lstHostList->setItemHeight(18);
	btnLanRefresh = env->addButton(irr::core::rect<irr::s32>(240, 325, 340, 350), wLanWindow, BUTTON_LAN_REFRESH, dataManager.GetSysString(1217));
	stLanHostIP = env->addStaticText(dataManager.GetSysString(1221), irr::core::rect<irr::s32>(10, 360, 220, 380), false, false, wLanWindow);
	ebJoinHost = env->addEditBox(gameConf.lasthost, irr::core::rect<irr::s32>(110, 355, 350, 380), true, wLanWindow);
	ebJoinHost->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	ebJoinPort = env->addEditBox(gameConf.lastport, irr::core::rect<irr::s32>(360, 355, 420, 380), true, wLanWindow);
	ebJoinPort->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stLanPassword = env->addStaticText(dataManager.GetSysString(1222), irr::core::rect<irr::s32>(10, 390, 220, 410), false, false, wLanWindow);
	ebJoinPass = env->addEditBox(gameConf.roompass, irr::core::rect<irr::s32>(110, 385, 420, 410), true, wLanWindow);
	ebJoinPass->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	ebJoinPass->setPasswordBox(true);
	btnJoinHost = env->addButton(irr::core::rect<irr::s32>(460, 355, 570, 380), wLanWindow, BUTTON_JOIN_HOST, dataManager.GetSysString(1223));
	btnJoinCancel = env->addButton(irr::core::rect<irr::s32>(460, 385, 570, 410), wLanWindow, BUTTON_JOIN_CANCEL, dataManager.GetSysString(1212));
	btnCreateHost = env->addButton(irr::core::rect<irr::s32>(460, 25, 570, 50), wLanWindow, BUTTON_CREATE_HOST, dataManager.GetSysString(1224));
	//create host
	wCreateHost = env->addWindow(irr::core::rect<irr::s32>(320, 100, 700, 520), false, dataManager.GetSysString(1224));
	wCreateHost->getCloseButton()->setVisible(false);
	wCreateHost->setVisible(false);
	env->addStaticText(dataManager.GetSysString(1226), irr::core::rect<irr::s32>(20, 30, 220, 50), false, false, wCreateHost);
	cbHostLFlist = env->addComboBox(irr::core::rect<irr::s32>(140, 25, 300, 50), wCreateHost);
	for(unsigned int i = 0; i < deckManager._lfList.size(); ++i)
		cbHostLFlist->addItem(deckManager._lfList[i].listName.c_str(), deckManager._lfList[i].hash);
	cbHostLFlist->setSelected(gameConf.use_lflist ? gameConf.default_lflist : cbHostLFlist->getItemCount() - 1);
	env->addStaticText(dataManager.GetSysString(1225), irr::core::rect<irr::s32>(20, 60, 220, 80), false, false, wCreateHost);
	cbRule = env->addComboBox(irr::core::rect<irr::s32>(140, 55, 300, 80), wCreateHost);
	cbRule->setMaxSelectionRows(10);
	cbRule->addItem(dataManager.GetSysString(1481));
	cbRule->addItem(dataManager.GetSysString(1482));
	cbRule->addItem(dataManager.GetSysString(1483));
	cbRule->addItem(dataManager.GetSysString(1484));
	cbRule->addItem(dataManager.GetSysString(1485));
	cbRule->addItem(dataManager.GetSysString(1486));
	switch(gameConf.defaultOT) {
	case 1:
		cbRule->setSelected(0);
		break;
	case 2:
		cbRule->setSelected(1);
		break;
	case 4:
		cbRule->setSelected(3);
		break;
	case 8:
		cbRule->setSelected(2);
		break;
	default:
		cbRule->setSelected(5);
		break;
	}	
	env->addStaticText(dataManager.GetSysString(1227), irr::core::rect<irr::s32>(20, 90, 220, 110), false, false, wCreateHost);
	cbMatchMode = env->addComboBox(irr::core::rect<irr::s32>(140, 85, 300, 110), wCreateHost);
	cbMatchMode->addItem(dataManager.GetSysString(1244));
	cbMatchMode->addItem(dataManager.GetSysString(1245));
	cbMatchMode->addItem(dataManager.GetSysString(1246));
	env->addStaticText(dataManager.GetSysString(1237), irr::core::rect<irr::s32>(20, 120, 320, 140), false, false, wCreateHost);
	myswprintf(strbuf, L"%d", 180);
	ebTimeLimit = env->addEditBox(strbuf, irr::core::rect<irr::s32>(140, 115, 220, 140), true, wCreateHost);
	ebTimeLimit->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	env->addStaticText(dataManager.GetSysString(1228), irr::core::rect<irr::s32>(20, 150, 320, 170), false, false, wCreateHost);
	env->addStaticText(dataManager.GetSysString(1236), irr::core::rect<irr::s32>(20, 180, 220, 200), false, false, wCreateHost);
	cbDuelRule = env->addComboBox(irr::core::rect<irr::s32>(140, 175, 300, 200), wCreateHost);
	cbDuelRule->addItem(dataManager.GetSysString(1260));
	cbDuelRule->addItem(dataManager.GetSysString(1261));
	cbDuelRule->addItem(dataManager.GetSysString(1262));
	cbDuelRule->addItem(dataManager.GetSysString(1263));
	cbDuelRule->addItem(dataManager.GetSysString(1264));
	cbDuelRule->setSelected(gameConf.default_rule - 1);
	chkNoCheckDeck = env->addCheckBox(false, irr::core::rect<irr::s32>(20, 210, 170, 230), wCreateHost, -1, dataManager.GetSysString(1229));
	chkNoShuffleDeck = env->addCheckBox(false, irr::core::rect<irr::s32>(180, 210, 360, 230), wCreateHost, -1, dataManager.GetSysString(1230));
	env->addStaticText(dataManager.GetSysString(1231), irr::core::rect<irr::s32>(20, 240, 320, 260), false, false, wCreateHost);
	myswprintf(strbuf, L"%d", 8000);
	ebStartLP = env->addEditBox(strbuf, irr::core::rect<irr::s32>(140, 235, 220, 260), true, wCreateHost);
	ebStartLP->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	env->addStaticText(dataManager.GetSysString(1232), irr::core::rect<irr::s32>(20, 270, 320, 290), false, false, wCreateHost);
	myswprintf(strbuf, L"%d", 5);
	ebStartHand = env->addEditBox(strbuf, irr::core::rect<irr::s32>(140, 265, 220, 290), true, wCreateHost);
	ebStartHand->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	env->addStaticText(dataManager.GetSysString(1233), irr::core::rect<irr::s32>(20, 300, 320, 320), false, false, wCreateHost);
	myswprintf(strbuf, L"%d", 1);
	ebDrawCount = env->addEditBox(strbuf, irr::core::rect<irr::s32>(140, 295, 220, 320), true, wCreateHost);
	ebDrawCount->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	env->addStaticText(dataManager.GetSysString(1234), irr::core::rect<irr::s32>(10, 360, 220, 380), false, false, wCreateHost);
	ebServerName = env->addEditBox(gameConf.gamename, irr::core::rect<irr::s32>(110, 355, 250, 380), true, wCreateHost);
	ebServerName->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	env->addStaticText(dataManager.GetSysString(1235), irr::core::rect<irr::s32>(10, 390, 220, 410), false, false, wCreateHost);
	ebServerPass = env->addEditBox(L"", irr::core::rect<irr::s32>(110, 385, 250, 410), true, wCreateHost);
	ebServerPass->setPasswordBox(true);
	ebServerPass->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	btnHostConfirm = env->addButton(irr::core::rect<irr::s32>(260, 355, 370, 380), wCreateHost, BUTTON_HOST_CONFIRM, dataManager.GetSysString(1211));
	btnHostCancel = env->addButton(irr::core::rect<irr::s32>(260, 385, 370, 410), wCreateHost, BUTTON_HOST_CANCEL, dataManager.GetSysString(1212));
	//host(single)
	wHostPrepare = env->addWindow(irr::core::rect<irr::s32>(270, 120, 750, 440), false, dataManager.GetSysString(1250));
	wHostPrepare->getCloseButton()->setVisible(false);
	wHostPrepare->setVisible(false);
	btnHostPrepDuelist = env->addButton(irr::core::rect<irr::s32>(10, 30, 110, 55), wHostPrepare, BUTTON_HP_DUELIST, dataManager.GetSysString(1251));
	for(int i = 0; i < 2; ++i) {
		stHostPrepDuelist[i] = env->addStaticText(L"", irr::core::rect<irr::s32>(40, 65 + i * 25, 240, 85 + i * 25), true, false, wHostPrepare);
		btnHostPrepKick[i] = env->addButton(irr::core::rect<irr::s32>(10, 65 + i * 25, 30, 85 + i * 25), wHostPrepare, BUTTON_HP_KICK, L"X");
		chkHostPrepReady[i] = env->addCheckBox(false, irr::core::rect<irr::s32>(250, 65 + i * 25, 270, 85 + i * 25), wHostPrepare, CHECKBOX_HP_READY, L"");
		chkHostPrepReady[i]->setEnabled(false);
	}
	for(int i = 2; i < 4; ++i) {
		stHostPrepDuelist[i] = env->addStaticText(L"", irr::core::rect<irr::s32>(40, 75 + i * 25, 240, 95 + i * 25), true, false, wHostPrepare);
		btnHostPrepKick[i] = env->addButton(irr::core::rect<irr::s32>(10, 75 + i * 25, 30, 95 + i * 25), wHostPrepare, BUTTON_HP_KICK, L"X");
		chkHostPrepReady[i] = env->addCheckBox(false, irr::core::rect<irr::s32>(250, 75 + i * 25, 270, 95 + i * 25), wHostPrepare, CHECKBOX_HP_READY, L"");
		chkHostPrepReady[i]->setEnabled(false);
	}
	btnHostPrepOB = env->addButton(irr::core::rect<irr::s32>(10, 180, 110, 205), wHostPrepare, BUTTON_HP_OBSERVER, dataManager.GetSysString(1252));
	myswprintf(strbuf, L"%ls%d", dataManager.GetSysString(1253), 0);
	stHostPrepOB = env->addStaticText(strbuf, irr::core::rect<irr::s32>(10, 285, 270, 305), false, false, wHostPrepare);
	stHostPrepRule = env->addStaticText(L"", irr::core::rect<irr::s32>(280, 30, 460, 230), false, true, wHostPrepare);
	env->addStaticText(dataManager.GetSysString(1254), irr::core::rect<irr::s32>(10, 210, 110, 230), false, false, wHostPrepare);
	cbCategorySelect = env->addComboBox(irr::core::rect<irr::s32>(10, 230, 138, 255), wHostPrepare, COMBOBOX_HP_CATEGORY);
	cbCategorySelect->setMaxSelectionRows(10);
	cbDeckSelect = env->addComboBox(irr::core::rect<irr::s32>(142, 230, 340, 255), wHostPrepare);
	cbDeckSelect->setMaxSelectionRows(10);
	btnHostPrepReady = env->addButton(irr::core::rect<irr::s32>(170, 180, 270, 205), wHostPrepare, BUTTON_HP_READY, dataManager.GetSysString(1218));
	btnHostPrepNotReady = env->addButton(irr::core::rect<irr::s32>(170, 180, 270, 205), wHostPrepare, BUTTON_HP_NOTREADY, dataManager.GetSysString(1219));
	btnHostPrepNotReady->setVisible(false);
	btnHostPrepStart = env->addButton(irr::core::rect<irr::s32>(230, 280, 340, 305), wHostPrepare, BUTTON_HP_START, dataManager.GetSysString(1215));
	btnHostPrepCancel = env->addButton(irr::core::rect<irr::s32>(350, 280, 460, 305), wHostPrepare, BUTTON_HP_CANCEL, dataManager.GetSysString(1210));
	//img
	wCardImg = env->addStaticText(L"", irr::core::rect<irr::s32>(1, 1, 1 + CARD_IMG_WIDTH + 20, 1 + CARD_IMG_HEIGHT + 18), true, false, 0, -1, true);
	wCardImg->setBackgroundColor(0xc0c0c0c0);
	wCardImg->setVisible(false);
	imgCard = env->addImage(irr::core::rect<irr::s32>(10, 9, 10 + CARD_IMG_WIDTH, 9 + CARD_IMG_HEIGHT), wCardImg);
	showingcode = 0;
	imgCard->setScaleImage(true);
	imgCard->setUseAlphaChannel(true);
	//phase
	wPhase = env->addStaticText(L"", irr::core::rect<irr::s32>(480, 310, 855, 330));
	wPhase->setVisible(false);
	btnPhaseStatus = env->addButton(irr::core::rect<irr::s32>(0, 0, 50, 20), wPhase, BUTTON_PHASE, L"");
	btnPhaseStatus->setIsPushButton(true);
	btnPhaseStatus->setPressed(true);
	btnPhaseStatus->setVisible(false);
	btnBP = env->addButton(irr::core::rect<irr::s32>(160, 0, 210, 20), wPhase, BUTTON_BP, L"\xff22\xff30");
	btnBP->setVisible(false);
	btnM2 = env->addButton(irr::core::rect<irr::s32>(160, 0, 210, 20), wPhase, BUTTON_M2, L"\xff2d\xff12");
	btnM2->setVisible(false);
	btnEP = env->addButton(irr::core::rect<irr::s32>(320, 0, 370, 20), wPhase, BUTTON_EP, L"\xff25\xff30");
	btnEP->setVisible(false);
	//tab
	wInfos = env->addTabControl(irr::core::rect<irr::s32>(1, 275, 301, 639), 0, true);
	wInfos->setTabExtraWidth(16);
	wInfos->setVisible(false);
	//info
	irr::gui::IGUITab* tabInfo = wInfos->addTab(dataManager.GetSysString(1270));
	stName = env->addStaticText(L"", irr::core::rect<irr::s32>(10, 10, 287, 32), true, false, tabInfo, -1, false);
	stName->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stInfo = env->addStaticText(L"", irr::core::rect<irr::s32>(15, 37, 296, 60), false, true, tabInfo, -1, false);
	stInfo->setOverrideColor(irr::video::SColor(255, 0, 0, 255));
	stDataInfo = env->addStaticText(L"", irr::core::rect<irr::s32>(15, 60, 296, 83), false, true, tabInfo, -1, false);
	stDataInfo->setOverrideColor(irr::video::SColor(255, 0, 0, 255));
	stSetName = env->addStaticText(L"", irr::core::rect<irr::s32>(15, 83, 296, 106), false, true, tabInfo, -1, false);
	stSetName->setOverrideColor(irr::video::SColor(255, 0, 0, 255));
	stText = env->addStaticText(L"", irr::core::rect<irr::s32>(15, 106, 287, 324), false, true, tabInfo, -1, false);
	scrCardText = env->addScrollBar(false, irr::core::rect<irr::s32>(267, 106, 287, 324), tabInfo, SCROLL_CARDTEXT);
	scrCardText->setLargeStep(1);
	scrCardText->setSmallStep(1);
	scrCardText->setVisible(false);
	//log
	irr::gui::IGUITab* tabLog = wInfos->addTab(dataManager.GetSysString(1271));
	lstLog = env->addListBox(irr::core::rect<irr::s32>(10, 10, 290, 290), tabLog, LISTBOX_LOG, false);
	lstLog->setItemHeight(18);
	btnClearLog = env->addButton(irr::core::rect<irr::s32>(160, 300, 260, 325), tabLog, BUTTON_CLEAR_LOG, dataManager.GetSysString(1272));
	//helper
	irr::gui::IGUITab* _tabHelper = wInfos->addTab(dataManager.GetSysString(1298));
	_tabHelper->setRelativePosition(irr::core::recti(16, 49, 299, 362));
	tabHelper = env->addWindow(irr::core::recti(0, 0, 250, 300), false, L"", _tabHelper);
	tabHelper->setDrawTitlebar(false);
	tabHelper->getCloseButton()->setVisible(false);
	tabHelper->setDrawBackground(false);
	tabHelper->setDraggable(false);
	scrTabHelper = env->addScrollBar(false, irr::core::rect<irr::s32>(252, 0, 272, 300), _tabHelper, SCROLL_TAB_HELPER);
	scrTabHelper->setLargeStep(1);
	scrTabHelper->setSmallStep(1);
	scrTabHelper->setVisible(false);
	int posX = 0;
	int posY = 0;
	int chkBoxLabelWidth = 520;
	chkMAutoPos = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabHelper, -1, dataManager.GetSysString(1274));
	chkMAutoPos->setChecked(gameConf.chkMAutoPos != 0);
	posY += 30;
	chkSTAutoPos = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabHelper, -1, dataManager.GetSysString(1278));
	chkSTAutoPos->setChecked(gameConf.chkSTAutoPos != 0);
	posY += 30;
	chkRandomPos = env->addCheckBox(false, irr::core::rect<irr::s32>(posX + 20, posY, posX + 20 + chkBoxLabelWidth, posY + 25), tabHelper, -1, dataManager.GetSysString(1275));
	chkRandomPos->setChecked(gameConf.chkRandomPos != 0);
	posY += 30;
	chkAutoChain = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabHelper, -1, dataManager.GetSysString(1276));
	chkAutoChain->setChecked(gameConf.chkAutoChain != 0);
	posY += 30;
	chkWaitChain = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabHelper, -1, dataManager.GetSysString(1277));
	chkWaitChain->setChecked(gameConf.chkWaitChain != 0);
	posY += 30;
	chkDefaultShowChain = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabHelper, -1, dataManager.GetSysString(1354));
	chkDefaultShowChain->setChecked(gameConf.chkDefaultShowChain != 0);
	posY += 30;
	chkQuickAnimation = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabHelper, CHECKBOX_QUICK_ANIMATION, dataManager.GetSysString(1299));
	chkQuickAnimation->setChecked(gameConf.quick_animation != 0);
	posY += 30;
	chkDrawSingleChain = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabHelper, CHECKBOX_DRAW_SINGLE_CHAIN, dataManager.GetSysString(1287));
	chkDrawSingleChain->setChecked(gameConf.draw_single_chain != 0);
	posY += 30;
	chkAutoSaveReplay = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabHelper, -1, dataManager.GetSysString(1366));
	chkAutoSaveReplay->setChecked(gameConf.auto_save_replay != 0);
	elmTabHelperLast = chkAutoSaveReplay;
	//system
	irr::gui::IGUITab* _tabSystem = wInfos->addTab(dataManager.GetSysString(1273));
	_tabSystem->setRelativePosition(irr::core::recti(16, 49, 299, 362));
	tabSystem = env->addWindow(irr::core::recti(0, 0, 250, 300), false, L"", _tabSystem);
	tabSystem->setDrawTitlebar(false);
	tabSystem->getCloseButton()->setVisible(false);
	tabSystem->setDrawBackground(false);
	tabSystem->setDraggable(false);
	scrTabSystem = env->addScrollBar(false, irr::core::rect<irr::s32>(252, 0, 272, 300), _tabSystem, SCROLL_TAB_SYSTEM);
	scrTabSystem->setLargeStep(1);
	scrTabSystem->setSmallStep(1);
	scrTabSystem->setVisible(false);
	posY = 0;
	chkIgnore1 = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, CHECKBOX_DISABLE_CHAT, dataManager.GetSysString(1290));
	chkIgnore1->setChecked(gameConf.chkIgnore1 != 0);
	posY += 30;
	chkIgnore2 = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, -1, dataManager.GetSysString(1291));
	chkIgnore2->setChecked(gameConf.chkIgnore2 != 0);
	posY += 30;
	chkHidePlayerName = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, CHECKBOX_HIDE_PLAYER_NAME, dataManager.GetSysString(1289));
	chkHidePlayerName->setChecked(gameConf.hide_player_name != 0);
	posY += 30;
	chkIgnoreDeckChanges = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, -1, dataManager.GetSysString(1357));
	chkIgnoreDeckChanges->setChecked(gameConf.chkIgnoreDeckChanges != 0);
	posY += 30;
	chkAutoSearch = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, CHECKBOX_AUTO_SEARCH, dataManager.GetSysString(1358));
	chkAutoSearch->setChecked(gameConf.auto_search_limit >= 0);
	posY += 30;
	chkMultiKeywords = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, CHECKBOX_MULTI_KEYWORDS, dataManager.GetSysString(1378));
	chkMultiKeywords->setChecked(gameConf.search_multiple_keywords > 0);
	posY += 30;
	chkPreferExpansionScript = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, CHECKBOX_PREFER_EXPANSION, dataManager.GetSysString(1379));
	chkPreferExpansionScript->setChecked(gameConf.prefer_expansion_script != 0);
	posY += 30;
	chkSwapYesNoButton = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, CHECKBOX_SWAP_YES_NO_BUTTON, dataManager.GetSysString(1388));
	chkSwapYesNoButton->setChecked(gameConf.swap_yes_no_button);
	posY += 30;
	chkResizeSelectWindow = env->addCheckBox(gameConf.resize_select_window, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, CHECKBOX_RESIZE_SELECT_WINDOW, dataManager.GetSysString(1387));
	posY += 30;
	env->addStaticText(dataManager.GetSysString(1282), irr::core::rect<irr::s32>(posX + 23, posY + 3, posX + 110, posY + 28), false, false, tabSystem);
	btnWinResizeS = env->addButton(irr::core::rect<irr::s32>(posX + 115, posY, posX + 145, posY + 25), tabSystem, BUTTON_WINDOW_RESIZE_S, dataManager.GetSysString(1283));
	btnWinResizeM = env->addButton(irr::core::rect<irr::s32>(posX + 150, posY, posX + 180, posY + 25), tabSystem, BUTTON_WINDOW_RESIZE_M, dataManager.GetSysString(1284));
	btnWinResizeL = env->addButton(irr::core::rect<irr::s32>(posX + 185, posY, posX + 215, posY + 25), tabSystem, BUTTON_WINDOW_RESIZE_L, dataManager.GetSysString(1285));
	btnWinResizeXL = env->addButton(irr::core::rect<irr::s32>(posX + 220, posY, posX + 250, posY + 25), tabSystem, BUTTON_WINDOW_RESIZE_XL, dataManager.GetSysString(1286));
	posY += 30;
	chkResizePopupMenu = env->addCheckBox(gameConf.resize_popup_menu > 0, irr::core::rect<irr::s32>(posX, posY, posX + 110, posY + 25), tabSystem, CHECKBOX_RESIZE_POPUP_MENU, dataManager.GetSysString(1386));
	scrResizePopupMenu = env->addScrollBar(true, irr::core::rect<irr::s32>(posX + 115, posY + 4, posX + 250, posY + 21), tabSystem, SCROLL_RESIZE_POPUP_MENU);
	scrResizePopupMenu->setMax(5);
	scrResizePopupMenu->setMin(1);
	scrResizePopupMenu->setPos(gameConf.resize_popup_menu > 0 ? gameConf.resize_popup_menu : 3);
	scrResizePopupMenu->setLargeStep(1);
	scrResizePopupMenu->setSmallStep(1);
	posY += 30;
	chkLFlist = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + 110, posY + 25), tabSystem, CHECKBOX_LFLIST, dataManager.GetSysString(1288));
	chkLFlist->setChecked(gameConf.use_lflist);
	cbLFlist = env->addComboBox(irr::core::rect<irr::s32>(posX + 115, posY, posX + 250, posY + 25), tabSystem, COMBOBOX_LFLIST);
	cbLFlist->setMaxSelectionRows(6);
	for(unsigned int i = 0; i < deckManager._lfList.size(); ++i)
		cbLFlist->addItem(deckManager._lfList[i].listName.c_str());
	cbLFlist->setEnabled(gameConf.use_lflist);
	cbLFlist->setSelected(gameConf.use_lflist ? gameConf.default_lflist : cbLFlist->getItemCount() - 1);
	posY += 30;
	chkEnableSound = env->addCheckBox(gameConf.enable_sound, irr::core::rect<irr::s32>(posX, posY, posX + 120, posY + 25), tabSystem, -1, dataManager.GetSysString(1279));
	chkEnableSound->setChecked(gameConf.enable_sound);
	scrSoundVolume = env->addScrollBar(true, irr::core::rect<irr::s32>(posX + 115, posY + 4, posX + 250, posY + 21), tabSystem, SCROLL_VOLUME);
	scrSoundVolume->setMax(100);
	scrSoundVolume->setMin(0);
	scrSoundVolume->setPos(gameConf.sound_volume);
	scrSoundVolume->setLargeStep(1);
	scrSoundVolume->setSmallStep(1);
	posY += 30;
	chkEnableMusic = env->addCheckBox(gameConf.enable_music, irr::core::rect<irr::s32>(posX, posY, posX + 120, posY + 25), tabSystem, CHECKBOX_ENABLE_MUSIC, dataManager.GetSysString(1280));
	chkEnableMusic->setChecked(gameConf.enable_music);
	scrMusicVolume = env->addScrollBar(true, irr::core::rect<irr::s32>(posX + 115, posY + 4, posX + 250, posY + 21), tabSystem, SCROLL_VOLUME);
	scrMusicVolume->setMax(100);
	scrMusicVolume->setMin(0);
	scrMusicVolume->setPos(gameConf.music_volume);
	scrMusicVolume->setLargeStep(1);
	scrMusicVolume->setSmallStep(1);
	posY += 30;
	chkMusicMode = env->addCheckBox(false, irr::core::rect<irr::s32>(posX, posY, posX + chkBoxLabelWidth, posY + 25), tabSystem, -1, dataManager.GetSysString(1281));
	chkMusicMode->setChecked(gameConf.music_mode != 0);
	elmTabSystemLast = chkMusicMode;
	// Theme is a system setting; keep it in the existing scrollable Settings tab.
	posY += 38;
	env->addStaticText(L"Theme", irr::core::rect<irr::s32>(posX, posY, posX + 240, posY + 22), false, false, tabSystem);
	posY += 24;
	cbTheme = env->addComboBox(irr::core::rect<irr::s32>(posX, posY, posX + 240, posY + 26), tabSystem, COMBOBOX_THEME);
	cbTheme->addItem(L"Classic");
	if(themeManager.IsAvailable("gilded"))
		cbTheme->addItem(L"Gilded Duel Terminal");
	cbTheme->setSelected(0);
	elmTabSystemLast = cbTheme;
	//
	wHand = env->addWindow(irr::core::rect<irr::s32>(500, 450, 825, 605), false, L"");
	wHand->getCloseButton()->setVisible(false);
	wHand->setDraggable(false);
	wHand->setDrawTitlebar(false);
	wHand->setVisible(false);
	for(int i = 0; i < 3; ++i) {
		btnHand[i] = env->addButton(irr::core::rect<irr::s32>(10 + 105 * i, 10, 105 + 105 * i, 144), wHand, BUTTON_HAND1 + i, L"");
		btnHand[i]->setImage(imageManager.tHand[i]);
		btnHand[i]->setUseAlphaChannel(true);
	}
	//
	wFTSelect = env->addWindow(irr::core::rect<irr::s32>(550, 240, 780, 340), false, L"");
	wFTSelect->getCloseButton()->setVisible(false);
	wFTSelect->setVisible(false);
	btnFirst = env->addButton(irr::core::rect<irr::s32>(10, 30, 220, 55), wFTSelect, BUTTON_FIRST, dataManager.GetSysString(100));
	btnSecond = env->addButton(irr::core::rect<irr::s32>(10, 60, 220, 85), wFTSelect, BUTTON_SECOND, dataManager.GetSysString(101));
	//message (310)
	wMessage = env->addWindow(irr::core::rect<irr::s32>(490, 200, 840, 340), false, dataManager.GetSysString(1216));
	wMessage->getCloseButton()->setVisible(false);
	wMessage->setVisible(false);
	stMessage =  env->addStaticText(L"", irr::core::rect<irr::s32>(20, 20, 350, 100), false, true, wMessage, -1, false);
	stMessage->setTextAlignment(irr::gui::EGUIA_UPPERLEFT, irr::gui::EGUIA_CENTER);
	btnMsgOK = env->addButton(irr::core::rect<irr::s32>(130, 105, 220, 130), wMessage, BUTTON_MSG_OK, dataManager.GetSysString(1211));
	//auto fade message (310)
	wACMessage = env->addWindow(irr::core::rect<irr::s32>(490, 240, 840, 300), false, L"");
	wACMessage->getCloseButton()->setVisible(false);
	wACMessage->setVisible(false);
	wACMessage->setDrawBackground(false);
	stACMessage = env->addStaticText(L"", irr::core::rect<irr::s32>(0, 0, 350, 60), true, true, wACMessage, -1, true);
	stACMessage->setBackgroundColor(0xc0c0c0ff);
	stACMessage->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	//yes/no (310)
	wQuery = env->addWindow(irr::core::rect<irr::s32>(490, 200, 840, 340), false, dataManager.GetSysString(560));
	wQuery->getCloseButton()->setVisible(false);
	wQuery->setVisible(false);
	stQMessage =  env->addStaticText(L"", irr::core::rect<irr::s32>(20, 20, 350, 100), false, true, wQuery, -1, false);
	stQMessage->setTextAlignment(irr::gui::EGUIA_UPPERLEFT, irr::gui::EGUIA_CENTER);
	btnYes = env->addButton(irr::core::rect<irr::s32>(100, 105, 150, 130), wQuery, BUTTON_YES, dataManager.GetSysString(1213));
	btnNo = env->addButton(irr::core::rect<irr::s32>(200, 105, 250, 130), wQuery, BUTTON_NO, dataManager.GetSysString(1214));
	//surrender yes/no (310)
	wSurrender = env->addWindow(irr::core::rect<irr::s32>(490, 200, 840, 340), false, dataManager.GetSysString(560));
	wSurrender->getCloseButton()->setVisible(false);
	wSurrender->setVisible(false);
	stSurrenderMessage = env->addStaticText(dataManager.GetSysString(1359), irr::core::rect<irr::s32>(20, 20, 350, 100), false, true, wSurrender, -1, false);
	stSurrenderMessage->setTextAlignment(irr::gui::EGUIA_UPPERLEFT, irr::gui::EGUIA_CENTER);
	btnSurrenderYes = env->addButton(irr::core::rect<irr::s32>(100, 105, 150, 130), wSurrender, BUTTON_SURRENDER_YES, dataManager.GetSysString(1213));
	btnSurrenderNo = env->addButton(irr::core::rect<irr::s32>(200, 105, 250, 130), wSurrender, BUTTON_SURRENDER_NO, dataManager.GetSysString(1214));
	//options (310)
	wOptions = env->addWindow(irr::core::rect<irr::s32>(490, 200, 840, 340), false, L"");
	wOptions->getCloseButton()->setVisible(false);
	wOptions->setVisible(false);
	stOptions = env->addStaticText(L"", irr::core::rect<irr::s32>(20, 20, 350, 100), false, true, wOptions, -1, false);
	stOptions->setTextAlignment(irr::gui::EGUIA_UPPERLEFT, irr::gui::EGUIA_CENTER);
	btnOptionOK = env->addButton(irr::core::rect<irr::s32>(130, 105, 220, 130), wOptions, BUTTON_OPTION_OK, dataManager.GetSysString(1211));
	btnOptionp = env->addButton(irr::core::rect<irr::s32>(20, 105, 60, 130), wOptions, BUTTON_OPTION_PREV, L"<<<");
	btnOptionn = env->addButton(irr::core::rect<irr::s32>(290, 105, 330, 130), wOptions, BUTTON_OPTION_NEXT, L">>>");
	for(int i = 0; i < 5; ++i) {
		btnOption[i] = env->addButton(irr::core::rect<irr::s32>(10, 30 + 40 * i, 340, 60 + 40 * i), wOptions, BUTTON_OPTION_0 + i, L"");
	}
	scrOption = env->addScrollBar(false, irr::core::rect<irr::s32>(350, 30, 365, 220), wOptions, SCROLL_OPTION_SELECT);
	scrOption->setLargeStep(1);
	scrOption->setSmallStep(1);
	scrOption->setMin(0);
	//pos select
	wPosSelect = env->addWindow(irr::core::rect<irr::s32>(425, 210, 900, 399), false, dataManager.GetSysString(561));
	wPosSelect->getCloseButton()->setVisible(false);
	wPosSelect->setVisible(false);
	btnPSAU = env->addButton(irr::core::rect<irr::s32>(27, 35, 164, 172), wPosSelect, BUTTON_POS_AU);
	btnPSAU->setUseAlphaChannel(true);
	btnPSAD = env->addButton(irr::core::rect<irr::s32>(27, 35, 164, 172), wPosSelect, BUTTON_POS_AD);
	btnPSAD->setUseAlphaChannel(true);
	btnFacedownImgInfo[btnPSAD] = {0, false};
	btnPSAD->setVisible(false); // PSAD = PoSition Attack face-Down, is not allowed in the rules, so the width of wPosSelect only support 3 buttons
	btnPSDU = env->addButton(irr::core::rect<irr::s32>(169, 35, 306, 172), wPosSelect, BUTTON_POS_DU);
	btnPSDU->setUseAlphaChannel(true);
	btnPSDD = env->addButton(irr::core::rect<irr::s32>(311, 35, 448, 172), wPosSelect, BUTTON_POS_DD);
	btnPSDD->setUseAlphaChannel(true);
	btnFacedownImgInfo[btnPSDD] = {0, true};
	//card select
	wCardSelect = env->addWindow(irr::core::rect<irr::s32>(320, 100, 1000, 400), false, L"");
	wCardSelect->getCloseButton()->setVisible(false);
	wCardSelect->setVisible(false);
	for(int i = 0; i < 5; ++i) {
		stCardPos[i] = env->addStaticText(L"", irr::core::rect<irr::s32>(30 + 125 * i, 30, 150 + 125 * i, 50), true, false, wCardSelect, -1, true);
		stCardPos[i]->setBackgroundColor(CARD_LIST_DEFAULT_BACKGROUND_COLOR);
		stCardPos[i]->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
		btnCardSelect[i] = env->addButton(irr::core::rect<irr::s32>(30 + 125 * i, 55, 150 + 125 * i, 225), wCardSelect, BUTTON_CARD_0 + i);
		btnCardSelect[i]->setUseAlphaChannel(true);
	}
	scrCardList = env->addScrollBar(true, irr::core::rect<irr::s32>(30, 235, 650, 255), wCardSelect, SCROLL_CARD_SELECT);
	btnSelectOK = env->addButton(irr::core::rect<irr::s32>(300, 265, 380, 290), wCardSelect, BUTTON_CARD_SEL_OK, dataManager.GetSysString(1211));
	//card display
	wCardDisplay = env->addWindow(irr::core::rect<irr::s32>(320, 100, 1000, 400), false, L"");
	wCardDisplay->getCloseButton()->setVisible(false);
	wCardDisplay->setVisible(false);
	for(int i = 0; i < 5; ++i) {
		stDisplayPos[i] = env->addStaticText(L"", irr::core::rect<irr::s32>(30 + 125 * i, 30, 150 + 125 * i, 50), true, false, wCardDisplay, -1, true);
		stDisplayPos[i]->setBackgroundColor(CARD_LIST_DEFAULT_BACKGROUND_COLOR);
		stDisplayPos[i]->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
		btnCardDisplay[i] = env->addButton(irr::core::rect<irr::s32>(30 + 125 * i, 55, 150 + 125 * i, 225), wCardDisplay, BUTTON_DISPLAY_0 + i);
		btnCardDisplay[i]->setUseAlphaChannel(true);
	}
	scrDisplayList = env->addScrollBar(true, irr::core::rect<irr::s32>(30, 235, 650, 255), wCardDisplay, SCROLL_CARD_DISPLAY);
	btnDisplayOK = env->addButton(irr::core::rect<irr::s32>(300, 265, 380, 290), wCardDisplay, BUTTON_CARD_DISP_OK, dataManager.GetSysString(1211));
	//announce number
	wANNumber = env->addWindow(irr::core::rect<irr::s32>(550, 180, 780, 430), false, L"");
	wANNumber->getCloseButton()->setVisible(false);
	wANNumber->setVisible(false);
	cbANNumber =  env->addComboBox(irr::core::rect<irr::s32>(40, 30, 190, 50), wANNumber, -1);
	cbANNumber->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	for(int i = 0; i < 12; ++i) {
		myswprintf(strbuf, L"%d", i + 1);
		btnANNumber[i] = env->addButton(irr::core::rect<irr::s32>(20 + 50 * (i % 4), 40 + 50 * (i / 4), 60 + 50 * (i % 4), 80 + 50 * (i / 4)), wANNumber, BUTTON_ANNUMBER_1 + i, strbuf);
		btnANNumber[i]->setIsPushButton(true);
	}
	btnANNumberOK = env->addButton(irr::core::rect<irr::s32>(80, 60, 150, 85), wANNumber, BUTTON_ANNUMBER_OK, dataManager.GetSysString(1211));
	//announce card
	wANCard = env->addWindow(irr::core::rect<irr::s32>(510, 120, 820, 420), false, L"");
	wANCard->getCloseButton()->setVisible(false);
	wANCard->setVisible(false);
	ebANCard = env->addEditBox(L"", irr::core::rect<irr::s32>(20, 25, 290, 45), true, wANCard, EDITBOX_ANCARD);
	ebANCard->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	lstANCard = env->addListBox(irr::core::rect<irr::s32>(20, 50, 290, 265), wANCard, LISTBOX_ANCARD, true);
	btnANCardOK = env->addButton(irr::core::rect<irr::s32>(110, 270, 200, 295), wANCard, BUTTON_ANCARD_OK, dataManager.GetSysString(1211));
	//announce attribute
	wANAttribute = env->addWindow(irr::core::rect<irr::s32>(500, 200, 830, 285), false, dataManager.GetSysString(562));
	wANAttribute->getCloseButton()->setVisible(false);
	wANAttribute->setVisible(false);
	for (int i = 0; i < ATTRIBUTES_COUNT; ++i)
		chkAttribute[i] = env->addCheckBox(false, irr::core::rect<irr::s32>(10 + (i % 4) * 80, 25 + (i / 4) * 25, 90 + (i % 4) * 80, 50 + (i / 4) * 25),
			wANAttribute, CHECK_ATTRIBUTE, dataManager.GetSysString(DataManager::STRING_ID_ATTRIBUTE + i));
	//announce race
	wANRace = env->addWindow(irr::core::rect<irr::s32>(480, 200, 850, 410), false, dataManager.GetSysString(563));
	wANRace->getCloseButton()->setVisible(false);
	wANRace->setVisible(false);
	for (int i = 0; i < RACES_COUNT; ++i)
		chkRace[i] = env->addCheckBox(false, irr::core::rect<irr::s32>(10 + (i % 4) * 90, 25 + (i / 4) * 25, 100 + (i % 4) * 90, 50 + (i / 4) * 25),
			wANRace, CHECK_RACE, dataManager.GetSysString(DataManager::STRING_ID_RACE + i));
	//selection hint
	stHintMsg = env->addStaticText(L"", irr::core::rect<irr::s32>(500, 60, 820, 90), true, false, 0, -1, false);
	stHintMsg->setBackgroundColor(0xc0ffffff);
	stHintMsg->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stHintMsg->setVisible(false);
	//cmd menu
	wCmdMenu = env->addWindow(irr::core::rect<irr::s32>(10, 10, 110, 179), false, L"");
	wCmdMenu->setDrawTitlebar(false);
	wCmdMenu->setVisible(false);
	wCmdMenu->getCloseButton()->setVisible(false);
	btnActivate = env->addButton(irr::core::rect<irr::s32>(1, 1, 99, 21), wCmdMenu, BUTTON_CMD_ACTIVATE, dataManager.GetSysString(1150));
	btnSummon = env->addButton(irr::core::rect<irr::s32>(1, 22, 99, 42), wCmdMenu, BUTTON_CMD_SUMMON, dataManager.GetSysString(1151));
	btnSPSummon = env->addButton(irr::core::rect<irr::s32>(1, 43, 99, 63), wCmdMenu, BUTTON_CMD_SPSUMMON, dataManager.GetSysString(1152));
	btnMSet = env->addButton(irr::core::rect<irr::s32>(1, 64, 99, 84), wCmdMenu, BUTTON_CMD_MSET, dataManager.GetSysString(1153));
	btnSSet = env->addButton(irr::core::rect<irr::s32>(1, 85, 99, 105), wCmdMenu, BUTTON_CMD_SSET, dataManager.GetSysString(1153));
	btnRepos = env->addButton(irr::core::rect<irr::s32>(1, 106, 99, 126), wCmdMenu, BUTTON_CMD_REPOS, dataManager.GetSysString(1154));
	btnAttack = env->addButton(irr::core::rect<irr::s32>(1, 127, 99, 147), wCmdMenu, BUTTON_CMD_ATTACK, dataManager.GetSysString(1157));
	btnShowList = env->addButton(irr::core::rect<irr::s32>(1, 148, 99, 168), wCmdMenu, BUTTON_CMD_SHOWLIST, dataManager.GetSysString(1158));
	btnOperation = env->addButton(irr::core::rect<irr::s32>(1, 169, 99, 189), wCmdMenu, BUTTON_CMD_ACTIVATE, dataManager.GetSysString(1161));
	btnReset = env->addButton(irr::core::rect<irr::s32>(1, 190, 99, 210), wCmdMenu, BUTTON_CMD_RESET, dataManager.GetSysString(1162));
	//deck edit
	wDeckEdit = env->addStaticText(L"", irr::core::rect<irr::s32>(309, 5, 605, 130), true, false, 0, -1, true);
	wDeckEdit->setVisible(false);
	btnManageDeck = env->addButton(irr::core::rect<irr::s32>(225, 5, 290, 30), wDeckEdit, BUTTON_MANAGE_DECK, dataManager.GetSysString(1328));
	//deck manage
	wDeckManage = env->addWindow(irr::core::rect<irr::s32>(310, 135, 800, 515), false, dataManager.GetSysString(1460), 0, WINDOW_DECK_MANAGE);
	wDeckManage->setVisible(false);
	lstCategories = env->addListBox(irr::core::rect<irr::s32>(10, 30, 140, 370), wDeckManage, LISTBOX_CATEGORIES, true);
	lstDecks = env->addListBox(irr::core::rect<irr::s32>(150, 30, 340, 370), wDeckManage, LISTBOX_DECKS, true);
	posY = 30;
	btnNewCategory = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_NEW_CATEGORY, dataManager.GetSysString(1461));
	posY += 35;
	btnRenameCategory = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_RENAME_CATEGORY, dataManager.GetSysString(1462));
	posY += 35;
	btnDeleteCategory = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_DELETE_CATEGORY, dataManager.GetSysString(1463));
	posY += 35;
	btnNewDeck = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_NEW_DECK, dataManager.GetSysString(1464));
	posY += 35;
	btnRenameDeck = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_RENAME_DECK, dataManager.GetSysString(1465));
	posY += 35;
	btnDMDeleteDeck = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_DELETE_DECK_DM, dataManager.GetSysString(1466));
	posY += 35;
	btnMoveDeck = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_MOVE_DECK, dataManager.GetSysString(1467));
	posY += 35;
	btnCopyDeck = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_COPY_DECK, dataManager.GetSysString(1468));
	posY += 35;
	btnImportDeckCode = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_IMPORT_DECK_CODE, dataManager.GetSysString(1478));
	posY += 35;
	btnExportDeckCode = env->addButton(irr::core::rect<irr::s32>(350, posY, 480, posY + 25), wDeckManage, BUTTON_EXPORT_DECK_CODE, dataManager.GetSysString(1479));
	//deck manage query
	wDMQuery = env->addWindow(irr::core::rect<irr::s32>(400, 200, 710, 320), false, dataManager.GetSysString(1460));
	wDMQuery->getCloseButton()->setVisible(false);
	wDMQuery->setVisible(false);
	stDMMessage = env->addStaticText(L"", irr::core::rect<irr::s32>(20, 25, 290, 45), false, false, wDMQuery);
	stDMMessage2 = env->addStaticText(L"", irr::core::rect<irr::s32>(20, 50, 290, 70), false, false, wDMQuery, -1, true);
	stDMMessage2->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	ebDMName = env->addEditBox(L"", irr::core::rect<irr::s32>(20, 50, 290, 70), true, wDMQuery, -1);
	ebDMName->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	cbDMCategory = env->addComboBox(irr::core::rect<irr::s32>(20, 50, 290, 70), wDMQuery, -1);
	stDMMessage2->setVisible(false);
	ebDMName->setVisible(false);
	cbDMCategory->setVisible(false);
	cbDMCategory->setMaxSelectionRows(10);
	btnDMOK = env->addButton(irr::core::rect<irr::s32>(70, 80, 140, 105), wDMQuery, BUTTON_DM_OK, dataManager.GetSysString(1211));
	btnDMCancel = env->addButton(irr::core::rect<irr::s32>(170, 80, 240, 105), wDMQuery, BUTTON_DM_CANCEL, dataManager.GetSysString(1212));
	scrPackCards = env->addScrollBar(false, irr::core::recti(775, 161, 795, 629), 0, SCROLL_FILTER);
	scrPackCards->setLargeStep(1);
	scrPackCards->setSmallStep(1);
	scrPackCards->setVisible(false);

	stDBCategory = env->addStaticText(dataManager.GetSysString(1300), irr::core::rect<irr::s32>(10, 9, 100, 29), false, false, wDeckEdit);
	cbDBCategory = env->addComboBox(irr::core::rect<irr::s32>(80, 5, 220, 30), wDeckEdit, COMBOBOX_DBCATEGORY);
	cbDBCategory->setMaxSelectionRows(15);
	stDeck = env->addStaticText(dataManager.GetSysString(1301), irr::core::rect<irr::s32>(10, 39, 100, 59), false, false, wDeckEdit);
	cbDBDecks = env->addComboBox(irr::core::rect<irr::s32>(80, 35, 220, 60), wDeckEdit, COMBOBOX_DBDECKS);
	cbDBDecks->setMaxSelectionRows(15);
	btnSaveDeck = env->addButton(irr::core::rect<irr::s32>(225, 35, 290, 60), wDeckEdit, BUTTON_SAVE_DECK, dataManager.GetSysString(1302));
	ebDeckname = env->addEditBox(L"", irr::core::rect<irr::s32>(80, 65, 220, 90), true, wDeckEdit, -1);
	ebDeckname->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	btnSaveDeckAs = env->addButton(irr::core::rect<irr::s32>(225, 65, 290, 90), wDeckEdit, BUTTON_SAVE_DECK_AS, dataManager.GetSysString(1303));
	btnDeleteDeck = env->addButton(irr::core::rect<irr::s32>(225, 95, 290, 120), wDeckEdit, BUTTON_DELETE_DECK, dataManager.GetSysString(1308));
	btnShuffleDeck = env->addButton(irr::core::rect<irr::s32>(5, 99, 55, 120), wDeckEdit, BUTTON_SHUFFLE_DECK, dataManager.GetSysString(1307));
	btnSortDeck = env->addButton(irr::core::rect<irr::s32>(60, 99, 110, 120), wDeckEdit, BUTTON_SORT_DECK, dataManager.GetSysString(1305));
	btnClearDeck = env->addButton(irr::core::rect<irr::s32>(115, 99, 165, 120), wDeckEdit, BUTTON_CLEAR_DECK, dataManager.GetSysString(1304));
	btnSideOK = env->addButton(irr::core::rect<irr::s32>(400, 40, 710, 80), 0, BUTTON_SIDE_OK, dataManager.GetSysString(1334));
	btnSideOK->setVisible(false);
	btnSideShuffle = env->addButton(irr::core::rect<irr::s32>(310, 100, 370, 130), 0, BUTTON_SHUFFLE_DECK, dataManager.GetSysString(1307));
	btnSideShuffle->setVisible(false);
	btnSideSort = env->addButton(irr::core::rect<irr::s32>(375, 100, 435, 130), 0, BUTTON_SORT_DECK, dataManager.GetSysString(1305));
	btnSideSort->setVisible(false);
	btnSideReload = env->addButton(irr::core::rect<irr::s32>(440, 100, 500, 130), 0, BUTTON_SIDE_RELOAD, dataManager.GetSysString(1309));
	btnSideReload->setVisible(false);
	//
	scrFilter = env->addScrollBar(false, irr::core::recti(999, 161, 1019, 629), 0, SCROLL_FILTER);
	scrFilter->setLargeStep(10);
	scrFilter->setSmallStep(1);
	scrFilter->setVisible(false);
	//sort type
	wSort = env->addStaticText(L"", irr::core::rect<irr::s32>(930, 132, 1020, 156), true, false, 0, -1, true);
	cbSortType = env->addComboBox(irr::core::rect<irr::s32>(10, 2, 85, 22), wSort, COMBOBOX_SORTTYPE);
	cbSortType->setMaxSelectionRows(10);
	for(int i = 1370; i <= 1373; i++)
		cbSortType->addItem(dataManager.GetSysString(i));
	wSort->setVisible(false);
	//filters
	wFilter = env->addStaticText(L"", irr::core::rect<irr::s32>(610, 5, 1020, 130), true, false, 0, -1, true);
	wFilter->setVisible(false);
	stCategory = env->addStaticText(dataManager.GetSysString(1311), irr::core::rect<irr::s32>(10, 2 + 25 / 6, 70, 22 + 25 / 6), false, false, wFilter);
	cbCardType = env->addComboBox(irr::core::rect<irr::s32>(60, 25 / 6, 120, 20 + 25 / 6), wFilter, COMBOBOX_MAINTYPE);
	cbCardType->addItem(dataManager.GetSysString(1310));
	cbCardType->addItem(dataManager.GetSysString(1312));
	cbCardType->addItem(dataManager.GetSysString(1313));
	cbCardType->addItem(dataManager.GetSysString(1314));
	cbCardType2 = env->addComboBox(irr::core::rect<irr::s32>(125, 25 / 6, 195, 20 + 25 / 6), wFilter, COMBOBOX_SECONDTYPE);
	cbCardType2->setMaxSelectionRows(10);
	cbCardType2->addItem(dataManager.GetSysString(1310), 0);
	stLimit = env->addStaticText(dataManager.GetSysString(1315), irr::core::rect<irr::s32>(205, 2 + 25 / 6, 280, 22 + 25 / 6), false, false, wFilter);
	cbLimit = env->addComboBox(irr::core::rect<irr::s32>(260, 25 / 6, 390, 20 + 25 / 6), wFilter, COMBOBOX_LIMIT);
	cbLimit->setMaxSelectionRows(10);
	cbLimit->addItem(dataManager.GetSysString(1310));
	cbLimit->addItem(dataManager.GetSysString(1316));
	cbLimit->addItem(dataManager.GetSysString(1317));
	cbLimit->addItem(dataManager.GetSysString(1318));
	cbLimit->addItem(dataManager.GetSysString(1481));
	cbLimit->addItem(dataManager.GetSysString(1482));
	cbLimit->addItem(dataManager.GetSysString(1483));
	cbLimit->addItem(dataManager.GetSysString(1484));
	cbLimit->addItem(dataManager.GetSysString(1487));
	cbLimit->addItem(dataManager.GetSysString(1488));
	cbLimit->addItem(dataManager.GetSysString(1485));
	stAttribute = env->addStaticText(dataManager.GetSysString(1319), irr::core::rect<irr::s32>(10, 22 + 50 / 6, 70, 42 + 50 / 6), false, false, wFilter);
	cbAttribute = env->addComboBox(irr::core::rect<irr::s32>(60, 20 + 50 / 6, 195, 40 + 50 / 6), wFilter, COMBOBOX_ATTRIBUTE);
	cbAttribute->setMaxSelectionRows(10);
	cbAttribute->addItem(dataManager.GetSysString(1310), 0);
	for (int i = 0; i < ATTRIBUTES_COUNT; ++i)
		cbAttribute->addItem(dataManager.GetSysString(DataManager::STRING_ID_ATTRIBUTE + i), 0x1U << i);
	stRace = env->addStaticText(dataManager.GetSysString(1321), irr::core::rect<irr::s32>(10, 42 + 75 / 6, 70, 62 + 75 / 6), false, false, wFilter);
	cbRace = env->addComboBox(irr::core::rect<irr::s32>(60, 40 + 75 / 6, 195, 60 + 75 / 6), wFilter, COMBOBOX_RACE);
	cbRace->setMaxSelectionRows(10);
	cbRace->addItem(dataManager.GetSysString(1310), 0);
	for (int i = 0; i < RACES_COUNT; ++i)
		cbRace->addItem(dataManager.GetSysString(DataManager::STRING_ID_RACE + i), 0x1U << i);
	stAttack = env->addStaticText(dataManager.GetSysString(1322), irr::core::rect<irr::s32>(205, 22 + 50 / 6, 280, 42 + 50 / 6), false, false, wFilter);
	ebAttack = env->addEditBox(L"", irr::core::rect<irr::s32>(260, 20 + 50 / 6, 340, 40 + 50 / 6), true, wFilter, EDITBOX_INPUTS);
	ebAttack->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stDefense = env->addStaticText(dataManager.GetSysString(1323), irr::core::rect<irr::s32>(205, 42 + 75 / 6, 280, 62 + 75 / 6), false, false, wFilter);
	ebDefense = env->addEditBox(L"", irr::core::rect<irr::s32>(260, 40 + 75 / 6, 340, 60 + 75 / 6), true, wFilter, EDITBOX_INPUTS);
	ebDefense->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stStar = env->addStaticText(dataManager.GetSysString(1324), irr::core::rect<irr::s32>(10, 62 + 100 / 6, 70, 82 + 100 / 6), false, false, wFilter);
	ebStar = env->addEditBox(L"", irr::core::rect<irr::s32>(60, 60 + 100 / 6, 95, 80 + 100 / 6), true, wFilter, EDITBOX_INPUTS);
	ebStar->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stScale = env->addStaticText(dataManager.GetSysString(1336), irr::core::rect<irr::s32>(105, 62 + 100 / 6, 165, 82 + 100 / 6), false, false, wFilter);
	ebScale = env->addEditBox(L"", irr::core::rect<irr::s32>(155, 60 + 100 / 6, 195, 80 + 100 / 6), true, wFilter, EDITBOX_INPUTS);
	ebScale->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stSearch = env->addStaticText(dataManager.GetSysString(1325), irr::core::rect<irr::s32>(205, 62 + 100 / 6, 280, 82 + 100 / 6), false, false, wFilter);
	ebCardName = env->addEditBox(L"", irr::core::rect<irr::s32>(260, 60 + 100 / 6, 390, 80 + 100 / 6), true, wFilter, EDITBOX_KEYWORD);
	ebCardName->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	btnEffectFilter = env->addButton(irr::core::rect<irr::s32>(345, 20 + 50 / 6, 390, 60 + 75 / 6), wFilter, BUTTON_EFFECT_FILTER, dataManager.GetSysString(1326));
	btnStartFilter = env->addButton(irr::core::rect<irr::s32>(205, 80 + 125 / 6, 390, 100 + 125 / 6), wFilter, BUTTON_START_FILTER, dataManager.GetSysString(1327));
	if(gameConf.separate_clear_button) {
		btnStartFilter->setRelativePosition(irr::core::rect<irr::s32>(260, 80 + 125 / 6, 390, 100 + 125 / 6));
		btnClearFilter = env->addButton(irr::core::rect<irr::s32>(205, 80 + 125 / 6, 255, 100 + 125 / 6), wFilter, BUTTON_CLEAR_FILTER, dataManager.GetSysString(1304));
	}
	wCategories = env->addWindow(irr::core::rect<irr::s32>(600, 55, 1000, 300), false, L"");
	wCategories->getCloseButton()->setVisible(false);
	wCategories->setDrawTitlebar(false);
	wCategories->setDraggable(false);
	wCategories->setVisible(false);
	btnCategoryOK = env->addButton(irr::core::rect<irr::s32>(150, 210, 250, 235), wCategories, BUTTON_CATEGORY_OK, dataManager.GetSysString(1211));
	int catewidth = 0;
	for(int i = 0; i < 32; ++i) {
		irr::core::dimension2d<unsigned int> dtxt = guiFont->getDimension(dataManager.GetSysString(1100 + i));
		if((int)dtxt.Width + 40 > catewidth)
			catewidth = dtxt.Width + 40;
	}
	for(int i = 0; i < 32; ++i)
		chkCategory[i] = env->addCheckBox(false, irr::core::recti(10 + (i % 4) * catewidth, 5 + (i / 4) * 25, 10 + (i % 4 + 1) * catewidth, 5 + (i / 4 + 1) * 25), wCategories, -1, dataManager.GetSysString(1100 + i));
	int wcatewidth = catewidth * 4 + 16;
	wCategories->setRelativePosition(irr::core::rect<irr::s32>(1000 - wcatewidth, 55, 1000, 300));
	btnCategoryOK->setRelativePosition(irr::core::recti(wcatewidth / 2 - 50, 210, wcatewidth / 2 + 50, 235));
	btnMarksFilter = env->addButton(irr::core::rect<irr::s32>(60, 80 + 125 / 6, 195, 100 + 125 / 6), wFilter, BUTTON_MARKS_FILTER, dataManager.GetSysString(1374));
	wLinkMarks = env->addWindow(irr::core::rect<irr::s32>(700, 30, 820, 150), false, L"");
	wLinkMarks->getCloseButton()->setVisible(false);
	wLinkMarks->setDrawTitlebar(false);
	wLinkMarks->setDraggable(false);
	wLinkMarks->setVisible(false);
	btnMarksOK = env->addButton(irr::core::recti(45, 45, 75, 75), wLinkMarks, BUTTON_MARKERS_OK, dataManager.GetSysString(1211));
	btnMark[0] = env->addButton(irr::core::recti(10, 10, 40, 40), wLinkMarks, -1, L"\u2196");
	btnMark[1] = env->addButton(irr::core::recti(45, 10, 75, 40), wLinkMarks, -1, L"\u2191");
	btnMark[2] = env->addButton(irr::core::recti(80, 10, 110, 40), wLinkMarks, -1, L"\u2197");
	btnMark[3] = env->addButton(irr::core::recti(10, 45, 40, 75), wLinkMarks, -1, L"\u2190");
	btnMark[4] = env->addButton(irr::core::recti(80, 45, 110, 75), wLinkMarks, -1, L"\u2192");
	btnMark[5] = env->addButton(irr::core::recti(10, 80, 40, 110), wLinkMarks, -1, L"\u2199");
	btnMark[6] = env->addButton(irr::core::recti(45, 80, 75, 110), wLinkMarks, -1, L"\u2193");
	btnMark[7] = env->addButton(irr::core::recti(80, 80, 110, 110), wLinkMarks, -1, L"\u2198");
	for(int i=0;i<8;i++)
		btnMark[i]->setIsPushButton(true);
	//replay window
	wReplay = env->addWindow(irr::core::rect<irr::s32>(220, 100, 800, 520), false, dataManager.GetSysString(1202));
	wReplay->getCloseButton()->setVisible(false);
	wReplay->setVisible(false);
	lstReplayList = env->addListBox(irr::core::rect<irr::s32>(10, 30, 350, 400), wReplay, LISTBOX_REPLAY_LIST, true);
	lstReplayList->setItemHeight(18);
	btnLoadReplay = env->addButton(irr::core::rect<irr::s32>(470, 355, 570, 380), wReplay, BUTTON_LOAD_REPLAY, dataManager.GetSysString(1348));
	btnDeleteReplay = env->addButton(irr::core::rect<irr::s32>(360, 355, 460, 380), wReplay, BUTTON_DELETE_REPLAY, dataManager.GetSysString(1361));
	btnRenameReplay = env->addButton(irr::core::rect<irr::s32>(360, 385, 460, 410), wReplay, BUTTON_RENAME_REPLAY, dataManager.GetSysString(1362));
	btnReplayCancel = env->addButton(irr::core::rect<irr::s32>(470, 385, 570, 410), wReplay, BUTTON_CANCEL_REPLAY, dataManager.GetSysString(1347));
	btnExportDeck = env->addButton(irr::core::rect<irr::s32>(470, 325, 570, 350), wReplay, BUTTON_EXPORT_DECK, dataManager.GetSysString(1369));
	stReplayHeading = env->addStaticText(dataManager.GetSysString(1349), irr::core::rect<irr::s32>(360, 30, 570, 50), false, true, wReplay);
	stReplayInfo = env->addStaticText(L"", irr::core::rect<irr::s32>(360, 60, 570, 320), false, true, wReplay);
	stReplayStartTurn = env->addStaticText(dataManager.GetSysString(1353), irr::core::rect<irr::s32>(360, 275, 570, 295), false, true, wReplay);
	ebRepStartTurn = env->addEditBox(L"", irr::core::rect<irr::s32>(360, 300, 460, 320), true, wReplay, -1);
	ebRepStartTurn->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	//single play window
	wSinglePlay = env->addWindow(irr::core::rect<irr::s32>(220, 100, 800, 520), false, dataManager.GetSysString(1201));
	wSinglePlay->getCloseButton()->setVisible(false);
	wSinglePlay->setVisible(false);
	irr::gui::IGUITabControl* wSingle = env->addTabControl(irr::core::rect<irr::s32>(0, 20, 579, 419), wSinglePlay, true);
	if(gameConf.enable_bot_mode) {
		irr::gui::IGUITab* tabBot = wSingle->addTab(dataManager.GetSysString(1380));
		lstBotList = env->addListBox(irr::core::rect<irr::s32>(10, 10, 350, 350), tabBot, LISTBOX_BOT_LIST, true);
		lstBotList->setItemHeight(18);
		btnStartBot = env->addButton(irr::core::rect<irr::s32>(459, 301, 569, 326), tabBot, BUTTON_BOT_START, dataManager.GetSysString(1211));
		btnBotCancel = env->addButton(irr::core::rect<irr::s32>(459, 331, 569, 356), tabBot, BUTTON_CANCEL_SINGLEPLAY, dataManager.GetSysString(1210));
		env->addStaticText(dataManager.GetSysString(1382), irr::core::rect<irr::s32>(360, 10, 550, 30), false, true, tabBot);
		stBotInfo = env->addStaticText(L"", irr::core::rect<irr::s32>(360, 40, 560, 160), false, true, tabBot);
		cbBotDeckCategory = env->addComboBox(irr::core::rect<irr::s32>(360, 95, 560, 120), tabBot, COMBOBOX_BOT_DECKCATEGORY);
		cbBotDeckCategory->setMaxSelectionRows(6);
		cbBotDeckCategory->setVisible(false);
		cbBotDeck = env->addComboBox(irr::core::rect<irr::s32>(360, 130, 560, 155), tabBot);
		cbBotDeck->setMaxSelectionRows(6);
		cbBotDeck->setVisible(false);
		cbBotRule = env->addComboBox(irr::core::rect<irr::s32>(360, 165, 560, 190), tabBot, COMBOBOX_BOT_RULE);
		cbBotRule->addItem(dataManager.GetSysString(1262));
		cbBotRule->addItem(dataManager.GetSysString(1263));
		cbBotRule->addItem(dataManager.GetSysString(1264));
		cbBotRule->setSelected(gameConf.default_rule - 3);
		chkBotHand = env->addCheckBox(false, irr::core::rect<irr::s32>(360, 200, 560, 220), tabBot, -1, dataManager.GetSysString(1384));
		chkBotNoCheckDeck = env->addCheckBox(false, irr::core::rect<irr::s32>(360, 230, 560, 250), tabBot, -1, dataManager.GetSysString(1229));
		chkBotNoShuffleDeck = env->addCheckBox(false, irr::core::rect<irr::s32>(360, 260, 560, 280), tabBot, -1, dataManager.GetSysString(1230));
	} else { // avoid null pointer
		btnStartBot = env->addButton(irr::core::rect<irr::s32>(0, 0, 0, 0), wSinglePlay);
		btnBotCancel = env->addButton(irr::core::rect<irr::s32>(0, 0, 0, 0), wSinglePlay);
		btnStartBot->setVisible(false);
		btnBotCancel->setVisible(false);
	}
	irr::gui::IGUITab* tabSingle = wSingle->addTab(dataManager.GetSysString(1381));
	lstSinglePlayList = env->addListBox(irr::core::rect<irr::s32>(10, 10, 350, 350), tabSingle, LISTBOX_SINGLEPLAY_LIST, true);
	lstSinglePlayList->setItemHeight(18);
	btnLoadSinglePlay = env->addButton(irr::core::rect<irr::s32>(459, 301, 569, 326), tabSingle, BUTTON_LOAD_SINGLEPLAY, dataManager.GetSysString(1211));
	btnSinglePlayCancel = env->addButton(irr::core::rect<irr::s32>(459, 331, 569, 356), tabSingle, BUTTON_CANCEL_SINGLEPLAY, dataManager.GetSysString(1210));
	env->addStaticText(dataManager.GetSysString(1352), irr::core::rect<irr::s32>(360, 10, 550, 30), false, true, tabSingle);
	stSinglePlayInfo = env->addStaticText(L"", irr::core::rect<irr::s32>(360, 40, 560, 160), false, true, tabSingle);
	chkSinglePlayReturnDeckTop = env->addCheckBox(false, irr::core::rect<irr::s32>(360, 260, 560, 280), tabSingle, -1, dataManager.GetSysString(1238));
	//replay save
	wReplaySave = env->addWindow(irr::core::rect<irr::s32>(510, 200, 820, 320), false, dataManager.GetSysString(1340));
	wReplaySave->getCloseButton()->setVisible(false);
	wReplaySave->setVisible(false);
	env->addStaticText(dataManager.GetSysString(1342), irr::core::rect<irr::s32>(20, 25, 290, 45), false, false, wReplaySave);
	ebRSName =  env->addEditBox(L"", irr::core::rect<irr::s32>(20, 50, 290, 70), true, wReplaySave, -1);
	ebRSName->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	btnRSYes = env->addButton(irr::core::rect<irr::s32>(70, 80, 140, 105), wReplaySave, BUTTON_REPLAY_SAVE, dataManager.GetSysString(1341));
	btnRSNo = env->addButton(irr::core::rect<irr::s32>(170, 80, 240, 105), wReplaySave, BUTTON_REPLAY_CANCEL, dataManager.GetSysString(1212));
	//replay control
	wReplayControl = env->addStaticText(L"", irr::core::rect<irr::s32>(205, 143, 295, 273), true, false, 0, -1, true);
	wReplayControl->setVisible(false);
	btnReplayStart = env->addButton(irr::core::rect<irr::s32>(5, 5, 85, 25), wReplayControl, BUTTON_REPLAY_START, dataManager.GetSysString(1343));
	btnReplayPause = env->addButton(irr::core::rect<irr::s32>(5, 5, 85, 25), wReplayControl, BUTTON_REPLAY_PAUSE, dataManager.GetSysString(1344));
	btnReplaySwap = env->addButton(irr::core::rect<irr::s32>(5, 30, 85, 50), wReplayControl, BUTTON_REPLAY_SWAP, dataManager.GetSysString(1346));
	btnReplayStep = env->addButton(irr::core::rect<irr::s32>(5, 55, 85, 75), wReplayControl, BUTTON_REPLAY_STEP, dataManager.GetSysString(1345));
	btnReplayUndo = env->addButton(irr::core::rect<irr::s32>(5, 80, 85, 100), wReplayControl, BUTTON_REPLAY_UNDO, dataManager.GetSysString(1360));
	btnReplayExit = env->addButton(irr::core::rect<irr::s32>(5, 105, 85, 125), wReplayControl, BUTTON_REPLAY_EXIT, dataManager.GetSysString(1347));
	//chat
	wChat = env->addWindow(irr::core::rect<irr::s32>(307, 615, GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT), false, L"");
	wChat->getCloseButton()->setVisible(false);
	wChat->setDraggable(false);
	wChat->setDrawTitlebar(false);
	wChat->setVisible(false);
	ebChatInput = env->addEditBox(L"", irr::core::rect<irr::s32>(3, 2, 711, 22), true, wChat, EDITBOX_CHAT);
	//swap
	btnSpectatorSwap = env->addButton(irr::core::rect<irr::s32>(205, 100, 295, 135), 0, BUTTON_REPLAY_SWAP, dataManager.GetSysString(1346));
	btnSpectatorSwap->setVisible(false);
	//chain buttons
	btnChainIgnore = env->addButton(irr::core::rect<irr::s32>(205, 100, 295, 135), 0, BUTTON_CHAIN_IGNORE, dataManager.GetSysString(1292));
	btnChainAlways = env->addButton(irr::core::rect<irr::s32>(205, 140, 295, 175), 0, BUTTON_CHAIN_ALWAYS, dataManager.GetSysString(1293));
	btnChainWhenAvail = env->addButton(irr::core::rect<irr::s32>(205, 180, 295, 215), 0, BUTTON_CHAIN_WHENAVAIL, dataManager.GetSysString(1294));
	btnChainIgnore->setIsPushButton(true);
	btnChainAlways->setIsPushButton(true);
	btnChainWhenAvail->setIsPushButton(true);
	btnChainIgnore->setVisible(false);
	btnChainAlways->setVisible(false);
	btnChainWhenAvail->setVisible(false);
	//shuffle
	btnShuffle = env->addButton(irr::core::rect<irr::s32>(205, 230, 295, 265), 0, BUTTON_CMD_SHUFFLE, dataManager.GetSysString(1297));
	btnShuffle->setVisible(false);
	//cancel or finish
	btnCancelOrFinish = env->addButton(irr::core::rect<irr::s32>(205, 230, 295, 265), 0, BUTTON_CANCEL_OR_FINISH, dataManager.GetSysString(1295));
	btnCancelOrFinish->setVisible(false);
	//big picture
	wBigCard = env->addWindow(irr::core::rect<irr::s32>(0, 0, 0, 0), false, L"");
	wBigCard->getCloseButton()->setVisible(false);
	wBigCard->setDrawTitlebar(false);
	wBigCard->setDrawBackground(false);
	wBigCard->setVisible(false);
	imgBigCard = env->addImage(irr::core::rect<irr::s32>(0, 0, 0, 0), wBigCard);
	imgBigCard->setScaleImage(false);
	imgBigCard->setUseAlphaChannel(true);
	btnBigCardOriginalSize = env->addButton(irr::core::rect<irr::s32>(205, 100, 295, 135), 0, BUTTON_BIG_CARD_ORIG_SIZE, dataManager.GetSysString(1443));
	btnBigCardZoomIn = env->addButton(irr::core::rect<irr::s32>(205, 140, 295, 175), 0, BUTTON_BIG_CARD_ZOOM_IN, dataManager.GetSysString(1441));
	btnBigCardZoomOut = env->addButton(irr::core::rect<irr::s32>(205, 180, 295, 215), 0, BUTTON_BIG_CARD_ZOOM_OUT, dataManager.GetSysString(1442));
	btnBigCardClose = env->addButton(irr::core::rect<irr::s32>(205, 230, 295, 265), 0, BUTTON_BIG_CARD_CLOSE, dataManager.GetSysString(1440));
	btnBigCardOriginalSize->setVisible(false);
	btnBigCardZoomIn->setVisible(false);
	btnBigCardZoomOut->setVisible(false);
	btnBigCardClose->setVisible(false);
	//leave/surrender/exit
	btnLeaveGame = env->addButton(irr::core::rect<irr::s32>(205, 5, 295, 80), 0, BUTTON_LEAVE_GAME, L"");
	btnLeaveGame->setVisible(false);
	//tip
	stTip = env->addStaticText(L"", irr::core::rect<irr::s32>(0, 0, 150, 150), false, true, 0, -1, true);
	stTip->setBackgroundColor(0xc0ffffff);
	stTip->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stTip->setVisible(false);
	//tip for cards in select / display list
	stCardListTip = env->addStaticText(L"", irr::core::rect<irr::s32>(0, 0, 150, 150), false, true, wCardSelect, TEXT_CARD_LIST_TIP, true);
	stCardListTip->setBackgroundColor(0xc0ffffff);
	stCardListTip->setTextAlignment(irr::gui::EGUIA_CENTER, irr::gui::EGUIA_CENTER);
	stCardListTip->setVisible(false);
	device->setEventReceiver(&menuHandler);
	if(!soundManager.Init()) {
		chkEnableSound->setChecked(false);
		chkEnableSound->setEnabled(false);
		chkEnableSound->setVisible(false);
		chkEnableMusic->setChecked(false);
		chkEnableMusic->setEnabled(false);
		chkEnableMusic->setVisible(false);
		scrSoundVolume->setVisible(false);
		scrMusicVolume->setVisible(false);
		chkMusicMode->setEnabled(false);
		chkMusicMode->setVisible(false);
	}
	SwapYesNoButtons(gameConf.swap_yes_no_button);
	env->getSkin()->setFont(guiFont);
	env->setFocus(wMainMenu);
	for (int i = 0; i < irr::gui::EGDC_COUNT; ++i) {
		auto col = env->getSkin()->getColor((irr::gui::EGUI_DEFAULT_COLOR)i);
		col.setAlpha(224);
		env->getSkin()->setColor((irr::gui::EGUI_DEFAULT_COLOR)i, col);
	}
	SelectTheme(gameConf.theme_id);
	window_size = driver->getScreenSize();
	xScale = window_size.Width / static_cast<float>(GAME_WINDOW_WIDTH);
	yScale = window_size.Height / static_cast<float>(GAME_WINDOW_HEIGHT);
	gMutex.lock();
	OnResize();
	gMutex.unlock();
	return true;
}
void Game::MainLoop() {
#ifdef YGOPROMCP_HTTP_PROBE
	const char* probeDir = std::getenv("YGOPROMCP_HTTP_PROBE_DIR");
	const bool probeSkipUi = std::getenv("YGOPROMCP_HTTP_PROBE_SKIP_UI") != nullptr;
	const bool probeControls = std::getenv("YGOPROMCP_HTTP_PROBE_CONTROLS") != nullptr;
	const bool probeInputs = std::getenv("YGOPROMCP_HTTP_PROBE_INPUTS") != nullptr;
	const bool probeDataUi = std::getenv("YGOPROMCP_HTTP_PROBE_DATA_UI") != nullptr;
	const bool probeUpdateUi = std::getenv("YGOPROMCP_HTTP_PROBE_UPDATE_UI") != nullptr;
	const bool probeUpdateCancel = std::getenv("YGOPROMCP_HTTP_PROBE_UPDATE_CANCEL") != nullptr;
	const bool probeUpdateRestore = std::getenv("YGOPROMCP_HTTP_PROBE_UPDATE_RESTORE") != nullptr;
	const bool probeHealthOnly = std::getenv("YGOPROMCP_HTTP_PROBE_HEALTH_ONLY") != nullptr;
	const bool probeFailAccept = std::getenv("YGOPROMCP_HTTP_PROBE_FAIL_ACCEPT") != nullptr;
	const auto probeStart = std::chrono::steady_clock::now();
	int probeStage = 0;
	auto probeDownloadStart = std::chrono::steady_clock::time_point{};
	bool probeEndpointWritten = false;
	bool probeAcceptBroken = false;
	bool probeFailureCaptured = false;
	const auto probePath = [probeDir](const char* name) {
		return std::string(probeDir ? probeDir : "") + "/" + name;
	};
	const auto probeCapture = [this, &probePath](const char* name) {
		if(FILE* trace = std::fopen(probePath("window-states.txt").c_str(), "ab")) {
			std::fprintf(trace, "%s menu=%d lan=%d mcp=%d settings=%d\n", name,
				wMainMenu->isVisible(), wLanWindow->isVisible(), wMcpPanel->isVisible(),
				wHomeSettings->isVisible());
			std::fclose(trace);
		}
		auto* image = driver->createScreenShot(irr::video::ECF_A8R8G8B8);
		if(image) {
			driver->writeImageToFile(image, probePath(name).c_str());
			image->drop();
		}
	};
	const auto probeThemeEvent = [this](int selected) {
		cbHomeTheme->setSelected(selected);
		irr::SEvent event{};
		event.EventType = irr::EET_GUI_EVENT;
		event.GUIEvent.EventType = irr::gui::EGET_COMBO_BOX_CHANGED;
		event.GUIEvent.Caller = cbHomeTheme;
		device->postEventFromUser(event);
	};
	const auto probeUpdateState = [this, &probePath](const char* phase) {
		if(FILE* out = std::fopen(probePath("update-ui-states.txt").c_str(), "ab")) {
			const auto status = BufferIO::EncodeUTF8String(stUpdateStatus->getText());
			const auto detail = BufferIO::EncodeUTF8String(stUpdateDetail->getText());
			std::fprintf(out, "%s status=%s detail=%s check=%d download=%d apply=%d restore=%d cancel=%d\n",
				phase, status.c_str(), detail.c_str(), btnUpdateCheck->isEnabled(),
				btnUpdateDownload->isEnabled(), btnUpdateApply->isEnabled(),
				btnUpdateRestore->isEnabled(), btnUpdateCancel->isEnabled());
			std::fclose(out);
		}
	};
#endif
	wchar_t cap[256];
	camera = smgr->addCameraSceneNode(0);
	if(gameConf.modern_field_view)
		matManager.UseModernZones();
	irr::core::matrix4 mProjection;
	const auto fieldProjection = GetFieldProjectionForView(window_size.Width, window_size.Height,
		gameConf.modern_field_view || gameConf.preserve_field_aspect, gameConf.modern_field_view);
	BuildProjectionMatrix(mProjection, fieldProjection.left, fieldProjection.right, fieldProjection.bottom, fieldProjection.top, 1.0f, 100.0f);
	camera->setProjectionMatrix(mProjection);

	const auto fieldCamera = GetFieldCamera(gameConf.modern_field_view);
	mProjection.buildCameraLookAtMatrixLH(irr::core::vector3df(4.2f, fieldCamera.y, fieldCamera.z), irr::core::vector3df(4.2f, 0, 0), irr::core::vector3df(0, 0, 1));
	camera->setViewMatrixAffector(mProjection);
	smgr->setAmbientLight(irr::video::SColorf(1.0f, 1.0f, 1.0f));
	float atkframe = 0.1f;
	int fps = 0;
	auto lastFpsTime = std::chrono::steady_clock::now();
#ifdef _WIN32
	HANDLE hWaitTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
	bool useHighResTimer = (hWaitTimer != NULL);
	if(!useHighResTimer)
		timeBeginPeriod(1);
#endif
	auto lastFrameTime = std::chrono::steady_clock::now();
	constexpr auto targetFrameDuration = std::chrono::microseconds(16667);
	while(device->run()) {
		auto size = driver->getScreenSize();
		if(window_size != size) {
			window_size = size;
			xScale = window_size.Width / static_cast<float>(GAME_WINDOW_WIDTH);
			yScale = window_size.Height / static_cast<float>(GAME_WINDOW_HEIGHT);
			gMutex.lock();
			OnResize();
			gMutex.unlock();
		}
		linePattern = (linePattern + 1) % 30;
		stippleMask = (stippleMask << 1) | (stippleMask >> 15);
		atkframe += 0.1f;
		if(atkframe > 6.2832f)
			atkframe -= 6.2832f;
		atkdy = (float)sin(atkframe);
		driver->beginScene(true, true, irr::video::SColor(0, 0, 0, 0));
		gMutex.lock();
		if(dInfo.isStarted) {
			if(dInfo.isFinished && showcardcode == 1)
				soundManager.PlayBGM(BGM_WIN);
			else if(dInfo.isFinished && (showcardcode == 2 || showcardcode == 3))
				soundManager.PlayBGM(BGM_LOSE);
			else if(dInfo.lp[0] > 0 && dInfo.lp[0] <= dInfo.lp[1] / 2)
				soundManager.PlayBGM(BGM_DISADVANTAGE);
			else if(dInfo.lp[0] > 0 && dInfo.lp[0] >= dInfo.lp[1] * 2)
				soundManager.PlayBGM(BGM_ADVANTAGE);
			else
				soundManager.PlayBGM(BGM_DUEL);
			if(themeManager.HasVisualStyle())
				themeManager.DrawBackdrop(driver, Resize(0, 0, GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT));
			else
				DrawBackImage(imageManager.tBackGround);
			DrawBackGround();
			DrawCards();
			DrawMisc();
			smgr->drawAll();
			driver->setMaterial(irr::video::IdentityMaterial);
			driver->clearZBuffer();
		} else if(is_building) {
			soundManager.PlayBGM(BGM_DECK);
			if(themeManager.HasVisualStyle())
				themeManager.DrawBackdrop(driver, Resize(0, 0, GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT));
			else
				DrawBackImage(imageManager.tBackGround_deck);
			DrawDeckBd();
		} else {
			soundManager.PlayBGM(BGM_MENU);
			if(themeManager.HasVisualStyle()) {
				themeManager.DrawBackdrop(driver, Resize(0, 0, GAME_WINDOW_WIDTH, GAME_WINDOW_HEIGHT));
				// Header art is anchored to the viewport, independent of the
				// currently open Home, LAN, Replay, MCP, or Settings window.
				const bool compactLogo = window_size.Width < 1360 || window_size.Height < 900;
				themeManager.DrawLogo(driver, compactLogo ?
					irr::core::recti(32, 10, 352, 63) : irr::core::recti(80, 45, 680, 145));
				if(wMainMenu->isVisible() && window_size.Width >= 1024 && imageManager.tCover[0]) {
					const auto card = Resize(700, 155, 835, 365);
					const auto size = imageManager.tCover[0]->getOriginalSize();
					driver->draw2DImage(imageManager.tCover[0], card,
						irr::core::recti(0, 0, size.Width, size.Height), nullptr, nullptr, true);
					driver->draw2DRectangleOutline(card, themeManager.GetDeckFrameColor());
				}
			} else
				DrawBackImage(imageManager.tBackGround_menu);
		}
		// Compact selectors can shorten a deck name visually. Keep the exact
		// selected value available without changing the real combo item or deck ID.
		if(is_building && themeManager.HasVisualStyle()) {
			for(auto* box : {cbDBCategory, cbDBDecks}) {
				const int selected = box->getSelected();
				const wchar_t* value = selected >= 0 ? box->getItem(selected) : L"";
				if(std::wcscmp(box->getToolTipText().c_str(), value))
					box->setToolTipText(value);
			}
		}
		DrawGUI();
		DrawSpec();
		gMutex.unlock();
		MCP_Poll();
		CardUpdateUiPoll(*this);
		UpdateMcpHomeStatus();
		if(signalFrame > 0) {
			signalFrame--;
			if(!signalFrame)
				frameSignal.Set();
		}
		if(waitFrame >= 0) {
			waitFrame++;
			if(waitFrame % 90 == 0) {
				stHintMsg->setText(dataManager.GetSysString(1390));
			} else if(waitFrame % 90 == 30) {
				stHintMsg->setText(dataManager.GetSysString(1391));
			} else if(waitFrame % 90 == 60) {
				stHintMsg->setText(dataManager.GetSysString(1392));
			}
		}
		driver->endScene();
#ifdef YGOPROMCP_HTTP_PROBE
		if(probeDir) {
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - probeStart).count();
			if(probeFailAccept && elapsed > 2000 && !probeAcceptBroken) {
				MCP_HTTP_ProbeBreakListener();
				probeAcceptBroken = true;
			}
			if(probeFailAccept && elapsed > 4500 && !probeFailureCaptured) {
				probeCapture("listener-error.png");
				probeFailureCaptured = true;
			}
			if(!probeEndpointWritten && MCP_HTTP_IsRunning()) {
				const auto endpoint = MCP_HTTP_Endpoint();
				if(FILE* out = std::fopen(probePath("endpoint-1.txt").c_str(), "wb")) {
					std::fwrite(endpoint.data(), 1, endpoint.size(), out);
					std::fclose(out);
					probeEndpointWritten = true;
				}
			}
			if(probeUpdateUi) {
				if(probeStage == 0 && elapsed > 500) {
					ClickButton(btnSettingsOpen);
					ClickButton(btnGameDataOpen);
					probeStage = 1;
				} else if(probeUpdateRestore && probeStage == 1 &&
					elapsed > 1300 && btnUpdateRestore->isEnabled()) {
					probeUpdateState("installed-restore-available");
					probeCapture("update-installed.png");
					ClickButton(btnUpdateRestore);
					probeStage = 2;
				} else if(probeUpdateRestore && probeStage == 2 &&
					btnUpdateCheck->isEnabled() && btnUpdateCancel->isEnabled()) {
					probeUpdateState("restore-scheduled");
					probeDownloadStart = std::chrono::steady_clock::now();
					probeStage = 3;
				} else if(probeUpdateRestore && probeStage == 3 &&
					std::chrono::steady_clock::now() - probeDownloadStart > std::chrono::milliseconds(100)) {
					probeCapture("update-restore-scheduled.png");
					probeStage = 99;
				} else if(!probeUpdateRestore && probeStage == 1 && elapsed > 1300 && btnUpdateCheck->isEnabled()) {
					probeUpdateState("status");
					probeCapture("update-status.png");
					ClickButton(btnUpdateCheck);
					probeStage = 2;
				} else if(probeStage == 2 && elapsed > 1800 && btnUpdateCheck->isEnabled()) {
					probeUpdateState("checked");
					probeCapture("update-checked.png");
					if(btnUpdateDownload->isEnabled()) {
						ClickButton(btnUpdateDownload);
						probeDownloadStart = std::chrono::steady_clock::now();
						probeStage = 3;
					} else probeStage = 99;
				} else if(probeUpdateCancel && probeStage == 3 &&
					std::chrono::steady_clock::now() - probeDownloadStart > std::chrono::milliseconds(150) &&
					btnUpdateCancel->isEnabled() && !btnUpdateCheck->isEnabled()) {
					probeUpdateState("cancel-requested");
					ClickButton(btnUpdateCancel);
					probeStage = 30;
				} else if(probeUpdateCancel && probeStage == 30 && btnUpdateCheck->isEnabled() &&
					!std::wcscmp(stUpdateStatus->getText(), L"Canceled")) {
					probeUpdateState("cancel-complete");
					ClickButton(btnUpdateCheck);
					probeStage = 31;
				} else if(probeUpdateCancel && probeStage == 31 && btnUpdateDownload->isEnabled() &&
					btnUpdateCheck->isEnabled()) {
					ClickButton(btnUpdateDownload);
					probeStage = 32;
				} else if(probeUpdateCancel && probeStage == 32 && btnUpdateApply->isEnabled() &&
					btnUpdateCheck->isEnabled()) {
					probeUpdateState("resumed-download");
					probeCapture("update-resumed.png");
					probeStage = 99;
				} else if(!probeUpdateCancel && !probeUpdateRestore && probeStage == 3 && btnUpdateCheck->isEnabled()) {
					probeUpdateState("downloaded");
					probeCapture("update-downloaded.png");
					if(btnUpdateApply->isEnabled()) {
						ClickButton(btnUpdateApply);
						probeStage = 4;
					} else probeStage = 99;
				} else if(probeStage == 4 && btnUpdateCheck->isEnabled()) {
					probeUpdateState("scheduled");
					probeCapture("update-scheduled.png");
					if(btnUpdateCancel->isEnabled()) {
						ClickButton(btnUpdateCancel);
						probeStage = 5;
					} else probeStage = 99;
				} else if(probeStage == 5 && btnUpdateCheck->isEnabled()) {
					probeUpdateState("canceled");
					probeCapture("update-canceled.png");
					probeStage = 99;
				}
			} else if(probeDataUi) {
				if(probeStage == 0 && elapsed > 500) {
					if(wMcpPanel->isVisible()) ClickButton(btnMcpClose);
					btnGameDataOpen->setVisible(true);
					ClickButton(btnSettingsOpen);
					probeStage = 1;
				} else if(probeStage == 1 && elapsed > 1300) {
					probeCapture("settings-entry-gilded.png");
					ClickButton(btnGameDataOpen);
					probeStage = 2;
				} else if(probeStage == 2 && elapsed > 2200) {
					probeCapture("game-data-gilded.png");
					ClickButton(btnGameDataClose);
					probeThemeEvent(0);
					ClickButton(btnGameDataOpen);
					probeStage = 3;
				} else if(probeStage == 3 && elapsed > 3200) {
					probeCapture("game-data-classic.png");
					probeStage = 4;
				}
			} else if(probeInputs) {
				if(probeStage == 0 && elapsed > 500) {
					ClickButton(btnMcpClose);
					ClickButton(btnLanMode);
					ebJoinHost->setText(L"");
					env->setFocus(ebJoinHost);
					probeStage = 1;
				} else if(probeStage == 1 && elapsed > 1400) {
					ebJoinHost->setText(L"");
					env->setFocus(ebJoinHost);
					const bool initialFocus = env->getFocus() == ebJoinHost;
					const auto key = [this](irr::EKEY_CODE code, wchar_t character = 0,
						bool control = false, bool shift = false) {
						irr::SEvent event{};
						event.EventType = irr::EET_KEY_INPUT_EVENT;
						event.KeyInput.Key = code;
						event.KeyInput.Char = character;
						event.KeyInput.Control = control;
						event.KeyInput.Shift = shift;
						event.KeyInput.PressedDown = true;
						device->postEventFromUser(event);
						event.KeyInput.PressedDown = false;
						device->postEventFromUser(event);
					};
					key(irr::KEY_KEY_A, L'a');
					key(irr::KEY_KEY_B, L'b');
					key(irr::KEY_KEY_C, L'c');
					const std::wstring typedValue = ebJoinHost->getText();
					const bool typed = !std::wcscmp(ebJoinHost->getText(), L"abc");
					key(irr::KEY_LEFT);
					key(irr::KEY_KEY_X, L'x');
					const std::wstring caretValue = ebJoinHost->getText();
					const bool caret = !std::wcscmp(ebJoinHost->getText(), L"abxc");
					key(irr::KEY_LEFT, 0, false, true);
					key(irr::KEY_DELETE);
					const std::wstring selectionValue = ebJoinHost->getText();
					const bool selection = !std::wcscmp(ebJoinHost->getText(), L"abc");
					key(irr::KEY_KEY_A, 0, true);
					key(irr::KEY_DELETE);
					const bool deletion = !std::wcscmp(ebJoinHost->getText(), L"");
					env->setFocus(ebNickName);
					key(irr::KEY_TAB);
					const bool tab = env->getFocus() != ebNickName && env->getFocus() != nullptr;
					const bool masked = ebJoinPass->isPasswordBox() && ebServerPass->isPasswordBox();
					ebJoinPass->setText(L"testpass");
					// Exercise the native button press and release path, including a
					// release outside the button. Neither path starts a LAN game.
					const auto button = btnCreateHost->getAbsolutePosition();
					const int bx = (button.UpperLeftCorner.X + button.LowerRightCorner.X) / 2;
					const int by = (button.UpperLeftCorner.Y + button.LowerRightCorner.Y) / 2;
					const auto mouse = [this](irr::EMOUSE_INPUT_EVENT type, int x, int y,
						irr::u32 buttons) {
						irr::SEvent event{};
						event.EventType = irr::EET_MOUSE_INPUT_EVENT;
						event.MouseInput.Event = type;
						event.MouseInput.X = x;
						event.MouseInput.Y = y;
						event.MouseInput.ButtonStates = buttons;
						device->postEventFromUser(event);
					};
					mouse(irr::EMIE_MOUSE_MOVED, bx, by, 0);
					mouse(irr::EMIE_LMOUSE_PRESSED_DOWN, bx, by, irr::EMBSM_LEFT);
					const bool pressed = btnCreateHost->isPressed();
					mouse(irr::EMIE_MOUSE_MOVED, 20, 20, irr::EMBSM_LEFT);
					mouse(irr::EMIE_LMOUSE_LEFT_UP, 20, 20, 0);
					const bool cancelled = !wCreateHost->isVisible();
					mouse(irr::EMIE_MOUSE_MOVED, bx, by, 0);
					mouse(irr::EMIE_LMOUSE_PRESSED_DOWN, bx, by, irr::EMBSM_LEFT);
					mouse(irr::EMIE_LMOUSE_LEFT_UP, bx, by, 0);
					const bool clicked = wCreateHost->isVisible();
					if(clicked) ClickButton(btnHostCancel);
					if(FILE* out = std::fopen(probePath("native-input-results.txt").c_str(), "wb")) {
						std::fprintf(out, "typed=%d caret=%d selection=%d deletion=%d tab=%d masked=%d focus=%d pressed=%d cancelled=%d clicked=%d\nvalues=%ls|%ls|%ls\n",
							typed, caret, selection, deletion, tab, masked, initialFocus,
							pressed, cancelled, clicked,
							typedValue.c_str(), caretValue.c_str(), selectionValue.c_str());
						std::fclose(out);
					}
					probeStage = 2;
				} else if(probeStage == 2 && elapsed > 2400) {
					probeCapture("native-password.png");
					if(MCP_HTTP_IsRunning()) MCP_HTTP_Stop();
					ClickButton(btnJoinCancel);
					ClickButton(btnMcpOpen);
					ebMcpPort->setText(L"70000");
					ClickButton(btnMcpToggle);
					probeStage = 3;
				} else if(probeStage == 3 && elapsed > 3400) {
					probeCapture("native-invalid-port.png");
					if(FILE* out = std::fopen(probePath("invalid-port-result.txt").c_str(), "wb")) {
						std::fprintf(out, "rejected=%d status=%d\n", !MCP_HTTP_IsRunning(),
							std::wcsstr(stMcpStatus->getText(), L"Error:") == stMcpStatus->getText());
						std::fclose(out);
					}
					probeStage = 4;
				}
			} else if(probeControls) {
				if(probeStage == 0 && elapsed > 500) {
					ClickButton(btnMcpClose);
					probeStage = 1;
				} else if(probeStage == 1 && elapsed > 1500) {
					probeCapture("controls-home.png");
					ClickButton(btnLanMode);
					probeStage = 2;
				} else if(probeStage == 2 && elapsed > 3000) {
					probeCapture("controls-lan.png");
					ebJoinHost->setText(L"127.0.0.1");
					env->setFocus(ebJoinHost);
					probeStage = 3;
				} else if(probeStage == 3 && elapsed > 4500) {
					probeCapture("controls-edit-focus.png");
					ClickButton(btnCreateHost);
					probeStage = 4;
				} else if(probeStage == 4 && elapsed > 6000) {
					probeCapture("controls-create-form.png");
					ClickButton(btnHostCancel);
					ClickButton(btnJoinCancel);
					ClickButton(btnReplayMode);
					probeStage = 5;
				} else if(probeStage == 5 && elapsed > 8000) {
					probeCapture("controls-replay.png");
					ClickButton(btnReplayCancel);
					ClickButton(btnDeckEdit);
					probeStage = 6;
				} else if(probeStage == 6 && elapsed > 10500) {
					const auto matchesSelected = [](irr::gui::IGUIComboBox* box) {
						const int selected = box->getSelected();
						return selected >= 0 && !std::wcscmp(box->getToolTipText().c_str(),
							box->getItem(selected));
					};
					if(FILE* out = std::fopen(probePath("deck-tooltip-results.txt").c_str(), "wb")) {
						std::fprintf(out, "category=%d deck=%d\n",
							matchesSelected(cbDBCategory), matchesSelected(cbDBDecks));
						std::fclose(out);
					}
					probeCapture("controls-deck.png");
					probeStage = 7;
				}
			} else if(!probeSkipUi && probeStage == 0 && elapsed > 1500) {
				probeCapture("mcp-listening.png");
				ClickButton(btnMcpClose);
				ClickButton(btnSettingsOpen);
				probeStage = 1;
			} else if(probeStage == 1 && elapsed > 3500) {
				probeCapture("settings-gilded.png");
				probeThemeEvent(0);
				probeStage = 2;
			} else if(probeStage == 2 && elapsed > 5500) {
				probeCapture("settings-classic.png");
				probeThemeEvent(1);
				probeStage = 3;
			} else if(probeStage == 3 && elapsed > 7500) {
				probeCapture("settings-gilded-restored.png");
				ClickButton(btnSettingsClose);
				probeStage = 4;
			} else if(probeStage == 4 && elapsed > 9500) {
				probeCapture("home-gilded.png");
				ClickButton(btnMcpOpen);
				ClickButton(btnMcpToggle);
				probeStage = 5;
			} else if(probeStage == 5 && elapsed > 11500) {
				probeCapture("mcp-stopped.png");
				ClickButton(btnMcpToggle);
				probeStage = 6;
			} else if(probeStage == 6 && elapsed > 13500) {
				probeCapture("mcp-reenabled.png");
				const auto endpoint = MCP_HTTP_Endpoint();
				if(FILE* out = std::fopen(probePath("endpoint-2.txt").c_str(), "wb")) {
					std::fwrite(endpoint.data(), 1, endpoint.size(), out);
					std::fclose(out);
				}
				probeStage = 7;
			}
			if((probeUpdateUi && probeStage == 99) || (probeHealthOnly && elapsed > 3500) ||
				elapsed > (probeUpdateUi ? 120000 : (probeFailAccept ? 6500 : (probeDataUi ? 4300 : (probeInputs ? 4800 :
				(probeControls ? 12500 : (probeSkipUi ? 38000 : 21000)))))))
				device->closeDevice();
		}
#endif
		if(closeSignal.TryWait())
			CloseDuelWindow();
		fps++;
		auto targetTime = lastFrameTime + targetFrameDuration;
		auto now = std::chrono::steady_clock::now();
		if(now < targetTime) {
#ifdef _WIN32
			if(useHighResTimer)
			{
				auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now);
				if(remaining.count() > 1500) {
					LARGE_INTEGER dueTime;
					dueTime.QuadPart = -(LONGLONG)(remaining.count() - 1000) * 10;
					if(SetWaitableTimer(hWaitTimer, &dueTime, 0, NULL, NULL, FALSE))
						WaitForSingleObject(hWaitTimer, INFINITE);
				}
			}
			else
#endif
			{
				auto sleepTime = targetTime - now - std::chrono::milliseconds(2);
				if(sleepTime > std::chrono::milliseconds(0)) {
					std::this_thread::sleep_for(sleepTime);
				}
			}
			// Spin-wait for sub-millisecond precision.
			// If the window is inactive, sleep 1ms per iteration to avoid wasting CPU.
			while(std::chrono::steady_clock::now() < targetTime) {
				if(device->isWindowActive())
					CPU_PAUSE();
				else
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		lastFrameTime = targetTime;
		now = std::chrono::steady_clock::now();
		if(now - targetTime > targetFrameDuration)
			lastFrameTime = now;
		if(now - lastFpsTime >= std::chrono::milliseconds(1000)) {
			myswprintf(cap, L"YGOPro FPS: %d", fps);
			device->setWindowCaption(cap);
			fps = 0;
			lastFpsTime += std::chrono::milliseconds(1000);
			if(now - lastFpsTime > std::chrono::milliseconds(1000))
				lastFpsTime = now;
			if(dInfo.time_player == 0 || dInfo.time_player == 1)
				if(dInfo.time_left[dInfo.time_player])
					dInfo.time_left[dInfo.time_player]--;
		}
	}
#ifdef _WIN32
	if(useHighResTimer)
		CloseHandle(hWaitTimer);
	else
		timeEndPeriod(1);
#endif
	DuelClient::StopClient(CLIENT_CLOSE_REASON_EXIT);
	if(dInfo.isSingleMode)
		SingleMode::StopPlay(true);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	SaveConfig();
	device->drop();
}
irr::core::matrix4 Game::GetCardRenderTransform(const ClientCard* pcard) const {
	auto transform = pcard->mTransform;
	if(gameConf.modern_field_view && pcard->location == LOCATION_HAND && pcard->controler == 0) {
		irr::core::matrix4 scale;
		scale.setScale(irr::core::vector3df(MODERN_HAND_SCALE, MODERN_HAND_SCALE, 1.0f));
		transform *= scale;
	}
	return transform;
}
void Game::BuildProjectionMatrix(irr::core::matrix4& mProjection, irr::f32 left, irr::f32 right, irr::f32 bottom, irr::f32 top, irr::f32 znear, irr::f32 zfar) {
	for(int i = 0; i < 16; ++i)
		mProjection[i] = 0;
	mProjection[0] = 2.0f * znear / (right - left);
	mProjection[5] = 2.0f * znear / (top - bottom);
	mProjection[8] = (left + right) / (left - right);
	mProjection[9] = (top + bottom) / (bottom - top);
	mProjection[10] = zfar / (zfar - znear);
	mProjection[11] = 1.0f;
	mProjection[14] = znear * zfar / (znear - zfar);
}
void Game::FixFontGlitch() {
	textFont->setTransparency(true);
	guiFont->setTransparency(true);
}
// Wrapper for source files which don't include CGUITTFont.
irr::core::dimension2d<irr::u32> Game::GetGUIFontDimension(const wchar_t* text) const {
	return guiFont->getDimension(text);
}
void Game::InitStaticText(irr::gui::IGUIStaticText* pControl, irr::u32 cWidth, irr::u32 cHeight, irr::gui::CGUITTFont* font, const wchar_t* text) {
	std::wstring format_text;
	format_text = SetStaticText(pControl, cWidth, font, text);
	if(font->getDimension(format_text.c_str()).Height <= cHeight) {
		scrCardText->setVisible(false);
		if(env->hasFocus(scrCardText))
			env->removeFocus(scrCardText);
		return;
	}
	format_text = SetStaticText(pControl, cWidth-25, font, text);
	irr::u32 fontheight = font->getDimension(L"A").Height + font->getKerningHeight();
	irr::u32 step = (font->getDimension(format_text.c_str()).Height - cHeight) / fontheight + 1;
	scrCardText->setVisible(true);
	scrCardText->setMin(0);
	scrCardText->setMax(step);
	scrCardText->setPos(0);
}
std::wstring Game::SetStaticText(irr::gui::IGUIStaticText* pControl, irr::u32 cWidth, irr::gui::CGUITTFont* font, const wchar_t* text, irr::u32 pos) {
	irr::u32 _width = 0, _height = 0;
	wchar_t prev = 0;
	std::wstring result;
	result.reserve(4096);
	const size_t text_len = std::wcslen(text);

	for(size_t i = 0; i < text_len ; ++i) {
		wchar_t c = text[i];
		irr::u32 w = font->getCharDimension(c).Width + font->getKerningWidth(c, prev);
		prev = c;
		if (c == L'\r') {
			continue;
		}
		if (c == L'\n') {
			result.push_back(L'\n');
			_width = 0;
			_height++;
			prev = 0;
			if (_height == pos)
				result.clear();
			continue;
		}
		if (_width > 0 && _width + w > cWidth) {
			result.push_back(L'\n');
			_width = 0;
			_height++;
			prev = 0;
			if (_height == pos)
				result.clear();
		}
		_width += w;
		result.push_back(c);
	}
	if (pControl)
		pControl->setText(result.c_str());
	return result;
}
void Game::LoadExpansions() {
	managedArchiveMounted = false;
	managedReleaseDatabaseLoaded = false;
	managedUpdateDatabaseLoaded = false;
	managedProbeScript.clear();
	FileSystem::TraversalDir("./expansions", [](const char* name, bool isdir) {
		if (isdir)
			return;
		char fpath[1024];
		mysnprintf(fpath, "./expansions/%s", name);
		if (IsExtension(name, ".cdb")) {
			if (!dataManager.LoadDB(fpath)) {
				std::string errmsg = "Warning: Failed to load DB file on disk (";
				errmsg.append(fpath);
				errmsg.append(")! ");
				errmsg.append(dataManager.errmsg);
				mainGame->ErrorLog(errmsg.c_str());
			}
			return;
		}
		if (IsExtension(name, ".conf")) {
			dataManager.LoadStrings(fpath);
			return;
		}
		if (IsExtension(name, ".zip") || IsExtension(name, ".ypk")) {
			const bool mounted = dataManager.IrrFileSystem->addFileArchive(
				fpath, true, false, irr::io::EFAT_ZIP);
			if (!std::strcmp(name, "ygopro-super-pre.ypk"))
				mainGame->managedArchiveMounted = mounted;
			return;
		}
	});
	for(irr::u32 i = 0; i < dataManager.IrrFileSystem->getFileArchiveCount(); ++i) {
		auto* fileArchive = dataManager.IrrFileSystem->getFileArchive(i);
		const char* archiveName = fileArchive->getArchiveName().c_str();
		const char* slash = std::strrchr(archiveName, '/');
		const char* backslash = std::strrchr(archiveName, '\\');
		const char* basename = std::max(slash ? slash + 1 : archiveName,
			backslash ? backslash + 1 : archiveName);
		const bool managed = !std::strcmp(basename, "ygopro-super-pre.ypk");
		auto archive = fileArchive->getFileList();
		for(irr::u32 j = 0; j < archive->getFileCount(); ++j) {
			const char* name = archive->getFullFileName(j).c_str();
			if (IsExtension(name, ".cdb")) {
				const bool loaded = dataManager.LoadDB(name);
				if(managed && !std::strcmp(name, "test-release.cdb"))
					managedReleaseDatabaseLoaded = loaded;
				if(managed && !std::strcmp(name, "test-update.cdb"))
					managedUpdateDatabaseLoaded = loaded;
				if (!loaded) {
					std::string errmsg = "Warning: Failed to load DB file in expansion archive (";
					errmsg.append(dataManager.IrrFileSystem->getFileArchive(i)->getArchiveName().c_str());
					errmsg.append(" : ");
					errmsg.append(name);
					errmsg.append(")! ");
					errmsg.append(dataManager.errmsg);
					ErrorLog(errmsg.c_str());
				}
				continue;
			}
			if(managed && managedProbeScript.empty() &&
				!std::strncmp(name, "script/c", 8) && IsExtension(name, ".lua"))
				managedProbeScript = name;
			if (IsExtension(name, ".conf")) {
				auto reader = dataManager.IrrFileSystem->createAndOpenFile(name);
				dataManager.LoadStrings(reader);
				continue;
			}
			if (!mystrncasecmp(name, "pack/", 5) && IsExtension(name, ".ydk")) {
				wchar_t fname[1024];
				int len = BufferIO::DecodeUTF8(name, fname);
				// TODO: zip file may contain non-UTF8 file name. DecodeUTF8 can't parse it and returns 0.
				if (!len) {
					std::string errmsg = "Warning: Failed to decode deck file name in expansion archive (";
					errmsg.append(dataManager.IrrFileSystem->getFileArchive(i)->getArchiveName().c_str());
					errmsg.append(" : ");
					errmsg.append(name);
					errmsg.append(")! Please make sure the file name is UTF-8 encoded in the archive.");
					ErrorLog(errmsg.c_str());
					continue;
				}
				deckBuilder.expansionPacks.push_back(fname);
				continue;
			}
		}
	}
}
void Game::RefreshCategoryDeck(irr::gui::IGUIComboBox* cbCategory, irr::gui::IGUIComboBox* cbDeck, bool selectlastused) {
	cbCategory->clear();
	cbCategory->addItem(dataManager.GetSysString(1450));
	cbCategory->addItem(dataManager.GetSysString(1451));
	cbCategory->addItem(dataManager.GetSysString(1452));
	cbCategory->addItem(dataManager.GetSysString(1453));
	FileSystem::TraversalDir(L"./deck", [cbCategory](const wchar_t* name, bool isdir) {
		if(isdir) {
			cbCategory->addItem(name);
		}
	});
	cbCategory->setSelected(2);
	if(selectlastused) {
		for(size_t i = 0; i < cbCategory->getItemCount(); ++i) {
			if(!std::wcscmp(cbCategory->getItem(i), gameConf.lastcategory)) {
				cbCategory->setSelected(i);
				break;
			}
		}
	}
	RefreshDeck(cbCategory, cbDeck);
	if(selectlastused) {
		for(size_t i = 0; i < cbDeck->getItemCount(); ++i) {
			if(!std::wcscmp(cbDeck->getItem(i), gameConf.lastdeck)) {
				cbDeck->setSelected(i);
				break;
			}
		}
	}
}
void Game::RefreshDeck(irr::gui::IGUIComboBox* cbCategory, irr::gui::IGUIComboBox* cbDeck) {
	if(cbCategory != cbDBCategory && cbCategory->getSelected() == 0) {
		// can't use pack list in duel
		cbDeck->clear();
		return;
	}
	wchar_t catepath[256];
	DeckManager::GetCategoryPath(catepath, cbCategory->getSelected(), cbCategory->getText());
	cbDeck->clear();
	RefreshDeck(catepath, [cbDeck](const wchar_t* item) { cbDeck->addItem(item); });
}
void Game::RefreshDeck(const wchar_t* deckpath, const std::function<void(const wchar_t*)>& additem) {
	if(!mywcsncasecmp(deckpath, L"./pack", 6)) {
		for(auto& pack : deckBuilder.expansionPacks) {
			// add pack/xxx.ydk
			additem(pack.substr(5, pack.size() - 9).c_str());
		}
	}
	FileSystem::TraversalDir(deckpath, [additem](const wchar_t* name, bool isdir) {
		if (!isdir && IsExtension(name, L".ydk")) {
			wchar_t deckname[256]{};
			BufferIO::CopyWideString(name, deckname, std::wcslen(name) - 4);
			additem(deckname);
		}
	});
}
void Game::RefreshReplay() {
	lstReplayList->clear();
	FileSystem::TraversalDir(L"./replay", [this](const wchar_t* name, bool isdir) {
		if (!isdir && IsExtension(name, L".yrp"))
			lstReplayList->addItem(name);
	});
}
void Game::RefreshSingleplay() {
	lstSinglePlayList->clear();
	stSinglePlayInfo->setText(L"");
	FileSystem::TraversalDir(L"./single", [this](const wchar_t* name, bool isdir) {
		if(!isdir && IsExtension(name, L".lua"))
			lstSinglePlayList->addItem(name);
	});
}
void Game::RefreshBot() {
	if(!gameConf.enable_bot_mode)
		return;
	botInfo.clear();
	FILE* fp = myfopen("bot.conf", "r");
	char linebuf[256]{};
	char strbuf[256]{};
	if(fp) {
		while(std::fgets(linebuf, 256, fp)) {
			if(linebuf[0] == '#')
				continue;
			if(linebuf[0] == '!') {
				BotInfo newinfo;
				if (std::sscanf(linebuf, "!%240[^\n]", strbuf) != 1)
					continue;
				BufferIO::DecodeUTF8(strbuf, newinfo.name);
				if (!std::fgets(linebuf, 256, fp))
					break;
				if (std::sscanf(linebuf, "%240[^\n]", strbuf) != 1)
					continue;
				BufferIO::DecodeUTF8(strbuf, newinfo.command);
				if (!std::fgets(linebuf, 256, fp))
					break;
				if (std::sscanf(linebuf, "%240[^\n]", strbuf) != 1)
					continue;
				BufferIO::DecodeUTF8(strbuf, newinfo.desc);
				if (!std::fgets(linebuf, 256, fp))
					break;
				newinfo.support_master_rule_3 = !!std::strstr(linebuf, "SUPPORT_MASTER_RULE_3");
				newinfo.support_new_master_rule = !!std::strstr(linebuf, "SUPPORT_NEW_MASTER_RULE");
				newinfo.support_master_rule_2020 = !!std::strstr(linebuf, "SUPPORT_MASTER_RULE_2020");
				newinfo.select_deckfile = !!std::strstr(linebuf, "SELECT_DECKFILE");
				int rule = cbBotRule->getSelected() + 3;
				if((rule == 3 && newinfo.support_master_rule_3)
					|| (rule == 4 && newinfo.support_new_master_rule)
					|| (rule == 5 && newinfo.support_master_rule_2020))
					botInfo.push_back(newinfo);
				continue;
			}
		}
		std::fclose(fp);
	}
	lstBotList->clear();
	stBotInfo->setText(L"");
	cbBotDeckCategory->setVisible(false);
	cbBotDeck->setVisible(false);
	for(unsigned int i = 0; i < botInfo.size(); ++i) {
		lstBotList->addItem(botInfo[i].name);
	}
	if(botInfo.size() == 0) {
		SetStaticText(stBotInfo, 200, guiFont, dataManager.GetSysString(1385));
	}
	else {
		RefreshCategoryDeck(cbBotDeckCategory, cbBotDeck);
	}
}
void Game::LoadConfig(const char* file) {
	FILE* fp = myfopen(file, "r");
	if(!fp)
		return;
	char linebuf[CONFIG_LINE_SIZE]{};
	char strbuf[64]{};
	char valbuf[960]{};
	while(std::fgets(linebuf, sizeof linebuf, fp)) {
		if (std::sscanf(linebuf, "%63s = %959s", strbuf, valbuf) != 2)
			continue;
		if(!std::strcmp(strbuf, "antialias")) {
			gameConf.antialias = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "use_d3d")) {
			gameConf.use_d3d = std::strtol(valbuf, nullptr, 10) > 0;
#ifdef _OPENMP
		} else if (!std::strcmp(strbuf, "use_image_scale_multi_thread")) {
			gameConf.use_image_scale_multi_thread = std::strtol(valbuf, nullptr, 10) > 0;
#endif
		} else if (!std::strcmp(strbuf, "use_image_load_background_thread")) {
			gameConf.use_image_load_background_thread = std::strtol(valbuf, nullptr, 10) > 0;
		} else if(!std::strcmp(strbuf, "errorlog")) {
			gameConf.enable_log = std::strtol(valbuf, nullptr, 10) & 0xff;
		} else if(!std::strcmp(strbuf, "serverport")) {
			gameConf.serverport = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "mcp_port")) {
			const long port = std::strtol(valbuf, nullptr, 10);
			if(port > 0 && port <= 65535) gameConf.mcp_port = static_cast<int>(port);
		} else if(!std::strcmp(strbuf, "local_host_only")) {
			gameConf.local_host_only = std::strtol(valbuf, nullptr, 10) > 0;
		} else if(!std::strcmp(strbuf, "lasthost")) {
			BufferIO::DecodeUTF8(valbuf, gameConf.lasthost);
		} else if(!std::strcmp(strbuf, "lastport")) {
			BufferIO::DecodeUTF8(valbuf, gameConf.lastport);
		} else if(!std::strcmp(strbuf, "automonsterpos")) {
			gameConf.chkMAutoPos = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "autospellpos")) {
			gameConf.chkSTAutoPos = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "randompos")) {
			gameConf.chkRandomPos = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "autochain")) {
			gameConf.chkAutoChain = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "waitchain")) {
			gameConf.chkWaitChain = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "showchain")) {
			gameConf.chkDefaultShowChain = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "mute_opponent")) {
			gameConf.chkIgnore1 = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "mute_spectators")) {
			gameConf.chkIgnore2 = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "use_lflist")) {
			gameConf.use_lflist = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "default_lflist")) {
			gameConf.default_lflist = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "default_rule")) {
			gameConf.default_rule = std::strtol(valbuf, nullptr, 10);
			if(gameConf.default_rule <= 0)
				gameConf.default_rule = DEFAULT_DUEL_RULE;
		} else if(!std::strcmp(strbuf, "hide_setname")) {
			gameConf.hide_setname = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "hide_hint_button")) {
			gameConf.hide_hint_button = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "control_mode")) {
			gameConf.control_mode = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "draw_field_spell")) {
			gameConf.draw_field_spell = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "preserve_field_aspect")) {
			gameConf.preserve_field_aspect = std::strtol(valbuf, nullptr, 10) != 0;
		} else if(!std::strcmp(strbuf, "modern_field_view")) {
			gameConf.modern_field_view = std::strtol(valbuf, nullptr, 10) != 0;
		} else if(!std::strcmp(strbuf, "theme")) {
			gameConf.theme_id = valbuf;
		} else if(!std::strcmp(strbuf, "separate_clear_button")) {
			gameConf.separate_clear_button = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "auto_search_limit")) {
			gameConf.auto_search_limit = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "search_multiple_keywords")) {
			gameConf.search_multiple_keywords = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "ignore_deck_changes")) {
			gameConf.chkIgnoreDeckChanges = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "default_ot")) {
			gameConf.defaultOT = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "enable_bot_mode")) {
			gameConf.enable_bot_mode = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "bot_room_public")) {
			gameConf.bot_room_public = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "quick_animation")) {
			gameConf.quick_animation = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "auto_save_replay")) {
			gameConf.auto_save_replay = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "draw_single_chain")) {
			gameConf.draw_single_chain = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "solid_selection_line")) {
			gameConf.solid_selection_line = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "hide_player_name")) {
			gameConf.hide_player_name = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "prefer_expansion_script")) {
			gameConf.prefer_expansion_script = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "swap_yes_no_button")) {
			gameConf.swap_yes_no_button = std::strtol(valbuf, nullptr, 10) > 0;
		} else if(!std::strcmp(strbuf, "window_maximized")) {
			gameConf.window_maximized = std::strtol(valbuf, nullptr, 10) > 0;
		} else if(!std::strcmp(strbuf, "window_width")) {
			gameConf.window_width = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "window_height")) {
			gameConf.window_height = std::strtol(valbuf, nullptr, 10);
		} else if(!std::strcmp(strbuf, "resize_select_window")) {
			gameConf.resize_select_window = std::strtol(valbuf, nullptr, 10) > 0;
		} else if(!std::strcmp(strbuf, "resize_popup_menu")) {
			int val = std::strtol(valbuf, nullptr, 10);
			gameConf.resize_popup_menu = myclamp(val, 0, 5);
#ifdef YGOPRO_USE_AUDIO
		} else if(!std::strcmp(strbuf, "enable_sound")) {
			gameConf.enable_sound = std::strtol(valbuf, nullptr, 10) > 0;
		} else if(!std::strcmp(strbuf, "sound_volume")) {
			int vol = std::strtol(valbuf, nullptr, 10);
			gameConf.sound_volume = myclamp(vol, 0, 100);
		} else if(!std::strcmp(strbuf, "enable_music")) {
			gameConf.enable_music = std::strtol(valbuf, nullptr, 10) > 0;
		} else if(!std::strcmp(strbuf, "music_volume")) {
			int vol = std::strtol(valbuf, nullptr, 10);
			gameConf.music_volume = myclamp(vol, 0, 100);
		} else if(!std::strcmp(strbuf, "music_mode")) {
			gameConf.music_mode = std::strtol(valbuf, nullptr, 10);
#endif
		} else {
			// options allowing multiple words
			if (std::sscanf(linebuf, "%63s = %959[^\n]", strbuf, valbuf) != 2)
				continue;
			if (!std::strcmp(strbuf, "textfont")) {
				char* last_space = std::strrchr(valbuf, ' ');
				if (last_space == nullptr)
					continue;
				int fontsize = std::strtol(last_space + 1, nullptr, 10);
				if (fontsize > 0)
					gameConf.textfontsize = fontsize;
				*last_space = 0;
				BufferIO::CopyString(valbuf, gameConf.textfont);
			} else if (!std::strcmp(strbuf, "numfont")) {
				BufferIO::CopyString(valbuf, gameConf.numfont);
			} else if (!std::strcmp(strbuf, "nickname")) {
				BufferIO::DecodeUTF8(valbuf, gameConf.nickname);
			} else if (!std::strcmp(strbuf, "gamename")) {
				BufferIO::DecodeUTF8(valbuf, gameConf.gamename);
			} else if (!std::strcmp(strbuf, "roompass")) {
				BufferIO::DecodeUTF8(valbuf, gameConf.roompass);
			} else if (!std::strcmp(strbuf, "lastcategory")) {
				BufferIO::DecodeUTF8(valbuf, gameConf.lastcategory);
			} else if (!std::strcmp(strbuf, "lastdeck")) {
				BufferIO::DecodeUTF8(valbuf, gameConf.lastdeck);
			} else if(!std::strcmp(strbuf, "bot_deck_path")) {
				BufferIO::DecodeUTF8(valbuf, gameConf.bot_deck_path);
			}
		}
	}
	std::fclose(fp);
}
bool Game::SelectTheme(const std::string& id) {
	std::string error;
	const bool selected = themeManager.Select(id, env, driver, &error);
	gameConf.theme_id = themeManager.ActiveId();
	if(cbTheme)
		cbTheme->setSelected(gameConf.theme_id == "gilded" ? 1 : 0);
	if(cbHomeTheme)
		cbHomeTheme->setSelected(gameConf.theme_id == "gilded" ? 1 : 0);
	ApplyThemeMenuLayout();
	ApplyThemeBrowserLayout();
	ApplyThemeDeckCompactLayout();
	ApplyThemeAppearanceLayout();
	ApplyThemeTypography();
	if(wCardImg)
		wCardImg->setBackgroundColor(themeManager.HasVisualStyle() ?
			themeManager.GetDeckPanelColor() : irr::video::SColor(0xc0c0c0c0));
	const auto metadataColor = themeManager.HasVisualStyle() ? themeManager.GetFocusColor() :
		irr::video::SColor(255, 0, 0, 255);
	if(stInfo) stInfo->setOverrideColor(metadataColor);
	if(stDataInfo) stDataInfo->setOverrideColor(metadataColor);
	if(stSetName) stSetName->setOverrideColor(metadataColor);
	if(!themeManager.HasVisualStyle()) {
		if(cbDBDecks) cbDBDecks->setToolTipText(nullptr);
		if(cbDBCategory) cbDBCategory->setToolTipText(nullptr);
	}
	if(!selected)
		std::fprintf(stderr, "Theme selection fell back to Classic: %s\n", error.c_str());
	return selected;
}
void Game::UpdateMcpHomeStatus() {
	if(!stMcpStatus || !btnMcpToggle)
		return;
	const auto listenerFailure = MCP_HTTP_TakeFailure();
	if(!listenerFailure.empty()) {
		MCP_HTTP_Stop();
		mcpStatusError = BufferIO::DecodeUTF8String(listenerFailure);
	}
	const bool listening = MCP_HTTP_IsRunning();
	btnMcpToggle->setText(listening ? L"Disable MCP" : L"Enable MCP");
	ebMcpPort->setEnabled(!listening);
	btnMcpCopy->setEnabled(listening);
	std::wstring status = listening ?
		(MCP_HTTP_ActiveRequestCount() ? L"Listening  |  request active" : L"Listening on this PC") :
		(mcpStatusError.empty() ? L"Stopped" : L"Error: " + mcpStatusError);
	if(status != stMcpStatus->getText()) stMcpStatus->setText(status.c_str());
	stMcpStatus->setOverrideColor(listening ?
		(themeManager.HasVisualStyle() ? themeManager.GetFocusColor() : irr::video::SColor(255, 20, 105, 40)) :
		(mcpStatusError.empty() ? irr::video::SColor(255, 220, 220, 220) : irr::video::SColor(255, 245, 100, 100)));
	const std::wstring endpoint = listening ? BufferIO::DecodeUTF8String(MCP_HTTP_Endpoint()) :
		L"Enable to get a local connection URL";
	const std::wstring display = endpoint.size() > 45 ? endpoint.substr(0, 42) + L"..." : endpoint;
	if(display != stMcpEndpoint->getText()) stMcpEndpoint->setText(display.c_str());
	stMcpEndpoint->setToolTipText(listening ? endpoint.c_str() : nullptr);
	if(stMcpHome) {
		const wchar_t* summary = listening ? L"MCP: listening on this PC" :
			(mcpStatusError.empty() ? L"MCP: stopped" : L"MCP: check connection settings");
		if(std::wcscmp(stMcpHome->getText(), summary)) stMcpHome->setText(summary);
	}
}
void Game::ApplyThemeMenuLayout() {
	if(!wMainMenu)
		return;
	const bool gilded = themeManager.HasVisualStyle();
	const bool compact = window_size.Width < 1024;
	wMainMenu->setRelativePosition(gilded ?
		(compact ? ResizeWin(252, 115, 772, 590) : ResizeWin(50, 65, 570, 585)) :
		ResizeWin(370, 165, 650, 475));
	wMcpPanel->setRelativePosition(ResizeWin(250, 155, 774, 475));
	wHomeSettings->setRelativePosition(ResizeWin(300, 180, 724, 420));
	wGameDataSettings->setRelativePosition(ResizeWin(232, 105, 792, 535));
	wchar_t classicTitle[256];
	myswprintf(classicTitle, L"YGOPro Version:%X.0%X.%X", (PRO_VERSION & 0xf000U) >> 12,
		(PRO_VERSION & 0x0ff0U) >> 4, PRO_VERSION & 0x000fU);
	wMainMenu->setText(gilded ? L"Home" : classicTitle);
	const irr::core::recti classic[] = {
		{10, 30, 270, 60}, {10, 65, 270, 95}, {10, 100, 270, 130},
		{10, 135, 270, 165}, {10, 170, 270, 200},
		{10, 205, 270, 235}, {10, 240, 270, 270}
	};
	const irr::core::recti gildedButtons[] = {
		{28, 52, 492, 102}, {28, 114, 492, 164}, {28, 176, 492, 226},
		{28, 238, 492, 288}, {28, 300, 492, 350},
		{28, 362, 492, 412}, {28, 424, 492, 474}
	};
	const irr::core::recti compactGildedButtons[] = {
		{28, 42, 492, 86}, {28, 98, 492, 142}, {28, 154, 492, 198},
		{28, 210, 492, 254}, {28, 266, 492, 310},
		{28, 322, 492, 366}, {28, 378, 492, 422}
	};
	irr::gui::IGUIButton* buttons[] = {btnLanMode, btnSingleMode, btnReplayMode,
		btnDeckEdit, btnSettingsOpen, btnMcpOpen, btnModeExit};
	for(int i = 0; i < 7; ++i)
		buttons[i]->setRelativePosition(gilded ? (compact ? compactGildedButtons[i] : gildedButtons[i]) : classic[i]);
	stMcpHome->setRelativePosition(gilded ? (compact ? irr::core::recti(28, 432, 492, 462) : irr::core::recti(28, 480, 492, 510)) :
		irr::core::recti(10, 278, 270, 301));
	if(gilded && wMainMenu->isVisible() && (!env->getFocus() || env->getFocus() == wMainMenu))
		env->setFocus(btnLanMode);
}
void Game::ApplyThemeBrowserLayout() {
	if(!wLanWindow || !wReplay)
		return;
	const bool expanded = themeManager.HasVisualStyle() && window_size.Width >= 1360 && window_size.Height >= 900;
	wLanWindow->setRelativePosition(expanded ? ResizeWin(22, 10, 1002, 630) : ResizeWin(220, 100, 800, 520));
	wReplay->setRelativePosition(expanded ? ResizeWin(22, 10, 1002, 630) : ResizeWin(220, 100, 800, 520));
	const auto set = [expanded](irr::gui::IGUIElement* element, const irr::core::recti& classic,
		const irr::core::recti& gilded) {
		element->setRelativePosition(expanded ? gilded : classic);
	};
	set(stLanNickname, {10, 30, 220, 50}, {25, 46, 145, 76});
	set(ebNickName, {110, 25, 450, 50}, {150, 40, 580, 82});
	set(btnCreateHost, {460, 25, 570, 50}, {775, 40, 950, 82});
	set(lstHostList, {10, 60, 570, 320}, {25, 105, 625, 505});
	lstHostList->setItemHeight(expanded ? 26 : 18);
	set(btnLanRefresh, {240, 325, 340, 350}, {25, 530, 190, 575});
	set(stLanHostIP, {10, 360, 220, 380}, {660, 125, 950, 155});
	set(ebJoinHost, {110, 355, 350, 380}, {660, 160, 865, 202});
	set(ebJoinPort, {360, 355, 420, 380}, {875, 160, 950, 202});
	set(stLanPassword, {10, 390, 220, 410}, {660, 230, 950, 260});
	set(ebJoinPass, {110, 385, 420, 410}, {660, 265, 950, 307});
	set(btnJoinHost, {460, 355, 570, 380}, {660, 530, 795, 575});
	set(btnJoinCancel, {460, 385, 570, 410}, {815, 530, 950, 575});
	set(lstReplayList, {10, 30, 350, 400}, {25, 65, 520, 555});
	lstReplayList->setItemHeight(expanded ? 26 : 18);
	set(stReplayHeading, {360, 30, 570, 50}, {545, 40, 950, 65});
	set(stReplayInfo, {360, 60, 570, 320}, {545, 75, 950, 425});
	set(stReplayStartTurn, {360, 275, 570, 295}, {545, 445, 700, 475});
	set(ebRepStartTurn, {360, 300, 460, 320}, {705, 440, 820, 480});
	set(btnExportDeck, {470, 325, 570, 350}, {545, 500, 730, 540});
	set(btnLoadReplay, {470, 355, 570, 380}, {745, 500, 950, 540});
	set(btnDeleteReplay, {360, 355, 460, 380}, {545, 555, 665, 595});
	set(btnRenameReplay, {360, 385, 460, 410}, {675, 555, 795, 595});
	set(btnReplayCancel, {470, 385, 570, 410}, {805, 555, 950, 595});
}
void Game::ApplyThemeDeckCompactLayout() {
	if(!cbCardType || !cbCardType2 || !cbRace || !cbAttribute)
		return;
	const bool compact = themeManager.HasVisualStyle() && window_size.Width < 1024;
	cbCardType->setRelativePosition(Resize(compact ? 80 : 60, 25 / 6,
		compact ? 133 : 120, 20 + 25 / 6));
	cbCardType2->setRelativePosition(Resize(compact ? 140 : 125, 25 / 6,
		195, 20 + 25 / 6));
	cbRace->setRelativePosition(Resize(compact ? 80 : 60, 40 + 75 / 6, 195, 60 + 75 / 6));
	cbAttribute->setRelativePosition(Resize(compact ? 80 : 60, 20 + 50 / 6, 195, 40 + 50 / 6));
	stCategory->setRelativePosition(Resize(10, 2 + 25 / 6, compact ? 80 : 70, 22 + 25 / 6));
	stAttribute->setRelativePosition(Resize(10, 22 + 50 / 6, compact ? 80 : 70, 42 + 50 / 6));
}
void Game::ApplyThemeAppearanceLayout() {
	if(!cbTheme || !tabSystem)
		return;
	const int top = cbTheme->getRelativePosition().UpperLeftCorner.Y;
	const int width = tabSystem->getRelativePosition().getWidth();
	cbTheme->setRelativePosition(irr::core::recti(0, top,
		std::min(240, width - 10), top + 26));
}
void Game::ApplyThemeTypography() {
	if(!env)
		return;
	if(themeManager.HasVisualStyle() && !themeGuiFont)
		themeGuiFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.textfont,
			std::max<int>(gameConf.textfontsize, 18));
	if(themeManager.HasVisualStyle() && !themeCompactDeckFont)
		themeCompactDeckFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.textfont, 10);
	// The compact layouts retain their original item heights and button geometry.
	const bool spaciousLayout = window_size.Width >= 1024 && window_size.Height >= 640;
	auto* font = themeManager.HasVisualStyle() && spaciousLayout ? themeGuiFont : nullptr;
	const auto compactDeckCaption = [this](irr::gui::IGUIElement* element) {
		return element == btnManageDeck || element == btnSaveDeck || element == btnSaveDeckAs ||
			element == btnDeleteDeck || element == btnShuffleDeck || element == btnSortDeck ||
			element == btnClearDeck || element == btnStartFilter || element == btnClearFilter ||
			element == btnEffectFilter || element == btnMarksFilter ||
			element == stDBCategory || element == stDeck || element == stCategory ||
			element == stLimit || element == stAttribute || element == stRace ||
			element == stAttack || element == stDefense || element == stStar ||
			element == stScale || element == stSearch;
	};
	// Only controls with fixed captions may keep an enlarged font. Duel prompts,
	// card details, hint messages, and other labels change after this pass.
	const auto fixedCaption = [this](irr::gui::IGUIElement* element) {
		return element == btnLanMode || element == btnSingleMode || element == btnReplayMode ||
			 element == btnDeckEdit || element == btnModeExit || element == btnSettingsOpen ||
			 element == btnMcpOpen || element == btnMcpCopy || element == btnMcpClose ||
			 element == btnSettingsClose || element == btnGameDataOpen ||
			 element == btnGameDataClose || element == btnGameDataImport ||
			 element == btnUpdateCheck || element == btnUpdateDownload ||
			 element == btnUpdateApply || element == btnUpdateRestore ||
			 element == btnUpdateCancel || element == stMcpLabel || element == btnCreateHost ||
			element == btnLanRefresh || element == btnJoinHost || element == btnJoinCancel ||
			element == btnExportDeck || element == btnLoadReplay || element == btnDeleteReplay ||
			element == btnRenameReplay || element == btnReplayCancel ||
			element == btnManageDeck || element == btnSaveDeck || element == btnSaveDeckAs ||
			element == btnDeleteDeck || element == btnShuffleDeck || element == btnSortDeck ||
			element == btnClearDeck || element == btnStartFilter || element == btnClearFilter ||
			element == stLanNickname || element == stLanHostIP || element == stLanPassword ||
			element == stReplayHeading || element == stReplayStartTurn ||
			element == stDBCategory || element == stDeck || element == stCategory ||
			element == stLimit || element == stAttribute || element == stRace ||
			element == stAttack || element == stDefense || element == stStar ||
			element == stScale || element == stSearch;
	};
	const auto visit = [&](auto&& self, irr::gui::IGUIElement* parent) -> void {
		for(auto it = parent->getChildren().begin(); it != parent->getChildren().end(); ++it) {
			auto* element = *it;
			irr::gui::IGUIFont* applied = nullptr;
			auto* candidate = font && fixedCaption(element) ? font :
				(themeManager.HasVisualStyle() && !spaciousLayout && compactDeckCaption(element) ?
					themeCompactDeckFont : nullptr);
			if(candidate && element->getText()) {
				const auto size = candidate->getDimension(element->getText());
				const auto bounds = element->getRelativePosition();
				const irr::u32 padding = candidate == themeCompactDeckFont ? 4 : 12;
				const irr::u32 heightPadding = candidate == themeCompactDeckFont ? 2 : 6;
				if(size.Width + padding <= static_cast<irr::u32>(bounds.getWidth()) &&
					size.Height + heightPadding <= static_cast<irr::u32>(bounds.getHeight()))
					applied = candidate;
			}
			switch(element->getType()) {
			case irr::gui::EGUIET_BUTTON:
				static_cast<irr::gui::IGUIButton*>(element)->setOverrideFont(applied);
				break;
			case irr::gui::EGUIET_STATIC_TEXT:
				static_cast<irr::gui::IGUIStaticText*>(element)->setOverrideFont(applied);
				break;
			case irr::gui::EGUIET_EDIT_BOX:
				static_cast<irr::gui::IGUIEditBox*>(element)->setOverrideFont(applied);
				break;
			default:
				break;
			}
			self(self, element);
		}
	};
	visit(visit, env->getRootGUIElement());
	if(lstHostList)
		lstHostList->setOverrideFont(font);
	if(lstReplayList)
		lstReplayList->setOverrideFont(font);
}
void Game::SaveConfig() {
	FileSystem::RemoveFile("load-once.conf");
	FILE* fp = myfopen("system.conf", "w");
	std::fprintf(fp, "#config file\n#nickname & gamename should be less than 20 characters\n");
	char linebuf[CONFIG_LINE_SIZE];
	std::fprintf(fp, "use_d3d = %d\n", gameConf.use_d3d ? 1 : 0);
#ifdef _OPENMP
	std::fprintf(fp, "use_image_scale_multi_thread = %d\n", gameConf.use_image_scale_multi_thread ? 1 : 0);
#endif
	std::fprintf(fp, "use_image_load_background_thread = %d\n", gameConf.use_image_load_background_thread ? 1 : 0);
	std::fprintf(fp, "antialias = %d\n", gameConf.antialias);
	std::fprintf(fp, "errorlog = %u\n", gameConf.enable_log);
	BufferIO::CopyWideString(ebNickName->getText(), gameConf.nickname);
	BufferIO::EncodeUTF8(gameConf.nickname, linebuf);
	std::fprintf(fp, "nickname = %s\n", linebuf);
	BufferIO::EncodeUTF8(gameConf.gamename, linebuf);
	std::fprintf(fp, "gamename = %s\n", linebuf);
	BufferIO::EncodeUTF8(gameConf.lastcategory, linebuf);
	std::fprintf(fp, "lastcategory = %s\n", linebuf);
	BufferIO::EncodeUTF8(gameConf.lastdeck, linebuf);
	std::fprintf(fp, "lastdeck = %s\n", linebuf);
	std::fprintf(fp, "textfont = %s %d\n", gameConf.textfont, gameConf.textfontsize);
	std::fprintf(fp, "numfont = %s\n", gameConf.numfont);
	std::fprintf(fp, "serverport = %d\n", gameConf.serverport);
	std::fprintf(fp, "mcp_port = %d\n", gameConf.mcp_port);
	std::fprintf(fp, "local_host_only = %d\n", gameConf.local_host_only ? 1 : 0);
	BufferIO::EncodeUTF8(gameConf.lasthost, linebuf);
	std::fprintf(fp, "lasthost = %s\n", linebuf);
	BufferIO::EncodeUTF8(gameConf.lastport, linebuf);
	std::fprintf(fp, "lastport = %s\n", linebuf);
	//settings
	std::fprintf(fp, "automonsterpos = %d\n", (chkMAutoPos->isChecked() ? 1 : 0));
	std::fprintf(fp, "autospellpos = %d\n", (chkSTAutoPos->isChecked() ? 1 : 0));
	std::fprintf(fp, "randompos = %d\n", (chkRandomPos->isChecked() ? 1 : 0));
	std::fprintf(fp, "autochain = %d\n", (chkAutoChain->isChecked() ? 1 : 0));
	std::fprintf(fp, "waitchain = %d\n", (chkWaitChain->isChecked() ? 1 : 0));
	std::fprintf(fp, "showchain = %d\n", (chkDefaultShowChain->isChecked() ? 1 : 0));
	std::fprintf(fp, "mute_opponent = %d\n", (chkIgnore1->isChecked() ? 1 : 0));
	std::fprintf(fp, "mute_spectators = %d\n", (chkIgnore2->isChecked() ? 1 : 0));
	std::fprintf(fp, "use_lflist = %d\n", gameConf.use_lflist);
	std::fprintf(fp, "default_lflist = %d\n", gameConf.default_lflist);
	std::fprintf(fp, "default_rule = %d\n", gameConf.default_rule == DEFAULT_DUEL_RULE ? 0 : gameConf.default_rule);
	std::fprintf(fp, "hide_setname = %d\n", gameConf.hide_setname);
	std::fprintf(fp, "hide_hint_button = %d\n", gameConf.hide_hint_button);
	std::fprintf(fp, "#control_mode = 0: Key A/S/D/R Chain Buttons. control_mode = 1: MouseLeft/MouseRight/NULL/F9 Without Chain Buttons\n");
	std::fprintf(fp, "control_mode = %d\n", gameConf.control_mode);
	std::fprintf(fp, "draw_field_spell = %d\n", gameConf.draw_field_spell);
	std::fprintf(fp, "preserve_field_aspect = %d\n", gameConf.preserve_field_aspect ? 1 : 0);
	std::fprintf(fp, "modern_field_view = %d\n", gameConf.modern_field_view ? 1 : 0);
	std::fprintf(fp, "theme = %s\n", gameConf.theme_id.c_str());
	std::fprintf(fp, "separate_clear_button = %d\n", gameConf.separate_clear_button);
	std::fprintf(fp, "#auto_search_limit >= 0: Start search automatically when the user enters N chars\n");
	std::fprintf(fp, "auto_search_limit = %d\n", gameConf.auto_search_limit);
	std::fprintf(fp, "#search_multiple_keywords = 0: Disable. 1: Search mutiple keywords with separator \" \". 2: with separator \"+\"\n");
	std::fprintf(fp, "search_multiple_keywords = %d\n", gameConf.search_multiple_keywords);
	std::fprintf(fp, "ignore_deck_changes = %d\n", (chkIgnoreDeckChanges->isChecked() ? 1 : 0));
	std::fprintf(fp, "default_ot = %d\n", gameConf.defaultOT);
	std::fprintf(fp, "enable_bot_mode = %d\n", gameConf.enable_bot_mode);
	BufferIO::EncodeUTF8(gameConf.bot_deck_path, linebuf);
	std::fprintf(fp, "bot_deck_path = %s\n", linebuf);
	std::fprintf(fp, "bot_room_public = %d\n", gameConf.bot_room_public);
	std::fprintf(fp, "quick_animation = %d\n", gameConf.quick_animation);
	std::fprintf(fp, "auto_save_replay = %d\n", (chkAutoSaveReplay->isChecked() ? 1 : 0));
	std::fprintf(fp, "draw_single_chain = %d\n", gameConf.draw_single_chain);
	std::fprintf(fp, "solid_selection_line = %d\n", gameConf.solid_selection_line);
	std::fprintf(fp, "hide_player_name = %d\n", gameConf.hide_player_name);
	std::fprintf(fp, "prefer_expansion_script = %d\n", gameConf.prefer_expansion_script);
	std::fprintf(fp, "swap_yes_no_button = %d\n", (chkSwapYesNoButton->isChecked() ? 1 : 0));
	std::fprintf(fp, "window_maximized = %d\n", (gameConf.window_maximized ? 1 : 0));
	std::fprintf(fp, "window_width = %d\n", gameConf.window_width);
	std::fprintf(fp, "window_height = %d\n", gameConf.window_height);
	std::fprintf(fp, "resize_select_window = %d\n", (chkResizeSelectWindow->isChecked() ? 1 : 0));
	std::fprintf(fp, "resize_popup_menu = %d\n", gameConf.resize_popup_menu);
#ifdef YGOPRO_USE_AUDIO
	std::fprintf(fp, "enable_sound = %d\n", (chkEnableSound->isChecked() ? 1 : 0));
	std::fprintf(fp, "enable_music = %d\n", (chkEnableMusic->isChecked() ? 1 : 0));
	std::fprintf(fp, "#Volume of sound and music, between 0 and 100\n");
	std::fprintf(fp, "sound_volume = %d\n", gameConf.sound_volume);
	std::fprintf(fp, "music_volume = %d\n", gameConf.music_volume);
	std::fprintf(fp, "music_mode = %d\n", (chkMusicMode->isChecked() ? 1 : 0));
#endif
	std::fclose(fp);
}
void Game::ShowCardInfo(int code, bool resize) {
	if(showingcode == code && !resize)
		return;
	wchar_t formatBuffer[256];
	auto& _datas = dataManager.GetDataTable();
	auto cit = _datas.find(code);
	bool is_valid = (cit != _datas.end());
	imgCard->setImage(imageManager.GetTexture(code, true));
	if (is_valid) {
		auto& cd = cit->second;
		myswprintf(formatBuffer, L"%ls[%08d]", dataManager.GetName(cd.get_original_code()), cd.get_original_code());
	}
	else {
		myswprintf(formatBuffer, L"%ls[%08d]", dataManager.GetName(code), code);
	}
	stName->setText(formatBuffer);
	if((int)guiFont->getDimension(formatBuffer).Width > stName->getRelativePosition().getWidth() - gameConf.textfontsize)
		stName->setToolTipText(formatBuffer);
	else
		stName->setToolTipText(nullptr);
	int offset = 0;
	if (is_valid && !gameConf.hide_setname) {
		auto& cd = cit->second;
		auto target = cit;
		if (cd.rule_code && _datas.count(cd.rule_code)) {
			target = _datas.find(cd.rule_code);
		}
		if (target->second.setcode[0]) {
			offset = 23;// *yScale;
			const auto& setname = dataManager.FormatSetName(target->second.setcode);
			myswprintf(formatBuffer, L"%ls%ls", dataManager.GetSysString(1329), setname.c_str());
			stSetName->setText(formatBuffer);
		}
		else
			stSetName->setText(L"");
	}
	else {
		stSetName->setText(L"");
	}
	if(is_valid && cit->second.type & TYPE_MONSTER) {
		auto& cd = cit->second;
		const auto& type = dataManager.FormatType(cd.type);
		const auto& race = dataManager.FormatRace(cd.race);
		const auto& attribute = dataManager.FormatAttribute(cd.attribute);
		myswprintf(formatBuffer, L"[%ls] %ls/%ls", type.c_str(), race.c_str(), attribute.c_str());
		stInfo->setText(formatBuffer);
		int offset_info = 0;
		irr::core::dimension2d<unsigned int> dtxt = guiFont->getDimension(formatBuffer);
		if(dtxt.Width > (300 * xScale - 13) - 15)
			offset_info = 15;
		const wchar_t* form = L"\u2605";
		wchar_t adBuffer[64]{};
		wchar_t scaleBuffer[16]{};
		if(!(cd.type & TYPE_LINK)) {
			if(cd.type & TYPE_XYZ)
				form = L"\u2606";
			if(cd.attack < 0 && cd.defense < 0)
				myswprintf(adBuffer, L"?/?");
			else if(cd.attack < 0)
				myswprintf(adBuffer, L"?/%d", cd.defense);
			else if(cd.defense < 0)
				myswprintf(adBuffer, L"%d/?", cd.attack);
			else
				myswprintf(adBuffer, L"%d/%d", cd.attack, cd.defense);
		} else {
			form = L"LINK-";
			const auto& link_marker = dataManager.FormatLinkMarker(cd.link_marker);
			if(cd.attack < 0)
				myswprintf(adBuffer, L"?/-   %ls", link_marker.c_str());
			else
				myswprintf(adBuffer, L"%d/-   %ls", cd.attack, link_marker.c_str());
		}
		if(cd.type & TYPE_PENDULUM) {
			myswprintf(scaleBuffer, L"   %d/%d", cd.lscale, cd.rscale);
		}
		myswprintf(formatBuffer, L"[%ls%d] %ls%ls", form, cd.level, adBuffer, scaleBuffer);
		stDataInfo->setText(formatBuffer);
		int offset_arrows = offset_info;
		dtxt = guiFont->getDimension(formatBuffer);
		if(dtxt.Width > (300 * xScale - 13) - 15)
			offset_arrows += 15;
		stInfo->setRelativePosition(irr::core::rect<irr::s32>(15, 37, 300 * xScale - 13, (60 + offset_info)));
		stDataInfo->setRelativePosition(irr::core::rect<irr::s32>(15, (60 + offset_info), 300 * xScale - 13, (83 + offset_arrows)));
		stSetName->setRelativePosition(irr::core::rect<irr::s32>(15, (83 + offset_arrows), 296 * xScale, (83 + offset_arrows) + offset));
		stText->setRelativePosition(irr::core::rect<irr::s32>(15, (83 + offset_arrows) + offset, 287 * xScale, 324 * yScale));
		scrCardText->setRelativePosition(irr::core::rect<irr::s32>(287 * xScale - 20, (83 + offset_arrows) + offset, 287 * xScale, 324 * yScale));
	}
	else {
		if (is_valid) {
			const auto& type = dataManager.FormatType(cit->second.type);
			myswprintf(formatBuffer, L"[%ls]", type.c_str());
		}
		else
			myswprintf(formatBuffer, L"[%ls]", dataManager.unknown_string);
		stInfo->setText(formatBuffer);
		stDataInfo->setText(L"");
		stSetName->setRelativePosition(irr::core::rect<irr::s32>(15, 60, 296 * xScale, 60 + offset));
		stText->setRelativePosition(irr::core::rect<irr::s32>(15, 60 + offset, 287 * xScale, 324 * yScale));
		scrCardText->setRelativePosition(irr::core::rect<irr::s32>(287 * xScale - 20, 60 + offset, 287 * xScale, 324 * yScale));
	}
	showingcode = code;
	showingtext = dataManager.GetText(code);
	const auto& tsize = stText->getRelativePosition();
	InitStaticText(stText, tsize.getWidth(), tsize.getHeight(), guiFont, showingtext);
}
void Game::ClearCardInfo(int player) {
	imgCard->setImage(imageManager.tCover[player]);
	showingcode = 0;
	stName->setText(L"");
	stInfo->setText(L"");
	stDataInfo->setText(L"");
	stSetName->setText(L"");
	stText->setText(L"");
	scrCardText->setVisible(false);
}
void Game::AddLog(const wchar_t* msg, int param) {
	logParam.push_back(param);
	lstLog->addItem(msg);
	if(!env->hasFocus(lstLog)) {
		lstLog->setSelected(-1);
	}
}
void Game::AddChatMsg(const wchar_t* msg, int player, bool play_sound) {
	for(int i = 7; i > 0; --i) {
		chatMsg[i] = chatMsg[i - 1];
		chatTiming[i] = chatTiming[i - 1];
		chatType[i] = chatType[i - 1];
	}
	chatMsg[0].clear();
	chatTiming[0] = 1200;
	chatType[0] = player;
	if(gameConf.hide_player_name && player < 4)
		player = 10;
	if(play_sound)
		soundManager.PlaySoundEffect(SOUND_CHAT);
	switch(player) {
	case 0: //from host
		chatMsg[0].append(dInfo.hostname);
		chatMsg[0].append(L": ");
		break;
	case 1: //from client
		chatMsg[0].append(dInfo.clientname);
		chatMsg[0].append(L": ");
		break;
	case 2: //host tag
		chatMsg[0].append(dInfo.hostname_tag);
		chatMsg[0].append(L": ");
		break;
	case 3: //client tag
		chatMsg[0].append(dInfo.clientname_tag);
		chatMsg[0].append(L": ");
		break;
	case 7: //local name
		chatMsg[0].append(ebNickName->getText());
		chatMsg[0].append(L": ");
		break;
	case 8: //system custom message, no prefix.
		chatMsg[0].append(L"[System]: ");
		break;
	case 9: //error message
		chatMsg[0].append(L"[Script Error]: ");
		break;
	case 10: //hidden name
		chatMsg[0].append(L"[********]: ");
		break;
	default: //from watcher or unknown
		if(player < 11 || player > 19)
			chatMsg[0].append(L"[---]: ");
	}
	chatMsg[0].append(msg);
}
void Game::ClearChatMsg() {
	for(int i = 7; i >= 0; --i) {
		chatTiming[i] = 0;
	}
}
void Game::AddDebugMsg(const char* msg) {
	if (gameConf.enable_log & 0x1) {
		wchar_t wbuf[1024];
		BufferIO::DecodeUTF8(msg, wbuf);
		AddChatMsg(wbuf, 9);
	}
	if (gameConf.enable_log & 0x2) {
		char msgbuf[1040];
		mysnprintf(msgbuf, "[Script Error]: %s", msg);
		ErrorLog(msgbuf);
	}
}
void Game::ErrorLog(const char* msg) {
#ifdef _WIN32
	OutputDebugStringA(msg);
#else
	std::fprintf(stderr, "%s\n", msg);
#endif
	FILE* fp = myfopen("error.log", "a");
	if(!fp)
		return;
	time_t nowtime = std::time(nullptr);
	char timebuf[40];
	std::strftime(timebuf, sizeof timebuf, "%Y-%m-%d %H:%M:%S", std::localtime(&nowtime));
	std::fprintf(fp, "[%s]%s\n", timebuf, msg);
	std::fclose(fp);
}
void Game::ClearTextures() {
	matManager.mCard.setTexture(0, 0);
	ClearCardInfo(0);
	btnPSAU->setImage(nullptr);
	btnPSDU->setImage(nullptr);
	for(int i=0; i<=4; ++i) {
		btnCardSelect[i]->setImage(nullptr);
		btnCardDisplay[i]->setImage(nullptr);
	}
	btnImagePending.clear();
	btnCardImgInfo.clear();
	btnFacedownImgInfo.clear();
	btnFacedownImgInfo[btnPSAD] = {0, false};
	btnFacedownImgInfo[btnPSDD] = {0, true};
	imageManager.ClearTexture();
}
void Game::CloseGameButtons() {
	btnChainIgnore->setVisible(false);
	btnChainAlways->setVisible(false);
	btnChainWhenAvail->setVisible(false);
	btnCancelOrFinish->setVisible(false);
	btnSpectatorSwap->setVisible(false);
	btnShuffle->setVisible(false);
	wSurrender->setVisible(false);
}
void Game::CloseGameWindow() {
	CloseGameButtons();
	for(auto wit = fadingList.begin(); wit != fadingList.end(); ++wit) {
		if(wit->isFadein)
			wit->autoFadeoutFrame = 1;
	}
	wACMessage->setVisible(false);
	wANAttribute->setVisible(false);
	wANCard->setVisible(false);
	wANNumber->setVisible(false);
	wANRace->setVisible(false);
	wCardSelect->setVisible(false);
	wCardDisplay->setVisible(false);
	wCmdMenu->setVisible(false);
	wFTSelect->setVisible(false);
	wHand->setVisible(false);
	wMessage->setVisible(false);
	wOptions->setVisible(false);
	wPhase->setVisible(false);
	wPosSelect->setVisible(false);
	wQuery->setVisible(false);
	wReplayControl->setVisible(false);
	wReplaySave->setVisible(false);
	stHintMsg->setVisible(false);
	stTip->setVisible(false);
}
void Game::CloseDuelWindow() {
	CloseGameWindow();
	wCardImg->setVisible(false);
	wInfos->setVisible(false);
	wChat->setVisible(false);
	btnSideOK->setVisible(false);
	btnSideShuffle->setVisible(false);
	btnSideSort->setVisible(false);
	btnSideReload->setVisible(false);
	btnLeaveGame->setVisible(false);
	btnSpectatorSwap->setVisible(false);
	lstLog->clear();
	logParam.clear();
	lstHostList->clear();
	DuelClient::hosts.clear();
	ClearTextures();
	ResizeChatInputWindow();
	closeDoneSignal.Set();
}
int Game::LocalPlayer(int player) const {
	int pid = player ? 1 : 0;
	return dInfo.isFirst ? pid : 1 - pid;
}
int Game::OppositePlayer(int player) {
	auto player_side_bit = dInfo.isTag ? 0x2 : 0x1;
	return player ^ player_side_bit;
}
int Game::ChatLocalPlayer(int player) {
	if(player > 3)
		return player;
	bool is_self;
	if(dInfo.isStarted || is_siding) {
		if(dInfo.isInDuel)
			// when in duel
			player = dInfo.isFirst ? player : OppositePlayer(player);
		else {
			// when changing side or waiting tp result
			auto selftype_boundary = dInfo.isTag ? 2 : 1;
			if(DuelClient::selftype >= selftype_boundary && DuelClient::selftype < 4)
				player = OppositePlayer(player);
		}
		if (DuelClient::selftype >= 4) {
			is_self = false;
		} else if (dInfo.isTag) {
			is_self =  (player & 0x2) == 0 && (player & 0x1) == (DuelClient::selftype & 0x1);
		} else {
			is_self = player == 0;
		}
	} else {
		// when in lobby
		is_self = player == DuelClient::selftype;
	}
	if(dInfo.isTag && (player == 1 || player == 2)) {
		player = 3 - player;
	}
	return player | (is_self ? 0x10 : 0);
}
const wchar_t* Game::LocalName(int local_player) {
	return local_player == 0 ? dInfo.hostname : dInfo.clientname;
}
void Game::SwapYesNoButtons(bool no_first) {
	if(no_first) {
		btnYes->setRelativePosition(irr::core::rect<irr::s32>(200, 105, 250, 130));
		btnNo->setRelativePosition(irr::core::rect<irr::s32>(100, 105, 150, 130));
		btnSurrenderYes->setRelativePosition(irr::core::rect<irr::s32>(200, 105, 250, 130));
		btnSurrenderNo->setRelativePosition(irr::core::rect<irr::s32>(100, 105, 150, 130));
		btnRSYes->setRelativePosition(irr::core::rect<irr::s32>(170, 80, 240, 105));
		btnRSNo->setRelativePosition(irr::core::rect<irr::s32>(70, 80, 140, 105));
		btnDMOK->setRelativePosition(irr::core::rect<irr::s32>(170, 80, 240, 105));
		btnDMCancel->setRelativePosition(irr::core::rect<irr::s32>(70, 80, 140, 105));
	} else {
		btnYes->setRelativePosition(irr::core::rect<irr::s32>(100, 105, 150, 130));
		btnNo->setRelativePosition(irr::core::rect<irr::s32>(200, 105, 250, 130));
		btnSurrenderYes->setRelativePosition(irr::core::rect<irr::s32>(100, 105, 150, 130));
		btnSurrenderNo->setRelativePosition(irr::core::rect<irr::s32>(200, 105, 250, 130));
		btnRSYes->setRelativePosition(irr::core::rect<irr::s32>(70, 80, 140, 105));
		btnRSNo->setRelativePosition(irr::core::rect<irr::s32>(170, 80, 240, 105));
		btnDMOK->setRelativePosition(irr::core::rect<irr::s32>(70, 80, 140, 105));
		btnDMCancel->setRelativePosition(irr::core::rect<irr::s32>(170, 80, 240, 105));
	}
}
void Game::OnResize() {
	// The saved pile target no longer matches the new projection until the next mouse event.
	dField.hovered_location = 0;
#ifdef _WIN32
	WINDOWPLACEMENT plc;
	plc.length = sizeof(WINDOWPLACEMENT);
	if(GetWindowPlacement(hWnd, &plc))
		gameConf.window_maximized = (plc.showCmd == SW_SHOWMAXIMIZED);
#endif // _WIN32
	if(!gameConf.window_maximized) {
		gameConf.window_width = window_size.Width;
		gameConf.window_height = window_size.Height;
	}
	if(camera) {
		const auto fieldProjection = GetFieldProjectionForView(window_size.Width, window_size.Height,
			gameConf.modern_field_view || gameConf.preserve_field_aspect, gameConf.modern_field_view);
		irr::core::matrix4 projection;
		BuildProjectionMatrix(projection, fieldProjection.left, fieldProjection.right, fieldProjection.bottom, fieldProjection.top, 1.0f, 100.0f);
		camera->setProjectionMatrix(projection);
	}

	irr::gui::CGUITTFont* old_numFont = numFont;
	irr::gui::CGUITTFont* old_adFont = adFont;
	irr::gui::CGUITTFont* old_lpcFont = lpcFont;
	irr::gui::CGUITTFont* old_textFont = textFont;
	numFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.numfont, (yScale > 0.5 ? 16 * yScale : 8));
	adFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.numfont, (yScale > 0.75 ? 12 * yScale : 9));
	lpcFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.numfont, 48 * yScale);
	textFont = irr::gui::CGUITTFont::createTTFont(env, gameConf.textfont, (yScale > 0.642 ? gameConf.textfontsize * yScale : 9));
	old_numFont->drop();
	old_adFont->drop();
	old_lpcFont->drop();
	old_textFont->drop();

	imageManager.ClearTexture();
	imageManager.ResizeTexture();

	for(auto& it : btnCardImgInfo) {
		auto button = it.first;
		std::pair<int, bool>& imgInfo = it.second;
		btnImagePending[button] = imgInfo;
	}
	for(auto& it : btnFacedownImgInfo) {
		auto button = it.first;
		std::pair<int, bool>& imgInfo = it.second;
		if(imgInfo.second)
			button->setImage(imageManager.tButtonFacedownDefense[imgInfo.first]);
		else
			button->setImage(imageManager.tButtonFacedown[imgInfo.first]);
	}

	ApplyThemeMenuLayout();
	wDeckEdit->setRelativePosition(Resize(309, 5, 605, 130));
	cbDBDecks->setRelativePosition(Resize(80, 35, 220, 60));
	btnClearDeck->setRelativePosition(Resize(115, 99, 165, 120));
	btnSortDeck->setRelativePosition(Resize(60, 99, 110, 120));
	btnShuffleDeck->setRelativePosition(Resize(5, 99, 55, 120));
	btnSaveDeck->setRelativePosition(Resize(225, 35, 290, 60));
	btnSaveDeckAs->setRelativePosition(Resize(225, 65, 290, 90));
	ebDeckname->setRelativePosition(Resize(80, 65, 220, 90));
	cbDBCategory->setRelativePosition(Resize(80, 5, 220, 30));
	btnManageDeck->setRelativePosition(Resize(225, 5, 290, 30));
	wDeckManage->setRelativePosition(ResizeWin(310, 135, 800, 515));
	scrPackCards->setRelativePosition(Resize(775, 161, 795, 629));

	wSort->setRelativePosition(Resize(930, 132, 1020, 156));
	cbSortType->setRelativePosition(Resize(10, 2, 85, 22));
	wFilter->setRelativePosition(Resize(610, 5, 1020, 130));
	scrFilter->setRelativePosition(Resize(999, 161, 1019, 629));
	cbCardType->setRelativePosition(Resize(60, 25 / 6, 120, 20 + 25 / 6));
	cbCardType2->setRelativePosition(Resize(125, 25 / 6, 195, 20 + 25 / 6));
	cbRace->setRelativePosition(Resize(60, 40 + 75 / 6, 195, 60 + 75 / 6));
	cbAttribute->setRelativePosition(Resize(60, 20 + 50 / 6, 195, 40 + 50 / 6));
	cbLimit->setRelativePosition(Resize(260, 25 / 6, 390, 20 + 25 / 6));
	ebStar->setRelativePosition(Resize(60, 60 + 100 / 6, 95, 80 + 100 / 6));
	ebScale->setRelativePosition(Resize(155, 60 + 100 / 6, 195, 80 + 100 / 6));
	ebAttack->setRelativePosition(Resize(260, 20 + 50 / 6, 340, 40 + 50 / 6));
	ebDefense->setRelativePosition(Resize(260, 40 + 75 / 6, 340, 60 + 75 / 6));
	ebCardName->setRelativePosition(Resize(260, 60 + 100 / 6, 390, 80 + 100 / 6));
	btnEffectFilter->setRelativePosition(Resize(345, 20 + 50 / 6, 390, 60 + 75 / 6));
	btnStartFilter->setRelativePosition(Resize(260, 80 + 125 / 6, 390, 100 + 125 / 6));
	if(btnClearFilter)
		btnClearFilter->setRelativePosition(Resize(205, 80 + 125 / 6, 255, 100 + 125 / 6));
	btnMarksFilter->setRelativePosition(Resize(60, 80 + 125 / 6, 195, 100 + 125 / 6));

	irr::core::recti btncatepos = btnEffectFilter->getAbsolutePosition();
	wCategories->setRelativePosition(irr::core::recti(
		btncatepos.LowerRightCorner.X - wCategories->getRelativePosition().getWidth(),
		btncatepos.LowerRightCorner.Y - btncatepos.getHeight() / 2,
		btncatepos.LowerRightCorner.X,
		btncatepos.LowerRightCorner.Y - btncatepos.getHeight() / 2 + 245));

	wLinkMarks->setRelativePosition(ResizeWin(700, 30, 820, 150));
	stDBCategory->setRelativePosition(Resize(10, 9, 100, 29));
	stDeck->setRelativePosition(Resize(10, 39, 100, 59));
	stCategory->setRelativePosition(Resize(10, 2 + 25 / 6, 70, 22 + 25 / 6));
	stLimit->setRelativePosition(Resize(205, 2 + 25 / 6, 280, 22 + 25 / 6));
	stAttribute->setRelativePosition(Resize(10, 22 + 50 / 6, 70, 42 + 50 / 6));
	stRace->setRelativePosition(Resize(10, 42 + 75 / 6, 70, 62 + 75 / 6));
	stAttack->setRelativePosition(Resize(205, 22 + 50 / 6, 280, 42 + 50 / 6));
	stDefense->setRelativePosition(Resize(205, 42 + 75 / 6, 280, 62 + 75 / 6));
	stStar->setRelativePosition(Resize(10, 62 + 100 / 6, 70, 82 + 100 / 6));
	stSearch->setRelativePosition(Resize(205, 62 + 100 / 6, 280, 82 + 100 / 6));
	stScale->setRelativePosition(Resize(105, 62 + 100 / 6, 165, 82 + 100 / 6));
	ApplyThemeDeckCompactLayout();
	btnSideOK->setRelativePosition(Resize(400, 40, 710, 80));
	btnSideShuffle->setRelativePosition(Resize(310, 100, 370, 130));
	btnSideSort->setRelativePosition(Resize(375, 100, 435, 130));
	btnSideReload->setRelativePosition(Resize(440, 100, 500, 130));
	btnDeleteDeck->setRelativePosition(Resize(225, 95, 290, 120));

	wLanWindow->setRelativePosition(ResizeWin(220, 100, 800, 520));
	wCreateHost->setRelativePosition(ResizeWin(320, 100, 700, 520));
	wHostPrepare->setRelativePosition(ResizeWin(270, 120, 750, 440));
	wReplay->setRelativePosition(ResizeWin(220, 100, 800, 520));
	ApplyThemeBrowserLayout();
	wSinglePlay->setRelativePosition(ResizeWin(220, 100, 800, 520));

	wHand->setRelativePosition(ResizeWin(500, 450, 825, 605));
	wFTSelect->setRelativePosition(ResizeWin(550, 240, 780, 340));
	wMessage->setRelativePosition(ResizeWin(490, 200, 840, 340));
	wACMessage->setRelativePosition(ResizeWin(490, 240, 840, 300));
	wQuery->setRelativePosition(ResizeWin(490, 200, 840, 340));
	wSurrender->setRelativePosition(ResizeWin(490, 200, 840, 340));
	wOptions->setRelativePosition(ResizeWin(490, 200, 840, 340));
	wANNumber->setRelativePosition(ResizeWin(550, 180, 780, 430));
	wANCard->setRelativePosition(ResizeWin(510, 120, 820, 420));
	wANAttribute->setRelativePosition(ResizeWin(500, 200, 830, 285));
	wANRace->setRelativePosition(ResizeWin(480, 200, 850, 410));
	wReplaySave->setRelativePosition(ResizeWin(510, 200, 820, 320));
	wDMQuery->setRelativePosition(ResizeWin(400, 200, 710, 320));
	if(wPosSelect->isVisible())
		ResizePosSelectButtons();
	if(wCardSelect->isVisible())
		ResizeCardSelectButtons(wCardSelect, stCardPos, btnCardSelect, scrCardList, btnSelectOK, dField.selectable_cards);
	if(wCardDisplay->isVisible())
		ResizeCardSelectButtons(wCardDisplay, stDisplayPos, btnCardDisplay, scrDisplayList, btnDisplayOK, dField.display_cards);

	stHintMsg->setRelativePosition(ResizeWin(660 - 160 * xScale, 60, 660 + 160 * xScale, 90));

	//sound / music volume bar
	scrSoundVolume->setRelativePosition(irr::core::recti(scrSoundVolume->getRelativePosition().UpperLeftCorner.X, scrSoundVolume->getRelativePosition().UpperLeftCorner.Y, 20 + (300 * xScale) - 70, scrSoundVolume->getRelativePosition().LowerRightCorner.Y));
	scrMusicVolume->setRelativePosition(irr::core::recti(scrMusicVolume->getRelativePosition().UpperLeftCorner.X, scrMusicVolume->getRelativePosition().UpperLeftCorner.Y, 20 + (300 * xScale) - 70, scrMusicVolume->getRelativePosition().LowerRightCorner.Y));
	scrResizePopupMenu->setRelativePosition(irr::core::recti(scrResizePopupMenu->getRelativePosition().UpperLeftCorner.X, scrResizePopupMenu->getRelativePosition().UpperLeftCorner.Y, 20 + (300 * xScale) - 70, scrResizePopupMenu->getRelativePosition().LowerRightCorner.Y));

	irr::core::recti tabHelperPos = irr::core::recti(0, 0, 300 * xScale - 50, 365 * yScale - 65);
	tabHelper->setRelativePosition(tabHelperPos);
	scrTabHelper->setRelativePosition(irr::core::recti(tabHelperPos.LowerRightCorner.X + 2, 0, tabHelperPos.LowerRightCorner.X + 22, tabHelperPos.LowerRightCorner.Y));
	irr::s32 tabHelperLastY = elmTabHelperLast->getRelativePosition().LowerRightCorner.Y;
	if(tabHelperLastY > tabHelperPos.LowerRightCorner.Y) {
		scrTabHelper->setMax(tabHelperLastY - tabHelperPos.LowerRightCorner.Y + 5);
		scrTabHelper->setPos(0);
		scrTabHelper->setVisible(true);
	}
	else
		scrTabHelper->setVisible(false);

	irr::core::recti tabSystemPos = irr::core::recti(0, 0, 300 * xScale - 50, 365 * yScale - 65);
	tabSystem->setRelativePosition(tabSystemPos);
	scrTabSystem->setRelativePosition(irr::core::recti(tabSystemPos.LowerRightCorner.X + 2, 0, tabSystemPos.LowerRightCorner.X + 22, tabSystemPos.LowerRightCorner.Y));
	irr::s32 tabSystemLastY = elmTabSystemLast->getRelativePosition().LowerRightCorner.Y;
	if(tabSystemLastY > tabSystemPos.LowerRightCorner.Y) {
		scrTabSystem->setMax(tabSystemLastY - tabSystemPos.LowerRightCorner.Y + 5);
		scrTabSystem->setPos(0);
		scrTabSystem->setVisible(true);
	} else
		scrTabSystem->setVisible(false);

	wCardImg->setRelativePosition(ResizeCardImgWin(1, 1, 20, 18));
	imgCard->setRelativePosition(ResizeCardImgWin(10, 9, 0, 0));
	wInfos->setRelativePosition(Resize(1, 275, 301, 639));
	ApplyThemeAppearanceLayout();
	stName->setRelativePosition(irr::core::recti(10, 10, 300 * xScale - 13, 10 + 22));
	lstLog->setRelativePosition(Resize(10, 10, 290, 290));
	if(showingcode)
		ShowCardInfo(showingcode, true);
	else
		ClearCardInfo();
	btnClearLog->setRelativePosition(Resize(160, 300, 260, 325));

	wPhase->setRelativePosition(Resize(480, 310, 855, 330));
	btnPhaseStatus->setRelativePosition(Resize(0, 0, 50, 20));
	btnBP->setRelativePosition(Resize(160, 0, 210, 20));
	btnM2->setRelativePosition(Resize(160, 0, 210, 20));
	btnEP->setRelativePosition(Resize(320, 0, 370, 20));

	ResizeChatInputWindow();
	ResizeCmdMenu();

	btnLeaveGame->setRelativePosition(Resize(205, 5, 295, 80));
	wReplayControl->setRelativePosition(Resize(205, 143, 295, 273));
	btnReplayStart->setRelativePosition(Resize(5, 5, 85, 25));
	btnReplayPause->setRelativePosition(Resize(5, 5, 85, 25));
	btnReplaySwap->setRelativePosition(Resize(5, 30, 85, 50));
	btnReplayStep->setRelativePosition(Resize(5, 55, 85, 75));
	btnReplayUndo->setRelativePosition(Resize(5, 80, 85, 100));
	btnReplayExit->setRelativePosition(Resize(5, 105, 85, 125));

	btnSpectatorSwap->setRelativePosition(Resize(205, 100, 295, 135));
	btnChainAlways->setRelativePosition(Resize(205, 140, 295, 175));
	btnChainIgnore->setRelativePosition(Resize(205, 100, 295, 135));
	btnChainWhenAvail->setRelativePosition(Resize(205, 180, 295, 215));
	btnShuffle->setRelativePosition(Resize(205, 230, 295, 265));
	btnCancelOrFinish->setRelativePosition(Resize(205, 230, 295, 265));

	btnBigCardOriginalSize->setRelativePosition(Resize(205, 100, 295, 135));
	btnBigCardZoomIn->setRelativePosition(Resize(205, 140, 295, 175));
	btnBigCardZoomOut->setRelativePosition(Resize(205, 180, 295, 215));
	btnBigCardClose->setRelativePosition(Resize(205, 230, 295, 265));

	irr::s32 barWidth = (xScale > 1) ? gameConf.textfontsize * xScale : gameConf.textfontsize;
	env->getSkin()->setSize(irr::gui::EGDS_SCROLLBAR_SIZE, barWidth);
	ApplyThemeTypography();
}
void Game::ResizeChatInputWindow() {
	irr::s32 x = wInfos->getRelativePosition().LowerRightCorner.X + 6;
	if(is_building) x = 802 * xScale;
	wChat->setRelativePosition(irr::core::recti(x, window_size.Height - 25, window_size.Width, window_size.Height));
	ebChatInput->setRelativePosition(irr::core::recti(3, 2, window_size.Width - wChat->getRelativePosition().UpperLeftCorner.X - 6, 22));
}
void Game::ResizeCmdMenu() {
	irr::s32 width = GetPopupMenuButtonWidth();
	irr::s32 height = GetPopupMenuButtonHeight();
	wCmdMenu->setRelativePosition(irr::core::recti(0, 0, width, 0));
	btnActivate->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnSummon->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnSPSummon->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnMSet->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnSSet->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnRepos->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnAttack->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnShowList->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnOperation->setRelativePosition(irr::core::recti(0, 0, width, height));
	btnReset->setRelativePosition(irr::core::recti(0, 0, width, height));
}
void Game::ResizePosSelectButtons() {
	float _xScale = gameConf.resize_select_window ? xScale : 1.0f;
	float _yScale = gameConf.resize_select_window ? yScale : 1.2f;
	irr::s32 imgHeight = CARD_IMG_HEIGHT * 0.5f * _yScale + 0.5f;
	irr::s32 gap = 5 * _xScale + 0.5f;
	irr::s32 btnPosWidth = imgHeight + gap * 2; // Square buttons, width = height
	irr::s32 stride = btnPosWidth + gap;
	int totalWidth = 0, visCount = 0;
	if(btnPSAU->isVisible()) { totalWidth += btnPosWidth; visCount++; }
	if(btnPSAD->isVisible()) { totalWidth += btnPosWidth; visCount++; }
	if(btnPSDU->isVisible()) { totalWidth += btnPosWidth; visCount++; }
	if(btnPSDD->isVisible()) { totalWidth += btnPosWidth; visCount++; }
	totalWidth += (visCount - 1) * gap;
	irr::s32 posY = 19 + 16 * _yScale;
	irr::s32 windowWidth = 30 * _xScale * 2 + stride * 3 - gap;
	irr::s32 windowHeight = posY + 155 * _yScale;
	irr::s32 posX = (windowWidth - totalWidth) / 2;
	if(btnPSAU->isVisible()) {
		btnPSAU->setRelativePosition(irr::core::recti(posX, posY, posX + btnPosWidth, posY + btnPosWidth));
		posX += btnPosWidth + gap;
	}
	if(btnPSAD->isVisible()) {
		btnPSAD->setRelativePosition(irr::core::recti(posX, posY, posX + btnPosWidth, posY + btnPosWidth));
		posX += btnPosWidth + gap;
	}
	if(btnPSDU->isVisible()) {
		btnPSDU->setRelativePosition(irr::core::recti(posX, posY, posX + btnPosWidth, posY + btnPosWidth));
		posX += btnPosWidth + gap;
	}
	if(btnPSDD->isVisible()) {
		btnPSDD->setRelativePosition(irr::core::recti(posX, posY, posX + btnPosWidth, posY + btnPosWidth));
		posX += btnPosWidth + gap;
	}
	wPosSelect->setRelativePosition(irr::core::recti(663 * xScale - windowWidth / 2, 303 * yScale - windowHeight / 2, 663 * xScale + windowWidth / 2, 303 * yScale + windowHeight / 2));
}
void Game::ResizeCardSelectButtons(irr::gui::IGUIWindow* window,
								   irr::gui::IGUIStaticText** labels,
								   irr::gui::IGUIButton** images,
								   irr::gui::IGUIScrollBar* scrollbar,
								   irr::gui::IGUIButton* buttonOK,
								   const std::vector<ClientCard*>& cards) {
	float _xScale = gameConf.resize_select_window ? xScale : 1.0f;
	float _yScale = gameConf.resize_select_window ? yScale : 1.2f;
	irr::s32 gap = 5 * _xScale + 0.5f;
	irr::s32 btnWidth = CARD_IMG_WIDTH * 0.55f * _yScale + 0.5f;
	irr::s32 btnHeight = CARD_IMG_HEIGHT * 0.55f * _yScale + 0.5f;
	irr::s32 stride = btnWidth + gap;
	int startpos = 30 * _xScale;
	int ct = 5;
	if (cards.size() < 5) {
		startpos = 30 * _xScale + stride * (5 - (int)cards.size()) / 2;
		ct = cards.size();
	}
	irr::s32 top = 19 + 11 * _yScale;
	irr::s32 labelHeight = 20 * _yScale;
	irr::s32 minTextHeight = gameConf.textfontsize * 1.4f + 0.5f;
	if (labelHeight < minTextHeight) labelHeight = minTextHeight;
	irr::s32 btnTop = top + labelHeight + gap;
	for (int i = 0; i < ct; ++i) {
		labels[i]->setRelativePosition(irr::core::recti(startpos + stride * i, top, startpos + stride * i + btnWidth, top + labelHeight));
		images[i]->setRelativePosition(irr::core::recti(startpos + stride * i, btnTop, startpos + stride * i + btnWidth, btnTop + btnHeight));
	}
	irr::s32 barTop = btnTop + btnHeight + gap;
	irr::s32 barWidth = stride * ct - gap;
	irr::s32 barHeight = 20 * _yScale;
	if (barHeight > 25) barHeight = 25;
	scrollbar->setRelativePosition(irr::core::recti(startpos, barTop, startpos + barWidth, barTop + barHeight));
	irr::s32 btnOKWidth = 80 * _xScale;
	irr::s32 btnOKHeight = 25 * _yScale;
	if (btnOKHeight < minTextHeight) btnOKHeight = minTextHeight;
	buttonOK->setRelativePosition(irr::core::recti(startpos + barWidth / 2 - btnOKWidth / 2, barTop + barHeight + gap * 2, startpos + barWidth / 2 + btnOKWidth / 2, barTop + barHeight + gap * 2 + btnOKHeight));
	irr::s32 windowWidth = 30 * _xScale * 2 + stride * 5 - gap;
	irr::s32 windowHeight = top + labelHeight + btnHeight + barHeight + btnOKHeight + gap * 6;
	window->setRelativePosition(irr::core::recti(663 * xScale - windowWidth / 2, 263 * yScale - windowHeight / 2, 663 * xScale + windowWidth / 2, 263 * yScale + windowHeight / 2));
}
irr::core::recti Game::Resize(irr::s32 x, irr::s32 y, irr::s32 x2, irr::s32 y2) {
	x = x * xScale;
	y = y * yScale;
	x2 = x2 * xScale;
	y2 = y2 * yScale;
	return irr::core::recti(x, y, x2, y2);
}
irr::core::recti Game::Resize(irr::s32 x, irr::s32 y, irr::s32 x2, irr::s32 y2, irr::s32 dx, irr::s32 dy, irr::s32 dx2, irr::s32 dy2) {
	x = x * xScale + dx;
	y = y * yScale + dy;
	x2 = x2 * xScale + dx2;
	y2 = y2 * yScale + dy2;
	return irr::core::recti(x, y, x2, y2);
}
irr::core::vector2di Game::Resize(irr::s32 x, irr::s32 y) {
	x = x * xScale;
	y = y * yScale;
	return irr::core::vector2di(x, y);
}
irr::core::vector2di Game::ResizeReverse(irr::s32 x, irr::s32 y) {
	x = x / xScale;
	y = y / yScale;
	return irr::core::vector2di(x, y);
}
irr::core::recti Game::ResizeWin(irr::s32 x, irr::s32 y, irr::s32 x2, irr::s32 y2) {
	irr::s32 w = x2 - x;
	irr::s32 h = y2 - y;
	x = (x + w / 2) * xScale - w / 2;
	y = (y + h / 2) * yScale - h / 2;
	x2 = w + x;
	y2 = h + y;
	return irr::core::recti(x, y, x2, y2);
}
irr::core::recti Game::ResizePhaseHint(irr::s32 x, irr::s32 y, irr::s32 x2, irr::s32 y2, irr::s32 width) {
	x = x * xScale - width / 2;
	y = y * yScale;
	x2 = x2 * xScale;
	y2 = y2 * yScale;
	return irr::core::recti(x, y, x2, y2);
}
irr::core::recti Game::ResizeCardImgWin(irr::s32 x, irr::s32 y, irr::s32 mx, irr::s32 my) {
	float mul = xScale;
	if(xScale > yScale)
		mul = yScale;
	irr::s32 w = CARD_IMG_WIDTH * mul + mx * xScale;
	irr::s32 h = CARD_IMG_HEIGHT * mul + my * yScale;
	x = x * xScale;
	y = y * yScale;
	return irr::core::recti(x, y, x + w, y + h);
}
irr::core::recti Game::ResizeCardHint(irr::s32 x, irr::s32 y, irr::s32 x2, irr::s32 y2) {
	return ResizeCardMid(x, y, x2, y2, (x + x2) * 0.5, (y + y2) * 0.5);
}
irr::core::vector2di Game::ResizeCardHint(irr::s32 x, irr::s32 y) {
	return ResizeCardMid(x, y, x + CARD_IMG_WIDTH * 0.5, y + CARD_IMG_HEIGHT * 0.5);
}
irr::core::recti Game::ResizeCardMid(irr::s32 x, irr::s32 y, irr::s32 x2, irr::s32 y2, irr::s32 midx, irr::s32 midy) {
	float mul = xScale;
	if(xScale > yScale)
		mul = yScale;
	irr::s32 cx = midx * xScale;
	irr::s32 cy = midy * yScale;
	x = cx + (x - midx) * mul;
	y = cy + (y - midy) * mul;
	x2 = cx + (x2 - midx) * mul;
	y2 = cy + (y2 - midy) * mul;
	return irr::core::recti(x, y, x2, y2);
}
irr::core::vector2di Game::ResizeCardMid(irr::s32 x, irr::s32 y, irr::s32 midx, irr::s32 midy) {
	float mul = xScale;
	if(xScale > yScale)
		mul = yScale;
	irr::s32 cx = midx * xScale;
	irr::s32 cy = midy * yScale;
	x = cx + (x - midx) * mul;
	y = cy + (y - midy) * mul;
	return irr::core::vector2di(x, y);
}
irr::core::recti Game::ResizeFit(irr::s32 x, irr::s32 y, irr::s32 x2, irr::s32 y2) {
	float mul = xScale;
	if(xScale > yScale)
		mul = yScale;
	x = x * mul;
	y = y * mul;
	x2 = x2 * mul;
	y2 = y2 * mul;
	return irr::core::recti(x, y, x2, y2);
}

void Game::FixMacOSBundleWorkingDirectory() {
#ifdef __APPLE__
	CFBundleRef main_bundle = CFBundleGetMainBundle();
	if(!main_bundle) {
		return;
	}
	CFURLRef bundle_url = CFBundleCopyBundleURL(main_bundle);
	if(!bundle_url) {
		return;
	}
	CFURLRef bundle_base_url = CFURLCreateCopyDeletingLastPathComponent(nullptr, bundle_url);
	if(!bundle_base_url) {
		CFRelease(bundle_url);
		return;
	}
	CFStringRef bundle_ext = CFURLCopyPathExtension(bundle_url);
	if(bundle_ext) {
		char path[PATH_MAX];
		if(CFStringCompare(bundle_ext, CFSTR("app"), kCFCompareCaseInsensitive) == kCFCompareEqualTo
			&& CFURLGetFileSystemRepresentation(bundle_base_url, true, (UInt8*)path, PATH_MAX)) {
			chdir(path);
		}
		CFRelease(bundle_ext);
	}
	CFRelease(bundle_url);
	CFRelease(bundle_base_url);
#endif //__APPLE__
}
void Game::SetWindowsIcon() {
#ifdef _WIN32
	HINSTANCE hInstance = (HINSTANCE)GetModuleHandleW(nullptr);
	HICON hSmallIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
	HICON hBigIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
	SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmallIcon);
	SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hBigIcon);
#endif
}
void Game::SetWindowsScale(float scale) {
	device->restoreWindow();
	device->setWindowSize(irr::core::dimension2du(GAME_WINDOW_WIDTH * scale, GAME_WINDOW_HEIGHT * scale));
}
void Game::FlashWindow() {
#ifdef _WIN32
	FLASHWINFO fi;
	fi.cbSize = sizeof(FLASHWINFO);
	fi.hwnd = hWnd;
	fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
	fi.uCount = 0;
	fi.dwTimeout = 0;
	FlashWindowEx(&fi);
#endif
}
void Game::SetCursor(irr::gui::ECURSOR_ICON icon) {
	irr::gui::ICursorControl* cursor = device->getCursorControl();
	if(cursor->getActiveIcon() != icon) {
		cursor->setActiveIcon(icon);
	}
}
bool Game::SpawnAsync(const std::wstring& exePath, const std::vector<std::wstring>& args) {
#ifdef _WIN32
	std::wstring cmdLine = L"\"" + exePath + L"\"";
	for (const auto& arg : args) {
		cmdLine += L" \"" + arg + L"\"";
	}

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	// CreateProcessW can modify the command line buffer, so we need to create a mutable copy of it
	// TODO: Move to C++17 and use cmdLine.data() directly without copying to a vector
	std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
	cmdBuffer.push_back(L'\0');

	if (!CreateProcessW(exePath.c_str(), cmdBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		return false;
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
#else
	std::string exePathUTF8 = BufferIO::EncodeUTF8String(exePath);

	std::vector<std::string> utf8Args;
	utf8Args.emplace_back(exePathUTF8);
	for (const auto& arg : args) {
		utf8Args.push_back(BufferIO::EncodeUTF8String(arg));
	}

	std::vector<char*> execArgs;
	execArgs.reserve(utf8Args.size() + 1);
	for (auto& arg : utf8Args) {
		execArgs.push_back(const_cast<char*>(arg.c_str()));
	}
	execArgs.push_back(nullptr);

	pid_t pid{}; // ignore pid return value, use SIG_IGN to prevent zombie process
	return posix_spawn(&pid, exePathUTF8.c_str(), nullptr, nullptr, execArgs.data(), GetEnviron()) == 0;
#endif
}

}
