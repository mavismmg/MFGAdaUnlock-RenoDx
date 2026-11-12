// SPDX-License-Identifier: MIT
// 1.10.0 (plan 3.2): image regression. Compares two images by SSIM on luma
// (8x8 windows, the usual constants for 8-bit images) and reports the mean and
// the worst window, so a change confined to one widget is not averaged away.
//
//   image_ssim reference.png candidate.png [min-mean] [min-window]
//
// Exit 0 when both scores reach their minimums (defaults 0.98 and 0.80), 1
// when not, 2 when the images cannot be read or differ in size. Used for the
// panel previews in every language and for game captures (docs/compat).
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {
struct Image {
  UINT width = 0, height = 0;
  std::vector<float> luma;  // 0..255
};
void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed");
}
Image Load(IWICImagingFactory* factory, const wchar_t* path) {
  ComPtr<IWICBitmapDecoder> decoder;
  Check(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "open");
  ComPtr<IWICBitmapFrameDecode> frame;
  Check(decoder->GetFrame(0, &frame), "frame");
  ComPtr<IWICFormatConverter> converter;
  Check(factory->CreateFormatConverter(&converter), "converter");
  Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom),
        "convert");
  Image image;
  Check(converter->GetSize(&image.width, &image.height), "size");
  std::vector<BYTE> pixels(size_t(image.width) * image.height * 4);
  Check(converter->CopyPixels(nullptr, image.width * 4, UINT(pixels.size()), pixels.data()), "pixels");
  image.luma.resize(size_t(image.width) * image.height);
  for (size_t i = 0; i < image.luma.size(); ++i)
    image.luma[i] = 0.114f * pixels[i * 4] + 0.587f * pixels[i * 4 + 1] + 0.299f * pixels[i * 4 + 2];
  return image;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::fputs("usage: image_ssim reference.png candidate.png [min-mean] [min-window]\n", stderr);
    return 2;
  }
  const double min_mean = argc > 3 ? _wtof(argv[3]) : 0.98;
  const double min_window = argc > 4 ? _wtof(argv[4]) : 0.80;
  try {
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC");
    const Image a = Load(factory.Get(), argv[1]), b = Load(factory.Get(), argv[2]);
    if (a.width != b.width || a.height != b.height) {
      std::printf("size differs: %ux%u vs %ux%u\n", a.width, a.height, b.width, b.height);
      return 2;
    }
    constexpr int kWindow = 8;
    constexpr double c1 = (0.01 * 255) * (0.01 * 255), c2 = (0.03 * 255) * (0.03 * 255);
    double sum = 0.0, worst = 1.0;
    UINT worst_x = 0, worst_y = 0;
    size_t windows = 0;
    for (UINT y = 0; y + kWindow <= a.height; y += kWindow) {
      for (UINT x = 0; x + kWindow <= a.width; x += kWindow) {
        double ma = 0, mb = 0;
        for (int j = 0; j < kWindow; ++j)
          for (int i = 0; i < kWindow; ++i) {
            const size_t k = size_t(y + j) * a.width + x + i;
            ma += a.luma[k];
            mb += b.luma[k];
          }
        ma /= kWindow * kWindow;
        mb /= kWindow * kWindow;
        double va = 0, vb = 0, cov = 0;
        for (int j = 0; j < kWindow; ++j)
          for (int i = 0; i < kWindow; ++i) {
            const size_t k = size_t(y + j) * a.width + x + i;
            const double da = a.luma[k] - ma, db = b.luma[k] - mb;
            va += da * da;
            vb += db * db;
            cov += da * db;
          }
        const double n = kWindow * kWindow - 1;
        va /= n;
        vb /= n;
        cov /= n;
        const double ssim = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
        sum += ssim;
        ++windows;
        if (ssim < worst) {
          worst = ssim;
          worst_x = x;
          worst_y = y;
        }
      }
    }
    const double mean = windows ? sum / windows : 1.0;
    std::printf("SSIM mean %.5f (min %.3f); worst window %.5f at %u,%u (min %.3f)\n", mean, min_mean, worst, worst_x,
                worst_y, min_window);
    return mean >= min_mean && worst >= min_window ? 0 : 1;
  } catch (const std::exception& error) {
    std::printf("cannot compare: %s\n", error.what());
    return 2;
  }
}
