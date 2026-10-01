#include <stdio.h>
#include "ghost.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"

ghost g_ghost;
PLUGIN_EXPOSE(ghost, g_ghost);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;
IGameEventSystem *g_gameEventSystem = nullptr;

IUtilsApi* g_pUtils;
IPlayersApi* g_pPlayers;
ISkinChangerApi* g_pSCApi;

bool g_bRedie[64];
std::string g_szModel;

funchook_t* m_CanAcquire;

namespace AcquireResult
{
  enum Type
  {
    Allowed,
    InvalidItem,
    AlreadyOwned,
    AlreadyPurchased,
    ReachedGrenadeTypeLimit,
    ReachedGrenadeTotalLimit,
    NotAllowedByTeam,
    NotAllowedByMap,
    NotAllowedByMode,
    NotAllowedForPurchase,
    NotAllowedByProhibition,
  };
}

namespace AcquireMethod
{
  enum Type
  {
    PickUp,
    Buy,
  };
}

AcquireResult::Type (*UTIL_CanAcquire)(CPlayer_ItemServices* pService, CEconItemView* pItemView, AcquireMethod::Type eType, uint* pLimit) = nullptr;

AcquireResult::Type CanAcquire(CPlayer_ItemServices* pService, CEconItemView* pItemView, AcquireMethod::Type eType, uint* pLimit)
{
	CCSPlayerPawn* pPlayerPawn = pService->GetPawn();
	if(!pPlayerPawn) return UTIL_CanAcquire(pService, pItemView, eType, pLimit);
	CBasePlayerController* pController = pPlayerPawn->GetController();
	if(!pController) return UTIL_CanAcquire(pService, pItemView, eType, pLimit);
	int iSlot = pController->GetPlayerSlot();
	if(g_bRedie[iSlot]) return AcquireResult::NotAllowedByMode;
	return UTIL_CanAcquire(pService, pItemView, eType, pLimit);
}

SH_DECL_HOOK8_void(IGameEventSystem, PostEventAbstract, SH_NOATTRIB, 0, CSplitScreenSlot, bool, int, const uint64*, INetworkMessageInternal*, const CNetMessage*, unsigned long, NetChannelBufType_t)

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();
}

void ghost::OnPostEvent(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients, INetworkMessageInternal* pEvent, const CNetMessage* pData, unsigned long nSize, NetChannelBufType_t bufType)
{
    NetMessageInfo_t *info = pEvent->GetNetMessageInfo();

    if (info->m_MessageId == 208)
    {
        auto msg = const_cast<CNetMessage*>(pData)->ToPB<CMsgSosStartSoundEvent>();
        int iIndex = msg->source_entity_index();
		CCSPlayerPawn* pPlayerPawn = (CCSPlayerPawn*)UTIL_GetEntityByIndex(iIndex);
		if(!pPlayerPawn) return;
		CBasePlayerController* pPlayerController = pPlayerPawn->GetController();
		if(!pPlayerController) return;
		int iSlot = pPlayerController->GetPlayerSlot();
        if (g_pPlayers->IsFakeClient(iSlot)) return;
        if (g_bRedie[iSlot])
        {
			nClientCount = 0;
			*(uint64*)clients = 0;
            RETURN_META(MRES_SUPERCEDE);
        }
    }
}

bool ghost::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_gameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	SH_ADD_HOOK(IGameEventSystem, PostEventAbstract, g_gameEventSystem, SH_MEMBER(this, &ghost::OnPostEvent), false);

	DynLibUtils::CModule libserver(g_pSource2Server);

	UTIL_CanAcquire = libserver.FindPattern("55 48 89 E5 41 57 41 56 41 55 49 89 CD 41 54 49 89 FC 53 48 89 F3 48 83 EC 78").RCast< decltype(UTIL_CanAcquire) >();
	if (!UTIL_CanAcquire)
	{
		Msg("[%s] Failed to find function to get UTIL_CanAcquire", g_PLAPI->GetLogTag());
	}
	else
	{
		m_CanAcquire = funchook_create();
		funchook_prepare(m_CanAcquire, (void**)&UTIL_CanAcquire, (void*)CanAcquire);
		funchook_install(m_CanAcquire, 0);
	}

	g_SMAPI->AddListener( this, this );

	return true;
}

bool ghost::Unload(char *error, size_t maxlen)
{
	ConVar_Unregister();
	
	return true;
}

void RediePlayer(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if(!pController) return;
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if(!pPawn) return;
	CCSPlayer_ItemServices* pItemServices = pPawn->m_pItemServices();
	if(!pItemServices) return;
	pItemServices->RemoveWeapons();
	g_pPlayers->Respawn(iSlot);

	g_pUtils->NextFrame([iSlot]() {
		CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
		if(!pController) return;
		CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
		if(!pPawn) return;
		if(g_szModel[0]) g_pUtils->SetEntityModel(pPawn, g_szModel.c_str());

		pPawn->m_flShadowStrength() = 0.0f;

		g_pUtils->CreateTimer(0.25f, [iSlot](){
			CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
			if(!pController) return -1.0f;
			CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
			if(!pPawn) return -1.0f;
			pPawn->m_lifeState() = LifeState_t::LIFE_DYING;
			return -1.0f;
		});
	});
}

