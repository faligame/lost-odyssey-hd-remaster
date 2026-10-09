// lostodyssey - ReXGlue Recompiled Project
//
// Ver disc_sources.h. Los lectores de ISO y de paquetes GOD son los del SDK
// (DiscImageDevice, StfsContainerDevice); aqui solo se decide cual usar y se
// lee la cabecera del default.xex.

#include "disc_sources.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <span>
#include <string>

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

namespace lo {

using rex::X_STATUS;  // lo usa la macro X_STATUS_SUCCESS

namespace {

constexpr uint32_t kXexExecutionInfo = 0x00040006;

std::string FileNameLower(const std::filesystem::path& p) {
  const auto u8 = p.filename().u8string();
  std::string s(u8.begin(), u8.end());
  std::transform(s.begin(), s.end(), s.begin(),
                 [](char c) { return char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c); });
  return s;
}

std::filesystem::path Canonical(const std::filesystem::path& p) {
  std::error_code ec;
  auto c = std::filesystem::weakly_canonical(p, ec);
  return ec ? p : c;
}

// Execution info de la cabecera XEX2 (sin cifrar): media id, version, version
// base, title id, plataforma, tabla de ejecutable, disco, total de discos.
bool ParseXexHeader(std::span<const uint8_t> h, DiscSource& s) {
  auto be32 = [&](size_t p) -> uint32_t {
    if (p + 4 > h.size()) return 0;
    return (uint32_t(h[p]) << 24) | (uint32_t(h[p + 1]) << 16) | (uint32_t(h[p + 2]) << 8) | h[p + 3];
  };
  if (h.size() < 24 || std::memcmp(h.data(), "XEX2", 4) != 0) return false;
  const uint32_t headers = be32(20);
  for (uint32_t i = 0; i < headers && 32 + size_t(i) * 8 <= h.size(); ++i) {
    if (be32(24 + size_t(i) * 8) != kXexExecutionInfo) continue;
    const size_t info = be32(28 + size_t(i) * 8);
    if (info + 20 > h.size()) return false;
    s.media_id = be32(info);
    s.title_id = be32(info + 12);
    s.number = h[info + 18];
    s.count = h[info + 19];
    return s.number >= 1 && s.count >= s.number;
  }
  return false;
}

bool IsXContentHeader(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  char magic[4] = {};
  f.read(magic, 4);
  return f.gcount() == 4 && (std::memcmp(magic, "CON ", 4) == 0 || std::memcmp(magic, "LIVE", 4) == 0 ||
                             std::memcmp(magic, "PIRS", 4) == 0);
}

}  // namespace

const char* DiscSourceKindName(DiscSourceKind kind) {
  switch (kind) {
    case DiscSourceKind::kFolder: return "carpeta";
    case DiscSourceKind::kIso: return "ISO";
    case DiscSourceKind::kGod: return "GOD";
  }
  return "?";
}

// Entrada del dispositivo "unir carpetas": directorios propios y ficheros que delegan en la entrada real.
class UnionEntry : public rex::filesystem::Entry {
 public:
  UnionEntry(rex::filesystem::Device* device, rex::filesystem::Entry* parent, const std::string_view path,
             rex::filesystem::Entry* backing)
      : Entry(device, parent, path), backing_(backing) {
    if (backing) {
      attributes_ = backing->attributes();
      size_ = backing->size();
      allocation_size_ = backing->allocation_size();
      create_timestamp_ = backing->create_timestamp();
      access_timestamp_ = backing->access_timestamp();
      write_timestamp_ = backing->write_timestamp();
    } else {
      attributes_ = rex::filesystem::kFileAttributeDirectory;
    }
  }
  X_STATUS Open(uint32_t desired_access, rex::filesystem::File** out_file) override {
    return backing_ ? backing_->Open(desired_access, out_file) : X_STATUS_ACCESS_DENIED;
  }
  bool can_map() const override { return backing_ && backing_->can_map(); }
  std::unique_ptr<rex::memory::MappedMemory> OpenMapped(rex::memory::MappedMemory::Mode mode, size_t offset,
                                                        size_t length) override {
    return backing_ ? backing_->OpenMapped(mode, offset, length) : nullptr;
  }
  UnionEntry* AddChild(std::unique_ptr<UnionEntry> child) {
    UnionEntry* raw = child.get();
    children_.push_back(std::move(child));
    return raw;
  }
  bool is_directory() const { return (attributes_ & rex::filesystem::kFileAttributeDirectory) != 0; }

