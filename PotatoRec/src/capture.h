#pragma once
/**
 * capture.h - DXGI Desktop Duplication API
 * 
 * Why DXGI Duplication instead of BitBlt/GDI?
 * - BitBlt:  Pulls frame from GPU to CPU every frame = massive bandwidth waste
 * - DXGI:    Compositor already has the frame on GPU, we just READ the pointer
 *            When screen hasn't changed → AcquireNextFrame returns timeout
 *            → ZERO CPU usage on static screens. Perfect for Celeron.
 * 
 * This is literally what OBS, Shadowplay and Xbox Game Bar use internally.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <vector>
#include <cstdint>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct CapturedFrame {
    std::vector<uint8_t> data;    // BGRA pixels
    uint32_t width, height, stride;
    int32_t  desktopX, desktopY;  // Monitor offset in virtual desktop
    int64_t  timestamp_100ns;     // QPC-based timestamp
};

class DXGICapture {
public:
    DXGICapture() = default;
    ~DXGICapture() { Shutdown(); }
    
    bool Init(int monitorIndex = 0) {
        // Create D3D11 device (we use WARP fallback if no GPU - potato proof)
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL gotLevel;
        
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0, // No debug flag - less overhead
            featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &m_device, &gotLevel, &m_ctx
        );
        
        if (FAILED(hr)) {
            // Celeron with broken driver? Use WARP (software rasterizer)
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   0, featureLevels, ARRAYSIZE(featureLevels),
                                   D3D11_SDK_VERSION, &m_device, &gotLevel, &m_ctx);
            if (FAILED(hr)) return false;
            m_usingWARP = true;
        }
        
        // Get DXGI device → adapter → output
        ComPtr<IDXGIDevice>  dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        
        m_device.As(&dxgiDevice);
        dxgiDevice->GetAdapter(&adapter);
        
        // Enumerate outputs (monitors)
        ComPtr<IDXGIOutput> output;
        int idx = 0;
        while (adapter->EnumOutputs(idx, &output) != DXGI_ERROR_NOT_FOUND) {
            if (idx == monitorIndex) break;
            output.Reset();
            idx++;
        }
        if (!output) {
            // Fallback: first output
            adapter->EnumOutputs(0, &output);
        }
        if (!output) return false;
        
        // Get monitor info
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);
        m_width   = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
        m_height  = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        m_desktopX = desc.DesktopCoordinates.left;
        m_desktopY = desc.DesktopCoordinates.top;
        
        // Get IDXGIOutput1 for duplication
        ComPtr<IDXGIOutput1> output1;
        output.As(&output1);
        if (!output1) return false;
        
        hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
        if (FAILED(hr)) {
            // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE = another app has exclusive fullscreen
            // This is fine, we'll retry when the game goes to windowed/borderless
            m_lastError = hr;
            return false;
        }
        
        // Create staging texture (CPU-readable) - reused every frame, no alloc
        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width              = m_width;
        texDesc.Height             = m_height;
        texDesc.MipLevels          = 1;
        texDesc.ArraySize          = 1;
        texDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM; // BGRA, native for GDI compat
        texDesc.SampleDesc.Count   = 1;
        texDesc.Usage              = D3D11_USAGE_STAGING;
        texDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
        
        hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_stagingTex);
        return SUCCEEDED(hr);
    }
    
    /**
     * AcquireFrame - gets the latest desktop frame
     * 
     * @param out      Output frame (filled on success)
     * @param timeoutMs Timeout in ms. Use 0 for non-blocking, 8 for ~1 frame at 120fps
     * @return true if new frame available
     * 
     * CRITICAL: Call ReleaseFrame() after you're done with the data!
     */
    bool AcquireFrame(CapturedFrame& out, UINT timeoutMs = 8) {
        if (!m_duplication) return false;
        
        // Release previous frame first (must do this before acquiring next)
        if (m_frameAcquired) {
            m_duplication->ReleaseFrame();
            m_frameAcquired = false;
        }
        
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;
        
        HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &frameInfo, &resource);
        
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return false; // No change on screen - zero work needed!
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Mode change or app went fullscreen exclusive - reinit needed
            Reinit();
            return false;
        }
        if (FAILED(hr)) return false;
        
        m_frameAcquired = true;
        m_lastTimestamp = frameInfo.LastPresentTime.QuadPart;
        
        // Copy from GPU texture to staging (CPU-readable) texture
        ComPtr<ID3D11Texture2D> tex;
        resource.As(&tex);
        m_ctx->CopyResource(m_stagingTex.Get(), tex.Get());
        
        // Map staging texture to CPU memory
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = m_ctx->Map(m_stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;
        
        m_texMapped = true;
        
        // Fill output (copy pixel data)
        out.width      = m_width;
        out.height     = m_height;
        out.stride     = mapped.RowPitch;
        out.desktopX   = m_desktopX;
        out.desktopY   = m_desktopY;
        out.timestamp_100ns = m_lastTimestamp;
        
        out.data.resize(mapped.RowPitch * m_height);
        memcpy(out.data.data(), mapped.pData, out.data.size());
        
        m_ctx->Unmap(m_stagingTex.Get(), 0);
        m_texMapped = false;
        
        return true;
    }
    
    void ReleaseFrame() {
        if (m_frameAcquired && m_duplication) {
            m_duplication->ReleaseFrame();
            m_frameAcquired = false;
        }
    }
    
    uint32_t GetWidth()  const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    bool IsUsingWARP()   const { return m_usingWARP; }
    
private:
    void Shutdown() {
        ReleaseFrame();
        m_stagingTex.Reset();
        m_duplication.Reset();
        m_ctx.Reset();
        m_device.Reset();
    }
    
    void Reinit() {
        // After mode change: need to re-create duplication interface
        m_duplication.Reset();
        // Simple retry - will be attempted on next AcquireFrame call
        // Full reinit would need to re-enumerate outputs
    }
    
    ComPtr<ID3D11Device>            m_device;
    ComPtr<ID3D11DeviceContext>     m_ctx;
    ComPtr<IDXGIOutputDuplication>  m_duplication;
    ComPtr<ID3D11Texture2D>         m_stagingTex;
    
    uint32_t m_width     = 0;
    uint32_t m_height    = 0;
    int32_t  m_desktopX  = 0;
    int32_t  m_desktopY  = 0;
    int64_t  m_lastTimestamp = 0;
    HRESULT  m_lastError = S_OK;
    bool     m_frameAcquired = false;
    bool     m_texMapped     = false;
    bool     m_usingWARP     = false;
};