void UnRediePlayer(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if(!pController) return;
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if(!pPawn) return;

	g_pUtils->NextFrame([iSlot]() {
		CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
		if(!pController) return;
		CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
		if(!pPawn) return;
		pPawn->m_flShadowStrength() = 1.0f;
		pPawn->m_lifeState() = LifeState_t::LIFE_ALIVE;
		g_pPlayers->CommitSuicide(iSlot, false, true);
	});
}

bool OnRedieCommand(int iSlot, const char* szContent)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if(!pController) return false;
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if(!pPawn || pPawn->IsAlive()) return false;
	g_bRedie[iSlot] = !g_bRedie[iSlot];
	if(g_bRedie[iSlot]) {
		RediePlayer(iSlot);
	} else {
		UnRediePlayer(iSlot);
	}
	return false;
}

void OnRoundStart(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	for(int i = 0; i < 64; i++) {
		if(g_bRedie[i]) {
			CCSPlayerController* pController = CCSPlayerController::FromSlot(i);
			if(!pController) return;
			CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
			if(!pPawn) return;
			pPawn->m_flShadowStrength() = 1.0f;
		}
		g_bRedie[i] = false;
	}
}

void OnPlayerSpawn(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	g_pUtils->NextFrame([iSlot]() {
		CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
		if(!pController) return;
		CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
		if(!pPawn) return;
		pPawn->m_pCollision()->m_CollisionGroup = COLLISION_GROUP_DISSOLVING;
		pPawn->m_pCollision()->m_collisionAttribute().m_nCollisionGroup = COLLISION_GROUP_DISSOLVING;
		pPawn->CollisionRulesChanged();
		if(g_bRedie[iSlot]) {
			pPawn->m_bTakesDamage(false);
		}
	});
}

void OnPlayerTeamDeath(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	g_bRedie[iSlot] = false;
}

bool OnTakeDamagePre(int iSlot, CTakeDamageInfo *pInfo)
{
	if(g_bRedie[iSlot]) {
		return false;
	}
	CCSPlayerPawn* pAttackerPawn = (CCSPlayerPawn*)pInfo->m_hAttacker().Get();
    if (!pAttackerPawn) return true;
    if (!pAttackerPawn->m_hController()) return true;
    int iAttackerSlot = pAttackerPawn->m_hController()->GetEntityIndex().Get() - 1;
	if (iAttackerSlot < 0 || iAttackerSlot > 63) return true;
	if (g_bRedie[iAttackerSlot]) {
		return false;
	}
	return true;
} 

void LoadConfig()
{
	KeyValues* hKv = new KeyValues("Ghost");
	const char *pszPath = "addons/configs/ghost.ini";

	if (!hKv->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		g_pUtils->ErrorLog("[Ghost] Failed to load %s", pszPath);
		return;
	}

	g_szModel = hKv->GetString("model", "");
}

bool OnSetGloveSkinCallback(int iSlot, int iTeam, int& iGlove)
{
	if(g_bRedie[iSlot]) return false;
	return true;
}

bool OnSetGloveCallback(int iSlot, int iTeam, int& iGlove)
{
	if(g_bRedie[iSlot]) return false;
	return true;
}

void ghost::AllPluginsLoaded()
{
	char error[64];
	int ret;
	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pSCApi = (ISkinChangerApi *)g_SMAPI->MetaFactory(SkinChanger_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing SkinChanger system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pUtils->StartupServer(g_PLID, StartupServer);
	g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStart);
	g_pUtils->HookEvent(g_PLID, "player_team", OnPlayerTeamDeath);
	g_pUtils->HookEvent(g_PLID, "player_death", OnPlayerTeamDeath);
	g_pUtils->HookEvent(g_PLID, "player_spawn", OnPlayerSpawn);
	g_pUtils->HookOnTakeDamagePre(g_PLID, OnTakeDamagePre);
	g_pUtils->RegCommand(g_PLID, { "mm_redie", "mm_ghost", "css_redie", "css_ghost" }, { "!redie", "!ghost" }, OnRedieCommand);
	g_pSCApi->OnSetGlove(g_PLID, OnSetGloveCallback);
	g_pSCApi->OnSetGloveSkin(g_PLID, OnSetGloveSkinCallback);

	LoadConfig();
}

///////////////////////////////////////
const char* ghost::GetLicense()
{
	return "GPL";
}

const char* ghost::GetVersion()
{
	return "1.0";
}

const char* ghost::GetDate()
{
	return __DATE__;
}

const char *ghost::GetLogTag()
{
	return "ghost";
}

const char* ghost::GetAuthor()
{
	return "Pisex";
}

const char* ghost::GetDescription()
{
	return "ghost";
}

const char* ghost::GetName()
{
	return "Ghost";
}

const char* ghost::GetURL()
{
	return "https://discord.gg/g798xERK5Y";
}
