#pragma once
// OPAD uses ordered JSON everywhere so that serialised ops have a stable, human-predictable key order.
#include <nlohmann/json.hpp>
namespace opad {
using json = nlohmann::ordered_json;
}

// Qt code (the app, opad-cli) that puts json in a signal or a QVariant registers it as a meta type, and Qt then asks
// whether `QDebug << json` compiles. json converts implicitly to almost anything, so that question tries every QDebug
// overload, among them Windows' `const MSG&`, an incomplete type there: GCC 16's type traits refuse it as a hard error.
// json is never printed through QDebug: answer "no" up front, before anything instantiates the question.
#if __has_include(<QtCore/qttypetraits.h>)
#include <QtCore/qttypetraits.h>
QT_BEGIN_NAMESPACE
class QDebug;
namespace QTypeTraits {
template <> struct has_ostream_operator<QDebug, nlohmann::ordered_json, void> : std::false_type {};
template <> struct has_ostream_operator<QDebug, nlohmann::json, void> : std::false_type {};
}  // namespace QTypeTraits
QT_END_NAMESPACE
#endif
