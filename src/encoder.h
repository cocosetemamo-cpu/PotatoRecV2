#pragma once
/**
 * encoder.h - Hardware H.264/H.265 encoder via Media Foundation
 * 
 * Priority chain for Celeron:
 * 1. Intel Quick Sync (QSV) via MF - Celeron has this! Even old ones.
 *    Uses the iGPU encoder, ~0% CPU usage
 * 2. AMD VCE / NVIDIA NVENC via MF - for systems with dGPU
 * 3. Software x264 via MF (Microsoft's built-in) - last resort
 *    Still lightweight if you set preset=ultrafast
 * 
 * Output: MP4 container via Sink Writer
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

using Microsoft::WRL::ComPtr;

struct VideoEncoderConfig {
    uint32_t     width        = 1280;
    uint32_t     height       = 720;
    uint32_t     fps          = 30;
    uint32_t     bitrate      = 3000000; // 3 Mbps - good for 720p
    bool         useHW        = true;    // Try hardware encoder first
    bool         audioEnabled = true;
    std::wstring outputPath;
};

class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder() { Finalize(); }
    
    bool Init(const VideoEncoderConfig& cfg) {
        m_cfg = cfg;
        
        // Create Sink Writer (handles MP4 container muxing)
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 4);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, cfg.useHW ? TRUE : FALSE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE); // Don't throttle - we pace ourselves
        
        HRESULT hr = MFCreateSinkWriterFromURL(
            cfg.outputPath.c_str(),
            nullptr, attrs.Get(), &m_writer
        );
        if (FAILED(hr)) return false;
        
        // ── Video output type (what goes into the MP4) ─────────────────────
        ComPtr<IMFMediaType> videoOut;
        MFCreateMediaType(&videoOut);
        videoOut->SetGUID(MF_MT_MAJOR_TYPE,   MFMediaType_Video);
        videoOut->SetGUID(MF_MT_SUBTYPE,      MFVideoFormat_H264);
        videoOut->SetUINT32(MF_MT_AVG_BITRATE, cfg.bitrate);
        videoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(videoOut.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
        MFSetAttributeRatio(videoOut.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
        MFSetAttributeRatio(videoOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        
        // H.264 profile: Baseline for Celeron (fastest to encode/decode)
        // Use Main if you want slightly better compression and have a bit more CPU
        videoOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        
        hr = m_writer->AddStream(videoOut.Get(), &m_videoStream);
        if (FAILED(hr)) return false;
        
        // ── Video input type (what we feed in) ────────────────────────────
        ComPtr<IMFMediaType> videoIn;
        MFCreateMediaType(&videoIn);
        videoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoIn->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_RGB32); // BGRA from DXGI
        videoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(videoIn.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
        MFSetAttributeRatio(videoIn.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
        MFSetAttributeRatio(videoIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        
        hr = m_writer->SetInputMediaType(m_videoStream, videoIn.Get(), nullptr);
        if (FAILED(hr)) return false;
        
        // ── Configure encoder (low-latency, potato-optimized) ─────────────
        ConfigureEncoder();
        
        // ── Audio stream (if needed) ───────────────────────────────────────
        if (cfg.audioEnabled) {
            if (!AddAudioStream()) {
                // Audio fail is non-fatal
                m_audioStream = -1;
                m_cfg.audioEnabled = false;
            }
        }
        
        hr = m_writer->BeginWriting();
        if (FAILED(hr)) return false;
        
        m_initialized = true;
        m_startTime = -1;
        return true;
    }
    
    /**
     * EncodeFrame - feed a BGRA frame to the encoder
     * 
     * @param data          BGRA pixels (width * stride bytes)  
     * @param w, h, stride  Frame dimensions
     * @param timestamp     QPC timestamp in 100ns units
     */
    bool EncodeFrame(const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride,
                     int64_t timestamp) {
        if (!m_initialized) return false;
        
        // Initialize base time on first frame
        if (m_startTime < 0) m_startTime = timestamp;
        int64_t pts = timestamp - m_startTime;
        
        // Create MF sample
        ComPtr<IMFSample>      sample;
        ComPtr<IMFMediaBuffer> buffer;
        
        DWORD dataSize = h * stride;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(dataSize, &buffer);
        
        BYTE* buf = nullptr;
        DWORD maxLen, curLen;
        buffer->Lock(&buf, &maxLen, &curLen);
        
        // Copy frame (MF expects bottom-up for RGB32... or top-down depending on stride sign)
        // We provide top-down, which MF handles correctly with positive stride
        memcpy(buf, data, dataSize);
        
        buffer->Unlock();
        buffer->SetCurrentLength(dataSize);
        
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(pts);
        sample->SetSampleDuration(10000000LL / m_cfg.fps); // 100ns units
        
        HRESULT hr = m_writer->WriteSample(m_videoStream, sample.Get());
        return SUCCEEDED(hr);
    }
    
    /**
     * WriteAudioSample - called by AudioCapture to mux audio
     */
    bool WriteAudioSample(IMFSample* sample) {
        if (!m_initialized || m_audioStream < 0) return false;
        return SUCCEEDED(m_writer->WriteSample((DWORD)m_audioStream, sample));
    }
    
    void Finalize() {
        if (m_writer && m_initialized) {
            m_writer->Finalize();
            m_initialized = false;
        }
        m_writer.Reset();
    }
    
    bool IsUsingHWEncoder() const { return m_usingHW; }
    
