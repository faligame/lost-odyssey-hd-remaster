// Fork (odisea): lector del pack de texturas cifrado. Ver odisea_lopack.h.
#include "odisea_lopack.h"

#include <atomic>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <zstd.h>

#include <rex/logging.h>

namespace rex::graphics::odisea::lopack {

namespace {

constexpr uint32_t kHeaderSize = 96;
constexpr uint32_t kEntrySize = 32;
constexpr uint32_t kFormat = 1;

std::atomic<SecretProviderFn> g_secret_provider{nullptr};

bool HmacSha256(const uint8_t* key, size_t key_size, const uint8_t* msg, size_t msg_size, uint8_t out[32]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
    return false;
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, const_cast<PUCHAR>(key), ULONG(key_size), 0) >= 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(msg), ULONG(msg_size), 0) >= 0 &&
            BCryptFinishHash(hash, out, 32, 0) >= 0;
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

// HKDF-SHA256 (RFC 5869) de 32 bytes: un solo bloque de expansion.
bool Hkdf32(const uint8_t* salt, size_t salt_size, const uint8_t* ikm, size_t ikm_size, const char* info,
            uint8_t out[32]) {
  uint8_t prk[32];
  if (!HmacSha256(salt, salt_size, ikm, ikm_size, prk)) return false;
  uint8_t msg[64];
  const size_t info_size = std::strlen(info);
  if (info_size + 1 > sizeof(msg)) return false;
  std::memcpy(msg, info, info_size);
  msg[info_size] = 1;
  return HmacSha256(prk, 32, msg, info_size + 1, out);
}

// AES-256-GCM, una sola llamada. La clave se crea en cada llamada: asi es seguro entre hilos y el
// coste (microsegundos) no se nota frente a descifrar megabytes.
bool GcmDecrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_size,
                const uint8_t* cipher_with_tag, size_t size, uint8_t* plain_out) {
  if (size < 16) return false;
  static BCRYPT_ALG_HANDLE alg = [] {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return BCRYPT_ALG_HANDLE(nullptr);
    BCryptSetProperty(h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                      ULONG(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0);
    return h;
  }();
  if (!alg) return false;
  BCRYPT_KEY_HANDLE k = nullptr;
  if (BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, const_cast<PUCHAR>(key), 32, 0) < 0) return false;
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = const_cast<PUCHAR>(nonce);
  info.cbNonce = 12;
  info.pbAuthData = const_cast<PUCHAR>(aad);
  info.cbAuthData = ULONG(aad_size);
  info.pbTag = const_cast<PUCHAR>(cipher_with_tag + size - 16);
  info.cbTag = 16;
  ULONG written = 0;
  const NTSTATUS st = BCryptDecrypt(k, const_cast<PUCHAR>(cipher_with_tag), ULONG(size - 16), &info, nullptr, 0,
                                    plain_out, ULONG(size - 16), &written, 0);
  BCryptDestroyKey(k);
  return st >= 0 && written == size - 16;
}

bool ReadAt(void* handle, uint64_t offset, void* dst, size_t size) {
  uint8_t* p = static_cast<uint8_t*>(dst);
  while (size) {
    OVERLAPPED ov = {};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD got = 0;
    const DWORD chunk = DWORD(std::min<size_t>(size, 1u << 30));
    if (!ReadFile(handle, p, chunk, &got, &ov) || got == 0) return false;
    p += got;
    offset += got;
    size -= got;
  }
  return true;
}

}  // namespace

void SetSecretProvider(SecretProviderFn fn) { g_secret_provider.store(fn, std::memory_order_release); }
SecretProviderFn GetSecretProvider() { return g_secret_provider.load(std::memory_order_acquire); }

void Pack::Close() {
  if (handle_) CloseHandle(handle_);
  handle_ = nullptr;
  entries_.clear();
  std::memset(key_, 0, sizeof(key_));
}

