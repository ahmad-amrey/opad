#pragma once
// A zip archive read from memory: its directory, and each member inflated on demand (zlib). 3MF files are zips of XML
// parts (formats.cpp), and so are slicer project files whose print settings a printed-part study reads (sim/printing.cpp).
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

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
      if (u16(at + 4) >> 8 == 3) e.mode = u32(at + 38) >> 16;  // made on Unix: the file's mode bits
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
      m_order.emplace_back(name, e);  // as written (case kept): what an unpacking reads
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
    return inflated(it->second, name);
  }
  // Every member in the directory's order, its name as written (case kept; a folder's ends in '/'): for unpacking a whole
  // archive (sim/engine_packs.cpp). `mode`: the Unix mode bits when the archive was made on Unix, else 0.
  size_t count() const { return m_order.size(); }
  const std::string& name(size_t i) const { return m_order.at(i).first; }
  uint32_t mode(size_t i) const { return m_order.at(i).second.mode; }
  uint64_t size(size_t i) const { return m_order.at(i).second.usize; }
  std::string read(size_t i) const { return inflated(m_order.at(i).second, m_order.at(i).first); }

 private:
  struct Entry { uint64_t csize = 0, usize = 0, local = 0; int method = 0; uint32_t mode = 0; };
  std::string inflated(const Entry& e, const std::string& name) const {
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
  std::vector<std::pair<std::string, Entry>> m_order;
};

// A zip archive of the given members (name, bytes), each deflated, in that order (3MF export). No zip64: an archive past
// 4 GB is refused. The time stamps are 1980-01-01, so the same members make the same bytes.
inline std::string zip_archive(const std::vector<std::pair<std::string, std::string>>& members) {
  std::string out, directory;
  auto put16 = [](std::string& s, uint32_t v) { s.push_back(char(v & 0xFF)), s.push_back(char((v >> 8) & 0xFF)); };
  auto put32 = [&](std::string& s, uint64_t v) {
    if (v > 0xFFFFFFFFull) throw Error("the 3MF file would pass 4 GB");
    put16(s, uint32_t(v & 0xFFFF)), put16(s, uint32_t(v >> 16));
  };
  for (const auto& [name, data] : members) {
    z_stream z{};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) throw Error("cannot compress " + name);
    std::string packed(deflateBound(&z, static_cast<uLong>(data.size())), '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(packed.data());
    z.avail_out = static_cast<uInt>(packed.size());
    const int r = deflate(&z, Z_FINISH);
    packed.resize(z.total_out);
    deflateEnd(&z);
    if (r != Z_STREAM_END) throw Error("cannot compress " + name);
    const uint32_t crc = static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
    const uint64_t local = out.size();
    auto header = [&](std::string& s, bool central) {
      put32(s, central ? 0x02014b50 : 0x04034b50);
      if (central) put16(s, 20);  // made by
      put16(s, 20);               // needed
      put16(s, 0);                // flags
      put16(s, 8);                // deflate
      put16(s, 0);                // time
      put16(s, 0x21);             // date: 1980-01-01
      put32(s, crc);
      put32(s, packed.size());
      put32(s, data.size());
      put16(s, uint32_t(name.size()));
      put16(s, 0);  // extra
      if (central) {
        put16(s, 0), put16(s, 0), put16(s, 0);  // comment, disk, internal attributes
        put32(s, 0);                            // external attributes
        put32(s, local);
      }
      s += name;
    };
    header(out, false);
    out += packed;
    header(directory, true);
  }
  const uint64_t at = out.size();
  out += directory;
  put32(out, 0x06054b50);
  put16(out, 0), put16(out, 0);
  put16(out, uint32_t(members.size())), put16(out, uint32_t(members.size()));
  put32(out, directory.size());
  put32(out, at);
  put16(out, 0);
  return out;
}

}  // namespace opad::detail