private:
    void ConfigureEncoder() {
        // Get the encoder transform to configure it
        ComPtr<ICodecAPI> codec;
        if (FAILED(m_writer->GetServiceForStream(
                m_videoStream, GUID_NULL, IID_PPV_ARGS(&codec)))) return;
        
        // Low-latency mode: minimize buffering (important for real-time capture)
        VARIANT vLowLatency;
        vLowLatency.vt = VT_BOOL;
        vLowLatency.boolVal = VARIANT_TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &vLowLatency);
        
        // Complexity: lowest (1) for Celeron
        // Range typically 0-100, where 0=fastest/worst, 100=slowest/best
        VARIANT vComplexity;
        vComplexity.vt = VT_UI4;
        vComplexity.uintVal = 0; // FASTEST possible
        codec->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &vComplexity);
        
        // Rate control: CBR is most CPU-friendly (no lookahead)
        VARIANT vRC;
        vRC.vt = VT_UI4;
        vRC.uintVal = eAVEncCommonRateControlMode_CBR;
        codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &vRC);
        
        // Keyframe interval: 2 seconds (less frequent = less CPU)
        VARIANT vKeyframe;
        vKeyframe.vt = VT_UI4;
        vKeyframe.uintVal = m_cfg.fps * 2;
        codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &vKeyframe);
        
        // Check if we actually got a hardware encoder
        // We do this by checking the transform's attributes
        ComPtr<IMFTransform> transform;
        m_writer->GetServiceForStream(m_videoStream, GUID_NULL, IID_PPV_ARGS(&transform));
        if (transform) {
            ComPtr<IMFAttributes> tAttrs;
            transform->GetAttributes(&tAttrs);
            if (tAttrs) {
                UINT32 hwAccel = 0;
                tAttrs->GetUINT32(MF_SA_D3D_AWARE, &hwAccel);
                m_usingHW = (hwAccel != 0);
            }
        }
    }
    
    bool AddAudioStream() {
        // Output: AAC audio
        ComPtr<IMFMediaType> audioOut;
        MFCreateMediaType(&audioOut);
        audioOut->SetGUID(MF_MT_MAJOR_TYPE,   MFMediaType_Audio);
        audioOut->SetGUID(MF_MT_SUBTYPE,      MFAudioFormat_AAC);
        audioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     16);
        audioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,  44100);
        audioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        2);
        audioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128kbps AAC
        
        DWORD streamIdx;
        if (FAILED(m_writer->AddStream(audioOut.Get(), &streamIdx))) return false;
        
        // Input: PCM float (what WASAPI gives us)
        ComPtr<IMFMediaType> audioIn;
        MFCreateMediaType(&audioIn);
        audioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioIn->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_Float);
        audioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     32);
        audioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,  44100);
        audioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        2);
        
        if (FAILED(m_writer->SetInputMediaType(streamIdx, audioIn.Get(), nullptr))) return false;
        
        m_audioStream = (int)streamIdx;
        return true;
    }
    
    ComPtr<IMFSinkWriter> m_writer;
    VideoEncoderConfig    m_cfg;
    DWORD                 m_videoStream = 0;
    int                   m_audioStream = -1;
    int64_t               m_startTime   = -1;
    bool                  m_initialized = false;
    bool                  m_usingHW     = false;
};