 private:
  rex::filesystem::Entry* backing_;
};

// Disco instalado como data\discN (lo propio del disco) + data\common (lo igual en varios discos). Para el
// juego es un unico disco: el arbol de entradas junta los dos y los ficheros de discN mandan sobre los de common.
class UnionDevice : public rex::filesystem::Device {
 public:
  UnionDevice(std::string_view mount_path, const std::filesystem::path& primary,
              const std::filesystem::path& common, bool read_only)
      : Device(mount_path),
        primary_(std::make_unique<rex::filesystem::HostPathDevice>(mount_path, primary, read_only)),
        common_(std::make_unique<rex::filesystem::HostPathDevice>(mount_path, common, read_only)),
        read_only_(read_only) {}

  bool Initialize() override {
    if (!primary_->Initialize() || !common_->Initialize()) return false;
    root_ = std::make_unique<UnionEntry>(this, nullptr, "", nullptr);
    for (rex::filesystem::Device* d : {static_cast<rex::filesystem::Device*>(primary_.get()),
                                       static_cast<rex::filesystem::Device*>(common_.get())}) {
      if (rex::filesystem::Entry* r = d->ResolvePath("")) Merge(root_.get(), r);
    }
    return true;
  }
  void Dump(rex::string::StringBuffer* buffer) override { primary_->Dump(buffer); }
  rex::filesystem::Entry* ResolvePath(const std::string_view path) override { return root_->ResolvePath(path); }
  bool is_read_only() const override { return read_only_; }
  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 128 * 1024; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  void Merge(UnionEntry* dst, rex::filesystem::Entry* src) {
    for (const auto& child : src->children()) {
      const std::string path = dst->path().empty() ? child->name() : dst->path() + "/" + child->name();
      auto* existing = static_cast<UnionEntry*>(dst->GetChild(child->name()));
      const bool is_dir = (child->attributes() & rex::filesystem::kFileAttributeDirectory) != 0;
      if (is_dir) {
        if (!existing) existing = dst->AddChild(std::make_unique<UnionEntry>(this, dst, path, nullptr));
        Merge(existing, child.get());
      } else if (!existing) {
        dst->AddChild(std::make_unique<UnionEntry>(this, dst, path, child.get()));
      }
    }
  }

  std::unique_ptr<rex::filesystem::HostPathDevice> primary_, common_;
  std::unique_ptr<UnionEntry> root_;
  std::string name_ = "LoUnion";
  bool read_only_;
};

std::unique_ptr<rex::filesystem::Device> CreateDiscDevice(const DiscSource& source,
                                                          std::string_view mount_path, bool read_only) {
  std::unique_ptr<rex::filesystem::Device> device;
  switch (source.kind) {
    case DiscSourceKind::kFolder:
      if (!source.common.empty()) {
        device = std::make_unique<UnionDevice>(mount_path, source.path, source.common, read_only);
      } else {
        device = std::make_unique<rex::filesystem::HostPathDevice>(mount_path, source.path, read_only);
      }
      break;
    case DiscSourceKind::kIso:
      device = std::make_unique<rex::filesystem::DiscImageDevice>(mount_path, source.path);
      break;
    case DiscSourceKind::kGod:
      device = std::make_unique<rex::filesystem::StfsContainerDevice>(mount_path, source.path);
      break;
  }
  if (!device || !device->Initialize()) return nullptr;
  return device;
}

DiscReader::DiscReader(std::unique_ptr<rex::filesystem::Device> device) : device_(std::move(device)) {}

DiscReader::~DiscReader() = default;

std::unique_ptr<DiscReader> DiscReader::Open(const DiscSource& source) {
  auto device = CreateDiscDevice(source, "\\Device\\LoDiscReader", true);
  if (!device) return nullptr;
  return std::unique_ptr<DiscReader>(new DiscReader(std::move(device)));
}

bool DiscReader::ReadFile(std::string_view name, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) {
  rex::filesystem::Entry* entry = device_->ResolvePath(name);
  if (!entry) return false;
  const uint64_t file_size = entry->size();
  if (offset > file_size) return false;
  size = std::min(size, file_size - offset);
  rex::filesystem::File* file = nullptr;
  if (entry->Open(uint32_t(rex::filesystem::FileAccess::kFileReadData), &file) != X_STATUS_SUCCESS || !file) {
    return false;
  }
  out.resize(size_t(size));
  size_t done = 0;
  bool ok = true;
  while (done < out.size()) {
    const size_t chunk = std::min<size_t>(out.size() - done, size_t(64) << 20);
    size_t read = 0;
    if (file->ReadSync(std::span<uint8_t>(out.data() + done, chunk), size_t(offset) + done, &read) !=
            X_STATUS_SUCCESS ||
        read == 0) {
      ok = false;
      break;
    }
    done += read;
  }
  file->Destroy();
  return ok;
}

