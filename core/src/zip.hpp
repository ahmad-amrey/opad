#pragma once
// A zip archive read from memory: its directory, and each member inflated on demand (zlib). 3MF files are zips of XML
// parts (formats.cpp), and so are slicer project files whose print settings a printed-part study reads (sim/printing.cpp).
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

#include "opad/util.hpp"

namespace opad::detail {

class Zip {
 public:
  explicit Zip(std::string data) : m_data(std::move(data)) {
    const size_t n = m_data.size();
    if (n < 22) throw Error("not a 3MF file (no zip directory)");
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > (n > 65557 ? n - 65557 : 0);)
      if (u32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) throw Error("not a 3MF file (no zip directory)");
    uint64_t count = u16(eocd + 10), offset = u32(eocd + 16);
    if ((count == 0xFFFF || offset == 0xFFFFFFFF) && eocd >= 20 && u32(eocd - 20) == 0x07064b50) {  // zip64
      const uint64_t z = u64(eocd - 20 + 8);
      if (z + 56 > n || u32(z) != 0x06064b50) throw Error("damaged 3MF zip directory");
      count = u64(z + 32);
      offset = u64(z + 48);
    }
    size_t at = static_cast<size_t>(offset);
    for (uint64_t i = 0; i < count; ++i) {
      if (at + 46 > n || u32(at) != 0x02014b50) throw Error("damaged 3MF zip directory");
      Entry e;
      e.method = u16(at + 10);
      e.csize = u32(at + 20);
      e.usize = u32(at + 24);
      const size_t nameLen = u16(at + 28), extraLen = u16(at + 30), commentLen = u16(at + 32);
      e.local = u32(at + 42);
      if (at + 46 + nameLen + extraLen > n) throw Error("damaged 3MF zip directory");
      std::string name = m_data.substr(at + 46, nameLen);
      for (size_t x = at + 46 + nameLen, xe = x + extraLen; x + 4 <= xe;) {  // zip64 sizes and offset
        const size_t id = u16(x), len = u16(x + 2);
        if (id == 0x0001) {
          size_t f = x + 4;
          if (e.usize == 0xFFFFFFFF && f + 8 <= xe) { e.usize = u64(f); f += 8; }
          if (e.csize == 0xFFFFFFFF && f + 8 <= xe) { e.csize = u64(f); f += 8; }
          if (e.local == 0xFFFFFFFF && f + 8 <= xe) e.local = u64(f);
        }
        x += 4 + len;
      }
      if (!name.empty() && name.front() == '/') name.erase(0, 1);
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      m_entries[name] = e;
      at += 46 + nameLen + extraLen + commentLen;
    }
  }
  bool has(std::string name) const { return m_entries.count(normal(std::move(name))) > 0; }
  std::string read(std::string name) const {
    auto it = m_entries.find(normal(name));
    if (it == m_entries.end()) throw Error("3MF part missing: " + name);
    const Entry& e = it->second;
    const size_t at = static_cast<size_t>(e.local);
    if (at + 30 > m_data.size() || u32(at) != 0x04034b50) throw Error("damaged 3MF part: " + name);
    const size_t start = at + 30 + u16(at + 26) + u16(at + 28);
    if (start + e.csize > m_data.size() || e.usize > (uint64_t(1) << 33)) throw Error("damaged 3MF part: " + name);
    if (e.method == 0) return m_data.substr(start, static_cast<size_t>(e.csize));
    if (e.method != 8) throw Error("3MF part uses an unsupported compression: " + name);
    std::string out(static_cast<size_t>(e.usize), '\0');
    z_stream s{};
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) throw Error("cannot decompress 3MF part: " + name);
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(m_data.data() + start));
    s.avail_in = static_cast<uInt>(e.csize);
    s.next_out = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    const int r = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (r != Z_STREAM_END) throw Error("cannot decompress 3MF part: " + name);
    out.resize(s.total_out);
    return out;
  }

 private:
  struct Entry { uint64_t csize = 0, usize = 0, local = 0; int method = 0; };
  static std::string normal(std::string name) {
    if (!name.empty() && name.front() == '/') name.erase(0, 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
  }
  uint32_t u16(size_t at) const { return uint8_t(m_data[at]) | uint32_t(uint8_t(m_data[at + 1])) << 8; }
  uint32_t u32(size_t at) const { return u16(at) | u16(at + 2) << 16; }
  uint64_t u64(size_t at) const { return u32(at) | uint64_t(u32(at + 4)) << 32; }
  std::string m_data;
  std::map<std::string, Entry> m_entries;
};

}  // namespace opad::detail
