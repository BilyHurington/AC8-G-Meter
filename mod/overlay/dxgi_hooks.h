#pragma once
// IDXGISwapChain vtable hooks (Present, Present1, SetColorSpace1) and discovery of what DXGI
// does not expose: the command queue a swap chain presents on and its colour space.
//
// Every flip-model swap chain dxgi.dll creates shares one vtable, so patching it through a
// throwaway swap chain catches the game's swap chain too, whenever it was created. Because
// this is the system dxgi.dll object, the hook runs below Streamline / DLSS frame generation
// and sees generated frames as well.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <string>

namespace ft::mod::overlay {

// Called from inside Present / Present1 (not for DXGI_PRESENT_TEST / DO_NOT_SEQUENCE, not
// re-entrantly), before the original function runs. Must not throw.
using PresentCallback = void (*)(IDXGISwapChain* swapChain);

// Creates the dummy device + swap chain, records where a swap chain stores its queue and
// colour space, then patches the vtable. Idempotent. Logs every step.
// True once Present is hooked; Present1 / SetColorSpace1 are optional (a failure there is
// logged and the overlay works without them). False if Present could not be hooked: then
// nothing is patched (discovery or Present is attempted before any other slot).
bool installDxgiHooks(PresentCallback onPresent);

// The DIRECT queue `swapChain` presents on, validated against the queue vtable, `device` and
// the queue type; null if not found. Not AddRef'd: valid while the swap chain is.
ID3D12CommandQueue* findPresentQueue(IDXGISwapChain* swapChain, ID3D12Device* device);

// The swap chain's colour space: the last value our SetColorSpace1 hook saw (stored as private
// data on the swap chain itself, so it dies with it), else the value read from the swap chain
// object at the offsets found on the dummy (covers SetColorSpace1 calls made before the hooks
// were installed). False if unknown.
bool swapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE* out);

// "module.dll+0x1234" for diagnostics. Does not throw (returns "?" on failure).
std::string describeAddress(const void* address) noexcept;

}  // namespace ft::mod::overlay
