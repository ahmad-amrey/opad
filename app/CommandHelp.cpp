#include "CommandHelp.hpp"

#include "I18n.hpp"

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

// Translations: {"<id>": {"title": ..., "summary": ..., "details": ..., "requires": ..., "keywords": [...]}}.
void addTranslation(Registry& r, const QJsonObject& file) {
  for (auto it = file.begin(); it != file.end(); ++it) {
    if (it.key().startsWith('@') || !r.index.contains(it.key())) continue;
    const QJsonObject o = it.value().toObject();
    CommandHelp& h = r.records[r.index.value(it.key())];
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
  return text;
}

bool matches(const CommandHelp& h, const QString& query) {
  const QString haystack = h.title + ' ' + h.keywords.join(' ') + ' ' + h.summary;
  for (const QString& word : query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts))
    if (!haystack.contains(word, Qt::CaseInsensitive)) return false;
  return true;
}

QString tooltip(const QAction* a) {
  QString title = a->text();
  title.remove('&');
  const CommandHelp* h = find(a->objectName());
  if (h && !h->title.isEmpty()) title = h->title;
  QString text = "<b>" + title.toHtmlEscaped() + "</b>";
  if (!a->shortcut().isEmpty()) text += "&nbsp;&nbsp;(" + a->shortcut().toString(QKeySequence::NativeText).toHtmlEscaped() + ")";
  if (h && !h->summary.isEmpty()) text += "<br>" + h->summary.toHtmlEscaped();
  return "<qt>" + text + "</qt>";
}

}  // namespace help
