/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XE_NO_MSL
// CTX1 -> EAC RG11 (VK_FORMAT_EAC_R11G11_UNORM_BLOCK) transcoding for all
// mobile GPUs with ETC2/EAC (Adreno, Mali, Immortalis, Samsung Xclipse): 8 bits
// per texel (50% of R8G8_UNORM) with 11-bit interpolated precision instead of
// 8-bit integer truncation.
//
// Guest CTX1 block (8 bytes after XeEndianSwap32):
// - Dword 0: 0xR1G1R0G0 (r0 in bits 8:15, g0 in bits 0:7, r1 in bits 24:31,
//   g1 in bits 16:23).
// - Dword 1: sixteen 2-bit selector codes in raster order (00=3:0, 01=0:3,
//   10=2:1, 11=1:2).

#include "pixel_formats.xesli"
#include "texture_load.xesli"

static_const_xe uint kXeEACModifiers[16] = {
    0xE852F963u, 0xC962DA73u, 0xC741D852u, 0xC531D642u, 0xB752C863u,
    0xA862B973u, 0xA763B874u, 0xA742B853u, 0x9751A862u, 0x9741A852u,
    0x9731A842u, 0x9641A752u, 0x9632A743u, 0x9210A321u, 0x87539864u,
    0x86429753u
};

int XeEACModifier(uint table_word, uint index) {
  int magnitude = int((table_word >> (index << 2u)) & 15u);
  return index < 4u ? -magnitude : magnitude;
}

uint2_xe XeCTX1ChannelToEACR11(int c0, int c1, uint weights_high, uint counts) {
  int palette0 = (c0 * 2047 + 127) / 255;
  int palette1 = ((c0 * 2 + c1) * 2047 + 382) / 765;
  int palette2 = ((c0 + c1 * 2) * 2047 + 382) / 765;
  int palette3 = (c1 * 2047 + 127) / 255;
  int4_xe palette = int4_xe(palette0, palette1, palette2, palette3);
  int lo = 2047;
  int hi = 0;
  uint w;
  unroll_xe for (w = 0u; w < 4u; ++w) {
    if (((counts >> (w << 3u)) & 0xFFu) != 0u) {
      lo = min(lo, palette[w]);
      hi = max(hi, palette[w]);
    }
  }
  int best_error = 0x7FFFFFFF;
  uint best_header = 0u;
  uint best_selection = 0u;
  uint table;
  dont_unroll for (table = 0u; table < 16u; ++table) {
    uint table_word = kXeEACModifiers[table];
    int modifier_min = XeEACModifier(table_word, 3u);
    int modifier_max = XeEACModifier(table_word, 7u);
    int span = modifier_max - modifier_min;
    int multiplier = min((hi - lo + span * 4) / (span * 8), 15);
    int scale = multiplier != 0 ? multiplier * 8 : 1;
    int base =
        min(max(lo + hi - (modifier_min + modifier_max) * scale, 0) / 16, 255);
    int error = 0;
    uint selection = 0u;
    unroll_xe for (w = 0u; w < 4u; ++w) {
      int count = int((counts >> (w << 3u)) & 0xFFu);
      int target = palette[w];
      int entry_best_error = 0x7FFFFFFF;
      uint entry_best_index = 0u;
      uint index;
      unroll_xe for (index = 0u; index < 8u; ++index) {
        int difference =
            clamp(base * 8 + 4 + XeEACModifier(table_word, index) * scale, 0,
                  2047) -
            target;
        int entry_error = difference * difference;
        if (entry_error < entry_best_error) {
          entry_best_error = entry_error;
          entry_best_index = index;
        }
      }
      error += entry_best_error * count;
      selection |= entry_best_index << (w * 3u);
    }
    if (error < best_error) {
      best_error = error;
      best_header = uint(base) | ((uint(multiplier) << 4u | table) << 8u);
      best_selection = selection;
    }
  }
  uint indices_lo = 0u;
  uint indices_hi = 0u;
  uint i;
  unroll_xe for (i = 0u; i < 16u; ++i) {
    uint weight = (weights_high >> (i << 1u)) & 3u;
    uint index = (best_selection >> (weight * 3u)) & 7u;
    uint position = ((3u - (i >> 2u)) + (3u - (i & 3u)) * 4u) * 3u;
    if (position < 32u) {
      indices_lo |= index << position;
      if (position > 29u) {
        indices_hi |= index >> (32u - position);
      }
    } else {
      indices_hi |= index << (position - 32u);
    }
  }
  return uint2_xe(
      best_header | (((indices_hi >> 8u) & 0xFFu) << 16u) |
          ((indices_hi & 0xFFu) << 24u),
      (indices_lo >> 24u) | (((indices_lo >> 16u) & 0xFFu) << 8u) |
          (((indices_lo >> 8u) & 0xFFu) << 16u) |
          ((indices_lo & 0xFFu) << 24u));
}

