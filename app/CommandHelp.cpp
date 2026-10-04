#include "CommandHelp.hpp"

#include "I18n.hpp"
#include "KeyText.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {
struct Registry {
  QString language;
  QList<CommandHelp> records;
  QHash<QString, qsizetype> index;
  bool loaded = false;
};
Registry& registry() {
  static Registry r;
  return r;
}

QJsonObject readJson(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}

QStringList strings(const QJsonValue& v) {
  QStringList out;
  for (const QJsonValue& s : v.toArray())
    if (!s.toString().isEmpty()) out << s.toString();
  return out;
}

// English records: {"commands": [{"id": ..., "title": ..., ...}, ...]}; a later file overrides an earlier one by id.
void addEnglish(Registry& r, const QJsonObject& file) {
  for (const QJsonValue& v : file.value("commands").toArray()) {
    const QJsonObject o = v.toObject();
    const QString id = o.value("id").toString();
    if (id.isEmpty()) continue;
    if (!r.index.contains(id)) {
      r.index.insert(id, r.records.size());
      r.records.append({id, {}, {}, {}, {}, id, {}, true});
    }
    CommandHelp& h = r.records[r.index.value(id)];
    auto set = [&](const char* key, QString& field) { if (o.contains(key)) field = o.value(key).toString(); };
    set("title", h.title); set("summary", h.summary); set("details", h.details); set("requires", h.requirement); set("clip", h.clip);
    if (o.contains("keywords")) h.keywords = strings(o.value("keywords"));
    if (h.clip.isEmpty()) h.clip = id;
  }
}

// Translations: {"<id>": {"title": ..., "summary": ..., "details": ..., "requires": ..., "keywords": [...]}}. The English
// title joins the keywords: search finds a command by either name.
void addTranslation(Registry& r, const QJsonObject& file) {
  for (auto it = file.begin(); it != file.end(); ++it) {
    if (it.key().startsWith('@') || !r.index.contains(it.key())) continue;
    const QJsonObject o = it.value().toObject();
    CommandHelp& h = r.records[r.index.value(it.key())];
    if (!o.value("title").toString().isEmpty() && !h.keywords.contains(h.title)) h.keywords << h.title;  // the English name finds it too
    auto set = [&](const char* key, QString& field) { if (!o.value(key).toString().isEmpty()) field = o.value(key).toString(); };
    set("title", h.title); set("summary", h.summary); set("details", h.details); set("requires", h.requirement);
    for (const QString& k : strings(o.value("keywords"))) if (!h.keywords.contains(k)) h.keywords.prepend(k);
  }
}

// A record is translated when the translation file has every text field its English record has.
void markTranslated(Registry& r, const QJsonObject& english, const QJsonObject& translation) {
  for (const QJsonValue& v : english.value("commands").toArray()) {
    const QJsonObject o = v.toObject();
    const QJsonObject t = translation.value(o.value("id").toString()).toObject();
    bool ok = true;
    for (const QString& key : help::textFields())
      if (key == "keywords" ? !o.value(key).toArray().isEmpty() && t.value(key).toArray().isEmpty() : !o.value(key).toString().isEmpty() && t.value(key).toString().isEmpty()) ok = false;
    if (r.index.contains(o.value("id").toString())) r.records[r.index.value(o.value("id").toString())].translated = ok;
  }
}

void ensureLoaded() {
  if (!registry().loaded) help::load(i18n::current());
}

}  // namespace

