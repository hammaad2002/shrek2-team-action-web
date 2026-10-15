// Web build: render-command queue between game threads and the page's main thread.
#pragma once

#include <cstdint>
#include <vector>

namespace web {

bool gl_ready();
// Queue recorded commands for the main thread. `present` marks the end of a frame: the caller
// then waits until at most one frame is pending (pacing to the display).
void submit(std::vector<uint32_t>&& words, bool present);
// Queue and wait until the main thread has executed everything (readbacks).
void submit_and_wait(std::vector<uint32_t>&& words);

}  // namespace web