uint4_xe XeCTX1BlockToEACRG11(uint endpoints, uint codes) {
  uint weights_high = XeDXTHighColorWeights(codes);
  uint counts = 0u;
  uint i;
  unroll_xe for (i = 0u; i < 16u; ++i) {
    uint w = (weights_high >> (i << 1u)) & 3u;
    counts += 1u << (w << 3u);
  }
  int r0 = int((endpoints >> 8u) & 0xFFu);
  int g0 = int(endpoints & 0xFFu);
  int r1 = int((endpoints >> 24u) & 0xFFu);
  int g1 = int((endpoints >> 16u) & 0xFFu);
  return uint4_xe(XeCTX1ChannelToEACR11(r0, r1, weights_high, counts),
                  XeCTX1ChannelToEACR11(g0, g1, weights_high, counts));
}

XE_TEXTURE_LOAD_ENTRY(16)
{
  // 1 thread = 4 CTX1 blocks (32B guest) to 4 EAC RG11 blocks (64B host).
  XeTextureLoadInfo load_info = XeTextureLoadGetInfo(pass_push_consts_xe);
  uint3_xe block_index = in_global_thread_id_xe << uint3_xe(2u, 0u, 0u);
  dont_flatten_xe
  if (any(greater_than_equal_xe(block_index.xy, load_info.size_blocks.xy))) {
    return;
  }
  uint block_offset_host =
      load_info.host_offset +
      uint(XeTextureHostLinearOffset(int3_xe(block_index), load_info.host_pitch,
                                     load_info.size_blocks.y, 16u));
  uint block_offset_guest =
      XeTextureLoadSourceAddress(load_info, block_index, 3u);
  uint4_xe blocks_01 = XeEndianSwap32(
      byte_buffer_align16_load16_xe(xe_texture_load_source, block_offset_guest),
      load_info.endian_32);
  block_offset_guest +=
      XeTextureLoadLocalXAddressXor(2u, 3u, load_info.is_tiled);
  uint4_xe blocks_23 = XeEndianSwap32(
      byte_buffer_align16_load16_xe(xe_texture_load_source, block_offset_guest),
      load_info.endian_32);
  byte_buffer_align16_store16_xe(
      xe_texture_load_dest, block_offset_host,
      XeCTX1BlockToEACRG11(blocks_01.x, blocks_01.y));
  byte_buffer_align16_store16_xe(
      xe_texture_load_dest, block_offset_host + 0x10u,
      XeCTX1BlockToEACRG11(blocks_01.z, blocks_01.w));
  byte_buffer_align16_store16_xe(
      xe_texture_load_dest, block_offset_host + 0x20u,
      XeCTX1BlockToEACRG11(blocks_23.x, blocks_23.y));
  byte_buffer_align16_store16_xe(
      xe_texture_load_dest, block_offset_host + 0x30u,
      XeCTX1BlockToEACRG11(blocks_23.z, blocks_23.w));
}
entry_code_end_compute_xe
