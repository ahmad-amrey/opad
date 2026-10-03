#include "OpHistory.hpp"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "opad/cache.hpp"

namespace ophistory {
namespace {
constexpr qint64 kBig = qint64(1) << 20;  // read on its own and ended at #bodies; smaller ones go through one batch
constexpr char kCacheHeader[] = "opad-ops 1";

bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// A UUID (8-4-4-4-12 hex) at s[i], not inside a longer run of hex (a body key).
bool uuidAt(std::string_view s, size_t i) {
  if (i + 36 > s.size() || (i > 0 && hex(s[i - 1])) || (i + 36 < s.size() && hex(s[i + 36]))) return false;
  for (size_t k = 0; k < 36; ++k) {
    const bool dash = k == 8 || k == 13 || k == 18 || k == 23;
    if (dash ? s[i + k] != '-' : !hex(s[i + k])) return false;
  }
  return true;
}

std::string uuidAfter(std::string_view s, std::string_view key) {
  const size_t at = s.find(key);
  return at != std::string_view::npos && uuidAt(s, at + key.size()) ? std::string(s.substr(at + key.size(), 36)) : std::string();
}

void mentions(std::string_view s, Record& r) {
  for (size_t i = s.find('-'); i != std::string_view::npos; i = s.find('-', i + 1)) {  // the first dash of a UUID is its 9th character
    if (i < 8 || !uuidAt(s, i - 8)) continue;
    std::string id(s.substr(i - 8, 36));
    if (id != r.id && id != r.target && (r.mentions.empty() || r.mentions.back() != id)) r.mentions.push_back(std::move(id));
    i += 27;
  }
}

std::string utf8(const QString& s) { return s.toStdString(); }

// git with its output read as it comes, on a worker; ended when dropped.
class Stream {
 public:
  Stream(const git::Context& c, const QStringList& args, const QByteArray& input = {}) {
    m_p.setProgram(c.program);
    m_p.setArguments(QStringList{"-c", "core.quotepath=off"} + args);
    m_p.setWorkingDirectory(c.dir);
    m_p.setProcessEnvironment(c.environment(false));
    m_p.start();
    if (!m_p.waitForStarted(15000)) throw std::runtime_error("git could not be started: " + utf8(m_p.errorString()));
    if (!input.isEmpty()) m_p.write(input);
    m_p.closeWriteChannel();
  }
  ~Stream() {
    if (m_p.state() == QProcess::NotRunning) return;
    m_p.kill();
    m_p.waitForFinished(3000);
  }
  // More output, waiting for it up to 200 ms; empty and at end() once git is done and everything was read.
  QByteArray read() {
    if (m_p.bytesAvailable() == 0 && m_p.state() != QProcess::NotRunning) m_p.waitForReadyRead(200);
    return m_p.readAllStandardOutput();
  }
  bool end() { return m_p.state() == QProcess::NotRunning && m_p.bytesAvailable() == 0; }
  QString error() { return QString::fromUtf8(m_p.readAllStandardError()).trimmed(); }
 private:
  QProcess m_p;
};

bool loadCached(const QString& file, std::vector<Record>& out) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) return false;
  const QByteArray text = f.readAll();
  const QList<QByteArray> lines = text.split('\n');
  if (lines.isEmpty() || lines.front() != kCacheHeader) return false;
  for (qsizetype i = 1; i < lines.size(); ++i) {
    if (lines[i].isEmpty()) continue;
    const QList<QByteArray> f4 = lines[i].split('\t');
    if (f4.size() < 4) return false;
    Record r;
    r.id = f4[0].toStdString();
    r.type = f4[1].toStdString();
    r.target = f4[2].toStdString();
    for (const QByteArray& m : f4[3].split(' '))
      if (!m.isEmpty()) r.mentions.push_back(m.toStdString());
    out.push_back(std::move(r));
  }
  return true;
}

void saveCached(const QString& file, const std::vector<Record>& records) {
  QByteArray text(kCacheHeader);
  text += '\n';
  for (const Record& r : records) {
    text += QByteArray::fromStdString(r.id) + '\t' + QByteArray::fromStdString(r.type) + '\t' + QByteArray::fromStdString(r.target) + '\t';
    for (size_t i = 0; i < r.mentions.size(); ++i) text += (i ? " " : "") + QByteArray::fromStdString(r.mentions[i]);
    text += '\n';
  }
  QSaveFile f(file);  // a cache: a failed write only costs a read next time
  if (f.open(QIODevice::WriteOnly) && f.write(text) == text.size()) f.commit();
}
}  // namespace

