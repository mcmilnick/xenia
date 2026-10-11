/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/draw_dump.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/memory.h"

DEFINE_path(dump_draws_path, "",
            "If set, F4 (GPU > Trace Frame) dumps the raw vertex, index, "
            "texture and shader constant data of every draw in the next frame "
            "to this folder instead of writing a GPU trace.",
            "GPU.Debug");

namespace xe {
namespace gpu {
namespace draw_dump {

namespace {

// Guest physical memory is 512 MB.
constexpr uint32_t kPhysicalMemorySize = 0x20000000;

std::atomic<bool> requested_{false};
bool active_ = false;
uint32_t draw_index_ = 0;
std::filesystem::path frame_path_;
FILE* draws_file_ = nullptr;
std::unordered_set<uint64_t> written_blobs_;
std::unordered_set<uint64_t> written_shaders_;

// Writes a guest memory range to blobs/<hash>.bin once per frame and returns
// the hash as hex, or an empty string if the range is invalid.
std::string WriteBlob(const Memory& memory, uint32_t address, uint32_t length) {
  if (!length || address >= kPhysicalMemorySize) {
    return {};
  }
  length = std::min(length, kPhysicalMemorySize - address);
  const uint8_t* data = memory.TranslatePhysical<const uint8_t*>(address);
  uint64_t hash = XXH3_64bits(data, length);
  std::string name = fmt::format("{:016X}", hash);
  if (written_blobs_.insert(hash).second) {
    auto path = frame_path_ / "blobs" / (name + ".bin");
    FILE* file = xe::filesystem::OpenFile(path, "wb");
    if (file) {
      fwrite(data, 1, length, file);
      fclose(file);
    }
  }
  return name;
}

// Writes the shader disassembly to shaders/<vs|ps>_<hash>.txt once per frame
// and returns the hash as hex.
std::string WriteShader(const Shader& shader) {
  std::string name = fmt::format("{:016X}", shader.ucode_data_hash());
  if (written_shaders_.insert(shader.ucode_data_hash()).second) {
    const char* prefix =
        shader.type() == xenos::ShaderType::kVertex ? "vs" : "ps";
    auto path = frame_path_ / "shaders" / fmt::format("{}_{}.txt", prefix, name);
    FILE* file = xe::filesystem::OpenFile(path, "wb");
    if (file) {
      const std::string& text = shader.ucode_disassembly();
      fwrite(text.data(), 1, text.size(), file);
      fclose(file);
    }
  }
  return name;
}

// Returns the byte size of the base mip level as stored in memory, rounded
// up to whole 32x32 block tiles so that tiled data is never cut short.
uint32_t GetBaseLevelStorageSize(const xenos::xe_gpu_texture_fetch_t& fetch,
                                 uint32_t width, uint32_t height,
                                 uint32_t depth) {
  const FormatInfo* format_info =
      FormatInfo::Get(static_cast<uint32_t>(fetch.format));
  uint32_t width_blocks = xe::align(width, format_info->block_width) /
                          format_info->block_width;
  uint32_t height_blocks = xe::align(height, format_info->block_height) /
                           format_info->block_height;
  uint32_t pitch_blocks = std::max(
      width_blocks, (uint32_t(fetch.pitch) << 5) / format_info->block_width);
  pitch_blocks = xe::align(pitch_blocks, uint32_t(32));
  height_blocks = xe::align(height_blocks, uint32_t(32));
  return pitch_blocks * height_blocks * depth *
         format_info->bytes_per_block();
}

}  // namespace

bool IsEnabled() { return !cvars::dump_draws_path.empty(); }

void Request() {
  XELOGI("Draw dump: requested, starting at the next frame");
  requested_.store(true, std::memory_order_relaxed);
}

bool IsActive() { return active_; }

void OnSwap(uint32_t frame_counter) {
  if (active_) {
    active_ = false;
    if (draws_file_) {
      fclose(draws_file_);
      draws_file_ = nullptr;
    }
    written_blobs_.clear();
    written_shaders_.clear();
    XELOGI("Draw dump: wrote {} draws to {}", draw_index_,
           xe::path_to_utf8(frame_path_));
  }
  if (requested_.exchange(false, std::memory_order_relaxed)) {
    frame_path_ =
        cvars::dump_draws_path / fmt::format("frame_{}", frame_counter);
    std::filesystem::create_directories(frame_path_ / "blobs");
    std::filesystem::create_directories(frame_path_ / "shaders");
    draws_file_ = xe::filesystem::OpenFile(frame_path_ / "draws.jsonl", "wb");
    if (!draws_file_) {
      XELOGE("Draw dump: could not create {}", xe::path_to_utf8(frame_path_));
      return;
    }
    draw_index_ = 0;
    active_ = true;
    XELOGI("Draw dump: dumping frame to {}", xe::path_to_utf8(frame_path_));
  }
}

void DumpDraw(const RegisterFile& regs, const Memory& memory,
              const Shader& vertex_shader, const Shader* pixel_shader,
              xenos::PrimitiveType primitive_type, uint32_t index_count,
              const IndexInfo& index_info) {
  if (!active_ || !draws_file_) {
    return;
  }
  std::string json;
  auto out = std::back_inserter(json);

  fmt::format_to(out,
                 "{{\"draw\":{},\"prim\":{},\"index_count\":{},"
                 "\"indexed\":{},\"index_format\":{},\"index_endian\":{},"
                 "\"index_base\":{},\"index_blob\":\"{}\","
                 "\"indx_offset\":{},\"min_vtx\":{},\"max_vtx\":{}",
                 draw_index_, uint32_t(primitive_type), index_count,
                 index_info.indexed, uint32_t(index_info.format),
                 uint32_t(index_info.endianness), index_info.guest_base,
                 index_info.indexed
                     ? WriteBlob(memory, index_info.guest_base,
                                 index_info.length_bytes)
                     : std::string(),
                 regs[XE_GPU_REG_VGT_INDX_OFFSET],
                 regs[XE_GPU_REG_VGT_MIN_VTX_INDX],
                 regs[XE_GPU_REG_VGT_MAX_VTX_INDX]);

  // Shaders and their float constants. Both shader types share the 512
  // float constant registers, SQ_VS_CONST and SQ_PS_CONST give each its base.
  fmt::format_to(out, ",\"vs\":\"{}\",\"ps\":\"{}\"",
                 WriteShader(vertex_shader),
                 pixel_shader ? WriteShader(*pixel_shader) : std::string());
  {
    const uint32_t* constants = &regs[XE_GPU_REG_SHADER_CONSTANT_000_X];
    uint32_t constants_size = 512 * 4 * sizeof(uint32_t);
    uint64_t hash = XXH3_64bits(constants, constants_size);
    std::string name = fmt::format("{:016X}", hash);
    if (written_blobs_.insert(hash).second) {
      auto path = frame_path_ / "blobs" / (name + ".bin");
      FILE* file = xe::filesystem::OpenFile(path, "wb");
      if (file) {
        fwrite(constants, 1, constants_size, file);
        fclose(file);
      }
    }
    fmt::format_to(out,
                   ",\"float_consts_blob\":\"{}\",\"vs_const\":{},"
                   "\"ps_const\":{}",
                   name, regs[XE_GPU_REG_SQ_VS_CONST],
                   regs[XE_GPU_REG_SQ_PS_CONST]);
  }

  // Vertex buffers and how the vertex shader reads them.
  fmt::format_to(out, ",\"vertex_bindings\":[");
  bool first_binding = true;
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    xenos::xe_gpu_vertex_fetch_t fetch =
        regs.GetVertexFetch(binding.fetch_constant);
    uint32_t address = fetch.address << 2;
    uint32_t size = fetch.size << 2;
    fmt::format_to(out,
                   "{}{{\"fetch_constant\":{},\"stride_words\":{},"
                   "\"address\":{},\"size_bytes\":{},\"endian\":{},"
                   "\"blob\":\"{}\",\"attributes\":[",
                   first_binding ? "" : ",", binding.fetch_constant,
                   binding.stride_words, address, size,
                   uint32_t(fetch.endian), WriteBlob(memory, address, size));
    first_binding = false;
    bool first_attribute = true;
    for (const Shader::VertexBinding::Attribute& attribute :
         binding.attributes) {
      const ParsedVertexFetchInstruction& instr = attribute.fetch_instr;
      fmt::format_to(
          out,
          "{}{{\"format\":{},\"offset_words\":{},\"signed\":{},"
          "\"integer\":{},\"exp_adjust\":{},\"signed_rf_mode\":{},"
          "\"dest_target\":{},\"dest_index\":{}}}",
          first_attribute ? "" : ",", uint32_t(instr.attributes.data_format),
          instr.attributes.offset, instr.attributes.is_signed,
          instr.attributes.is_integer, instr.attributes.exp_adjust,
          uint32_t(instr.attributes.signed_rf_mode),
          uint32_t(instr.result.storage_target), instr.result.storage_index);
      first_attribute = false;
    }
    fmt::format_to(out, "]}}");
  }
  fmt::format_to(out, "]");