namespace {
void ListEntries(rex::filesystem::Entry* dir, const std::string& prefix, std::vector<DiscFileInfo>& out) {
  for (const auto& child : dir->children()) {
    const std::string path = prefix.empty() ? child->name() : prefix + "/" + child->name();
    if (child->attributes() & rex::filesystem::kFileAttributeDirectory) {
      ListEntries(child.get(), path, out);
    } else {
      out.push_back({path, uint64_t(child->size())});
    }
  }
}
}  // namespace

bool DiscReader::SameAs(std::string_view name, const std::filesystem::path& other,
                        const std::atomic<bool>* cancel) {
  rex::filesystem::Entry* entry = device_->ResolvePath(name);
  if (!entry) return false;
  const uint64_t file_size = entry->size();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(other, ec) || std::filesystem::file_size(other, ec) != file_size) return false;
  rex::filesystem::File* file = nullptr;
  if (entry->Open(uint32_t(rex::filesystem::FileAccess::kFileReadData), &file) != X_STATUS_SUCCESS || !file) {
    return false;
  }
  std::ifstream local(other, std::ios::binary);
  std::vector<uint8_t> a(size_t(8) << 20), b(size_t(8) << 20);
  bool same = bool(local);
  uint64_t done = 0;
  while (same && done < file_size) {
    if (cancel && cancel->load()) {
      same = false;
      break;
    }
    const size_t chunk = size_t(std::min<uint64_t>(a.size(), file_size - done));
    size_t read = 0;
    if (file->ReadSync(std::span<uint8_t>(a.data(), chunk), size_t(done), &read) != X_STATUS_SUCCESS || read == 0) {
      same = false;
      break;
    }
    local.read(reinterpret_cast<char*>(b.data()), std::streamsize(read));
    if (size_t(local.gcount()) != read || std::memcmp(a.data(), b.data(), read) != 0) same = false;
    done += read;
  }
  file->Destroy();
  return same && done == file_size;
}

bool DiscReader::List(std::vector<DiscFileInfo>& out) {
  rex::filesystem::Entry* root = device_->ResolvePath("");
  if (!root) return false;
  ListEntries(root, {}, out);
  return !out.empty();
}

bool DiscReader::CopyFileTo(std::string_view name, const std::filesystem::path& dest, std::atomic<uint64_t>* progress,
                            const std::atomic<bool>* cancel) {
  rex::filesystem::Entry* entry = device_->ResolvePath(name);
  if (!entry) return false;
  const uint64_t file_size = entry->size();
  std::error_code ec;
  if (std::filesystem::is_regular_file(dest, ec) && std::filesystem::file_size(dest, ec) == file_size) {
    if (progress) *progress += file_size;
    return true;
  }
  rex::filesystem::File* file = nullptr;
  if (entry->Open(uint32_t(rex::filesystem::FileAccess::kFileReadData), &file) != X_STATUS_SUCCESS || !file) {
    return false;
  }
  std::filesystem::create_directories(dest.parent_path(), ec);
  std::filesystem::path part = dest;
  part += ".part";
  bool ok = true;
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    std::vector<uint8_t> buffer(size_t(8) << 20);
    uint64_t done = 0;
    while (ok && done < file_size) {
      if (cancel && cancel->load()) {
        ok = false;
        break;
      }
      const size_t chunk = size_t(std::min<uint64_t>(buffer.size(), file_size - done));
      size_t read = 0;
      if (file->ReadSync(std::span<uint8_t>(buffer.data(), chunk), size_t(done), &read) != X_STATUS_SUCCESS ||
          read == 0) {
        ok = false;
        break;
      }
      out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(read));
      if (!out) {
        ok = false;
        break;
      }
      done += read;
      if (progress) *progress += read;
    }
  }
  file->Destroy();
  if (!ok) {
    std::filesystem::remove(part, ec);
    return false;
  }
  std::filesystem::remove(dest, ec);
  std::filesystem::rename(part, dest, ec);
  return !ec;
}

