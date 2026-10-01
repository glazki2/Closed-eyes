#ifndef _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
#define _INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_

#include <ISmmPlugin.h>
#include <sh_vector.h>
#include "utlvector.h"
#include "ehandle.h"
#include <iserver.h>
#include <entity2/entitysystem.h>
#include "engine/igameeventsystem.h"
#include "igameevents.h"
#include "vector.h"
#include <deque>
#include <functional>
#include <utlstring.h>
#include <KeyValues.h>
#include "CGameRules.h"
#include "CCSPlayerController.h"
#include "gameevents.pb.h"
#include "module.h"
#include "funchook.h"
#include "include/menus.h"
#include "include/skinchanger_api.h"

class ghost final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	void AllPluginsLoaded();
	void HookGameEvents();
private:
	const char* GetAuthor();
	const char* GetName();
	const char* GetDescription();
	const char* GetURL();
	const char* GetLicense();
	const char* GetVersion();
	const char* GetDate();
	const char* GetLogTag();
private:
	void OnPostEvent(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients, INetworkMessageInternal* pEvent, const CNetMessage* pData, unsigned long nSize, NetChannelBufType_t bufType);
	bool OnFireEvent(IGameEvent* pEvent, bool bDontBroadcast);
	void OnGameFrame(bool simulating, bool bFirstTick, bool bLastTick);
	void OnCheckTransmit(CCheckTransmitInfo** ppInfoList, int infoCount, CBitVec<16384>& unionTransmitEdicts, CBitVec<16384>&, const Entity2Networkable_t** pNetworkables, const uint16* pEntityIndicies, int nEntities);
};

#endif //_INCLUDE_METAMOD_SOURCE_STUB_PLUGIN_H_
