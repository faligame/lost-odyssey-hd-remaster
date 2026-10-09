/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/odisea_gpu_profiler.h>
#include <rex/graphics/shared_memory.h>
#include <rex/math.h>
#include <rex/memory.h>

// Fork (odisea): cada subida de memoria del guest para un dibujo cuesta dos
// barreras que paran la GPU (lectura -> copia -> lectura). Cuando hay que subir
// algo, se suben tambien todas las paginas que la CPU ha vuelto a escribir
// desde la ultima subida (buferes dinamicos que casi seguro usara un dibujo
// proximo), para pagar las barreras una vez por lote y no una por dibujo.
// DESACTIVADO por defecto: el usuario vio "explosiones de vertices" en partida
// con el lote activo (datos viejos o parciales en algun bufer); con
// clear_memory_page_state=false ya quedan pocas subidas y no hace falta.
// Causa sin encontrar (ver notas de memoria del 3-oct-2026).
REXCVAR_DEFINE_BOOL(odisea_upload_dirty_batch, false, "GPU",
                    "Al subir memoria del guest para un dibujo, subir tambien las paginas que la "
                    "CPU ha vuelto a escribir desde la ultima subida (menos barreras). En caliente.");
REXCVAR_DEFINE_INT32(odisea_upload_dirty_batch_mb, 16, "GPU",
                     "Tope en MB de paginas agrupadas de mas por cada subida.");

namespace rex::graphics {

namespace {
// Estado por proceso (solo hay una memoria compartida viva): asi no cambia la
// disposicion de la clase, cuya cabecera es la del SDK. Protegido por la
// region critica global de la memoria compartida.
std::vector<uint64_t> g_redirtied_pages;
std::atomic<uint64_t> g_upload_events{0};
std::atomic<uint64_t> g_upload_pages_requested{0};
std::atomic<uint64_t> g_upload_pages_speculative{0};
std::atomic<uint32_t> g_page_size_log2{12};
}  // namespace

namespace odisea {
void SharedMemoryUploadStats(uint64_t& events, uint64_t& pages_requested,
                             uint64_t& pages_speculative, uint32_t& page_size_log2) {
  events = g_upload_events.load(std::memory_order_relaxed);
  pages_requested = g_upload_pages_requested.load(std::memory_order_relaxed);
  pages_speculative = g_upload_pages_speculative.load(std::memory_order_relaxed);
  page_size_log2 = g_page_size_log2.load(std::memory_order_relaxed);
}
}  // namespace odisea

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  // Fork (odisea)
  g_redirtied_pages.assign(num_system_page_flags_, 0);
  g_page_size_log2.store(page_size_log2_, std::memory_order_relaxed);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));
  std::fill(system_page_flags_valid_and_gpu_written_.begin(),
            system_page_flags_valid_and_gpu_written_.end(), uint64_t(0));
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory.
  system_page_flags_valid_ = system_page_flags_valid_and_gpu_written_;
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    auto global_lock = global_critical_region_.Acquire();

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      system_page_flags_valid_[i] |= valid_bits;
      uint64_t& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~valid_bits);
    }
  }

  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  // Fork (Odisea): vector reutilizado por hilo; antes se asignaba y liberaba en cada llamada (varias
  // por dibujo), ~5 % del hilo de comandos en malloc/free.
  static thread_local std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;
  merged_ranges.clear();
  merged_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    auto global_lock = global_critical_region_.Acquire();
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = system_page_flags_valid_[i];
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  if (upload_ranges_.empty()) {
    return true;
  }

  // Fork (odisea): ya que hay que subir algo (y pagar las barreras), subir
  // tambien las paginas que la CPU ha vuelto a escribir desde la ultima subida.
  {
    uint64_t requested_pages = 0;
    for (const std::pair<uint32_t, uint32_t>& range : upload_ranges_) {
      requested_pages += range.second;
    }
    g_upload_events.fetch_add(1, std::memory_order_relaxed);
    g_upload_pages_requested.fetch_add(requested_pages, std::memory_order_relaxed);
    if (REXCVAR_GET(odisea_upload_dirty_batch) &&
        g_redirtied_pages.size() == num_system_page_flags_) {
      uint32_t budget_pages = uint32_t(
          (uint64_t(std::max(REXCVAR_GET(odisea_upload_dirty_batch_mb), 0)) << 20) >>
          page_size_log2_);
      std::vector<std::pair<uint32_t, uint32_t>> extra_ranges;
      uint32_t extra_pages = 0;
      {
        auto global_lock = global_critical_region_.Acquire();
        for (uint32_t i = 0; i < num_system_page_flags_ && extra_pages < budget_pages; ++i) {
          uint64_t bits = g_redirtied_pages[i];
          if (!bits) {
            continue;
          }
          // Las que ya han vuelto a ser validas no hacen falta.
          bits &= ~system_page_flags_valid_[i];
          while (bits && extra_pages < budget_pages) {
            uint32_t bit;
            rex::bit_scan_forward(bits, &bit);
            uint64_t shifted = bits >> bit;
            uint32_t run = (~shifted == 0) ? (64 - bit) : uint32_t(rex::tzcnt(~shifted));
            uint64_t run_mask = (run >= 64) ? UINT64_MAX : (((uint64_t(1) << run) - 1) << bit);
            bits &= ~run_mask;
            uint32_t page_start = (i << 6) + bit;
            if (!extra_ranges.empty() &&
                extra_ranges.back().first + extra_ranges.back().second == page_start) {
              extra_ranges.back().second += run;
            } else {
              extra_ranges.emplace_back(page_start, run);
            }
            extra_pages += run;
          }
          g_redirtied_pages[i] = bits;
        }
      }
      if (!extra_ranges.empty()) {
        // Fusionar con las pedidas (las dos listas van ordenadas por pagina).
        std::vector<std::pair<uint32_t, uint32_t>> merged;
        merged.reserve(upload_ranges_.size() + extra_ranges.size());
        auto push = [&merged](const std::pair<uint32_t, uint32_t>& range) {
          if (!merged.empty()) {
            std::pair<uint32_t, uint32_t>& last = merged.back();
            if (range.first <= last.first + last.second) {
              uint32_t end = std::max(last.first + last.second, range.first + range.second);
              last.second = end - last.first;
              return;
            }
          }
          merged.push_back(range);
        };
        size_t a = 0, b = 0;
        while (a < upload_ranges_.size() || b < extra_ranges.size()) {
          if (b >= extra_ranges.size() ||
              (a < upload_ranges_.size() && upload_ranges_[a].first <= extra_ranges[b].first)) {
            push(upload_ranges_[a++]);
          } else {
            push(extra_ranges[b++]);
          }
        }
        upload_ranges_.swap(merged);
        g_upload_pages_speculative.fetch_add(extra_pages, std::memory_order_relaxed);
      }
    }
  }

  return UploadRanges(upload_ranges_);
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  // Fork (odisea): apuntar las paginas validas subidas por la CPU que vuelven a
  // escribirse, para subirlas en lote con la siguiente subida.
  bool track_redirtied = REXCVAR_GET(odisea_upload_dirty_batch) &&
                         g_redirtied_pages.size() == num_system_page_flags_;
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (track_redirtied) {
      g_redirtied_pages[i] |= system_page_flags_valid_[i] & invalidate_bits &
                              ~system_page_flags_valid_and_gpu_written_[i];
    }
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
  }

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
