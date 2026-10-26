#include "FirstRunSetup.h"
#include "NativeUpdater.h"
#include <windows.h>
#include <commctrl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using json = nlohmann::json;

std::wstring Wide(const std::string& s) {
 const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
 if(n <= 0) return L"Unknown setup error.";
 std::wstring out(static_cast<size_t>(n), L'\0');
 MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, &out[0], n);
 out.pop_back(); return out;
}
std::string Field(const json& object, const char* name) {
 return object.is_object() && object.contains(name) && object[name].is_string() ?
  object[name].get<std::string>() : "";
}
json Run(const std::wstring& root, const char* action) {
 const auto value = json::parse(cardupdate::Run(root, action), nullptr, false);
 if(!value.is_object() || Field(value, "state").empty())
  throw std::runtime_error("First-run setup returned an invalid status.");
 return value;
}
void Explain(const std::wstring& detail) {
 TaskDialog(nullptr, nullptr, L"YGOPro first-run setup", L"Setup needs attention",
  detail.c_str(), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
}
std::wstring ExePath(const std::wstring& root) { return root + L"\\YGOProUpdateLauncher.exe"; }
std::wstring TemporaryLog(const std::wstring& root) {
 const std::wstring folder = root + L"\\card-updates";
 if(!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
  throw std::runtime_error("Cannot create the game setup state folder.");
 const DWORD attributes = GetFileAttributesW(folder.c_str());
 if(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
  (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
  throw std::runtime_error("Unsafe game setup state folder.");
 wchar_t path[MAX_PATH]{};
 if(folder.size() + 16 >= MAX_PATH || !GetTempFileNameW(folder.c_str(), L"YGU", 0, path))
  throw std::runtime_error("Cannot create a temporary setup log.");
 return path;
}
std::string ReadLog(const std::wstring& path) {
 FILE* f = _wfopen(path.c_str(), L"rb");
 if(!f) return "No setup detail was returned.";
 std::string output;
 char buffer[4096]; size_t n = 0;
 while((n = fread(buffer, 1, sizeof buffer, f)) > 0) {
  if(output.size() + n > 16384) { output += "\nOutput was truncated."; break; }
  output.append(buffer, n);
 }
 fclose(f); return output;
}
struct Step {
 HANDLE process = nullptr;
 HANDLE log = nullptr;
 std::wstring logPath;
 bool canceled = false;
 bool finished = false;
 DWORD exitCode = 1;
 ~Step() {
  if(process) CloseHandle(process);
  if(log) CloseHandle(log);
  if(!logPath.empty()) {
   for(int attempt = 0; attempt < 5 && !DeleteFileW(logPath.c_str()); ++attempt)
    Sleep(50); // A just-exited child may still be releasing inherited handles.
  }
 }
};
void Start(const std::wstring& root, const wchar_t* action, Step& step) {
 step.logPath = TemporaryLog(root);
 step.log = CreateFileW(step.logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
 if(step.log == INVALID_HANDLE_VALUE) { step.log = nullptr; throw std::runtime_error("Cannot open setup output."); }
 SetHandleInformation(step.log, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
 const auto exe = ExePath(root);
 std::wstring command = L"\"" + exe + L"\" --update-action " + action;
 STARTUPINFOW startup{sizeof startup};
 startup.dwFlags = STARTF_USESTDHANDLES;
 startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
 startup.hStdOutput = step.log;
 startup.hStdError = step.log;
 PROCESS_INFORMATION child{};
 const BOOL started = CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, TRUE,
  CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &child);
 SetHandleInformation(step.log, HANDLE_FLAG_INHERIT, 0);
 CloseHandle(step.log); step.log = nullptr;
 if(!started) throw std::runtime_error("Cannot start the packaged native setup engine.");
 step.process = child.hProcess;
 CloseHandle(child.hThread);
}
HRESULT CALLBACK Progress(HWND window, UINT event, WPARAM button, LPARAM, LONG_PTR context) {
 auto& step = *reinterpret_cast<Step*>(context);
 if(event == TDN_CREATED) {
  SendMessageW(window, TDM_ENABLE_BUTTON, IDOK, FALSE);
  SendMessageW(window, TDM_SET_MARQUEE_PROGRESS_BAR, TRUE, 0);
  SendMessageW(window, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
 } else if(event == TDN_TIMER && !step.finished &&
  WaitForSingleObject(step.process, 0) == WAIT_OBJECT_0) {
  step.finished = true;
  GetExitCodeProcess(step.process, &step.exitCode);
  SendMessageW(window, TDM_ENABLE_BUTTON, IDOK, TRUE);
  SendMessageW(window, TDM_CLICK_BUTTON, IDOK, 0);
 } else if(event == TDN_BUTTON_CLICKED && button == IDCANCEL && !step.finished) {
  step.canceled = true;
  TerminateProcess(step.process, ERROR_CANCELLED);
  WaitForSingleObject(step.process, 5000);
  step.finished = true;
  step.exitCode = ERROR_CANCELLED;
 }
 return S_OK;
}
bool DoStep(const std::wstring& root, const wchar_t* action,
 const wchar_t* instruction, const wchar_t* detail, bool& canceled,
 std::wstring& error) {
 Step step;
 Start(root, action, step);
 TASKDIALOGCONFIG dialog{sizeof dialog};
 dialog.pszWindowTitle = L"YGOPro first-run setup";
 dialog.pszMainInstruction = instruction;
 dialog.pszContent = detail;
 dialog.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
 dialog.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
 dialog.pfCallback = Progress;
 dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(&step);
 int clicked = 0;
 const HRESULT hr = TaskDialogIndirect(&dialog, &clicked, nullptr, nullptr);
 if(FAILED(hr)) {
  TerminateProcess(step.process, ERROR_CANCELLED);
  WaitForSingleObject(step.process, 5000);
  throw std::runtime_error("Cannot display setup progress.");
 }
 if(!step.finished) {
  TerminateProcess(step.process, ERROR_CANCELLED);
  WaitForSingleObject(step.process, 5000);
  step.canceled = true;
 }
 canceled = step.canceled || clicked == IDCANCEL;
 if(canceled) return false;
 if(step.exitCode != 0) {
  error = Wide(ReadLog(step.logPath));
  return false;
 }
 return true;
}
void WriteStderr(const std::string& line) {
 const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
 DWORD written = 0;
 if(handle && handle != INVALID_HANDLE_VALUE)
  WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}
}

bool EnsureFirstRunData(const std::wstring& root, bool stdioMode) {
 for(int attempts = 0; attempts < 3; ++attempts) {
  const auto status = Run(root, "BaseStatus");
  const auto state = Field(status, "state");
  if(state == "base-ready" && status.contains("core") && status["core"].is_object() &&
   status["core"].value("compatible", false) && status.value("playable", false)) return true;
  if(stdioMode) {
   WriteStderr("YGOPro first-run setup: " + state +
    "; open the normal launcher to complete setup.\r\n");
   return false;
  }
  if(state == "base-recovery-needed") {
   try { Run(root, "BaseRecover"); continue; }
   catch(const std::exception& ex) {
    Explain(L"An interrupted install needs recovery. Existing changed files were preserved.\n\n" + Wide(ex.what()));
    return false;
   }
  }
  if(state == "existing-data-unverified") {
   int choice = IDNO;
   const HRESULT hr = TaskDialog(nullptr, nullptr, L"YGOPro existing game data",
    L"Use this existing installation?",
    L"Its card database, scripts and interface files are present, but their versions were not verified against the managed baseline. Your files will not be changed. The game may still reject incompatible data. Choose No to keep this folder untouched.",
    TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_WARNING_ICON, &choice);
   if(FAILED(hr)) throw std::runtime_error("Cannot display the existing-data choice.");
   return choice == IDYES;
  }
  if(state == "managed-base-changed") {
   Explain(L"Installed managed game data or its validation archive has changed. Your files were preserved. Use a fresh package folder or restore your own backup; setup will not overwrite this folder.");
   return false;
  }
  if(state == "incompatible-core") {
   Explain(L"This executable and card engine do not match the reviewed baseline. Existing game data was preserved; no game was started.");
   return false;
  }
  if(state == "base-installed-needs-presentation" || state == "incomplete-existing-data") {
   Explain(state == "base-installed-needs-presentation" ?
    L"Card data is installed, but this package lacks the required original interface textures or licensed fonts. Use a complete YGOPro package; your card data was preserved." :
    L"This installation has existing game data, but its required database, scripts, interface assets or tested core could not all be verified. Setup will not replace or remove it. Restore a complete compatible installation or use a clean package folder.");
   return false;
  }
  if(state != "base-missing" && state != "verified-base-staged" && state != "incomplete-base-stage") {
   Explain(L"Setup cannot safely continue: " + Wide(state)); return false;
  }
  if(!status.contains("core") || !status["core"].is_object() ||
   !status["core"].value("compatible", false)) {
   Explain(L"This executable and card engine do not match the reviewed baseline. No game data was changed.");
   return false;
  }
  const wchar_t* prompt = state == "verified-base-staged" ?
   L"Verified game data is ready to install." :
   L"Download the compatible game data?";
  const wchar_t* detail = state == "verified-base-staged" ?
   L"Install the already verified English card database and matching scripts. Existing decks and settings are preserved." :
   L"Download the pinned English card database from MyCard and matching scripts from Fluorohydride, then validate their hashes and install them. This requires internet access and may take a few minutes. Existing files are never replaced.";
  int choice = IDNO;
  if(FAILED(TaskDialog(nullptr, nullptr, L"YGOPro first-run setup", prompt, detail,
   TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_INFORMATION_ICON, &choice)))
   throw std::runtime_error("Cannot display the first-run setup prompt.");
  if(choice != IDYES) return false;
  bool canceled = false;
  std::wstring error;
  try {
   if(state == "incomplete-base-stage") Run(root, "BaseCleanup");
   if(state != "verified-base-staged" && !DoStep(root, L"BasePrepare",
    L"Downloading and validating base game data",
    L"Fixed source files are checked before anything is installed. Cancel safely stops the download.",
    canceled, error)) {
    if(canceled) { Run(root, "BaseCleanup"); return false; }
    Explain(L"Download or validation failed. Your files were preserved.\n\n" + error);
    const auto current = Run(root, "BaseStatus");
    if(Field(current, "state") == "incomplete-base-stage") Run(root, "BaseCleanup");
    continue;
   }
   if(!DoStep(root, L"BaseInstall", L"Installing verified game data",
    L"A recovery journal protects the install. Cancel will restore the previous empty state.",
    canceled, error)) {
    const auto current = Run(root, "BaseStatus");
    if(Field(current, "state") == "base-recovery-needed") Run(root, "BaseRecover");
    if(canceled) return false;
    Explain(L"Installation failed. Recovery was attempted.\n\n" + error);
    continue;
   }
   const auto current = Run(root, "BaseStatus");
   if(Field(current, "state") == "base-ready") return true;
   if(Field(current, "state") == "base-installed-needs-presentation") {
    Explain(L"Card data installed successfully, but the package still needs its original interface textures and licensed fonts. Your installed data is preserved.");
    return false;
   }
   Explain(L"The installed game data did not pass the final readiness check. No game was started.");
   return false;
  } catch(const std::exception& ex) {
   Explain(L"Setup stopped safely. Existing files were preserved.\n\n" + Wide(ex.what()));
   return false;
  }
 }
 Explain(L"Setup did not complete after three attempts. Restart the launcher to retry.");
 return false;
}