void Reader::line(std::string_view l) {
  if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
  if (l.empty()) return;
  if (l[0] == '#') {
    if (l == "#ops") m_section = 1;
    else if (l == "#bodies") m_section = 2;
    return;
  }
  if (m_section != 1) return;
  if (l.substr(0, 7) == "{\"op\":\"") {  // a record's first line: op, id, ts, by, then its own fields (docs/format.md)
    Record r;
    const size_t end = l.find('"', 7);
    r.type = std::string(l.substr(7, end == std::string_view::npos ? 0 : end - 7));
    r.id = uuidAfter(l, "\"id\":\"");
    if (r.id.empty()) return;
    r.target = uuidAfter(l, "\"target\":\"");
    mentions(l, r);
    m_records.push_back(std::move(r));
  } else if (!m_records.empty()) {
    mentions(l, m_records.back());  // a multiline record goes on
  }
}

void Reader::feed(std::string_view chunk) {
  while (!done() && !chunk.empty()) {
    const size_t nl = chunk.find('\n');
    if (nl == std::string_view::npos) {
      m_pending.append(chunk);
      return;
    }
    if (m_pending.empty()) line(chunk.substr(0, nl));
    else {
      m_pending.append(chunk.substr(0, nl));
      line(m_pending);
      m_pending.clear();
    }
    chunk.remove_prefix(nl + 1);
  }
}

std::vector<Record> Reader::finish() {
  if (!m_pending.empty() && !done()) line(m_pending);
  m_pending.clear();
  return std::move(m_records);
}

const Provenance* Index::find(const std::string& op) const {
  const auto it = ops.find(op);
  return it == ops.end() ? nullptr : &it->second;
}

