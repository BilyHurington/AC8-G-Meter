// macOS standalone prototype host: SDL3 window + Metal renderer + Dear ImGui.
// Stands in for the game: the UE4SS mod will host the same ft_ui code from a DX11/DX12
// Present hook. Based on imgui/examples/example_sdl3_metal.
//
// Usage: ac8_gmeter_proto [--settings <file.json>] [--scenario <name>] [--size WxH]
//                       [--menu] [--edit] [--history] [--no-debug] [--end-mission-at <s>] [--velocity-noise <m/s>]
//                       [--missions <dir>]
//                       [--screenshot <file.png> [--frames N]]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_sdl3.h>

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "proto_app.h"
#include "proto_cli.h"

namespace fs = std::filesystem;

namespace {

// Writes BGRA8 pixels as an opaque PNG via ImageIO.
bool writePng(const std::string& path, const void* bgra, int w, int h, size_t bytesPerRow) {
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(const_cast<void*>(bgra), static_cast<size_t>(w), static_cast<size_t>(h), 8,
                                           bytesPerRow, cs,
                                           static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst) |
                                               static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little));
  CGColorSpaceRelease(cs);
  if (!ctx) return false;
  CGImageRef image = CGBitmapContextCreateImage(ctx);
  CGContextRelease(ctx);
  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  CGImageDestinationRef dest = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
  bool ok = false;
  if (dest) {
    CGImageDestinationAddImage(dest, image, nullptr);
    ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
  }
  CGImageRelease(image);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  ft::ProtoCommandLine args;
  if (!ft::parseProtoCommandLine(std::vector<std::string>(argv + 1, argv + argc), args)) {
    std::fprintf(stderr, "usage: %s %s", argv[0], ft::protoUsage());
    return 2;
  }
  const bool screenshotMode = !args.screenshotPath.empty();
  const std::string screenshotPath = ft::pathToUtf8(args.screenshotPath);

  // Paths: assets/replays next to the executable (copied post-build), else the source tree.
  const fs::path base = SDL_GetBasePath() ? ft::pathFromUtf8(SDL_GetBasePath()) : fs::current_path();
  const char* home = std::getenv("HOME");
  ft::resolveProtoPaths(args, base, FT_SOURCE_DIR, home ? fs::path(home) / "Desktop" : fs::path());

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  // Window size is in points; Metal renders at the full Retina pixel density.
  const SDL_WindowFlags flags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  SDL_Window* window = SDL_CreateWindow("AC8 G-Meter \xE2\x80\x94 Prototype", args.width, args.height, flags);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_ShowWindow(window);

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    std::fprintf(stderr, "no Metal device\n");
    return 1;
  }
  SDL_MetalView view = SDL_Metal_CreateView(window);
  CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
  layer.device = device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = screenshotMode ? NO : YES;  // read-back needs a blittable drawable
  id<MTLCommandQueue> queue = [device newCommandQueue];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the overlay must never write imgui.ini next to the game
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  {
    ft::ProtoApp app(args.proto);
    app.initImGui();
    ImGui_ImplMetal_Init(device);
    ImGui_ImplSDL3_InitForMetal(window);

    bool done = false;
    int frame = 0;
    Uint64 prevTicks = SDL_GetTicksNS();
    while (!done) {
      @autoreleasepool {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
          ImGui_ImplSDL3_ProcessEvent(&event);
          if (event.type == SDL_EVENT_QUIT) done = true;
          if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
            done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
          SDL_Delay(10);
          continue;
        }
        if (auto size = app.takeResizeRequest()) SDL_SetWindowSize(window, size->first, size->second);

        // Screenshot mode runs on a fixed 60 Hz clock so captures are reproducible.
        const Uint64 ticks = SDL_GetTicksNS();
        const double realDt = screenshotMode ? 1.0 / 60.0 : std::min(static_cast<double>(ticks - prevTicks) * 1e-9, 0.25);
        const double now = screenshotMode ? frame / 60.0 : static_cast<double>(ticks) * 1e-9;
        prevTicks = ticks;

        // "Game thread": source -> pipeline -> LatestValue.
        app.update(realDt);

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        layer.drawableSize = CGSizeMake(w, h);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) continue;

        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];

        // "Render thread": fake game scene, telemetry overlay/menus, debug panel.
        ImGui_ImplMetal_NewFrame(pass);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.draw(io.DisplaySize.x, io.DisplaySize.y, now);
        ImGui::Render();
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, enc);
        [enc endEncoding];

        const bool capture = screenshotMode && frame + 1 >= args.frames;
        id<MTLBuffer> readback = nil;
        const NSUInteger bytesPerRow = static_cast<NSUInteger>(w) * 4;
        if (capture) {
          readback = [device newBufferWithLength:bytesPerRow * static_cast<NSUInteger>(h)
                                         options:MTLResourceStorageModeShared];
          id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
          [blit copyFromTexture:drawable.texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(static_cast<NSUInteger>(w), static_cast<NSUInteger>(h), 1)
                            toBuffer:readback
                   destinationOffset:0
              destinationBytesPerRow:bytesPerRow
            destinationBytesPerImage:bytesPerRow * static_cast<NSUInteger>(h)];
          [blit endEncoding];
        }
        [cmd presentDrawable:drawable];
        [cmd commit];
        if (capture) {
          [cmd waitUntilCompleted];
          const bool ok = writePng(screenshotPath, readback.contents, w, h, bytesPerRow);
          std::printf("%s %s (%dx%d px, %.0fx%.0f pt)\n", ok ? "wrote" : "FAILED to write",
                      screenshotPath.c_str(), w, h, static_cast<double>(io.DisplaySize.x),
                      static_cast<double>(io.DisplaySize.y));
          done = true;
        }
        ++frame;
      }
    }
    app.shutdown();
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplSDL3_Shutdown();
  }
  ImGui::DestroyContext();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
