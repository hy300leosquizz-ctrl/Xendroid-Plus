/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/shader.h"

#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/gpu/ucode.h"

namespace xe {
namespace gpu {
using namespace ucode;

Shader::Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
               const uint32_t* ucode_dwords, size_t ucode_dword_count,
               std::endian ucode_source_endian)
    : shader_type_(shader_type), ucode_data_hash_(ucode_data_hash) {
  // We keep ucode data in host native format so it's easier to work with.
  ucode_data_.resize(ucode_dword_count);
  if (std::endian::native != ucode_source_endian) {
    xe::copy_and_swap(ucode_data_.data(), ucode_dwords, ucode_dword_count);
  } else {
    std::memcpy(ucode_data_.data(), ucode_dwords,
                sizeof(uint32_t) * ucode_dword_count);
  }
}

Shader::~Shader() {
  for (auto it : translations_) {
    delete it.second;
  }
}

std::string Shader::Translation::GetTranslatedBinaryString() const {
  std::string result;
  result.resize(translated_binary_.size());
  std::memcpy(const_cast<char*>(result.data()), translated_binary_.data(),
              translated_binary_.size());
  return result;
}

std::pair<std::filesystem::path, std::filesystem::path>
Shader::Translation::Dump(const std::filesystem::path& base_path,
                          const char* path_prefix) const {
  if (!is_valid()) {
    return std::make_pair(std::filesystem::path(), std::filesystem::path());
  }

  std::filesystem::path path = base_path;
  // Ensure target path exists.
  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    target_path = std::filesystem::absolute(target_path);
    std::filesystem::create_directories(target_path);
  }

  const char* type_extension =
      shader().type() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / fmt::format("shader_{:016X}_{:016X}.{}.bin.{}",
                                shader().ucode_data_hash(), modification(),
                                path_prefix, type_extension);
  FILE* binary_file = filesystem::OpenFile(binary_path, "wb");
  if (binary_file) {
    fwrite(translated_binary_.data(), sizeof(*translated_binary_.data()),
           translated_binary_.size(), binary_file);
    fclose(binary_file);
  }

