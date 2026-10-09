// lostodyssey - ReXGlue Recompiled Project
//
// SHA-256 con CNG de Windows (BCrypt): comprobacion de discos y descargas, y el secreto del pack
// de texturas.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace lo {

// Hash incremental. Uso: Sha256 h; h.Update(...); h.Final(out).
class Sha256 {
 public:
  Sha256() {
#ifdef _WIN32
    ok_ = BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
          BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) >= 0;
#endif
  }
  ~Sha256() {
#ifdef _WIN32
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
#endif
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  bool Update(const void* data, size_t size) {
#ifdef _WIN32
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (ok_ && size) {
      const ULONG chunk = ULONG(std::min<size_t>(size, 1u << 28));
      ok_ = BCryptHashData(hash_, const_cast<PUCHAR>(p), chunk, 0) >= 0;
      p += chunk;
      size -= chunk;
    }
#endif
    return ok_;
  }
  bool Final(uint8_t out[32]) {
#ifdef _WIN32
    ok_ = ok_ && BCryptFinishHash(hash_, out, 32, 0) >= 0;
#endif
    return ok_;
  }

 private:
#ifdef _WIN32
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
#endif
  bool ok_ = false;
};

inline bool Sha256Of(const void* data, size_t size, uint8_t out[32]) {
  Sha256 h;
  return h.Update(data, size) && h.Final(out);
}

inline std::string HexOf(const uint8_t* data, size_t size) {
  static const char* kDigits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < size; ++i) {
    s += kDigits[data[i] >> 4];
    s += kDigits[data[i] & 15];
  }
  return s;
}

}  // namespace lo
