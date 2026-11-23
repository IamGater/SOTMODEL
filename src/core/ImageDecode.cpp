#include "core/ImageDecode.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

bool IsImageFileName(std::string_view name) {
    for (const char* e : {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff", ".ico"})
        if (IEndsWith(name, e)) return true;
    return false;
}

bool DecodeImage(const u8* data, size_t size, Bytes& rgba, int& width, int& height, std::string* error) {
    auto fail = [&](const char* what, HRESULT hr) {
        if (error) *error = Fmt("%s (0x%08X)", what, (unsigned)hr);
        return false;
    };
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // per thread; S_FALSE / RPC_E_CHANGED_MODE: already set up
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    bool ok = false;
    if (FAILED(hr)) ok = fail("WIC unavailable", hr);
    else {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        UINT w = 0, h = 0;
        if (FAILED(hr = factory->CreateStream(&stream))) ok = fail("stream", hr);
        else if (FAILED(hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)size))) ok = fail("stream", hr);
        else if (FAILED(hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) ok = fail("unsupported image", hr);
        else if (FAILED(hr = decoder->GetFrame(0, &frame))) ok = fail("no frame", hr);
        else if (FAILED(hr = factory->CreateFormatConverter(&conv))) ok = fail("converter", hr);
        else if (FAILED(hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) ok = fail("convert", hr);
        else if (FAILED(hr = conv->GetSize(&w, &h)) || !w || !h || (u64)w * h > (64ull << 20)) ok = fail("size", hr);
        else {
            rgba.resize((size_t)w * h * 4);
            if (FAILED(hr = conv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()))) ok = fail("pixels", hr);
            else {
                width = (int)w;
                height = (int)h;
                ok = true;
            }
        }
    }
    factory.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}
