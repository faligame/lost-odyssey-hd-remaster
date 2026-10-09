// lostodyssey - ReXGlue Recompiled Project
//
// Utilidades de red compartidas (descarga del pack de texturas, comprobacion de actualizaciones): GET con
// WinHTTP y rangos, lista de servidores permitidos y lectura de JSON plano.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include "lo_version.h"

namespace lo::http {

inline std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

inline std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

inline bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Servidores permitidos: nunca una URL cualquiera. El bucle local vale para pruebas.
inline bool HostAllowed(std::string host) {
  std::transform(host.begin(), host.end(), host.begin(), [](char c) { return char(std::tolower(c)); });
  return host == "archive.org" || EndsWith(host, ".archive.org") || host == "github.com" ||
         host == "raw.githubusercontent.com" || EndsWith(host, ".githubusercontent.com") ||
         EndsWith(host, ".r2.dev") || EndsWith(host, ".r2.cloudflarestorage.com") || host == "packs.faligame.tv" || host == "huggingface.co" ||
         EndsWith(host, ".hf.co") || host == "127.0.0.1" || host == "localhost";
}

// Una peticion GET con WinHTTP. sink recibe los bytes; devolver false la corta.
class Http {
 public:
  explicit Http(int connect_ms = 15000, int receive_ms = 60000) {
    session_ = WinHttpOpen(L"LostOdysseyHDRemaster/" LO_VERSION_WSTRING, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) WinHttpSetTimeouts(session_, connect_ms, connect_ms, 30000, receive_ms);
  }
  ~Http() {
    if (session_) WinHttpCloseHandle(session_);
  }
  bool ok() const { return session_ != nullptr; }

  // status: codigo HTTP; total_size: tamano completo del recurso (Content-Range o Content-Length).
  // to > 0: rango cerrado [from, to] (inclusive); 0 = hasta el final.
  bool Get(const std::string& url, uint64_t from, const std::function<bool(const uint8_t*, size_t)>& sink,
           int& status, uint64_t& total_size, std::string& error, uint64_t to = 0) {
    status = 0;
    total_size = 0;
    const std::wstring wurl = Wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
      error = "URL no válida";
      return false;
    }
    if (!HostAllowed(Narrow(host))) {
      error = "servidor no permitido: " + Narrow(host);
      return false;
    }
    std::wstring full_path = std::wstring(path, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength && uc.lpszExtraInfo) full_path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    HINTERNET connect = WinHttpConnect(session_, host, uc.nPort, 0);
    if (!connect) {
      error = "no se puede conectar";
      return false;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET req = WinHttpOpenRequest(connect, L"GET", full_path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    bool ok = false;
    if (req) {
      std::wstring headers;
      if (from > 0 || to > 0) {
        headers = L"Range: bytes=" + std::to_wstring(from) + L"-" + (to > 0 ? std::to_wstring(to) : L"") + L"\r\n";
      }
      if (WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                             headers.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(req, nullptr)) {
        // El servidor final (tras las redirecciones) tambien debe estar permitido.
        wchar_t final_url[2048] = {};
        DWORD size = sizeof(final_url);
        if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, final_url, &size)) {
          URL_COMPONENTS fc{};
          fc.dwStructSize = sizeof(fc);
          wchar_t fhost[256] = {};
          fc.lpszHostName = fhost;
          fc.dwHostNameLength = 255;
          if (WinHttpCrackUrl(final_url, 0, 0, &fc) && !HostAllowed(Narrow(fhost))) {
            error = "redirección a un servidor no permitido";
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(connect);
            return false;
          }
        }
        DWORD code = 0;
        size = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = int(code);
        wchar_t buf[128] = {};
        size = sizeof(buf);
        if (status == 206 &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size,
                                WINHTTP_NO_HEADER_INDEX)) {
          const std::string cr = Narrow(buf);  // "bytes a-b/total"
          const size_t slash = cr.find('/');
          if (slash != std::string::npos) total_size = std::strtoull(cr.c_str() + slash + 1, nullptr, 10);
        } else if (status == 200) {
          size = sizeof(buf);
          if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size,
                                  WINHTTP_NO_HEADER_INDEX)) {
            total_size = std::strtoull(Narrow(buf).c_str(), nullptr, 10);
          }
        }
        if (status == 200 || status == 206) {
          std::vector<uint8_t> chunk(1 << 20);
          ok = true;
          for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), DWORD(chunk.size()), &got)) {
              error = "conexión interrumpida";
              ok = false;
              break;
            }
            if (got == 0) break;
            if (!sink(chunk.data(), got)) break;
          }
        } else {
          error = "el servidor respondió " + std::to_string(status);
        }
      } else {
        error = "no hay conexión con el servidor";
      }
      WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(connect);
    return ok;
  }

 private:
  HINTERNET session_ = nullptr;
};

// --- JSON plano del manifiesto -------------------------------------------------------------

inline std::string JsonString(const std::string& json, const std::string& key) {
  const size_t k = json.find("\"" + key + "\"");
  if (k == std::string::npos) return {};
  size_t p = json.find(':', k);
  if (p == std::string::npos) return {};
  p = json.find('"', p);
  if (p == std::string::npos) return {};
  std::string out;
  for (++p; p < json.size() && json[p] != '"'; ++p) {
    if (json[p] == '\\' && p + 1 < json.size()) {
      ++p;
      out += json[p] == 'n' ? '\n' : json[p];  // \n y \" (las notas de la version llevan saltos de linea)
      continue;
    }
    out += json[p];
  }
  return out;
}

inline uint64_t JsonNumber(const std::string& json, const std::string& key) {
  const size_t k = json.find("\"" + key + "\"");
  if (k == std::string::npos) return 0;
  const size_t p = json.find(':', k);
  return p == std::string::npos ? 0 : std::strtoull(json.c_str() + p + 1, nullptr, 10);
}

inline std::vector<std::string> JsonStrings(const std::string& json, const std::string& key) {
  std::vector<std::string> out;
  const size_t k = json.find("\"" + key + "\"");
  if (k == std::string::npos) return out;
  size_t p = json.find('[', k);
  const size_t end = json.find(']', k);
  if (p == std::string::npos || end == std::string::npos) return out;
  while ((p = json.find('"', p)) != std::string::npos && p < end) {
    std::string s;
    for (++p; p < end && json[p] != '"'; ++p) {
      if (json[p] == '\\' && p + 1 < end) ++p;
      s += json[p];
    }
    out.push_back(std::move(s));
    ++p;
  }
  return out;
}

}  // namespace lo::http
