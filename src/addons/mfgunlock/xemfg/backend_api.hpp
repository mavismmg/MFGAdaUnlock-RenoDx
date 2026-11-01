#pragma once
#include <cstdint>
#include <string>
#include <windows.h>
namespace mfgunlock::xemfg {
struct Status {
  uint32_t size = sizeof(Status), version = 1;
  uint32_t hooks = 0, model = 0, contexts = 0, initialized = 0;
  uint32_t enabled = 0, generated = 0, presented = 0;
  int32_t result = 0;
  uint64_t tensor_launches = 0;
  char detail[256] = "XeMFG companion not initialized";
};
using GetStatus = bool (*)(Status*, uint32_t);
struct LogStatus { uint64_t pending{},dropped{},batches{},failed{}; bool started{}; };
LogStatus LogSnapshot() noexcept;
#ifdef MFGUNLOCK_NATIVE_XEMFG
bool PublishMultiFrameModel(HMODULE fg,HMODULE ll,HMODULE payload,std::string& error) noexcept;
// Overlay readers must not wait behind an SDK call that may join/present.
bool TryModelStatus(Status& out);
struct NativeRuntimePair { void* frameGeneration{}; void* latency{}; };
bool NativeRuntimeModules(NativeRuntimePair& out);
bool ModelStartupFailed() noexcept;
// 1.5.5: what startup found when it refused the Intel files, for the panel.
enum class RuntimeFileState : uint32_t { Unknown, Missing, Pinned, AcceptedOlder, Different, Unreadable };
struct RuntimeFileReport { RuntimeFileState state{}; char version[32]{}; };
// [0] libxess_fg.dll, [1] libxell.dll. False unless startup refused them.
bool RuntimeFilesRejected(RuntimeFileReport (&out)[2]) noexcept;
// Plan 5.2: game-owned XeFG contexts generate multiplier-1 frames (2x-6x).
void SetGameMultiplier(uint32_t multiplier) noexcept;
// True while a game-owned context initializes on this thread: the swapchain
// it creates belongs to the game's XeFG and is never wrapped by the bridge.
bool GameContextInitializing() noexcept;
// The game runs its own XeFG (a game-owned context initializing on any thread,
// or alive): it owns presentation, and the bridge adds no Intel output.
bool GameOwnsFrameGeneration() noexcept;
// 1.7 (plan J): back buffers for XeFG's native swap chain while the flip
// queue is on ([XeFG] XeFGNativeBuffers); 0 keeps the game's count.
uint32_t FlipQueueNativeBuffers() noexcept;
#endif
}