  std::filesystem::path disasm_path;
  if (!host_disassembly_.empty()) {
    disasm_path =
        target_path / fmt::format("shader_{:016X}_{:016X}.{}.{}",
                                  shader().ucode_data_hash(), modification(),
                                  path_prefix, type_extension);
    FILE* disasm_file = filesystem::OpenFile(disasm_path, "w");
    if (disasm_file) {
      fwrite(host_disassembly_.data(), sizeof(*host_disassembly_.data()),
             host_disassembly_.size(), disasm_file);
      fclose(disasm_file);
    }
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

bool Shader::has_only_forward_jumps() const {
  int8_t cached = has_only_forward_jumps_.load(std::memory_order_acquire);
  if (cached >= 0) {
    return cached != 0;
  }
  bool forward_only = !uses_subroutine_calls_;
  if (forward_only && !label_addresses_.empty()) {
    struct FlowTransfer {
      uint32_t index;
      uint32_t kind;
      uint32_t address;
    };
    std::vector<FlowTransfer> flow_transfers;
    for (uint32_t i = 0; i < cf_pair_index_bound_ && forward_only; ++i) {
      ControlFlowInstruction cf_ab[2];
      UnpackControlFlowInstructions(ucode_data_.data() + i * 3, cf_ab);
      for (uint32_t j = 0; j < 2; ++j) {
        const ControlFlowInstruction& cf = cf_ab[j];
        switch (cf.opcode()) {
          case ControlFlowOpcode::kCondCall:
          case ControlFlowOpcode::kReturn:
            forward_only = false;
            break;
          case ControlFlowOpcode::kCondJmp:
            flow_transfers.push_back({i * 2 + j, 0, cf.cond_jmp.address()});
            break;
          case ControlFlowOpcode::kLoopStart:
            flow_transfers.push_back({i * 2 + j, 1, cf.loop_start.address()});
            break;
          case ControlFlowOpcode::kLoopEnd:
            flow_transfers.push_back({i * 2 + j, 2, cf.loop_end.address()});
            break;
          default:
            break;
        }
      }
    }
    if (forward_only) {
      std::vector<std::pair<uint32_t, uint32_t>> loop_stack;
      std::vector<std::pair<uint32_t, uint32_t>> loop_bodies;
      for (const FlowTransfer& transfer : flow_transfers) {
        if (transfer.kind == 0) {
          if (transfer.address <= transfer.index) {
            forward_only = false;
            break;
          }
        } else if (transfer.kind == 1) {
          if (transfer.address <= transfer.index + 1) {
            forward_only = false;
            break;
          }
          loop_stack.emplace_back(transfer.index, transfer.address);
        } else {
          if (loop_stack.empty() ||
              transfer.address != loop_stack.back().first + 1 ||
              loop_stack.back().second != transfer.index + 1) {
            forward_only = false;
            break;
          }
          loop_bodies.emplace_back(loop_stack.back().first, transfer.index);
          loop_stack.pop_back();
        }
      }
      if (!loop_stack.empty()) {
        forward_only = false;
      }
      for (const FlowTransfer& transfer : flow_transfers) {
        if (transfer.kind != 0 || !forward_only) {
          continue;
        }
        for (const auto& body : loop_bodies) {
          bool source_inside =
              transfer.index > body.first && transfer.index <= body.second;
          bool target_inside =
              transfer.address > body.first && transfer.address <= body.second;
          if (target_inside && !source_inside) {
            forward_only = false;
            break;
          }
        }
      }
    }
  }
  has_only_forward_jumps_.store(forward_only ? 1 : 0,
                                std::memory_order_release);
  return forward_only;
}

Shader::Translation* Shader::GetOrCreateTranslation(uint64_t modification,
                                                    bool* is_new) {
  if (last_translation_ && last_translation_modification_ == modification) {
    if (is_new) {
      *is_new = false;
    }
    return last_translation_;
  }
  auto it = translations_.find(modification);
  if (it != translations_.end()) {
    if (is_new) {
      *is_new = false;
    }
    last_translation_modification_ = modification;
    last_translation_ = it->second;
    return it->second;
  }
  Translation* translation = CreateTranslationInstance(modification);
  translations_.emplace(modification, translation);
  last_translation_modification_ = modification;
  last_translation_ = translation;
  if (is_new) {
    *is_new = true;
  }
  return translation;
}

void Shader::DestroyTranslation(uint64_t modification) {
  auto it = translations_.find(modification);
  if (it == translations_.end()) {
    return;
  }
  if (last_translation_ == it->second) {
    last_translation_ = nullptr;
  }
  delete it->second;
  translations_.erase(it);
}

std::pair<std::filesystem::path, std::filesystem::path> Shader::DumpUcode(
    const std::filesystem::path& base_path) const {
  // Ensure target path exists.
  std::filesystem::path target_path = base_path;
  if (!target_path.empty()) {
    target_path = std::filesystem::absolute(target_path);
    std::filesystem::create_directories(target_path);
  }

  const char* type_extension =
      type() == xenos::ShaderType::kVertex ? "vert" : "frag";

  std::filesystem::path binary_path =
      target_path / fmt::format("shader_{:016X}.ucode.bin.{}",
                                ucode_data_hash(), type_extension);
  FILE* binary_file = filesystem::OpenFile(binary_path, "wb");
  if (binary_file) {
    fwrite(ucode_data().data(), sizeof(*ucode_data().data()),
           ucode_data().size(), binary_file);
    fclose(binary_file);
  }

  std::filesystem::path disasm_path;
  if (is_ucode_analyzed()) {
    disasm_path = target_path / fmt::format("shader_{:016X}.ucode.{}",
                                            ucode_data_hash(), type_extension);
    FILE* disasm_file = filesystem::OpenFile(disasm_path, "w");
    if (disasm_file) {
      fwrite(ucode_disassembly().data(), sizeof(*ucode_disassembly().data()),
             ucode_disassembly().size(), disasm_file);
      fclose(disasm_file);
    }
  }

  return std::make_pair(std::move(binary_path), std::move(disasm_path));
}

Shader::Translation* Shader::CreateTranslationInstance(uint64_t modification) {
  // Default implementation for simple cases like ucode disassembly.
  return new Translation(*this, modification);
}

}  //  namespace gpu
}  //  namespace xe
