/* OPAD native plugin ABI (F35). Plain C so any language can implement a plugin.
 *
 * A plugin is a shared library exporting
 *     int opad_plugin_init(const opad_host_api* host, opad_host* h, opad_plugin_info* out);
 * It must check host->abi_version, fill in `out`, and register what it provides. Return 0 on success.
 * All strings are UTF-8 JSON. Strings the host hands out are released with host->free_string; strings a
 * plugin hands back are allocated with malloc and released by the host with free.
 */
#ifndef OPAD_PLUGIN_H
#define OPAD_PLUGIN_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPAD_PLUGIN_ABI_VERSION 1u

#if defined(_WIN32)
#define OPAD_PLUGIN_EXPORT __declspec(dllexport)
#else
#define OPAD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef struct opad_host opad_host;         /* opaque */
typedef struct opad_document opad_document; /* opaque; valid only for the duration of a callback */

typedef int (*opad_command_fn)(opad_host* host, opad_document* doc, const char* args_json, char** out_json, void* user);
typedef int (*opad_export_fn)(opad_host* host, opad_document* doc, const char* args_json, char** out_json, void* user);

typedef struct opad_host_api {
  uint32_t abi_version;
  /* Run any command of the command layer against `doc` (NULL for path-based commands). */
  int (*run_command)(opad_host* host, opad_document* doc, const char* name, const char* args_json, char** out_json);
  void (*free_string)(char* s);
  int (*register_command)(opad_host* host, const char* name, const char* description, opad_command_fn fn, void* user);
  int (*register_exporter)(opad_host* host, const char* format, opad_export_fn fn, void* user);
  void (*log)(opad_host* host, int level, const char* message);
} opad_host_api;

typedef struct opad_plugin_info {
  uint32_t abi_version; /* set to OPAD_PLUGIN_ABI_VERSION */
  const char* name;
  const char* version;
} opad_plugin_info;

typedef int (*opad_plugin_init_fn)(const opad_host_api* host, opad_host* h, opad_plugin_info* out);

#ifdef __cplusplus
}
#endif
#endif