namespace help {

QStringList textFields() { return {"title", "summary", "details", "requires", "keywords"}; }

void load(const QString& language, const QString& dir) {
  Registry& r = registry();
  r = Registry{};
  r.language = language;
  r.loaded = true;
  const QString base = dir.isEmpty() ? QStringLiteral(":/help") : dir;
  const QString local = QCoreApplication::applicationDirPath() + "/help";
  const QJsonObject english = readJson(base + "/commands.json");
  addEnglish(r, english);
  if (dir.isEmpty()) addEnglish(r, readJson(local + "/commands.json"));
  if (language.isEmpty() || language == "en") return;
  const QJsonObject translation = readJson(base + "/commands." + language + ".json");
  markTranslated(r, english, translation);
  addTranslation(r, translation);
  if (dir.isEmpty()) addTranslation(r, readJson(local + "/commands." + language + ".json"));
}

QString language() {
  ensureLoaded();
  return registry().language;
}

const CommandHelp* find(const QString& id) {
  ensureLoaded();
  const Registry& r = registry();
  const auto it = r.index.constFind(id);
  return it == r.index.constEnd() ? nullptr : &r.records[*it];
}

const QList<CommandHelp>& all() {
  ensureLoaded();
  return registry().records;
}

QString requirement(const CommandHelp& h, const QVariantMap& args) {
  QString text = h.requirement;
  for (auto it = args.begin(); it != args.end(); ++it) text.replace("{" + it.key() + "}", it.value().toString());
  return expand(text);
}

namespace {
const QRegularExpression& tokenPattern() {
  static const QRegularExpression re(R"(\{(key|press|fixed):([A-Za-z0-9_.]+)\})");
  return re;
}
}  // namespace

QStringList tokens(const QString& text) {
  QStringList out;
  if (!text.contains('{')) return out;
  for (const auto& m : tokenPattern().globalMatch(text)) out << m.captured(1) + ':' + m.captured(2);
  return out;
}

QString title(const QString& id) {
  if (const CommandHelp* h = find(id); h && !h->title.isEmpty()) return h->title;
  if (QAction* a = keys::action(id)) {
    QString label = a->text();
    return label.remove('&').remove(QString::fromUtf8("…")).remove("...").trimmed();
  }
  return id;
}

QString expand(const QString& text, bool* allBound) {
  if (allBound) *allBound = true;
  if (!text.contains('{')) return text;
  QString out;
  qsizetype last = 0;
  for (const auto& m : tokenPattern().globalMatch(text)) {
    const QString kind = m.captured(1), name = m.captured(2);
    qsizetype start = m.capturedStart(), end = m.capturedEnd();
    QString replacement;
    if (kind == "fixed") {
      replacement = keys::fixedText(name);
      if (replacement.isEmpty()) replacement = m.captured(0);  // an unknown name stays as written (the tests name it)
    } else if (const QString key = keys::text(name); !key.isEmpty()) {
      replacement = kind == "press" ? QCoreApplication::translate("help", "Press %1").arg(key) : key;
    } else {
      if (allBound) *allBound = false;
      if (kind == "press") {
        replacement = QCoreApplication::translate("help", "Choose %1").arg(title(name));
      } else {
        // "(token)": the brackets go too, with the space before them; a bare token names the command.
        qsizetype open = start, close = end;
        while (open > last && text[open - 1].isSpace()) --open;
        while (close < text.size() && text[close].isSpace()) ++close;
        if (open > last && text[open - 1] == '(' && close < text.size() && text[close] == ')') {
          start = open - 1;
          if (start > last && text[start - 1].isSpace()) --start;
          end = close + 1;
        } else {
          replacement = title(name);
        }
      }
    }
    out += text.mid(last, start - last) + replacement;
    last = end;
  }
  return out + text.mid(last);
}

bool matches(const CommandHelp& h, const QString& query) {
  const QString haystack = h.title + ' ' + h.keywords.join(' ') + ' ' + h.summary;
  bool all = true;
  for (const QString& word : query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts))
    if (!haystack.contains(word, Qt::CaseInsensitive)) all = false;
  if (all) return true;
  // The key it has now, typed as shown ("Ctrl+Alt+F") or as Qt writes it; spaces and case do not count.
  const QKeySequence key = keys::binding(h.id);
  if (key.isEmpty()) return false;
  auto squeeze = [](QString s) { return s.remove(QRegularExpression("\\s+")).toLower(); };
  const QString q = squeeze(query);
  return !q.isEmpty() && (q == squeeze(keys::plain(key)) || q == squeeze(key.toString(QKeySequence::PortableText)));
}

QString group(const QString& id) {
  const QString area = id.section('.', 0, 0);
  if (area == "file" || area == "files") return QCoreApplication::translate("help", "File");
  if (area == "edit") return QCoreApplication::translate("help", "Edit");
  if (area == "select") return QCoreApplication::translate("help", "Select");
  // The areas' commands go where their menus have them: version control in File, the timeline's and the 2D drawings'
  // switches in View, components and exploded views in Design; the Drawings workspace's commands have a group of their own.
  if (area == "vcs") return QCoreApplication::translate("help", "File");
  if (area == "view" || area == "nav" || area == "panel" || area == "workspace" || area == "timeline" || area == "drawing2d")
    return QCoreApplication::translate("help", "View");
  if (area == "inspect") return QCoreApplication::translate("help", "Inspect");
  if (area == "annotate") return QCoreApplication::translate("help", "Annotate");
  if (area == "design" || area == "assembly") return QCoreApplication::translate("help", "Design");
  if (area == "drawings") return QCoreApplication::translate("help", "Drawings");
  if (id.startsWith("sketch.c.") || id == "sketch.dimension" || id == "sketch.constraints" || id == "sketch.moreConstrain")
    return QCoreApplication::translate("help", "Sketch constraints");
  if (area == "sketch") return QCoreApplication::translate("help", "Sketch");
  if (area == "tools" || area == "help") return QCoreApplication::translate("help", "Tools and help");
  return QCoreApplication::translate("help", "Other");
}

QStringList areas() {
  QStringList out;
  for (const char* id : {"file.", "edit.", "select.", "view.", "inspect.", "annotate.", "design.", "drawings.", "sketch.", "sketch.c.", "tools.", "x."}) out << group(id);
  return out;
}

QString tooltip(const QAction* a) {
  QString title = a->text();
  title.remove('&');
  const CommandHelp* h = find(a->objectName());
  if (h && !h->title.isEmpty()) title = h->title;
  QString text = "<b>" + title.toHtmlEscaped() + "</b>";
  if (const QString key = keys::text(keys::binding(a)); !key.isEmpty()) text += "&nbsp;&nbsp;(" + key.toHtmlEscaped() + ")";
  if (h && !h->summary.isEmpty()) text += "<br>" + expand(h->summary).toHtmlEscaped();
  return "<qt>" + text + "</qt>";
}

}  // namespace help
