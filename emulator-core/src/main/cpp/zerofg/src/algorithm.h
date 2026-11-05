#pragma once

#include <memory>

#include "zerofg/zerofg.h"

namespace zerofg {

struct Rc3Shared;

class AlgorithmContext {
 public:
  virtual ~AlgorithmContext() = default;

  virtual Status Resize(uint32_t width, uint32_t height,
                        VkFormat input_format,
                        VkFormat output_format) = 0;

  virtual Status Interpolate(VkCommandBuffer command_buffer,
                             const Image& previous,
                             const Image& current, float phase,
                             const Image& output) = 0;
};

// The frame contexts of one Interpolator share the temporal history.
std::unique_ptr<AlgorithmContext> CreateRc3Context(
    const CreateInfo& create_info, std::shared_ptr<Rc3Shared> shared,
    Status* status);
std::shared_ptr<Rc3Shared> CreateRc3Shared(const CreateInfo& create_info);
// The dense flow (Mode::kDenseFlow): one context whatever the frame context index.
std::unique_ptr<AlgorithmContext> CreateDfContext(const CreateInfo& create_info, Status* status);
// RC7 (Mode::kRC7): one context whatever the frame context index.
std::unique_ptr<AlgorithmContext> CreateRc7Context(const CreateInfo& create_info, Status* status);

// RC6: the dense flow as a source of D4 proposals. It solves the A -> B flow down to `finest` (2: the Eco form, a quarter of the
// resolution) and stops there (no middle picture). The flow is a vec2 per pixel of the finest level, in that level's pixels, at
// flow_offset() vec2s into flow_buffer(), written by the compute stage.
class DenseFlowSource {
 public:
  virtual ~DenseFlowSource() = default;
  virtual Status Resize(uint32_t width, uint32_t height, VkFormat input_format) = 0;
  virtual Status RecordFlow(VkCommandBuffer command_buffer, const Image& previous, const Image& current) = 0;
  // The flow and the RC5 middle picture (RC6 mixes it in where its slim mask is up).
  virtual Status RecordPicture(VkCommandBuffer command_buffer, const Image& previous, const Image& current, const Image& output) = 0;
  // The flow from `previous` and `current`, the picture's last steps from larger Reals into a larger image: their size, set before Resize.
  virtual void SetPictureSize(uint32_t width, uint32_t height) = 0;
  virtual Status RecordPictureFrom(VkCommandBuffer command_buffer, const Image& previous, const Image& current, const Image& picture_previous, const Image& picture_current, const Image& output) = 0;
  virtual VkBuffer flow_buffer() const = 0;
  virtual uint32_t flow_offset() const = 0;
  virtual uint32_t flow_width() const = 0;
  virtual uint32_t flow_height() const = 0;
};
// Optional exact pyramid retention is separate from the temporal flow starts.
// It preserves the FP32 flow-buffer contract; callers opt in explicitly.
std::unique_ptr<DenseFlowSource> CreateDenseFlowSource(const CreateInfo& create_info, uint32_t finest, Status* status, bool starts = false, bool retain_pyramid = false);

}  // namespace zerofg
