#pragma once
// OPAD uses ordered JSON everywhere so that serialised ops have a stable, human-predictable key order.
#include <nlohmann/json.hpp>
namespace opad {
using json = nlohmann::ordered_json;
}
