/**
 * PotatoRec - Ultra-lightweight screen recorder for potato PCs
 * Designed for Celeron-class CPUs with minimal overhead
 * 
 * Key features:
 * - DXGI Desktop Duplication API (GPU-accelerated capture, near-zero CPU)
 * - Raw Input mouse cursor overlay (NO interference with game mouse)
 * - Hardware H.264 encoding via MF (Media Foundation) or software x264 fallback
 * - Minimal thread model: capture thread + encode thread + audio thread
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <shellapi.h>
#include <commctrl.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <functional>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

#include "capture.h"
#include "encoder.h"
#include "audio.h"
#include "cursor.h"
#include "tray.h"
#include "config.h"

// ─── Global State ────────────────────────────────────────────────────────────

static RecorderConfig g_config;
static std::atomic<bool> g_recording{false};
static std::atomic<bool> g_running{true};
static HWND g_hwnd = nullptr;

// Hotkey IDs
#define HOTKEY_TOGGLE_RECORD  1
#define HOTKEY_STOP_RECORD    2

// Window messages
#define WM_TRAYICON  (WM_USER + 1)
#define WM_RECSTART  (WM_USER + 2)
#define WM_RECSTOP   (WM_USER + 3)

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::wstring GetTimestampedFilename(const std::wstring& dir, const std::wstring& ext) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    std::wostringstream ss;
    ss << dir << L"\\PotatoRec_"
       << std::put_time(&tm_info, L"%Y%m%d_%H%M%S")
       << L"." << ext;
    return ss.str();
}

// ─── Recording Session ───────────────────────────────────────────────────────

struct RecordingSession {
    std::unique_ptr<DXGICapture>  capture;
    std::unique_ptr<VideoEncoder> encoder;
    std::unique_ptr<AudioCapture> audio;
    std::unique_ptr<CursorOverlay> cursor;
    
    std::thread captureThread;
    std::thread encodeThread;
    std::thread audioThread;
    
    std::atomic<bool> active{false};
    std::wstring outputPath;
    
    // Frame queue between capture and encoder
    struct Frame {
        std::vector<uint8_t> data;
        uint32_t width, height, stride;
        int64_t timestamp_100ns; // 100-nanosecond units for MF
    };
    
    std::queue<Frame> frameQueue;
    std::mutex frameMutex;
    std::condition_variable frameCV;
    static constexpr size_t MAX_QUEUE = 4; // Low on potato: 4 frames max buffered
    
    ~RecordingSession() { Stop(); }
    
    bool Start(const RecorderConfig& cfg) {
        outputPath = GetTimestampedFilename(cfg.outputDir, L"mp4");
        
        // 1. Init DXGI capture
        capture = std::make_unique<DXGICapture>();
        if (!capture->Init(cfg.monitorIndex)) {
            MessageBoxW(nullptr, L"Failed to init DXGI capture.\nMake sure you have DirectX 11.", L"PotatoRec Error", MB_ICONERROR);
            return false;
        }
        
        uint32_t capW = capture->GetWidth();
        uint32_t capH = capture->GetHeight();
        
        // Scale down if configured (saves massive CPU on Celeron)
        uint32_t encW = cfg.outputWidth  > 0 ? cfg.outputWidth  : capW;
        uint32_t encH = cfg.outputHeight > 0 ? cfg.outputHeight : capH;
        
        // 2. Init cursor overlay (raw input, NO system hook)
        cursor = std::make_unique<CursorOverlay>();
        cursor->Init();
        
        // 3. Init video encoder
        encoder = std::make_unique<VideoEncoder>();
        VideoEncoderConfig encCfg;
        encCfg.width     = encW;
        encCfg.height    = encH;
        encCfg.fps       = cfg.fps;
        encCfg.bitrate   = cfg.videoBitrate;
        encCfg.useHW     = cfg.useHardwareEncoder;
        encCfg.outputPath = outputPath;
        encCfg.audioEnabled = cfg.captureAudio;
        
        if (!encoder->Init(encCfg)) {
            MessageBoxW(nullptr, L"Failed to init video encoder.\nFalling back to software encoding.", L"PotatoRec", MB_ICONWARNING);
            encCfg.useHW = false;
            if (!encoder->Init(encCfg)) {
                MessageBoxW(nullptr, L"Software encoder also failed. Cannot record.", L"PotatoRec Error", MB_ICONERROR);
                return false;
            }
        }
        
        // 4. Init audio (loopback = system audio)
        if (cfg.captureAudio) {
            audio = std::make_unique<AudioCapture>();
            if (!audio->Init(cfg.captureLoopback)) {
                // Audio failure is non-fatal
                audio.reset();
            }
        }
        
        active = true;
        
        // 5. Start threads
        captureThread = std::thread([this, &cfg, capW, capH, encW, encH]() {
            CaptureThreadProc(cfg, capW, capH, encW, encH);
        });
        
        encodeThread = std::thread([this]() {
            EncodeThreadProc();
        });
        
        if (audio) {
            audioThread = std::thread([this]() {
                AudioThreadProc();
            });
        }
        
        return true;
    }
    
    void Stop() {
        if (!active.exchange(false)) return;
        
        frameCV.notify_all();
        
        if (captureThread.joinable()) captureThread.join();
        if (encodeThread.joinable())  encodeThread.join();
        if (audioThread.joinable())   audioThread.join();
        
        if (encoder) encoder->Finalize();
        if (audio)   audio->Stop();
        cursor.reset();
        encoder.reset();
        capture.reset();
        audio.reset();
    }
    
    // ── Capture Thread ────────────────────────────────────────────────────────
    // Runs at fps rate, captures DXGI frame, composites cursor, queues for encode
    void CaptureThreadProc(const RecorderConfig& cfg,
                           uint32_t capW, uint32_t capH,
                           uint32_t encW, uint32_t encH) {
        // Boost thread priority: we want consistent frame pacing
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        
        const auto frameDuration = std::chrono::microseconds(1000000 / cfg.fps);
        auto nextFrame = std::chrono::steady_clock::now();
        
        while (active) {
            auto now = std::chrono::steady_clock::now();
            if (now < nextFrame) {
                std::this_thread::sleep_until(nextFrame);
            }
            nextFrame += frameDuration;
            
            // Grab frame from DXGI (non-blocking with 0ms timeout when using duplication)
            CapturedFrame raw;
            if (!capture->AcquireFrame(raw, 8)) {
                // No new frame (nothing changed on screen) - duplicate last frame
                // This is zero-CPU-cost on static screens!
                capture->ReleaseFrame();
                continue;
            }
            
            // Get cursor position WITHOUT raw input hook interference
            POINT cursorPos;
            cursor->GetPosition(&cursorPos);
            
            // Map cursor to capture coords
            int cx = cursorPos.x - raw.desktopX;
            int cy = cursorPos.y - raw.desktopY;
            
            // Composite cursor onto frame (software, on the CPU-mapped texture)
            if (cfg.showCursor) {
                cursor->CompositeCursor(raw.data.data(), raw.width, raw.height,
                                        raw.stride, cx, cy);
            }
            
            // Scale if needed (bilinear, fast integer path for Celeron)
            Frame frame;
            frame.timestamp_100ns = raw.timestamp_100ns;
            frame.width  = encW;
            frame.height = encH;
            frame.stride = encW * 4;
            
            if (encW == capW && encH == capH) {
                frame.data = std::move(raw.data);
            } else {
                frame.data.resize(encW * encH * 4);
                ScaleBilinear(raw.data.data(), capW, capH, raw.stride,
                              frame.data.data(), encW, encH);
            }
            
            capture->ReleaseFrame();
            
            // Push to encode queue (drop if full - Celeron can't keep up, skip frame)
            {
                std::unique_lock<std::mutex> lk(frameMutex);
                if (frameQueue.size() < MAX_QUEUE) {
                    frameQueue.push(std::move(frame));
                    frameCV.notify_one();
                }
                // If queue is full: silently drop frame (better than lag)
            }
        }
    }
    
    // ── Encode Thread ─────────────────────────────────────────────────────────
    void EncodeThreadProc() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        
        while (active || !frameQueue.empty()) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lk(frameMutex);
                frameCV.wait(lk, [this] { return !frameQueue.empty() || !active; });
                if (frameQueue.empty()) break;
                frame = std::move(frameQueue.front());
                frameQueue.pop();
            }
            encoder->EncodeFrame(frame.data.data(), frame.width, frame.height,
                                  frame.stride, frame.timestamp_100ns);
        }
    }
    
    // ── Audio Thread ──────────────────────────────────────────────────────────
    void AudioThreadProc() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        audio->Start(encoder.get());
    }
    
    // ── Fast bilinear scale (integer math, Celeron-friendly) ──────────────────
    static void ScaleBilinear(const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t sstride,
                               uint8_t* dst, uint32_t dw, uint32_t dh) {
        const uint32_t xRatio = ((sw - 1) << 16) / dw;
        const uint32_t yRatio = ((sh - 1) << 16) / dh;
        
        for (uint32_t y = 0; y < dh; y++) {
            uint32_t yFP = y * yRatio;
            uint32_t y0  = yFP >> 16;
            uint32_t yf  = (yFP >> 8) & 0xFF;
            
            const uint8_t* row0 = src + y0 * sstride;
            const uint8_t* row1 = src + (y0 + 1 < sh ? y0 + 1 : y0) * sstride;
            uint8_t* dstRow = dst + y * dw * 4;
            
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t xFP = x * xRatio;
                uint32_t x0  = xFP >> 16;
                uint32_t xf  = (xFP >> 8) & 0xFF;
                uint32_t x1  = (x0 + 1 < sw) ? x0 + 1 : x0;
                
                for (int c = 0; c < 4; c++) {
                    uint32_t p00 = row0[x0*4+c], p10 = row0[x1*4+c];
                    uint32_t p01 = row1[x0*4+c], p11 = row1[x1*4+c];
                    uint32_t top = p00 + (((p10-p00) * xf) >> 8);
                    uint32_t bot = p01 + (((p11-p01) * xf) >> 8);
                    dstRow[x*4+c] = (uint8_t)(top + (((bot-top) * yf) >> 8));
                }
            }
        }
    }
};

// ─── Main Window Proc ────────────────────────────────────────────────────────

static std::unique_ptr<RecordingSession> g_session;
static TrayIcon* g_tray = nullptr;

static void StartRecording() {
    if (g_recording) return;
    g_session = std::make_unique<RecordingSession>();
    if (g_session->Start(g_config)) {
        g_recording = true;
        if (g_tray) g_tray->SetRecording(true, g_session->outputPath);
    } else {
        g_session.reset();
    }
}

static void StopRecording() {
    if (!g_recording) return;
    g_recording = false;
    if (g_session) {
        std::wstring path = g_session->outputPath;
        g_session->Stop();
        g_session.reset();
        if (g_tray) g_tray->SetRecording(false, path);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE_RECORD) {
            if (g_recording) StopRecording();
            else StartRecording();
        } else if (wp == HOTKEY_STOP_RECORD) {
            StopRecording();
        }
        break;
        
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            if (g_tray) g_tray->ShowContextMenu(hwnd);
        }
        break;
        
    case WM_RECSTART: StartRecording(); break;
    case WM_RECSTOP:  StopRecording();  break;
        
    case WM_DESTROY:
        StopRecording();
        g_running = false;
        PostQuitMessage(0);
        break;
        
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

// ─── Entry Point ─────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int) {
    // Init COM
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    
    // Load config
    g_config.Load();
    
    // Create hidden message-only window
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"PotatoRecWnd";
    RegisterClassExW(&wc);
    
    g_hwnd = CreateWindowExW(0, L"PotatoRecWnd", L"PotatoRec",
                             0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    
    // Register hotkeys
    RegisterHotKey(g_hwnd, HOTKEY_TOGGLE_RECORD,
                   g_config.hotkeyMod, g_config.hotkeyKey);
    
    // Tray icon
    TrayIcon tray(g_hwnd, hInst, WM_TRAYICON, WM_RECSTART, WM_RECSTOP);
    g_tray = &tray;
    tray.Install();
    
    // Message loop
    MSG msg;
    while (g_running && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    
    tray.Remove();
    MFShutdown();
    CoUninitialize();
    return 0;
}
