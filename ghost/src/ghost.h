#ifndef _GHOST_PLUGIN_H_
#define _GHOST_PLUGIN_H_

#include <ISmmPlugin.h>
#include <igameevents.h>
#include "version_gen.h"

class GhostPlugin : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	void AllPluginsLoaded();

public:
	const char* GetAuthor() { return PLUGIN_AUTHOR; }
	const char* GetName() { return PLUGIN_DISPLAY_NAME; }
	const char* GetDescription() { return PLUGIN_DESCRIPTION; }
	const char* GetURL() { return PLUGIN_URL; }
	const char* GetLicense() { return PLUGIN_LICENSE; }
	const char* GetVersion() { return PLUGIN_FULL_VERSION; }
	const char* GetDate() { return __DATE__; }
	const char* GetLogTag() { return PLUGIN_LOGTAG; }
};

extern GhostPlugin g_GhostPlugin;

PLUGIN_GLOBALVARS();

#endif // _GHOST_PLUGIN_H_