std::vector<int> Index::touching(const std::vector<std::string>& ids) const {
  std::vector<int> out;
  for (const std::string& id : ids)
    if (const auto it = touched.find(id); it != touched.end()) out.insert(out.end(), it->second.begin(), it->second.end());
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

QString cacheFolder() { return QDir(QString::fromStdU16String(opad::cache_dir().u16string())).filePath("git-ops"); }

Index build(const git::Context& base, const QString& top, const QString& rel, const QString& cacheDir, const std::function<bool()>& cancelled,
            const std::function<void(int, int)>& progress) {
  Index ix;
  ix.top = top;
  ix.rel = rel;
  git::Context c = base;
  c.dir = top;
  git::RunOptions o;
  o.timeoutMs = 300000;
  o.optionalLocks = false;
  o.cancelled = cancelled;
  auto stop = [&] { return cancelled && cancelled(); };
  const git::Result head = git::run(c, {"rev-parse", "-q", "--verify", "HEAD^{commit}"}, o);
  if (!head.ok()) return ix;  // no commit yet
  ix.head = QString::fromLatin1(head.out).trimmed();
  // The commits that changed the document, oldest first.
  const git::Result log = git::run(c, {"log", "--topo-order", "--reverse", "-z", "--format=%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%s", ix.head, "--", rel}, o);
  if (!log.ok()) throw std::runtime_error(utf8(log.error()));
  for (const QByteArray& record : log.out.split('\0')) {
    const QList<QByteArray> f = record.trimmed().split('\x1f');
    if (f.size() < 6 || f[0].isEmpty()) continue;
    git::Commit k;
    k.hash = QString::fromLatin1(f[0]);
    k.shortHash = QString::fromLatin1(f[1]);
    k.author = QString::fromUtf8(f[2]);
    k.email = QString::fromUtf8(f[3]);
    k.date = QString::fromLatin1(f[4]);
    k.subject = QString::fromUtf8(f.mid(5).join('\x1f'));
    ix.commits.push_back(std::move(k));
  }
  if (ix.commits.empty() || stop()) return ix;
  // The document's blob in each.
  QByteArray ask;
  for (const git::Commit& k : ix.commits) ask += k.hash.toLatin1() + ':' + rel.toUtf8() + '\n';
  git::RunOptions check = o;
  check.input = ask;
  const git::Result blobs = git::run(c, {"cat-file", "--batch-check"}, check);
  if (!blobs.ok()) throw std::runtime_error(utf8(blobs.error()));
  const QList<QByteArray> answers = blobs.out.split('\n');
  std::vector<QByteArray> blobOf(ix.commits.size());
  std::unordered_map<std::string, qint64> sizes;
  for (size_t k = 0; k < ix.commits.size() && qsizetype(k) < answers.size(); ++k) {
    const QList<QByteArray> f = answers[qsizetype(k)].split(' ');
    if (f.size() == 3 && f[1] == "blob") {
      blobOf[k] = f[0];
      sizes[f[0].toStdString()] = f[2].toLongLong();
    }
  }
  ix.blobs = int(sizes.size());
  // Each blob's records: from the cache, else read from git (small ones in one batch, big ones on their own).
  std::unordered_map<std::string, std::vector<Record>> records;
  std::vector<QByteArray> small, big;
  QDir().mkpath(cacheDir);
  for (const auto& [oid, size] : sizes) {
    std::vector<Record> cached;
    if (loadCached(QDir(cacheDir).filePath(QString::fromStdString(oid)), cached)) records[oid] = std::move(cached);
    else (size < kBig ? small : big).push_back(QByteArray::fromStdString(oid));
  }
  const int total = int(small.size() + big.size());
  int done = 0;
  auto got = [&](const QByteArray& oid, std::vector<Record> r) {
    saveCached(QDir(cacheDir).filePath(QString::fromLatin1(oid)), r);
    records[oid.toStdString()] = std::move(r);
    ++ix.blobsRead;
    if (progress) progress(++done, total);
  };
  if (!small.empty()) {
    QByteArray input;
    for (const QByteArray& oid : small) input += oid + '\n';
    Stream s(c, {"cat-file", "--batch"}, input);
    QByteArray buf, oid;
    qint64 left = -1;  // of the blob being read (with the newline after it); -1: its header comes next
    Reader reader;
    for (size_t next = 0; next < small.size();) {
      if (stop()) return Index{};
      if (buf.isEmpty()) {
        if (s.end()) throw std::runtime_error("git cat-file --batch stopped early: " + utf8(s.error()));
        buf = s.read();
        continue;
      }
      if (left < 0) {
        const qsizetype nl = buf.indexOf('\n');
        if (nl < 0) {
          if (s.end()) throw std::runtime_error("git cat-file --batch stopped early");
          buf += s.read();
          continue;
        }
        const QList<QByteArray> f = buf.left(nl).split(' ');
        buf.remove(0, nl + 1);
        if (f.size() != 3) {  // "<oid> missing": nothing to read of it
          got(f.value(0), {});
          ++next;
          continue;
        }
        oid = f[0];
        left = f[2].toLongLong() + 1;
        reader = Reader();
        continue;
      }
      const qint64 take = std::min<qint64>(left, buf.size());
      if (!reader.done()) reader.feed(std::string_view(buf.constData(), size_t(take == left ? take - 1 : take)));
      ix.bytesRead += take;
      left -= take;
      buf.remove(0, take);
      if (left == 0) {
        got(oid, reader.finish());
        left = -1;
        ++next;
      }
    }
  }
  for (const QByteArray& oid : big) {  // ended once its ops are read: the bodies after them are most of the file
    Stream s(c, {"cat-file", "blob", QString::fromLatin1(oid)});
    Reader reader;
    while (!reader.done() && !s.end()) {
      if (stop()) return Index{};
      const QByteArray chunk = s.read();
      ix.bytesRead += chunk.size();
      reader.feed(std::string_view(chunk.constData(), size_t(chunk.size())));
    }
    got(oid, reader.finish());
  }
  // Who brought what, in history order.
  std::unordered_set<std::string> seen;
  for (int k = 0; k < int(ix.commits.size()); ++k) {
    const auto it = records.find(blobOf[size_t(k)].toStdString());
    if (it == records.end()) continue;
    for (const Record& r : it->second) {
      if (!seen.insert(r.id).second) continue;
      ix.ops[r.id].added = k;
      auto touch = [&ix, k](const std::string& id) {
        auto& at = ix.touched[id];
        if (at.empty() || at.back() != k) at.push_back(k);
      };
      touch(r.id);
      if (!r.target.empty()) touch(r.target);
      for (const std::string& m : r.mentions) touch(m);
      if (r.target.empty()) continue;
      Provenance& p = ix.ops[r.target];
      if (r.type == "edit") {
        if (p.lastEdit != k) ++p.edits;
        p.lastEdit = k;
      } else if (r.type == "delete") {
        p.deleted = k;
      }
    }
  }
  return ix;
}

}  // namespace ophistory
