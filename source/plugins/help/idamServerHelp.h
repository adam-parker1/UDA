#ifndef UDA_PLUGIN_IDAMSERVERHELP_H
#define UDA_PLUGIN_IDAMSERVERHELP_H

#include <plugins/pluginStructs.h>

#if defined(_WIN32)
#  define LIBRARY_API __declspec(dllexport)
#else
#  define LIBRARY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define THISPLUGIN_VERSION                  1
#define THISPLUGIN_MAX_INTERFACE_VERSION    1
#define THISPLUGIN_DEFAULT_METHOD           "help"

LIBRARY_API int idamServerHelp(IDAM_PLUGIN_INTERFACE * idam_plugin_interface);

#ifdef __cplusplus
}
#endif

#endif // UDA_PLUGIN_IDAMSERVERHELP_H
