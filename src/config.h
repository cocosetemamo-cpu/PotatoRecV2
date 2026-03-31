#pragma once
/**
 * config.h - Recorder configuration (INI file based)
 * tray.h   - System tray icon
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "shell32.lib")

// ─── Config ──────────────────────────────────────────────────────────────────

struct RecorderConfig {
    // Output
    std::wstring outputDir    = L"";    // Empty = desktop
    uint32_t     fps          = 30;
    uint32_t     videoBitrate = 3000000; // 3 Mbps
    int          outputWidth  = 0;       // 0 = native
    int          outputHeight = 0;       // 0 = native
    int          monitorIndex = 0;
    
    // Features
    bool captureAudio       = true;
    bool captureLoopback    = true;  // true=system audio, false=mic
    bool useHardwareEncoder = true;  // Intel QSV / AMD VCE / NVENC
    bool showCursor         = true;
    
    // Hotkey (default: F9)
    UINT hotkeyMod = 0;        // No modifier
    UINT hotkeyKey = VK_F9;
    
    void Load() {
        // Get config file path next to exe
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring cfgPath(exePath);
        auto slashPos = cfgPath.rfind(L'\\');
        if (slashPos != std::wstring::npos)
            cfgPath = cfgPath.substr(0, slashPos + 1);
        cfgPath += L"PotatoRec.ini";
        
        // Default output dir: Documents\PotatoRec
        if (outputDir.empty()) {
            wchar_t docs[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, docs))) {
                outputDir = std::wstring(docs) + L"\\PotatoRec";
                std::filesystem::create_directories(outputDir);
            } else {
                outputDir = L".";
            }
        }
        
        auto readInt = [&](const wchar_t* sec, const wchar_t* key, int def) -> int {
            return (int)GetPrivateProfileIntW(sec, key, def, cfgPath.c_str());
        };
        auto readStr = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* def) -> std::wstring {
            wchar_t buf[MAX_PATH] = {};
            GetPrivateProfileStringW(sec, key, def, buf, MAX_PATH, cfgPath.c_str());
            return buf;
        };
        
        // Load settings (with sane potato defaults)
        fps             = readInt(L"Video", L"FPS",         30);
        videoBitrate    = readInt(L"Video", L"Bitrate",     3000000);
        outputWidth     = readInt(L"Video", L"Width",       0);
        outputHeight    = readInt(L"Video", L"Height",      0);
        monitorIndex    = readInt(L"Video", L"Monitor",     0);
        
        useHardwareEncoder = readInt(L"Video", L"HWEncode",  1) != 0;
        showCursor         = readInt(L"Video", L"ShowCursor", 1) != 0;
        captureAudio       = readInt(L"Audio", L"Enable",    1) != 0;
        captureLoopback    = readInt(L"Audio", L"Loopback",  1) != 0;
        
        std::wstring dir = readStr(L"Output", L"Dir", L"");
        if (!dir.empty()) outputDir = dir;
        
        hotkeyKey = readInt(L"Hotkey", L"Key", VK_F9);
        hotkeyMod = readInt(L"Hotkey", L"Mod", 0);
        
        // Clamp fps
        if (fps < 1)  fps = 1;
        if (fps > 60) fps = 60;
    }
    
    void Save(const std::wstring& cfgPath) {
        auto wi = [&](const wchar_t* s, const wchar_t* k, int v) {
            WritePrivateProfileStringW(s, k, std::to_wstring(v).c_str(), cfgPath.c_str());
        };
        wi(L"Video", L"FPS",       fps);
        wi(L"Video", L"Bitrate",   videoBitrate);
        wi(L"Video", L"Width",     outputWidth);
        wi(L"Video", L"Height",    outputHeight);
        wi(L"Video", L"Monitor",   monitorIndex);
        wi(L"Video", L"HWEncode",  useHardwareEncoder ? 1 : 0);
        wi(L"Video", L"ShowCursor",showCursor ? 1 : 0);
        wi(L"Audio", L"Enable",    captureAudio ? 1 : 0);
        wi(L"Audio", L"Loopback",  captureLoopback ? 1 : 0);
        wi(L"Hotkey",L"Key",       hotkeyKey);
        wi(L"Hotkey",L"Mod",       hotkeyMod);
        WritePrivateProfileStringW(L"Output", L"Dir", outputDir.c_str(), cfgPath.c_str());
    }
};

// ─── Tray Icon ───────────────────────────────────────────────────────────────

// Menu IDs
#define MENU_START  101
#define MENU_STOP   102
#define MENU_EXIT   103

class TrayIcon {
public:
    TrayIcon(HWND hwnd, HINSTANCE hInst, UINT trayMsg, UINT startMsg, UINT stopMsg)
        : m_hwnd(hwnd), m_hInst(hInst),
          m_trayMsg(trayMsg), m_startMsg(startMsg), m_stopMsg(stopMsg) {}
    
    ~TrayIcon() { Remove(); }
    
    bool Install() {
        m_nid.cbSize           = sizeof(m_nid);
        m_nid.hWnd             = m_hwnd;
        m_nid.uID              = 1;
        m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        m_nid.uCallbackMessage = m_trayMsg;
        m_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(m_nid.szTip, L"PotatoRec - Press F9 to record");
        
        return Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;
    }
    
    void Remove() {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
    }
    
    void SetRecording(bool recording, const std::wstring& path = L"") {
        m_recording = recording;
        
        if (recording) {
            wcscpy_s(m_nid.szTip, L"PotatoRec - RECORDING... (F9 to stop)");
        } else {
            wcscpy_s(m_nid.szTip, L"PotatoRec - Idle (F9 to record)");
            if (!path.empty()) {
                m_nid.uFlags |= NIF_INFO;
                wcscpy_s(m_nid.szInfoTitle, L"Recording saved!");
                std::wstring display = path;
                if (display.length() > 60)
                    display = L"..." + display.substr(display.length() - 57);
                wcscpy_s(m_nid.szInfo, display.c_str());
                m_nid.dwInfoFlags = NIIF_INFO;
            }
        }
        
        Shell_NotifyIconW(NIM_MODIFY, &m_nid);
        m_nid.uFlags &= ~NIF_INFO;
    }
    
    void ShowContextMenu(HWND hwnd) {
        HMENU hMenu = CreatePopupMenu();
        
        if (m_recording) {
            AppendMenuW(hMenu, MF_STRING, MENU_STOP,  L"Stop Recording (F9)");
            AppendMenuW(hMenu, MF_GRAYED, MENU_START, L"Start Recording");
        } else {
            AppendMenuW(hMenu, MF_GRAYED, MENU_STOP,  L"Stop Recording");
            AppendMenuW(hMenu, MF_STRING, MENU_START, L"Start Recording (F9)");
        }
        
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, MENU_EXIT, L"Exit PotatoRec");
        
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        
        switch (cmd) {
        case MENU_START: PostMessageW(hwnd, m_startMsg, 0, 0); break;
        case MENU_STOP:  PostMessageW(hwnd, m_stopMsg,  0, 0); break;
        case MENU_EXIT:  PostMessageW(hwnd, WM_DESTROY, 0, 0); break;
        }
    }
    
private:
    HWND       m_hwnd;
    HINSTANCE  m_hInst;
    UINT       m_trayMsg, m_startMsg, m_stopMsg;
    NOTIFYICONDATAW m_nid{};
    bool       m_recording = false;
};
