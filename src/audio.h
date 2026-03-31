#pragma once
/**
 * audio.h - System audio capture via WASAPI Loopback
 * 
 * WASAPI Loopback captures what you HEAR (game audio, music, etc.)
 * without any virtual audio device or VB-Cable needed.
 * 
 * This is the cleanest way to capture audio on Windows 7+.
 * Near-zero CPU, built into Windows.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>

using Microsoft::WRL::ComPtr;

// Forward declare to avoid circular include
class VideoEncoder;

class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture() { Stop(); }
    
    /**
     * Init WASAPI for loopback (system audio) or microphone capture
     * 
     * @param loopback true = capture system audio (what you hear)
     *                 false = capture microphone
     */
    bool Init(bool loopback = true) {
        m_loopback = loopback;
        
        // Get audio endpoint
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator)
        );
        if (FAILED(hr)) return false;
        
        ComPtr<IMMDevice> device;
        if (loopback) {
            // Default render device (speakers/headphones) for loopback
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        } else {
            // Default capture device (microphone)
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        }
        if (FAILED(hr)) return false;
        
        // Activate audio client
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_audioClient);
        if (FAILED(hr)) return false;
        
        // Get mix format (what the audio engine uses internally)
        WAVEFORMATEX* mixFmt = nullptr;
        m_audioClient->GetMixFormat(&mixFmt);
        
        // Store format info
        m_sampleRate  = mixFmt->nSamplesPerSec;
        m_channels    = mixFmt->nChannels;
        m_bitsPerSample = mixFmt->wBitsPerSample;
        
        // Initialize: AUDCLNT_STREAMFLAGS_LOOPBACK = capture render stream
        DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK;
        if (!loopback) flags = 0;
        
        // 100ms buffer (enough for smooth capture, low latency)
        REFERENCE_TIME bufferDuration = 1000000; // 100ms in 100ns units
        
        hr = m_audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            flags,
            bufferDuration,
            0,
            mixFmt,
            nullptr
        );
        
        CoTaskMemFree(mixFmt);
        
        if (FAILED(hr)) return false;
        
        hr = m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient));
        return SUCCEEDED(hr);
    }
    
    /**
     * Start - begin capturing audio and feeding to encoder
     * Runs on the audio thread (called from RecordingSession::AudioThreadProc)
     */
    void Start(VideoEncoder* encoder) {
        m_encoder = encoder;
        m_active = true;
        
        // Boost thread to audio class for jitter-free capture
        DWORD taskIdx = 0;
        HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Audio", &taskIdx);
        
        m_audioClient->Start();
        
        int64_t audioTime = 0;
        const int64_t frameDuration100ns = 10000000LL / 100; // ~10ms per chunk
        
        while (m_active) {
            // Wait for data (10ms sleep - very low CPU)
            DWORD waitResult = WaitForSingleObject(nullptr, 10);
            
            UINT32 packetSize = 0;
            if (FAILED(m_captureClient->GetNextPacketSize(&packetSize))) break;
            
            while (packetSize > 0) {
                BYTE*  data;
                UINT32 numFrames;
                DWORD  flags;
                UINT64 devicePosition;
                UINT64 qpcPosition;
                
                HRESULT hr = m_captureClient->GetBuffer(
                    &data, &numFrames, &flags,
                    &devicePosition, &qpcPosition
                );
                if (FAILED(hr)) break;
                
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && numFrames > 0) {
                    // Create MF sample from audio data
                    SendAudioToEncoder(data, numFrames, audioTime);
                }
                
                audioTime += (int64_t)numFrames * 10000000LL / m_sampleRate;
                
                m_captureClient->ReleaseBuffer(numFrames);
                m_captureClient->GetNextPacketSize(&packetSize);
            }
        }
        
        m_audioClient->Stop();
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
    }
    
    void Stop() {
        m_active = false;
    }
    
private:
    void SendAudioToEncoder(BYTE* data, UINT32 numFrames, int64_t timestamp) {
        if (!m_encoder) return;
        
        UINT32 byteCount = numFrames * m_channels * (m_bitsPerSample / 8);
        
        ComPtr<IMFSample>      sample;
        ComPtr<IMFMediaBuffer> buffer;
        
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(byteCount, &buffer);
        
        BYTE* buf = nullptr;
        buffer->Lock(&buf, nullptr, nullptr);
        memcpy(buf, data, byteCount);
        buffer->Unlock();
        buffer->SetCurrentLength(byteCount);
        
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(timestamp);
        sample->SetSampleDuration((int64_t)numFrames * 10000000LL / m_sampleRate);
        
        m_encoder->WriteAudioSample(sample.Get());
    }
    
    ComPtr<IAudioClient>        m_audioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;
    VideoEncoder*               m_encoder = nullptr;
    
    std::atomic<bool> m_active{false};
    bool     m_loopback     = true;
    UINT32   m_sampleRate   = 44100;
    UINT32   m_channels     = 2;
    UINT32   m_bitsPerSample = 32;
};
