#include "zerofg/zerofg.h"

#include <utility>

#include "algorithm.h"

namespace zerofg {

std::unique_ptr<Interpolator> Interpolator::Create(
    const CreateInfo& create_info, Status* status) {
  const auto fail = [status](Status failure) {
    if (status) {
      *status = failure;
    }
    return std::unique_ptr<Interpolator>();
  };

  if (create_info.vulkan.instance == VK_NULL_HANDLE ||
      create_info.vulkan.physical_device == VK_NULL_HANDLE ||
      create_info.vulkan.device == VK_NULL_HANDLE ||
      create_info.vulkan.get_instance_proc_addr == nullptr ||
      create_info.frame_context_count == 0 ||
      create_info.frame_context_count > 8) {
    return fail(Status::kInvalidArgument);
  }
  if (create_info.mode != Mode::kZero &&
      create_info.mode != Mode::kReallyZero &&
      create_info.mode != Mode::kRC4 &&
      create_info.mode != Mode::kRC6 &&
      create_info.mode != Mode::kRC7 &&
      create_info.mode != Mode::kDenseFlow) {
    return fail(Status::kInvalidArgument);
  }

  // Auto takes the generic fast paths the device really has enabled (Modern:
  // half colour, subgroup propagation, hardware cubic); Compat runs the golden
  // forms. QCOM is the Modern backend (no QCOM-specific form measured to pay).
  Backend resolved_backend = create_info.backend;
  if (resolved_backend == Backend::kAuto) {
    resolved_backend = Backend::kModern;
  }
  if (resolved_backend != Backend::kCompat &&
      resolved_backend != Backend::kModern &&
      resolved_backend != Backend::kQcom) {
    return fail(Status::kUnsupported);
  }
  // The ZeroFG floor: an effective Vulkan 1.3 device with synchronization2.
  if (create_info.capabilities.effective_api_version < VK_API_VERSION_1_3 ||
      !create_info.capabilities.synchronization2_enabled) {
    return fail(Status::kUnsupported);
  }
  // The frame contexts share the temporal history (the RC3 family).
  const bool dense_flow = create_info.mode == Mode::kDenseFlow;
  const bool rc7 = create_info.mode == Mode::kRC7;
  std::shared_ptr<Rc3Shared> shared;
  if (!dense_flow && !rc7) {
    shared = CreateRc3Shared(create_info);
    if (!shared) {
      return fail(Status::kUnsupported);
    }
  }

  // RC7 and the dense flow have one context whatever the index: the surfaces, the search under way and the flow's
  // starts are history, and one per index would see only some of the pairs and work from history several pairs old.
  // What must differ per index is only the descriptor sets that bind the Reals and the output: they keep one copy each.
  const uint32_t context_count = rc7 || dense_flow ? 1u : create_info.frame_context_count;
  std::vector<std::unique_ptr<AlgorithmContext>> contexts;
  contexts.reserve(context_count);
  for (uint32_t i = 0; i < context_count; ++i) {
    CreateInfo context_create_info = create_info;
    context_create_info.backend = resolved_backend;
    Status context_status = Status::kSuccess;
    std::unique_ptr<AlgorithmContext> context =
        rc7 ? CreateRc7Context(context_create_info, &context_status)
        : dense_flow ? CreateDfContext(context_create_info, &context_status)
                     : CreateRc3Context(context_create_info, shared, &context_status);
    if (!context) {
      return fail(context_status);
    }
    contexts.push_back(std::move(context));
  }

  if (status) {
    *status = Status::kSuccess;
  }
  return std::unique_ptr<Interpolator>(
      new Interpolator(std::move(contexts), create_info.frame_context_count));
}

Interpolator::Interpolator(
    std::vector<std::unique_ptr<AlgorithmContext>> contexts,
    uint32_t frame_context_count)
    : contexts_(std::move(contexts)), frame_context_count_(frame_context_count) {}

Interpolator::~Interpolator() = default;

Status Interpolator::Resize(uint32_t width, uint32_t height,
                            VkFormat input_format,
                            VkFormat output_format) {
  if (contexts_.empty()) {
    return Status::kInvalidArgument;
  }
  for (const auto& context : contexts_) {
    const Status status =
        context->Resize(width, height, input_format, output_format);
    if (status != Status::kSuccess) {
      return status;
    }
  }
  return Status::kSuccess;
}

Status Interpolator::Interpolate(VkCommandBuffer command_buffer,
                                 uint32_t frame_context_index,
                                 const Image& previous,
                                 const Image& current, float phase,
                                 const Image& output) {
  if (frame_context_index >= frame_context_count_) {
    return Status::kInvalidArgument;
  }
  return contexts_[contexts_.size() == 1 ? 0 : frame_context_index]->Interpolate(
      command_buffer, previous, current, phase, output);
}

}  // namespace zerofg