  // Textures sampled by the pixel shader, base mip level only.
  fmt::format_to(out, ",\"textures\":[");
  if (pixel_shader) {
    bool first_texture = true;
    std::unordered_set<uint32_t> seen_fetch_constants;
    for (const Shader::TextureBinding& binding :
         pixel_shader->texture_bindings()) {
      if (!seen_fetch_constants.insert(binding.fetch_constant).second) {
        continue;
      }
      xenos::xe_gpu_texture_fetch_t fetch =
          regs.GetTextureFetch(binding.fetch_constant);
      uint32_t width = 1, height = 1, depth = 1;
      switch (fetch.dimension) {
        case xenos::DataDimension::k1D:
          width = fetch.size_1d.width + 1;
          break;
        case xenos::DataDimension::k2DOrStacked:
          width = fetch.size_2d.width + 1;
          height = fetch.size_2d.height + 1;
          if (fetch.stacked) {
            depth = fetch.size_2d.stack_depth + 1;
          }
          break;
        case xenos::DataDimension::k3D:
          width = fetch.size_3d.width + 1;
          height = fetch.size_3d.height + 1;
          depth = fetch.size_3d.depth + 1;
          break;
        case xenos::DataDimension::kCube:
          width = fetch.size_2d.width + 1;
          height = fetch.size_2d.height + 1;
          depth = 6;
          break;
      }
      uint32_t base_address = uint32_t(fetch.base_address) << 12;
      uint32_t mip_address = uint32_t(fetch.mip_address) << 12;
      // Some textures only have mips, with the largest at mip_address.
      uint32_t data_address = base_address ? base_address : mip_address;
      uint32_t size = GetBaseLevelStorageSize(fetch, width, height, depth);
      fmt::format_to(
          out,
          "{}{{\"fetch_constant\":{},\"format\":{},\"endian\":{},"
          "\"dimension\":{},\"width\":{},\"height\":{},\"depth\":{},"
          "\"pitch_pixels\":{},\"tiled\":{},\"packed_mips\":{},"
          "\"mip_min_level\":{},\"mip_max_level\":{},\"swizzle\":{},"
          "\"base_address\":{},\"mip_address\":{},\"size_bytes\":{},"
          "\"blob\":\"{}\",\"dwords\":[{},{},{},{},{},{}]}}",
          first_texture ? "" : ",", binding.fetch_constant,
          uint32_t(fetch.format), uint32_t(fetch.endianness),
          uint32_t(fetch.dimension), width, height, depth,
          uint32_t(fetch.pitch) << 5, bool(fetch.tiled),
          bool(fetch.packed_mips), uint32_t(fetch.mip_min_level),
          uint32_t(fetch.mip_max_level), uint32_t(fetch.swizzle),
          base_address, mip_address, size,
          WriteBlob(memory, data_address, size), fetch.dword_0, fetch.dword_1,
          fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      first_texture = false;
    }
  }
  fmt::format_to(out, "]}}\n");

  fwrite(json.data(), 1, json.size(), draws_file_);
  ++draw_index_;
}

}  // namespace draw_dump
}  // namespace gpu
}  // namespace xe