bool Pack::Open(const std::filesystem::path& file, const uint8_t secret[32]) {
  Close();
  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  uint8_t header[kHeaderSize];
  if (!ReadAt(h, 0, header, kHeaderSize) || std::memcmp(header, "LOPK", 4) != 0) {
    CloseHandle(h);
    return false;
  }
  uint32_t format, content, edition, count;
  uint64_t index_offset, index_size;
  std::memcpy(&format, header + 4, 4);
  std::memcpy(&content, header + 8, 4);
  std::memcpy(&edition, header + 12, 4);
  std::memcpy(&index_offset, header + 48, 8);
  std::memcpy(&index_size, header + 56, 8);
  std::memcpy(&count, header + 76, 4);
  LARGE_INTEGER total;
  if (format != kFormat || !GetFileSizeEx(h, &total) || index_size < 16 ||
      index_size != uint64_t(count) * kEntrySize + 16 || index_offset + index_size > uint64_t(total.QuadPart)) {
    CloseHandle(h);
    return false;
  }
  if (!Hkdf32(header + 16, 32, secret, 32, "lopack key v1", key_)) {
    CloseHandle(h);
    return false;
  }
  std::vector<uint8_t> cipher(static_cast<size_t>(index_size));
  std::vector<uint8_t> plain(static_cast<size_t>(index_size) - 16);
  if (!ReadAt(h, index_offset, cipher.data(), cipher.size()) ||
      !GcmDecrypt(key_, header + 64, header, kHeaderSize, cipher.data(), cipher.size(), plain.data())) {
    // La etiqueta no cuadra: otra edicion, discos modificados o fichero manipulado.
    CloseHandle(h);
    std::memset(key_, 0, sizeof(key_));
    return false;
  }
  entries_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = plain.data() + size_t(i) * kEntrySize;
    uint64_t hash, offset;
    uint32_t csize, rsize;
    std::memcpy(&hash, e, 8);
    std::memcpy(&offset, e + 8, 8);
    std::memcpy(&csize, e + 16, 4);
    std::memcpy(&rsize, e + 20, 4);
    entries_[hash] = Entry{offset, csize, rsize, i, e[24], e[25]};
  }
  handle_ = h;
  content_version_ = content;
  (void)edition;
  return true;
}

bool Pack::KindOf(uint64_t hash, Kind& kind) const {
  auto it = entries_.find(hash);
  if (it == entries_.end()) return false;
  kind = Kind(it->second.kind);
  return true;
}

bool Pack::Read(uint64_t hash, std::vector<uint8_t>& out, Kind& kind) const {
  auto it = entries_.find(hash);
  if (it == entries_.end() || !handle_) return false;
  const Entry& e = it->second;
  if (e.cipher_size < 16 || e.raw_size > (256u << 20)) return false;
  std::vector<uint8_t> cipher(e.cipher_size);
  if (!ReadAt(handle_, e.offset, cipher.data(), cipher.size())) return false;
  uint8_t nonce[12] = {0, 0, 0, 0};
  const uint64_t index = e.index;
  std::memcpy(nonce + 4, &index, 8);
  uint8_t aad[13];
  std::memcpy(aad, &hash, 8);
  std::memcpy(aad + 8, &e.raw_size, 4);
  aad[12] = e.kind;
  std::vector<uint8_t> plain(e.cipher_size - 16);
  if (!GcmDecrypt(key_, nonce, aad, sizeof(aad), cipher.data(), cipher.size(), plain.data())) return false;
  if (e.codec == 1) {
    out.resize(e.raw_size);
    const size_t got = ZSTD_decompress(out.data(), out.size(), plain.data(), plain.size());
    if (ZSTD_isError(got) || got != e.raw_size) return false;
  } else {
    if (plain.size() != e.raw_size) return false;
    out = std::move(plain);
  }
  kind = Kind(e.kind);
  return true;
}

}  // namespace rex::graphics::odisea::lopack

// El exe da el secreto de los discos (ver shader_prewarm.cpp / texture_pack_secret en el exe).
extern "C" __declspec(dllexport) void odisea_SetTexturePackSecretProvider(
    rex::graphics::odisea::lopack::SecretProviderFn fn) {
  rex::graphics::odisea::lopack::SetSecretProvider(fn);
}
