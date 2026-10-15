// Recording side of the render command stream (game thread).
#pragma once

#include "cmd.h"
#include "d3d9.h"

namespace d3d {

void recorder_create_device(Device* dev);
cmd::Stream& rec();
uint32_t new_resource_id();
// Hand the recorded commands to the executor. Natively they run synchronously; on the web they
// are queued for the page's main thread.
void flush();
// Execute everything recorded so far and wait for completion (readbacks need this).
void flush_and_wait();

// Surface addressing used by render-target, stretch and readback commands.
struct SurfaceRef {
  uint32_t kind, id, face, level;  // kind: 0 none, 1 back buffer, 2 texture level, 3 renderbuffer
};
SurfaceRef surface_ref(Surface* s);

}  // namespace d3d
