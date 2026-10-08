#include "wic_png.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace ft {

namespace {

struct Failure {
  const char* step = nullptr;  // null = success
  HRESULT hr = S_OK;
  bool fileCreated = false;
};

Failure encodePng(const std::filesystem::path& path, unsigned width, unsigned height, const std::uint8_t* bgra) {
  Failure f;
  auto failed = [&f](HRESULT hr, const char* step) {
    if (SUCCEEDED(hr)) return false;
    f.step = step;
    f.hr = hr;
    return true;
  };
  ComPtr<IWICImagingFactory> factory;
  if (failed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
             "creating the WIC factory"))
    return f;
  ComPtr<IWICStream> stream;
  if (failed(factory->CreateStream(&stream), "CreateStream")) return f;
  if (failed(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "opening the file")) return f;
  f.fileCreated = true;
  ComPtr<IWICBitmapEncoder> encoder;
  if (failed(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder")) return f;
  if (failed(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "encoder Initialize")) return f;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (failed(encoder->CreateNewFrame(&frame, nullptr), "CreateNewFrame")) return f;
  if (failed(frame->Initialize(nullptr), "frame Initialize")) return f;
  if (failed(frame->SetSize(width, height), "SetSize")) return f;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (failed(frame->SetPixelFormat(&format), "SetPixelFormat")) return f;
  if (format != GUID_WICPixelFormat32bppBGRA && failed(E_FAIL, "PNG encoder without 32bpp BGRA support")) return f;
  const UINT stride = width * 4;
  if (failed(frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgra)), "WritePixels")) return f;
  if (failed(frame->Commit(), "frame Commit")) return f;
  failed(encoder->Commit(), "encoder Commit");
  return f;
}

}  // namespace

bool writePngBgra(const std::filesystem::path& path, unsigned width, unsigned height, const std::uint8_t* bgra,
                  std::string* error) {
  const Failure f = encodePng(path, width, height, bgra);  // COM objects (and the file) closed on return
  if (!f.step) return true;
  if (f.fileCreated) {  // do not leave a truncated PNG behind
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  if (error) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s failed (HRESULT 0x%08lX)", f.step, static_cast<unsigned long>(f.hr));
    *error = buf;
  }
  return false;
}

}  // namespace ft
