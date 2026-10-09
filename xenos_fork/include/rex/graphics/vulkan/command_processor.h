#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/odisea_gpu_profiler.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/primitive_processor.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/linked_type_descriptor_set_allocator.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor : public CommandProcessor {
 public:
  // Fork (odisea): memoria del guest para el volcado/sustitución de texturas
  // (mismo accesor que en el command processor de D3D12).
  memory::Memory* guest_memory() const { return memory_; }

  // Single-descriptor layouts for use within a single frame.
  enum class SingleTransientDescriptorLayout {
    kStorageBufferCompute,
    kStorageBufferPairCompute,
    kCount,
  };

  class ScratchBufferAcquisition {
   public:
    explicit ScratchBufferAcquisition() = default;
    explicit ScratchBufferAcquisition(VulkanCommandProcessor& command_processor, VkBuffer buffer,
                                      VkPipelineStageFlags stage_mask, VkAccessFlags access_mask)
        : command_processor_(&command_processor),
          buffer_(buffer),
          stage_mask_(stage_mask),
          access_mask_(access_mask) {}

    ScratchBufferAcquisition(const ScratchBufferAcquisition& acquisition) = delete;
    ScratchBufferAcquisition& operator=(const ScratchBufferAcquisition& acquisition) = delete;

    ScratchBufferAcquisition(ScratchBufferAcquisition&& acquisition) {
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
    }
    ScratchBufferAcquisition& operator=(ScratchBufferAcquisition&& acquisition) {
      if (this == &acquisition) {
        return *this;
      }
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
      return *this;
    }

    ~ScratchBufferAcquisition() {
      if (buffer_ != VK_NULL_HANDLE) {
        assert_true(command_processor_->scratch_buffer_used_);
        assert_true(command_processor_->scratch_buffer_ == buffer_);
        command_processor_->scratch_buffer_last_stage_mask_ = stage_mask_;
        command_processor_->scratch_buffer_last_access_mask_ = access_mask_;
        command_processor_->scratch_buffer_last_usage_submission_ =
            command_processor_->GetCurrentSubmission();
        command_processor_->scratch_buffer_used_ = false;
      }
    }

    // VK_NULL_HANDLE if failed to acquire or if moved.
    VkBuffer buffer() const { return buffer_; }

    VkPipelineStageFlags GetStageMask() const { return stage_mask_; }
    VkPipelineStageFlags SetStageMask(VkPipelineStageFlags new_stage_mask) {
      VkPipelineStageFlags old_stage_mask = stage_mask_;
      stage_mask_ = new_stage_mask;
      return old_stage_mask;
    }
    VkAccessFlags GetAccessMask() const { return access_mask_; }
    VkAccessFlags SetAccessMask(VkAccessFlags new_access_mask) {
      VkAccessFlags old_access_mask = access_mask_;
      access_mask_ = new_access_mask;
      return old_access_mask;
    }

   private:
    VulkanCommandProcessor* command_processor_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineStageFlags stage_mask_ = 0;
    VkAccessFlags access_mask_ = 0;
  };

  VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~VulkanCommandProcessor();

  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  ui::vulkan::VulkanDevice* GetVulkanDevice() const {
    return static_cast<const ui::vulkan::VulkanProvider*>(graphics_system_->provider())
        ->vulkan_device();
  }

  bool CompileGlslToSpirv(VkShaderStageFlagBits stage, std::string_view source,
                          std::vector<uint32_t>& spirv_out, std::string& error_out) const;

  // Returns the deferred drawing command list for the currently open
  // submission.
  DeferredCommandBuffer& deferred_command_buffer() {
    assert_true(submission_open_);
    return deferred_command_buffer_;
  }

  bool submission_open() const { return submission_open_; }
  // Fork (Odisea): envio asincrono (odisea_async_submit). Espera a que el hilo de envio haya pasado
  // a la cola todos los envios pendientes.
  void WaitAsyncSubmissions();
  uint64_t GetCurrentSubmission() const {
    return submission_completed_ + uint64_t(submissions_in_flight_fences_.size()) + 1;
  }
  uint64_t GetCompletedSubmission() const { return submission_completed_; }

  // Sparse binds are:
  // - In a single submission, all submitted in one vkQueueBindSparse.
  // - Sent to the queue without waiting for a semaphore.
  // Thus, multiple sparse binds between the completed and the current
  // submission, and within one submission, must not touch any overlapping
  // memory regions.
  void SparseBindBuffer(VkBuffer buffer, uint32_t bind_count, const VkSparseMemoryBind* binds,
                        VkPipelineStageFlags wait_stage_mask);

  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }

  // Submission must be open to insert barriers. If no pipeline stages access
  // the resource in a synchronization scope, the stage masks should be 0 (top /
  // bottom of pipe should be specified only if explicitly needed). Returning
  // true if the barrier has actually been inserted and not dropped.
  bool PushBufferMemoryBarrier(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                               VkPipelineStageFlags src_stage_mask,
                               VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
                               VkAccessFlags dst_access_mask,
                               uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                               uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                               bool skip_if_equal = true);
  bool PushImageMemoryBarrier(VkImage image, const VkImageSubresourceRange& subresource_range,
                              VkPipelineStageFlags src_stage_mask,
                              VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
                              VkAccessFlags dst_access_mask, VkImageLayout old_layout,
                              VkImageLayout new_layout,
                              uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                              uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                              bool skip_if_equal = true);
  // Returns whether any barriers have been submitted - if true is returned, the
  // render pass will also be closed.
  bool SubmitBarriers(bool force_end_render_pass);

  // If not started yet, begins a render pass from the render target cache.
  // Submission must be open.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer);
  // Overload for transfer operations with dynamic rendering.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer,
      VkImageView transfer_dest_view, bool transfer_dest_is_depth);
  // Must be called before doing anything outside the render pass scope,
  // including adding pipeline barriers that are not a part of the render pass
  // scope. Submission must be open.
  void EndRenderPass();

  VkDescriptorSetLayout GetSingleTransientDescriptorLayout(
      SingleTransientDescriptorLayout transient_descriptor_layout) const {
    return descriptor_set_layouts_single_transient_[size_t(transient_descriptor_layout)];
  }
  // A frame must be open.
  VkDescriptorSet AllocateSingleTransientDescriptor(
      SingleTransientDescriptorLayout transient_descriptor_layout);

  // The returned reference is valid until a cache clear.
  VkDescriptorSetLayout GetTextureDescriptorSetLayout(bool is_vertex, size_t texture_count,
                                                      size_t sampler_count);
  // The returned reference is valid until a cache clear.
  const VulkanPipelineCache::PipelineLayoutProvider* GetPipelineLayout(size_t texture_count_pixel,
                                                                       size_t sampler_count_pixel,
                                                                       size_t texture_count_vertex,
                                                                       size_t sampler_count_vertex);

  // Returns a single temporary GPU-side buffer within a submission for tasks
  // like texture untiling and resolving. May push a buffer memory barrier into
  // the initial usage. Submission must be open.
  ScratchBufferAcquisition AcquireScratchGpuBuffer(VkDeviceSize size,
                                                   VkPipelineStageFlags initial_stage_mask,
                                                   VkAccessFlags initial_access_mask);

  // Binds a graphics pipeline for host-specific purposes, invalidating the
  // affected state. keep_dynamic_* must be false (to invalidate the dynamic
  // state after binding the pipeline with the same state being static, or if
  // the caller changes the dynamic state bypassing the VulkanCommandProcessor)
  // unless the caller has these state variables as dynamic and uses the
  // tracking in VulkanCommandProcessor to modify them.
  void BindExternalGraphicsPipeline(VkPipeline pipeline, bool keep_dynamic_depth_bias = false,
                                    bool keep_dynamic_blend_constants = false,
                                    bool keep_dynamic_stencil_mask_ref = false);
  void BindExternalComputePipeline(VkPipeline pipeline);
  void SetViewport(const VkViewport& viewport);
  void SetScissor(const VkRect2D& scissor);

  // Returns the text to display in the GPU backend name in the window title.
  std::string GetWindowTitleText() const;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;
  void OnPrimaryBufferEnd() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  struct CommandBuffer {
    VkCommandPool pool;
    VkCommandBuffer buffer;
  };

  struct SparseBufferBind {
    VkBuffer buffer;
    size_t bind_offset;
    uint32_t bind_count;
  };

  union TextureDescriptorSetLayoutKey {
    uint32_t key;
    struct {
      // If texture and sampler counts are both 0, use
      // descriptor_set_layout_empty_ instead as these are owning references.
      uint32_t texture_count : 16;
      uint32_t sampler_count : 15;
      uint32_t is_vertex : 1;
    };

    TextureDescriptorSetLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TextureDescriptorSetLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const TextureDescriptorSetLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const TextureDescriptorSetLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  union PipelineLayoutKey {
    uint64_t key;
    struct {
      // Pixel textures in the low bits since those are varied much more
      // commonly.
      uint16_t texture_count_pixel;
      uint16_t sampler_count_pixel;
      uint16_t texture_count_vertex;
      uint16_t sampler_count_vertex;
    };

    PipelineLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const PipelineLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const PipelineLayoutKey& other_key) const { return key == other_key.key; }
    bool operator!=(const PipelineLayoutKey& other_key) const { return !(*this == other_key); }
  };

  class PipelineLayout : public VulkanPipelineCache::PipelineLayoutProvider {
   public:
    explicit PipelineLayout(VkPipelineLayout pipeline_layout,
                            VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref,
                            VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref)
        : pipeline_layout_(pipeline_layout),
          descriptor_set_layout_textures_vertex_ref_(descriptor_set_layout_textures_vertex_ref),
          descriptor_set_layout_textures_pixel_ref_(descriptor_set_layout_textures_pixel_ref) {}
    VkPipelineLayout GetPipelineLayout() const override { return pipeline_layout_; }
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref() const {
      return descriptor_set_layout_textures_vertex_ref_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref() const {
      return descriptor_set_layout_textures_pixel_ref_;
    }

   private:
    VkPipelineLayout pipeline_layout_;
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref_;
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref_;
  };

  struct UsedSingleTransientDescriptor {
    uint64_t frame;
    SingleTransientDescriptorLayout layout;
    VkDescriptorSet set;
  };

  struct UsedTextureTransientDescriptorSet {
    uint64_t frame;
    TextureDescriptorSetLayoutKey layout;
    VkDescriptorSet set;
  };

  enum SwapApplyGammaDescriptorSet : uint32_t {
    kSwapApplyGammaDescriptorSetRamp,
    kSwapApplyGammaDescriptorSetSource,

    kSwapApplyGammaDescriptorSetCount,
  };
  enum SwapApplyGammaComputeDescriptorSet : uint32_t {
    kSwapApplyGammaComputeDescriptorSetRamp,
    kSwapApplyGammaComputeDescriptorSetSource,
    kSwapApplyGammaComputeDescriptorSetDestination,

    kSwapApplyGammaComputeDescriptorSetCount,
  };
  struct SwapApplyGammaConstants {
    uint32_t size[2];
  };
  enum SwapFxaaDescriptorSet : uint32_t {
    kSwapFxaaDescriptorSetSource,
    kSwapFxaaDescriptorSetDestination,

    kSwapFxaaDescriptorSetCount,
  };
  struct SwapFxaaConstants {
    uint32_t size[2];
    float size_inv[2];
  };

  // Framebuffer for the current presenter's guest output image revision, and
  // its usage tracking.
  struct SwapFramebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = UINT64_MAX;
    uint64_t last_submission = 0;
  };

  // BeginSubmission and EndSubmission may be called at any time. If there's an
  // open non-frame submission, BeginSubmission(true) will promote it to a
  // frame. EndSubmission(true) will close the frame no matter whether the
  // submission has already been closed.
  // Unlike on Direct3D 12, submission boundaries do not imply any memory
  // barriers aside from an incoming host write (but not outgoing host read)
  // dependency.

  // Rechecks submission number and reclaims per-submission resources. Pass 0 as
  // the submission to await to simply check status, or pass
  // GetCurrentSubmission() to wait for all queue operations to be completed.
  void CheckSubmissionFenceAndDeviceLoss(uint64_t await_submission);
  // If is_guest_command is true, a new full frame - with full cleanup of
  // resources and, if needed, starting capturing - is opened if pending (as
  // opposed to simply resuming after mid-frame synchronization). Returns
  // whether a submission is open currently and the device is not lost.
  bool BeginSubmission(bool is_guest_command);
  // If is_swap is true, a full frame is closed - with, if needed, cache
  // clearing and stopping capturing. Returns whether the submission was done
  // successfully, if it has failed, leaves it open.
  bool EndSubmission(bool is_swap);
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionFenceAndDeviceLoss(GetCurrentSubmission());
    return !submission_open_ && submissions_in_flight_fences_.empty();
  }
  // Keep primary-buffer-end submit behavior aligned with D3D12: only submit
  // when immediate submission is safe.
  bool CanEndSubmissionImmediately() const;

  void ClearTransientDescriptorPools();
  bool IssueCopy_ReadbackResolvePath();
  bool IssueDraw_MemexportReadbackFullPath(uint32_t total_size);
  bool IssueDraw_MemexportReadbackFastPath(uint32_t total_size);

  void SplitPendingBarrier();

  void DestroyScratchBuffer();
  bool InitializeOcclusionQueryResources();
  void ShutdownOcclusionQueryResources();
  // Fork (odisea): perfilador de tiempo de GPU (marcas de tiempo por categoria).
  bool InitializeGpuProfileResources();
  void ShutdownGpuProfileResources();

 public:
  // only_if_commands: no marcar si no se ha grabado nada desde la ultima marca.
  // Publico para que la cache de texturas atribuya sus cargas.
  void GpuProfileMark(odisea::GpuProfileCat cat, uint64_t shader_hash, bool only_if_commands);

 private:
  bool BeginGuestOcclusionQuery(uint32_t sample_count_address);
  bool EndGuestOcclusionQuery(uint32_t sample_count_address);
  bool AcquireOcclusionQueryIndex(uint32_t& host_index_out);
  void DisableHostOcclusionQueries();
  uint64_t NormalizeOcclusionSamples(uint64_t samples) const;
  void WriteGuestOcclusionResult(uint32_t sample_count_address, uint64_t samples);
  void InvalidateAllVertexBufferResidency();
  void InvalidateVertexBufferResidency(uint32_t vfetch_index);
  void InvalidateVertexBufferResidencyRange(uint32_t first_vfetch, uint32_t last_vfetch);
  struct ReadbackBuffer {
    VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* mapped_data[2] = {nullptr, nullptr};
    uint32_t sizes[2] = {0, 0};
    uint64_t submission_written[2] = {0, 0};
    uint32_t written_size[2] = {0, 0};
    uint32_t current_index = 0;
    uint64_t last_used_frame = 0;
  };
  void EvictOldReadbackBuffers(std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map);
  static constexpr uint32_t kReadbackBufferSizeIncrement = 16 * 1024 * 1024;
  static constexpr size_t kMaxReadbackBuffers = 256;
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;
  static inline uint32_t AlignReadbackBufferSize(uint32_t size) {
    if (size < 1 * 1024 * 1024) {
      return rex::align(size, 256u * 1024u);
    }
    if (size < 4 * 1024 * 1024) {
      return rex::align(size, 1u * 1024u * 1024u);
    }
    return rex::align(size, kReadbackBufferSizeIncrement);
  }
  static inline uint64_t MakeReadbackResolveKey(uint32_t address, uint32_t length) {
    return (uint64_t(address) << 32) | uint64_t(length);
  }
  static inline uint64_t MakeMemexportReadbackKey(uint32_t first_base_address_dwords,
                                                  uint32_t total_size) {
    return (uint64_t(first_base_address_dwords) << 32) | uint64_t(total_size);
  }
  struct ResolveDownscaleConstants {
    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t pixel_size_log2;
    uint32_t tile_count;
    uint32_t source_offset_bytes;
    uint32_t half_pixel_offset;
  };
  bool EnsureSwapFxaaSourceImage(uint32_t width, uint32_t height);
  void DestroySwapFxaaSourceImage();

  void UpdateDynamicState(const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,
                          reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(
      bool primitive_polygonal,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool shader_32bit_index_dma, uint32_t compute_memexport_vertex_count,
      const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      // Fork (Odisea): sombreadores traducidos sin escala (interfaz fuera de la EDRAM):
      // constantes de escala x1.
      bool odisea_unscaled_shaders);
  bool UpdateBindings(const VulkanShader* vertex_shader, const VulkanShader* pixel_shader);
  // Allocates a descriptor set and fills one or two VkWriteDescriptorSet
  // structure instances (for images and samplers).
  // The descriptor set layout must be the one for the given is_vertex,
  // texture_count, sampler_count (from GetTextureDescriptorSetLayout - may be
  // already available at the moment of the call, no need to locate it again).
  // Returns how many VkWriteDescriptorSet structure instances have been
  // written, or 0 if there was a failure to allocate the descriptor set or no
  // bindings were requested.
  uint32_t WriteTransientTextureBindings(bool is_vertex, uint32_t texture_count,
                                         uint32_t sampler_count,
                                         VkDescriptorSetLayout descriptor_set_layout,
                                         const VkDescriptorImageInfo* texture_image_info,
                                         const VkDescriptorImageInfo* sampler_image_info,
                                         VkWriteDescriptorSet* descriptor_set_writes_out);

  bool device_lost_ = false;

  bool cache_clear_requested_ = false;

  // Host shader types that guest shaders can be translated into - they can
  // access the shared memory (via vertex fetch, memory export, or manual index
  // buffer reading) and textures.
  VkPipelineStageFlags guest_shader_pipeline_stages_ = 0;
  VkShaderStageFlags guest_shader_vertex_stages_ = 0;

  std::vector<VkFence> fences_free_;
  std::vector<VkSemaphore> semaphores_free_;

  bool submission_open_ = false;
  uint64_t submission_completed_ = 0;
  // In case vkQueueSubmit fails after something like a successful
  // vkQueueBindSparse, to wait correctly on the next attempt.
  std::vector<VkSemaphore> current_submission_wait_semaphores_;
  std::vector<VkPipelineStageFlags> current_submission_wait_stage_masks_;
  std::vector<VkFence> submissions_in_flight_fences_;
  std::deque<std::pair<uint64_t, VkSemaphore>> submissions_in_flight_semaphores_;

  // Fork (Odisea): envio asincrono. El hilo de comandos graba, reinicia la fence y la apunta en
  // submissions_in_flight_fences_; este hilo reinicia el pool, reproduce el flujo
  // (DeferredCommandBuffer::ExecuteStream), cierra el command buffer y hace vkQueueSubmit.
  struct AsyncSubmitJob {
    std::vector<uintmax_t> stream;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stage_masks;
  };
  void StartAsyncSubmitThread();
  void StopAsyncSubmitThread();
  void AsyncSubmitThreadMain();
  std::thread async_submit_thread_;
  std::mutex async_submit_mutex_;
  std::condition_variable async_submit_request_cond_;
  std::condition_variable async_submit_done_cond_;
  std::deque<AsyncSubmitJob> async_submit_jobs_;
  std::vector<std::vector<uintmax_t>> async_submit_free_streams_;
  bool async_submit_busy_ = false;
  bool async_submit_shutdown_ = false;

  static constexpr uint32_t kMaxFramesInFlight = 3;
  bool frame_open_ = false;
  // Tracks whether any draw in the current frame used an async placeholder
  // graphics pipeline and may have produced incomplete output.
  bool frame_used_async_placeholder_pipeline_ = false;
  // Guest frame index, since some transient resources can be reused across
  // submissions. Values updated in the beginning of a frame.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Submission indices of frames that have already been submitted.
  uint64_t closed_frame_submissions_[kMaxFramesInFlight] = {};

  // <Submission where last used, resource>, sorted by the submission number.
  std::deque<std::pair<uint64_t, VkDeviceMemory>> destroy_memory_;
  std::deque<std::pair<uint64_t, VkBuffer>> destroy_buffers_;
  std::deque<std::pair<uint64_t, VkImageView>> destroy_image_views_;
  std::deque<std::pair<uint64_t, VkImage>> destroy_images_;
  std::deque<std::pair<uint64_t, VkFramebuffer>> destroy_framebuffers_;

  std::vector<CommandBuffer> command_buffers_writable_;
  std::deque<std::pair<uint64_t, CommandBuffer>> command_buffers_submitted_;
  DeferredCommandBuffer deferred_command_buffer_;

  std::vector<VkSparseMemoryBind> sparse_memory_binds_;
  std::vector<SparseBufferBind> sparse_buffer_binds_;
  // SparseBufferBind converted to VkSparseBufferMemoryBindInfo to this buffer
  // on submission (because pBinds should point to a place in std::vector, but
  // it may be reallocated).
  std::vector<VkSparseBufferMemoryBindInfo> sparse_buffer_bind_infos_temp_;
  VkPipelineStageFlags sparse_bind_wait_stage_mask_ = 0;

  // Temporary storage with reusable memory for creating descriptor set layouts.
  std::vector<VkDescriptorSetLayoutBinding> descriptor_set_layout_bindings_;
  // Temporary storage with reusable memory for writing image and sampler
  // descriptors.
  std::vector<VkDescriptorImageInfo> descriptor_write_image_info_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> uniform_buffer_pool_;

  // Descriptor set layouts used by different shaders.
  VkDescriptorSetLayout descriptor_set_layout_empty_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, size_t(SingleTransientDescriptorLayout::kCount)>
      descriptor_set_layouts_single_transient_{};
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_and_edram_ = VK_NULL_HANDLE;

  // Descriptor set layouts are referenced by pipeline_layouts_.
  std::unordered_map<TextureDescriptorSetLayoutKey, VkDescriptorSetLayout,
                     TextureDescriptorSetLayoutKey::Hasher>
      descriptor_set_layouts_textures_;
  // Pipeline layouts are referenced by VulkanPipelineCache.
  std::unordered_map<PipelineLayoutKey, PipelineLayout, PipelineLayoutKey::Hasher>
      pipeline_layouts_;

  // No specific reason for 32768, just the "too much" descriptor count from
  // Direct3D 12 PIX warnings.
  static constexpr uint32_t kLinkedTypeDescriptorPoolSetCount = 32768;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeTextures[2];
  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_uniform_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_storage_buffer_;
  std::deque<UsedSingleTransientDescriptor> single_transient_descriptors_used_;
  std::array<std::vector<VkDescriptorSet>, size_t(SingleTransientDescriptorLayout::kCount)>
      single_transient_descriptors_free_;
  // <Usage frame, set>.
  std::deque<std::pair<uint64_t, VkDescriptorSet>> constants_transient_descriptors_used_;
  std::vector<VkDescriptorSet> constants_transient_descriptors_free_;

  ui::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_textures_;
  std::deque<UsedTextureTransientDescriptorSet> texture_transient_descriptor_sets_used_;
  std::unordered_map<TextureDescriptorSetLayoutKey, std::vector<VkDescriptorSet>,
                     TextureDescriptorSetLayoutKey::Hasher>
      texture_transient_descriptor_sets_free_;

  std::unique_ptr<VulkanSharedMemory> shared_memory_;

  std::unique_ptr<VulkanPrimitiveProcessor> primitive_processor_;

  std::unique_ptr<VulkanRenderTargetCache> render_target_cache_;

  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;

  std::unique_ptr<VulkanTextureCache> texture_cache_;

  VkDescriptorPool shared_memory_and_edram_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_and_edram_descriptor_set_;

  // Bytes 0x0...0x3FF - 256-entry gamma ramp table with B10G10R10X2 data (read
  // as R10G10B10X2 with swizzle).
  // Bytes 0x400...0x9FF - 128-entry PWL R16G16 gamma ramp (R - base, G - delta,
  // low 6 bits of each are zero, 3 elements per entry).
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  VkDeviceMemory gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_buffer_ = VK_NULL_HANDLE;
  // kMaxFramesInFlight pairs, only when the gamma ramp buffer is not
  // host-visible.
  VkDeviceMemory gamma_ramp_upload_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_upload_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize gamma_ramp_upload_memory_size_;
  uint32_t gamma_ramp_upload_memory_type_;
  // Mapping of either gamma_ramp_buffer_memory_ (if it's host-visible) or
  // gamma_ramp_upload_buffer_memory_ (otherwise).
  void* gamma_ramp_upload_mapping_;
  std::array<VkBufferView, 2 * kMaxFramesInFlight> gamma_ramp_buffer_views_{};
  // UINT32_MAX if outdated.
  uint32_t gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  uint32_t gamma_ramp_pwl_current_frame_ = UINT32_MAX;

  VkDescriptorSetLayout swap_descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_combined_image_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_storage_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_uniform_texel_buffer_ = VK_NULL_HANDLE;

  // Descriptor pool for allocating descriptors needed for presentation, such as
  // the destination images and the gamma ramps.
  VkDescriptorPool swap_descriptor_pool_ = VK_NULL_HANDLE;
  // Interleaved 256-entry table and PWL texel buffer descriptors.
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  std::array<VkDescriptorSet, 2 * kMaxFramesInFlight> swap_descriptors_gamma_ramp_;
  // Sampled images.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_source_;
  // Combined image sampler descriptors for FXAA.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_fxaa_source_;
  // Storage image descriptors for the apply-gamma compute pass destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_destination_storage_;
  // Separate storage image descriptors for the FXAA compute destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_fxaa_destination_storage_;

  VkSampler swap_sampler_linear_clamp_ = VK_NULL_HANDLE;

  VkPipelineLayout swap_apply_gamma_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout swap_apply_gamma_compute_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout swap_fxaa_pipeline_layout_ = VK_NULL_HANDLE;
  // Has no dependencies on specific pipeline stages on both ends to simplify
  // use in different scenarios with different pipelines - use explicit barriers
  // for synchronization.
  VkRenderPass swap_apply_gamma_render_pass_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_fxaa_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_fxaa_extreme_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout resolve_downscale_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_downscale_pipeline_ = VK_NULL_HANDLE;
  VkBuffer resolve_downscale_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
  uint32_t resolve_downscale_buffer_size_ = 0;

  VkImage swap_fxaa_source_image_ = VK_NULL_HANDLE;
  VkDeviceMemory swap_fxaa_source_image_memory_ = VK_NULL_HANDLE;
  VkImageView swap_fxaa_source_image_view_ = VK_NULL_HANDLE;
  uint32_t swap_fxaa_source_image_width_ = 0;
  uint32_t swap_fxaa_source_image_height_ = 0;
  uint64_t swap_fxaa_source_image_submission_ = 0;
  VkPipelineStageFlags swap_fxaa_source_stage_mask_ = 0;
  VkAccessFlags swap_fxaa_source_access_mask_ = 0;
  VkImageLayout swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  // Fork (odisea): SMAA 1x en el swap, en el mismo punto que el FXAA (ver
  // vulkan/odisea_smaa_vulkan.cpp). OJO: estos miembros cambian el tamano de
  // la clase; tras tocarlos hay que recompilar el plugin entero.
  // No es fatal si falla: sin SMAA se sigue con FXAA o sin post-proceso.
  bool InitializeSmaa();
  void ShutdownSmaa();
  // Imagenes intermedias y texturas de consulta. Se preparan en IssueSwap antes
  // de decidir el destino de la rampa de gamma, para que ApplySmaa no falle.
  bool EnsureSmaaImages(uint32_t width, uint32_t height);
  bool UploadSmaaLookupImages();
  // La rampa de gamma escribe el color en smaa_color_: BeginSmaaColorWrite lo
  // pasa a GENERAL antes de ese dispatch, y ApplySmaa lo lee desde ahi hasta
  // dest_view (la imagen del presentador, ya en GENERAL).
  void BeginSmaaColorWrite();
  void ApplySmaa(VkImageView dest_view, uint32_t width, uint32_t height, uint32_t frame_index);
  struct SmaaImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  VkDescriptorSetLayout smaa_descriptor_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool smaa_descriptor_pool_ = VK_NULL_HANDLE;
  // [frame en vuelo][pasada]
  std::array<std::array<VkDescriptorSet, 3>, kMaxFramesInFlight> smaa_descriptor_sets_{};
  VkPipelineLayout smaa_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, 3> smaa_pipelines_{};
  VkImage smaa_area_image_ = VK_NULL_HANDLE;
  VkDeviceMemory smaa_area_image_memory_ = VK_NULL_HANDLE;
  VkImageView smaa_area_image_view_ = VK_NULL_HANDLE;
  VkImage smaa_search_image_ = VK_NULL_HANDLE;
  VkDeviceMemory smaa_search_image_memory_ = VK_NULL_HANDLE;
  VkImageView smaa_search_image_view_ = VK_NULL_HANDLE;
  bool smaa_lookup_uploaded_ = false;
  // Color tras la rampa de gamma, en el formato de la salida del presentador:
  // la rampa sin luma declara su destino como rgb10_a2, y escribir en una imagen
  // de otro formato (la fuente del FXAA es RGBA16F) da valores indefinidos.
  SmaaImage smaa_color_;
  SmaaImage smaa_edges_;
  SmaaImage smaa_weights_;
  uint32_t smaa_images_width_ = 0;
  uint32_t smaa_images_height_ = 0;
  uint64_t smaa_images_submission_ = 0;

  // Fork (odisea): interfaz a la resolucion de salida, fuera de la EDRAM, y
  // compuesta sobre la escena reescalada en el swap (ver
  // vulkan/odisea_hud_vulkan.cpp). OJO: cambian el tamano de la clase.
  struct HudImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  // No es fatal si falla: la interfaz se sigue dibujando en la EDRAM.
  bool InitializeHud();
  void ShutdownHud();
  bool GetHudOutputSize(uint32_t& width, uint32_t& height) const;
  bool EnsureHudImage(HudImage& hud_image, VkFormat format, uint32_t width, uint32_t height,
                      VkImageUsageFlags usage);
  // Imagen de la interfaz como destino (limpia en el primer dibujo tras el swap)
  // y entrada en su render pass.
  bool BeginHudDraw(uint32_t width, uint32_t height);
  // Imagen intermedia de la escena en GENERAL para el ultimo paso del swap.
  bool BeginHudSceneWrite(uint32_t width, uint32_t height);
  // Escena reescalada + interfaz a dest_view (la salida del presentador, en
  // GENERAL).
  // scene: la escena a reescalar (hud_scene_image_, o la salida de DLSS).
  void ComposeHud(VkImageView dest_view, uint32_t width, uint32_t height, uint32_t frame_index,
                  HudImage* scene = nullptr);
  VkDescriptorSetLayout hud_descriptor_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool hud_descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kMaxFramesInFlight> hud_descriptor_sets_{};
  VkPipelineLayout hud_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline hud_compose_pipeline_ = VK_NULL_HANDLE;
  HudImage hud_image_;
  HudImage hud_scene_image_;
  VulkanRenderTargetCache::Framebuffer hud_framebuffer_ = {};
  uint64_t hud_image_submission_ = 0;
  uint64_t hud_scene_image_submission_ = 0;
  bool hud_has_content_ = false;
  // Fork (odisea): NVIDIA DLSS para la escena 3D (ver vulkan/odisea_dlss_vulkan.cpp
  // y odisea_dlss.h). Solo con la interfaz aparte. OJO: cambian el tamano de la
  // clase.
  bool InitializeDlss();
  void ShutdownDlss();
  // Despues de un resolve: si es la profundidad de la escena, saca de la EDRAM
  // la profundidad y los vectores de movimiento para DLSS.
  void DlssOnResolve();
  // En el swap, con hud_scene_image_ ya escrita: la escena reconstruida a
  // width x height, o nullptr si DLSS no esta listo este fotograma.
  HudImage* DlssEvaluate(uint32_t scene_width, uint32_t scene_height, uint32_t width,
                         uint32_t height);
  void DlssEndFrameWithoutScene();
  static void DlssCommandBufferCallback(void* context, VkCommandBuffer command_buffer);
  struct DlssImageRef {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
  };
  struct DlssCallbackRequest {
    VulkanCommandProcessor* processor;
    bool create;
    bool reset;
    bool depth_inverted;
    int32_t preset;
    uint32_t size[4];  // entrada x, y; salida x, y
    float jitter[2];
    DlssImageRef color, depth, motion, output;
  } dlss_request_ = {};
  bool dlss_available_ = false;
  bool dlss_frame_active_ = false;
  // Sesgo de mip de DLSS: el dibujo en curso lo quiere / la copia subida lo lleva.
  bool dlss_mip_bias_draw_ = false;
  bool dlss_mip_bias_uploaded_ = false;
  bool dlss_inputs_ready_ = false;
  bool dlss_reset_ = true;
  bool dlss_depth_inverted_ = false;
  bool dlss_feature_depth_inverted_ = false;
  int32_t dlss_feature_preset_ = 0;
  uint32_t dlss_inputs_size_[2] = {};
  uint32_t dlss_feature_size_[4] = {};
  void* dlss_parameters_ = nullptr;
  void* dlss_feature_ = nullptr;
  VkDescriptorSetLayout dlss_descriptor_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool dlss_descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kMaxFramesInFlight> dlss_descriptor_sets_{};
  VkPipelineLayout dlss_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline dlss_inputs_pipeline_ = VK_NULL_HANDLE;
  HudImage dlss_depth_image_;
  HudImage dlss_motion_image_;
  HudImage dlss_output_image_;

  // Rectangulo 16:9 de la interfaz para la tijera de UpdateDynamicState
  // (solo mientras se dibuja la interfaz).
  struct HudBox {
    bool active = false;
    float x = 0.0f, y = 0.0f, scale_x = 1.0f, scale_y = 1.0f;
    uint32_t width = 0, height = 0;
  } hud_box_;

  std::array<SwapFramebuffer, ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      swap_framebuffers_;

  // Pending pipeline barriers.
  std::vector<VkBufferMemoryBarrier> pending_barriers_buffer_memory_barriers_;
  std::vector<VkImageMemoryBarrier> pending_barriers_image_memory_barriers_;
  struct PendingBarrier {
    VkPipelineStageFlags src_stage_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;
    size_t buffer_memory_barriers_offset = 0;
    size_t image_memory_barriers_offset = 0;
  };
  std::vector<PendingBarrier> pending_barriers_;
  PendingBarrier current_pending_barrier_;

  // GPU-local scratch buffer.
  static constexpr VkDeviceSize kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  VkDeviceMemory scratch_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_buffer_size_ = 0;
  VkPipelineStageFlags scratch_buffer_last_stage_mask_ = 0;
  VkAccessFlags scratch_buffer_last_access_mask_ = 0;
  uint64_t scratch_buffer_last_usage_submission_ = 0;
  bool scratch_buffer_used_ = false;

  static constexpr uint32_t kMaxOcclusionQueries = 8192;
  VkQueryPool occlusion_query_pool_ = VK_NULL_HANDLE;
  VkBuffer occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  uint32_t occlusion_query_readback_memory_type_ = UINT32_MAX;
  VkDeviceSize occlusion_query_readback_memory_size_ = 0;
  uint8_t* occlusion_query_readback_mapping_ = nullptr;
  uint32_t occlusion_query_cursor_ = 0;
  bool occlusion_query_resources_available_ = false;
  struct ActiveOcclusionQuery {
    uint32_t sample_count_address = 0;
    uint32_t host_index = UINT32_MAX;
    bool valid = false;
  } active_occlusion_query_;
  // Fork (odisea): perfilador de tiempo de GPU.
  odisea::GpuProfiler gpu_profiler_;
  VkQueryPool gpu_profile_pool_ = VK_NULL_HANDLE;
  PFN_vkCmdWriteTimestamp gpu_profile_write_timestamp_ = nullptr;
  PFN_vkGetQueryPoolResults gpu_profile_get_results_ = nullptr;
  double gpu_profile_ms_per_tick_ = 0.0;
  uint64_t gpu_profile_tick_mask_ = UINT64_MAX;
  size_t gpu_profile_last_mark_size_ = 0;
  struct VertexBufferState {
    uint32_t address = UINT32_MAX;
    uint32_t size = UINT32_MAX;
  };
  std::array<VertexBufferState, 96> vertex_buffer_states_{};
  uint64_t vertex_buffers_in_sync_[2] = {};
  std::unordered_map<uint64_t, ReadbackBuffer> readback_buffers_;
  std::unordered_map<uint64_t, ReadbackBuffer> memexport_readback_buffers_;

  // The current dynamic state of the graphics pipeline bind point. Note that
  // binding any pipeline to the bind point with static state (even if it's
  // unused, like depth bias being disabled, but the values themselves still not
  // declared as dynamic in the pipeline) invalidates such dynamic state.
  VkViewport dynamic_viewport_;
  VkRect2D dynamic_scissor_;
  // Dynamic fixed-function depth bias, blend constants, stencil state are
  // applicable only to the render target implementations where they are
  // actually involved.
  float dynamic_depth_bias_constant_factor_;
  float dynamic_depth_bias_slope_factor_;
  float dynamic_blend_constants_[4];
  // The stencil values are pre-initialized (to D3D11_DEFAULT_STENCIL_*, and the
  // initial values for front and back are the same for portability subset
  // safety) because they're updated conditionally to avoid changing the back
  // face values when stencil is disabled and the primitive type is changed
  // between polygonal and non-polygonal.
  uint32_t dynamic_stencil_compare_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_compare_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_reference_front_ = 0;
  uint32_t dynamic_stencil_reference_back_ = 0;
  bool dynamic_viewport_update_needed_;
  bool dynamic_scissor_update_needed_;
  bool dynamic_depth_bias_update_needed_;
  bool dynamic_blend_constants_update_needed_;
  bool dynamic_stencil_compare_mask_front_update_needed_;
  bool dynamic_stencil_compare_mask_back_update_needed_;
  bool dynamic_stencil_write_mask_front_update_needed_;
  bool dynamic_stencil_write_mask_back_update_needed_;
  bool dynamic_stencil_reference_front_update_needed_;
  bool dynamic_stencil_reference_back_update_needed_;

  // Currently used samplers.
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>> current_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>> current_samplers_pixel_;

  // Cache render pass currently started in the command buffer with the
  // framebuffer. For dynamic rendering, current_render_pass_ is VK_NULL_HANDLE
  // but in_render_pass_ is true.
  VkRenderPass current_render_pass_;
  const VulkanRenderTargetCache::Framebuffer* current_framebuffer_;
  bool in_render_pass_ = false;

  // Currently bound graphics pipeline, either from the pipeline cache (with
  // potentially deferred creation - current_external_graphics_pipeline_ is
  // VK_NULL_HANDLE in this case) or a non-Xenos one
  // (current_guest_graphics_pipeline_ is VK_NULL_HANDLE in this case).
  // TODO(Triang3l): Change to a deferred compilation handle.
  VkPipeline current_guest_graphics_pipeline_;
  VkPipeline current_external_graphics_pipeline_;
  VkPipeline current_external_compute_pipeline_;

  // Pipeline layout of the current guest graphics pipeline.
  const PipelineLayout* current_guest_graphics_pipeline_layout_;
  VkDescriptorBufferInfo
      current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferCount];
  // Whether up-to-date data has been written to constant (uniform) buffers, and
  // the buffer infos in current_constant_buffer_infos_ point to them.
  uint32_t current_constant_buffers_up_to_date_;
  VkDescriptorSet current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetCount];
  // Whether descriptor sets in current_graphics_descriptor_sets_ point to
  // up-to-date data.
  uint32_t current_graphics_descriptor_set_values_up_to_date_;
  // Whether the descriptor sets currently bound to the command buffer - only
  // low bits for the descriptor set layouts that remained the same are kept
  // when changing the pipeline layout. May be out of sync with
  // current_graphics_descriptor_set_values_up_to_date_, but should be ensured
  // to be a subset of it at some point when it becomes important; bits for
  // non-existent descriptor set layouts may also be set, but need to be ignored
  // when they start to matter.
  uint32_t current_graphics_descriptor_sets_bound_up_to_date_;
  static_assert(SpirvShaderTranslator::kDescriptorSetCount <=
                    sizeof(current_graphics_descriptor_set_values_up_to_date_) * CHAR_BIT,
                "Bit fields storing descriptor set validity must be large enough");
  static_assert(SpirvShaderTranslator::kDescriptorSetCount <=
                    sizeof(current_graphics_descriptor_sets_bound_up_to_date_) * CHAR_BIT,
                "Bit fields storing descriptor set validity must be large enough");

  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4];
  uint64_t current_float_constant_map_pixel_[4];

  // System shader constants.
  SpirvShaderTranslator::SystemConstants system_constants_;

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;
};

}  // namespace rex::graphics::vulkan
