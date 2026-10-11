# Draw Dumps

A draw dump saves the raw inputs of every draw in one frame to disk: vertex
buffers, index buffers, the vertex layout the shader reads, textures, shader
disassembly and shader constants. External scripts can then rebuild meshes and
textures from it, for asset extraction, modding or offline inspection.

Xenia only copies memory and records what it is. All decoding (vertex formats,
texture untiling, DXT) happens in the script that reads the dump. This keeps the
emulator side small and read-only: dumping never changes how a game runs.

It is not tied to any game. Anything a game draws through the GPU is dumped the
same way.

## Usage

1. Start Xenia with an output folder:

   ```
   xenia_canary.exe --dump_draws_path="C:\dumps" "path\to\game.iso"
   ```

   It can also be set as `dump_draws_path` under `[GPU.Debug]` in the config
   file.

2. In game, press **F4** (or **GPU > Trace Frame**). While `dump_draws_path` is
   set, F4 makes a draw dump instead of a GPU trace.

3. The log shows the request and the result:

   ```
   Draw dump: requested, starting at the next frame
   Draw dump: dumping frame to C:\dumps\frame_12881
   Draw dump: wrote 268 draws to C:\dumps\frame_12881
   ```

The dump covers one whole frame, from the swap after F4 to the next swap. Press
F4 again for another frame. Each frame goes into its own folder.

Limitation: only the D3D12 backend calls the dumper for now. With
`gpu = "vulkan"`, F4 requests a dump but no draws are written.

## Output

```
frame_<N>/            N is the command processor's frame counter
  draws.jsonl         one JSON object per draw, in draw order
  blobs/<hash>.bin    raw guest memory, named by XXH3-64 of its contents
  shaders/vs_<hash>.txt, ps_<hash>.txt
                      ucode disassembly, named by the shader's ucode hash
```

A blob is written once per frame, however many draws use it. Draws refer to
blobs by hash, so a mesh drawn in several passes (depth, shadow, color) points
at the same vertex data each time.

Blobs are raw guest memory, unchanged: still in the Xbox 360's byte order and,
for textures, still tiled. The fields below say how to read them.

### draws.jsonl fields

Each line is one draw.

| Field | Meaning |
|---|---|
| `draw` | Index of the draw within the frame. |
| `prim` | `xenos::PrimitiveType` (4 = triangle list, 5 = fan, 6 = strip, 8 = rectangle list, 13 = quad list). |
| `index_count` | Index count, or vertex count for a non-indexed draw. |
| `indexed` | False for auto-indexed draws, which use vertices `0..index_count-1`. |
| `index_format` | 0 = 16-bit, 1 = 32-bit. |
| `index_endian` | `xenos::Endian` swap for the index data. |
| `index_base`, `index_blob` | Guest address and blob of the index buffer. |
| `indx_offset` | `VGT_INDX_OFFSET`, added to every index. |
| `min_vtx`, `max_vtx` | `VGT_MIN_VTX_INDX` and `VGT_MAX_VTX_INDX`. |
| `vs`, `ps` | Shader hashes, matching the files in `shaders/`. `ps` is empty when there is no pixel shader. |
| `float_consts_blob` | All 512 float constant registers, as 512 × 4 little-endian floats. |
| `vs_const`, `ps_const` | `SQ_VS_CONST` and `SQ_PS_CONST`, which give each shader's base in those registers. |
| `vertex_bindings` | One entry per vertex fetch constant the vertex shader uses, see below. |
| `textures` | One entry per texture fetch constant the pixel shader uses, see below. |

`vertex_bindings[]`:

| Field | Meaning |
|---|---|
| `fetch_constant` | Vertex fetch constant index (0-95). |
| `stride_words` | Size of one vertex in 32-bit words. |
| `address`, `size_bytes`, `blob` | The vertex buffer. |
| `endian` | `xenos::Endian` swap for every 32-bit word of the buffer. |
| `attributes[]` | What the shader fetches from each vertex. |
| `attributes[].format` | `xenos::VertexFormat`, for example 57 = `32_32_32_FLOAT`, 32 = `16_16_16_16_FLOAT`, 31 = `16_16_FLOAT`, 7 = `2_10_10_10`, 6 = `8_8_8_8`. |
| `attributes[].offset_words` | Offset of the attribute within a vertex. |
| `attributes[].signed`, `integer` | Signed or unsigned, and integer or normalized. |
| `attributes[].exp_adjust` | Power of two the value is scaled by. |
| `attributes[].signed_rf_mode` | `xenos::SignedRepeatingFractionMode`. |
| `attributes[].dest_target`, `dest_index` | Where the shader stores it (`InstructionStorageTarget`, usually a temporary register). |

