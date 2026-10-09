// Fork (odisea): perfilador de tiempo de GPU por categorias (ver la cabecera).
#include <rex/graphics/odisea_gpu_profiler.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(odisea_gpu_profile, false, "GPU/Debug",
                    "Mide el tiempo de GPU por categorias (opacos, profundidad, efectos, "
                    "subidas, resolves, presentacion) con marcas de tiempo de la tarjeta e "
                    "informa en el registro y en odisea_gpu_profile.txt. En caliente.");
REXCVAR_DEFINE_DOUBLE(odisea_gpu_profile_period, 5.0, "GPU/Debug",
                      "Segundos entre informes del perfilador de GPU.");

namespace rex::graphics::odisea {

namespace {

double NowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

const char* CatName(GpuProfileCat cat) {
  switch (cat) {
    case GpuProfileCat::kBase:
      return "base";
    case GpuProfileCat::kPrepIndex:
      return "prep: conversion de indices";
    case GpuProfileCat::kPrepEdram:
      return "prep: destinos de render (EDRAM)";
    case GpuProfileCat::kPrepTextures:
      return "prep: cargas de texturas";
    case GpuProfileCat::kPrepVertex:
      return "prep: subidas de memoria del guest (+barreras)";
    case GpuProfileCat::kPrepOther:
      return "prep: resto (barreras, constantes, descriptores)";
    case GpuProfileCat::kOpaque:
      return "dibujos opacos (con ps)";
    case GpuProfileCat::kBlend:
      return "dibujos con mezcla (efectos)";
    case GpuProfileCat::kDepthOnly:
      return "dibujos sin ps (profundidad, sombras)";
    case GpuProfileCat::kResolve:
      return "resolves";
    case GpuProfileCat::kSwap:
      return "presentacion (gamma, AA, copia)";
    case GpuProfileCat::kOther:
      return "otros";
    default:
      return "?";
  }
}

const char* CatShort(GpuProfileCat cat) {
  switch (cat) {
    case GpuProfileCat::kOpaque:
      return "opaco";
    case GpuProfileCat::kBlend:
      return "mezcla";
    case GpuProfileCat::kDepthOnly:
      return "sin ps";
    default:
      return "";
  }
}

}  // namespace

GpuProfiler::GpuProfiler() {
  entries_.resize(kQueryCount);
  read_buffer_.resize(kQueryCount);
  period_start_s_ = NowSeconds();
}

bool GpuProfiler::Enabled() { return REXCVAR_GET(odisea_gpu_profile); }

bool GpuProfiler::BeginSubmission(uint32_t& base_index, uint32_t& reserved_count) {
  recording_ = false;
  bool enabled = Enabled();
  if (enabled != was_enabled_) {
    was_enabled_ = enabled;
    ResetAccumulators();
    period_start_s_ = NowSeconds();
  }
  if (!enabled) {
    return false;
  }
  // Anillo: no pisar rangos de envios que aun no se han leido.
  if (cursor_ + kMinSubmissionQueries > kQueryCount) {
    cursor_ = 0;
  }
  uint32_t available;
  if (pending_.empty()) {
    available = kQueryCount - cursor_;
  } else {
    uint32_t oldest = pending_.front().first;
    if (oldest > cursor_) {
      available = oldest - cursor_;
    } else if (oldest < cursor_) {
      available = kQueryCount - cursor_;
    } else {
      available = 0;
    }
  }
  if (available < kMinSubmissionQueries) {
    return false;
  }
  range_first_ = cursor_;
  range_limit_ = available;
  range_count_ = 1;
  entries_[range_first_] = {0, GpuProfileCat::kBase};
  recording_ = true;
  base_index = range_first_;
  reserved_count = available;
  return true;
}

bool GpuProfiler::Mark(GpuProfileCat cat, uint64_t shader_hash, uint32_t& index) {
  if (!recording_) {
    return false;
  }
  if (range_count_ >= range_limit_) {
    ++marks_dropped_;
    return false;
  }
  index = range_first_ + range_count_;
  entries_[index] = {shader_hash, cat};
  ++range_count_;
  return true;
}

void GpuProfiler::EndSubmission(uint64_t submission, uint32_t& first, uint32_t& count) {
  if (!recording_) {
    first = 0;
    count = 0;
    return;
  }
  recording_ = false;
  first = range_first_;
  count = range_count_;
  cursor_ = range_first_ + range_count_;
  if (cursor_ >= kQueryCount) {
    cursor_ = 0;
  }
  pending_.push_back({submission, first, count});
}

void GpuProfiler::NoteFrame() { ++frames_; }

void GpuProfiler::SubmissionsCompleted(uint64_t completed_submission, double ms_per_tick,
                                       uint64_t tick_mask, const ReadFn& read,
                                       const char* backend) {
  while (!pending_.empty()) {
    const Range& range = pending_.front();
    if (range.submission > completed_submission) {
      break;
    }
    if (range.count >= 2 && read(range.first, range.count, read_buffer_.data())) {
      for (uint32_t i = 1; i < range.count; ++i) {
        const Entry& entry = entries_[range.first + i];
        uint64_t delta = (read_buffer_[i] - read_buffer_[i - 1]) & tick_mask;
        double ms = double(delta) * ms_per_tick;
        if (ms < 0.0 || ms > 1000.0) {
          continue;  // marca invalida
        }
        cat_ms_[size_t(entry.cat)] += ms;
        ++cat_n_[size_t(entry.cat)];
        if (entry.hash) {
          ShaderStat& stat = shaders_[entry.hash];
          stat.ms += ms;
          ++stat.draws;
          stat.cat = entry.cat;
        }
      }
      ++submissions_read_;
    }
    pending_.pop_front();
  }
  MaybeReport(backend);
}

void GpuProfiler::ResetAccumulators() {
  for (double& v : cat_ms_) v = 0.0;
  for (uint64_t& v : cat_n_) v = 0;
  shaders_.clear();
  frames_ = 0;
  marks_dropped_ = 0;
  submissions_read_ = 0;
}

void GpuProfiler::MaybeReport(const char* backend) {
  // Version publica: sin informe (ni el fichero odisea_gpu_profile.txt) salvo que se pida con odisea_gpu_profile.
  if (!Enabled()) return;
  double now = NowSeconds();
  double period = std::max(REXCVAR_GET(odisea_gpu_profile_period), 1.0);
  if (now - period_start_s_ < period) {
    return;
  }
  double elapsed = now - period_start_s_;
  period_start_s_ = now;
  if (!frames_) {
    ResetAccumulators();
    return;
  }
  double frames = double(frames_);
  double total = 0.0;
  for (size_t c = 1; c < size_t(GpuProfileCat::kCount); ++c) {
    total += cat_ms_[c];
  }
  std::string report = fmt::format(
      "odisea gpu-profile [{}] {:.1f} s: {} fotogramas ({:.1f} fps), GPU ocupada {:.2f} "
      "ms/fotograma ({} envios leidos{})\n",
      backend, elapsed, frames_, frames / elapsed, total / frames, submissions_read_,
      marks_dropped_ ? fmt::format(", {} marcas perdidas", marks_dropped_) : std::string());
  for (size_t c = 1; c < size_t(GpuProfileCat::kCount); ++c) {
    double ms = cat_ms_[c] / frames;
    report += fmt::format("  {:7.3f} ms/fot {:5.1f}%  {:7.1f}/fot  {}\n", ms,
                          total > 0.0 ? 100.0 * cat_ms_[c] / total : 0.0,
                          double(cat_n_[c]) / frames, CatName(GpuProfileCat(c)));
  }
  {
    uint64_t events, pages_requested, pages_speculative;
    uint32_t page_size_log2;
    SharedMemoryUploadStats(events, pages_requested, pages_speculative, page_size_log2);
    double kb_per_page = double(uint64_t(1) << page_size_log2) / 1024.0;
    report += fmt::format(
        "  subidas de memoria del guest: {:.1f} eventos/fot, {:.0f} KB/fot pedidos, {:.0f} KB/fot "
        "agrupados de mas\n",
        double(events - upload_events_last_) / frames,
        double(pages_requested - upload_pages_requested_last_) * kb_per_page / frames,
        double(pages_speculative - upload_pages_speculative_last_) * kb_per_page / frames);
    upload_events_last_ = events;
    upload_pages_requested_last_ = pages_requested;
    upload_pages_speculative_last_ = pages_speculative;
  }
  std::vector<std::pair<uint64_t, ShaderStat>> top(shaders_.begin(), shaders_.end());
  std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
  size_t shown = std::min<size_t>(top.size(), 16);
  if (shown) {
    report += "  sombreadores mas caros (ms/fot, dibujos/fot):\n";
    size_t listed = 0;
    for (size_t i = 0; i < top.size() && listed < shown; ++i) {
      const auto& [hash, stat] = top[i];
      if (stat.cat == GpuProfileCat::kPrepTextures) {
        continue;
      }
      ++listed;
      report += fmt::format("    {:7.3f} ms  {:6.1f}  {:016X} {}\n", stat.ms / frames,
                            double(stat.draws) / frames, hash, CatShort(stat.cat));
    }
  }
  {
    size_t listed = 0;
    for (size_t i = 0; i < top.size() && listed < 12; ++i) {
      const auto& [hash, stat] = top[i];
      if (stat.cat != GpuProfileCat::kPrepTextures) {
        continue;
      }
      if (!listed) {
        report += "  cargas de texturas mas caras (ms/fot, cargas/fot):\n";
      }
      ++listed;
      report += fmt::format("    {:7.3f} ms  {:6.1f}  {}x{} fmt {}{}{}{} base 0x{:X}\n",
                            stat.ms / frames, double(stat.draws) / frames, (hash >> 48) & 0xFFFF,
                            (hash >> 32) & 0xFFFF, (hash >> 26) & 0x3F,
                            (hash & (uint64_t(1) << 25)) ? " resolve-escalado" : "",
                            (hash & (uint64_t(1) << 24)) ? " pack" : "",
                            (hash & (uint64_t(1) << 23)) ? " (copia)" : " (calculo)",
                            (hash & 0x7FFFFF) << 12);
    }
  }
  REXGPU_INFO("{}", report);
  if (FILE* f = std::fopen("odisea_gpu_profile.txt", "wb")) {
    std::fwrite(report.data(), 1, report.size(), f);
    std::fclose(f);
  }
  ResetAccumulators();
}

}  // namespace rex::graphics::odisea