std::optional<DiscSource> IdentifyDiscSource(const std::filesystem::path& input) {
  std::error_code ec;
  DiscSource source;
  std::filesystem::path p = input;
  if (std::filesystem::is_regular_file(p, ec) && FileNameLower(p) == "default.xex") {
    p = p.parent_path();
  }
  if (std::filesystem::is_directory(p, ec)) {
    const std::string name = FileNameLower(p);
    if (std::filesystem::is_regular_file(p / "default.xex", ec)) {
      source.kind = DiscSourceKind::kFolder;
    } else if (name.size() > 5 && name.ends_with(".data")) {
      // La carpeta .data de un GOD: la cabecera es el fichero hermano sin ".data".
      std::filesystem::path header = p.parent_path() / p.stem();
      if (!IsXContentHeader(header)) return std::nullopt;
      source.kind = DiscSourceKind::kGod;
      p = header;
    } else {
      return std::nullopt;
    }
  } else if (std::filesystem::is_regular_file(p, ec)) {
    std::filesystem::path data = p;
    data += ".data";
    if (FileNameLower(p).ends_with(".iso")) {
      source.kind = DiscSourceKind::kIso;
    } else if (IsXContentHeader(p) && std::filesystem::is_directory(data, ec)) {
      source.kind = DiscSourceKind::kGod;
    } else {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
  source.path = Canonical(p);
  if (source.kind == DiscSourceKind::kFolder) {
    // data\discN con data\common al lado: los ficheros comunes a varios discos viven alli.
    const std::string folder = FileNameLower(p);
    const std::filesystem::path common = p.parent_path() / "common";
    if (folder.rfind("disc", 0) == 0 && std::filesystem::is_directory(common, ec)) source.common = Canonical(common);
  }

  auto reader = DiscReader::Open(source);
  std::vector<uint8_t> head;
  if (!reader || !reader->ReadFile("default.xex", 0, 0x10000, head) || !ParseXexHeader(head, source)) {
    return std::nullopt;
  }
  return source;
}

std::vector<DiscSource> ScanDiscSources(const std::vector<std::filesystem::path>& roots, int depth) {
  // Carpetas que nunca contienen discos y pueden ser enormes.
  static const std::set<std::string> kSkip = {".git",   "generated", "out",     "build",
                                              "cache",  "logs",      "savedata", "saves",   "config", "textures",
                                              "dump",   "thirdparty", "shaders", "node_modules"};
  std::vector<DiscSource> found;
  std::set<std::filesystem::path> seen;
  auto consider = [&](const std::filesystem::path& p) {
    if (auto s = IdentifyDiscSource(p); s && seen.insert(s->path).second) found.push_back(*s);
  };
  for (const auto& root : roots) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) continue;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied |
            std::filesystem::directory_options::follow_directory_symlink,  // enlaces y uniones a carpetas de ISO
        ec);
    const std::filesystem::recursive_directory_iterator end{};
    for (; !ec && it != end; it.increment(ec)) {
      const std::filesystem::path& path = it->path();
      const std::string name = FileNameLower(path);
      std::error_code e;
      if (it->is_directory(e)) {
        if (it.depth() + 1 >= depth || kSkip.count(name) || name.ends_with(".data")) {
          it.disable_recursion_pending();
        }
        if (std::filesystem::is_regular_file(path / "default.xex", e)) {
          consider(path);
          it.disable_recursion_pending();  // un disco extraido no contiene otros
        }
      } else if (it->is_regular_file(e)) {
        if (name.ends_with(".iso")) {
          consider(path);
        } else if (!path.has_extension() && FileNameLower(path.parent_path()) == "00007000") {
          consider(path);  // GOD: <titulo>\00007000\<cabecera> + <cabecera>.data
        }
      }
    }
  }
  std::stable_sort(found.begin(), found.end(),
                   [](const DiscSource& a, const DiscSource& b) { return int(a.kind) < int(b.kind); });
  return found;
}

bool ExtractDiscFile(const DiscSource& source, std::string_view name, const std::filesystem::path& dest) {
  auto reader = DiscReader::Open(source);
  std::vector<uint8_t> data;
  if (!reader || !reader->ReadFile(name, 0, ~0ull, data)) return false;
  std::error_code ec;
  if (std::filesystem::is_regular_file(dest, ec) && std::filesystem::file_size(dest, ec) == data.size()) {
    return true;
  }
  std::filesystem::create_directories(dest.parent_path(), ec);
  std::filesystem::path part = dest;
  part += ".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) return false;
  }
  std::filesystem::rename(part, dest, ec);
  return !ec;
}

}  // namespace lo
