// Executes the D3D9 render command stream with OpenGL ES 3.0 (native) / WebGL 2 (web).
#pragma once

#include <cstddef>
#include <cstdint>

namespace gles {
void execute(const uint32_t* words, size_t n);
}
