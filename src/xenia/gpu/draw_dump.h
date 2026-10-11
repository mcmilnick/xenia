/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_DRAW_DUMP_H_
#define XENIA_GPU_DRAW_DUMP_H_

#include <cstdint>

#include "xenia/gpu/xenos.h"

namespace xe {
class Memory;
namespace gpu {

class RegisterFile;
class Shader;

// Writes the raw inputs of every draw in one frame to disk, for asset
// extraction (models, textures) with external scripts.
//
// How it works:
// 1. Set the dump_draws_path cvar and press F4 (GPU > Trace Frame) in game.
// 2. At the next swap, a new folder frame_<N> is made under dump_draws_path.
// 3. Every draw until the swap after that writes one line to draws.jsonl,
//    describing the vertex fetch layout, index buffer, textures and shader
//    constants. Raw buffers go into blobs/, named by content hash so that
//    buffers shared between draws are written once.
// 4. Shader disassembly goes into shaders/.
namespace draw_dump {

// True when dump_draws_path is set, so F4 should request a draw dump.
bool IsEnabled();
// Requests a dump of the next full frame.
void Request();
// Called at each swap, starts or stops the frame being dumped.
void OnSwap(uint32_t frame_counter);
// True while a frame is being dumped.
bool IsActive();

struct IndexInfo {
  bool indexed = false;
  xenos::IndexFormat format = xenos::IndexFormat::kInt16;
  xenos::Endian endianness = xenos::Endian::kNone;
  uint32_t guest_base = 0;
  uint32_t length_bytes = 0;
};

// Dumps one draw. Call only after both shaders have been analyzed.
void DumpDraw(const RegisterFile& regs, const Memory& memory,
              const Shader& vertex_shader, const Shader* pixel_shader,
              xenos::PrimitiveType primitive_type, uint32_t index_count,
              const IndexInfo& index_info);

}  // namespace draw_dump
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_DRAW_DUMP_H_