`textures[]`:

| Field | Meaning |
|---|---|
| `fetch_constant` | Texture fetch constant index (0-31). |
| `format` | `xenos::TextureFormat`, for example 18 = DXT1, 20 = DXT4_5, 49 = DXN, 6 = `8_8_8_8`. |
| `endian` | `xenos::Endian` swap for the texture data. |
| `dimension` | `xenos::DataDimension` (0 = 1D, 1 = 2D or stacked, 2 = 3D, 3 = cube). |
| `width`, `height`, `depth` | Size in texels, with the fetch constant's minus-one already undone. |
| `pitch_pixels` | Row pitch in texels. |
| `tiled` | True if the data uses the Xenos tiled layout. |
| `packed_mips`, `mip_min_level`, `mip_max_level`, `mip_address` | Mip information. Only the base level is dumped. |
| `swizzle` | Destination swizzle, 3 bits each for x, y, z, w: 0-3 pick a source component, 4 = 0, 5 = 1. |
| `base_address`, `size_bytes`, `blob` | The base level, rounded up to whole 32×32-block tiles. |
| `dwords` | The raw fetch constant, for anything not broken out above. |

### Reading the data

Each step below matches what Xenia's own shader translators and texture loaders
do. Each one points to the code to compare against.

- **Endian swaps.** Read a blob as little-endian 32-bit words, then apply the
  `endian` swap: 1 = swap bytes in each 16 bits, 2 = reverse all 4 bytes, 3 =
  swap the two 16-bit halves.
- **Packed vertex components.** After the swap, 8-bit and 10/11-bit formats
  keep x in the low bits (`8_8_8_8`: x = bits 0-7). 16-bit formats keep x in
  the **high** half: `16_16` is `x << 16 | y`, and `16_16_16_16` puts z, w in
  the second word the same way. See
  [dxbc_shader_translator_fetch.cc](../src/xenia/gpu/dxbc_shader_translator_fetch.cc).
- **Vertex addressing.** Vertex `i` of an attribute starts at word
  `(index + indx_offset) * stride_words + offset_words`.
- **Strips.** Triangle strips flip winding on every other triangle. An index of
  0xFFFF (16-bit) or 0xFFFFFFFF (32-bit) restarts the strip.
- **Tiled textures.** Block `(x, y)` is at byte
  `texture_address::Tiled2D(x, y, align(pitch_in_blocks, 32), log2(bytes per block))`.
  See [texture_address.h](../src/xenia/gpu/texture_address.h). Linear
  textures have rows aligned to 256 bytes.
- **Skinning.** Skinned meshes are stored in bind pose, and the bone matrices
  live in the float constants. Positions read straight from the vertex buffer
  give the mesh in its rest pose (usually a T-pose), which is what most
  extraction wants.

## Example: extracting a character model

This example is from
[mcmilnick/halo_bomberman](https://github.com/mcmilnick/halo_bomberman), a
personal mod project to put the Bomberman Act:Zero character into Halo. Its
script
[scripts/xenia_draws_to_obj.py](https://github.com/mcmilnick/halo_bomberman/blob/main/scripts/xenia_draws_to_obj.py)
reads a draw dump and writes OBJ meshes and PNG/DDS textures.

1. Run the game with `--dump_draws_path`, stand the character in view, and
   press F4.
2. Convert the whole frame and read the summary:

   ```
   python xenia_draws_to_obj.py C:\dumps\frame_12881 C:\dumps\frame_12881_out
   ```

   `summary.txt` has one line per draw: vertex and triangle counts, model-space
   size, shader hashes, attribute layout and textures. Skinned meshes stand out
   because they fetch an `8_8_8_8` integer attribute (bone indices):

   ```
   draw  118: tristrip    182v    152t | size 7.90 3.25 3.04 | vs 5C892959 ...
   draw  119: tristrip   2482v   2838t | size 11.80 11.94 2.35 | vs 22323923 ...
   draw  120: tristrip  11343v   8686t | size 16.50 13.00 3.72 | vs 615ACE2F ...
   ```

   The same three meshes appear again in other passes of this frame (draws
   25-27 and 214-216), pointing at the same vertex blobs. Draws 25-27 have no
   textures bound, and draws 118-120 have them, so 118-120 are the ones to
   export.

3. Export just the character as one OBJ:

   ```
   python xenia_draws_to_obj.py C:\dumps\frame_12881 C:\dumps\character ^
       --draws 118-120 --merge character
   ```

   The result is the character in its T-pose, with UVs, normals and its
   textures as PNG and DDS, ready to import into Blender and
   re-rig to another game's skeleton.
