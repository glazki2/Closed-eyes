#include <stdio.h>
#include <string.h>
#include "ghost.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "cs_usercmd.pb.h"

ghost g_ghost;
PLUGIN_EXPOSE(ghost, g_ghost);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;
IGameEventSystem *g_gameEventSystem = nullptr;
IGameEventManager2 *g_pGameEventManager = nullptr;

IUtilsApi* g_pUtils;
IPlayersApi* g_pPlayers;
ISkinChangerApi* g_pSCApi;

#define MAX_SLOTS 64

#ifndef EF_NOSHADOW
#define EF_NOSHADOW 0x010
#endif
#ifndef EF_NODRAW
#define EF_NODRAW 0x020
#endif

#define GHOST_BLOCKED_BUTTONS (IN_ATTACK | IN_USE | IN_ATTACK2 | IN_RELOAD)

struct GhostState
{
	bool bActive;
	bool bSilentDeath;   // following player_death must not be broadcast
	float flCooldownUntil;
	uint64_t nSavedInteractsAs;
	bool bSavedCollision;
};

GhostState g_Ghost[MAX_SLOTS];
bool g_bRoundActive = false;

// config
std::string g_szModel;
float g_flCooldown = 5.0f;
bool g_bEnabled = true;
std::string g_szSigCanAcquire = "55 48 89 E5 41 57 41 56 41 55 49 89 CD 41 54 49 89 FC 53 48 89 F3 48 83 EC 78";
std::string g_szSigProcessUsercmds;
int g_iUserCmdPbOffset = 0x10;
int g_iUserCmdSize = 0;

funchook_t* m_CanAcquire = nullptr;
funchook_t* m_ProcessUsercmds = nullptr;
CTimer* g_pReapplyTimer = nullptr;

inline bool IsValidSlot(int iSlot) { return iSlot >= 0 && iSlot < MAX_SLOTS; }
inline bool IsGhost(int iSlot) { return IsValidSlot(iSlot) && g_Ghost[iSlot].bActive; }

// Returns slot of the player owning this pawn or -1 if the entity is not a player pawn.
int GetSlotFromPawnEntity(CEntityInstance* pEnt)
{
	if(!pEnt) return -1;
	const char* szClass = pEnt->GetClassname();
	if(!szClass || strcmp(szClass, "player")) return -1;
	CBasePlayerController* pController = ((CCSPlayerPawn*)pEnt)->GetController();
	if(!pController) return -1;
	int iSlot = pController->GetPlayerSlot();
	return IsValidSlot(iSlot) ? iSlot : -1;
}

///////////////////////////////////////
// Pickup / buy

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
	if(IsGhost(pController->GetPlayerSlot())) return AcquireResult::NotAllowedByMode;
	return UTIL_CanAcquire(pService, pItemView, eType, pLimit);
}

///////////////////////////////////////
// Input: strip +use / attacks before the game processes them (doors, buttons, pickups by E)

class CUserCmd;
void* (*UTIL_ProcessUsercmds)(CCSPlayerController* pController, CUserCmd* cmds, int numcmds, bool paused, float margin) = nullptr;

void StripButtons(CSGOUserCmdPB* pCmd)
{
	if(!pCmd->has_base()) return;
	CBaseUserCmdPB* pBase = pCmd->mutable_base();
	if(pBase->has_buttons_pb())
	{
		CInButtonStatePB* pButtons = pBase->mutable_buttons_pb();
		pButtons->set_buttonstate1(pButtons->buttonstate1() & ~(uint64)GHOST_BLOCKED_BUTTONS);
		pButtons->set_buttonstate2(pButtons->buttonstate2() & ~(uint64)GHOST_BLOCKED_BUTTONS);
		pButtons->set_buttonstate3(pButtons->buttonstate3() & ~(uint64)GHOST_BLOCKED_BUTTONS);
	}
	for(int i = 0; i < pBase->subtick_moves_size(); i++)
	{
		CSubtickMoveStep* pStep = pBase->mutable_subtick_moves(i);
		if(pStep->button() & GHOST_BLOCKED_BUTTONS)
			pStep->set_button(0);
	}
	pCmd->clear_attack1_start_history_index();
	pCmd->clear_attack2_start_history_index();
}

void* ProcessUsercmds(CCSPlayerController* pController, CUserCmd* cmds, int numcmds, bool paused, float margin)
{
	if(pController && IsGhost(pController->GetPlayerSlot()))
	{
		int iStride = g_iUserCmdSize > 0 ? g_iUserCmdSize : (int)(g_iUserCmdPbOffset + sizeof(CSGOUserCmdPB) + 0x38);
		for(int i = 0; i < numcmds; i++)
		{
			CSGOUserCmdPB* pCmd = (CSGOUserCmdPB*)((uint8*)cmds + i * iStride + g_iUserCmdPbOffset);
			StripButtons(pCmd);
		}
	}
	return UTIL_ProcessUsercmds(pController, cmds, numcmds, paused, margin);
}

///////////////////////////////////////
// Hooks

SH_DECL_HOOK8_void(IGameEventSystem, PostEventAbstract, SH_NOATTRIB, 0, CSplitScreenSlot, bool, int, const uint64*, INetworkMessageInternal*, const CNetMessage*, unsigned long, NetChannelBufType_t)
SH_DECL_HOOK2(IGameEventManager2, FireEvent, SH_NOATTRIB, 0, bool, IGameEvent*, bool);

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

// Footsteps / jump / land sounds of a ghost are not sent to anyone else.
void ghost::OnPostEvent(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients, INetworkMessageInternal* pEvent, const CNetMessage* pData, unsigned long nSize, NetChannelBufType_t bufType)
{
	NetMessageInfo_t *info = pEvent->GetNetMessageInfo();
	if(!info || !info->m_pBinding) RETURN_META(MRES_IGNORED);
	const char* szName = info->m_pBinding->GetName();
	if(!szName || strcmp(szName, "CMsgSosStartSoundEvent")) RETURN_META(MRES_IGNORED);

	auto msg = const_cast<CNetMessage*>(pData)->ToPB<CMsgSosStartSoundEvent>();
	int iSlot = GetSlotFromPawnEntity(UTIL_GetEntityByIndex(msg->source_entity_index()));
	if(!IsGhost(iSlot)) RETURN_META(MRES_IGNORED);

	// keep the sound only for the ghost himself
	uint64* pClients = const_cast<uint64*>(clients);
	*pClients &= (1ull << iSlot);
	if(*pClients == 0) RETURN_META(MRES_SUPERCEDE);
	RETURN_META(MRES_HANDLED);
}

// Entering/leaving ghost mode must not look like a new life / death for other plugins and the kill feed.
bool ghost::OnFireEvent(IGameEvent* pEvent, bool bDontBroadcast)
{
	if(!pEvent) RETURN_META_VALUE(MRES_IGNORED, false);
	const char* szName = pEvent->GetName();
	if(!strcmp(szName, "player_death"))
	{
		int iSlot = pEvent->GetInt("userid");
		if(IsValidSlot(iSlot) && g_Ghost[iSlot].bSilentDeath)
		{
			g_Ghost[iSlot].bSilentDeath = false;
			g_pGameEventManager->FreeEvent(pEvent);
			RETURN_META_VALUE(MRES_SUPERCEDE, true);
		}
	}
	else if(!strcmp(szName, "player_spawn"))
	{
		int iSlot = pEvent->GetInt("userid");
		if(IsGhost(iSlot))
		{
			g_pGameEventManager->FreeEvent(pEvent);
			RETURN_META_VALUE(MRES_SUPERCEDE, true);
		}
	}
	RETURN_META_VALUE(MRES_IGNORED, false);
}

///////////////////////////////////////
// Ghost apply / remove

void ApplyGhost(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if(!pController) return;
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if(!pPawn) return;

	// no weapons (spawn loadout included)
	CCSPlayer_ItemServices* pItemServices = pPawn->m_pItemServices();
	if(pItemServices) pItemServices->RemoveWeapons();

	pPawn->m_bTakesDamage(false);

	// Intangible: knife/bullet/grenade traces and triggers ignore the pawn,
	// movement still collides with the world (m_nInteractsWith untouched).
	CCollisionProperty* pCollision = pPawn->m_pCollision();
	if(pCollision)
	{
		VPhysicsCollisionAttribute_t& attr = pCollision->m_collisionAttribute();
		if(!g_Ghost[iSlot].bSavedCollision)
		{
			g_Ghost[iSlot].nSavedInteractsAs = attr.m_nInteractsAs();
			g_Ghost[iSlot].bSavedCollision = true;
		}
		if(attr.m_nInteractsAs() != 0)
		{
			attr.m_nInteractsAs = 0;
			pPawn->CollisionRulesChanged();
		}
	}

	// Invisible, no shadow
	pPawn->m_flShadowStrength() = 0.0f;
	if((pPawn->m_fEffects() & (EF_NODRAW | EF_NOSHADOW)) != (EF_NODRAW | EF_NOSHADOW))
		pPawn->m_fEffects = pPawn->m_fEffects() | EF_NODRAW | EF_NOSHADOW;
	if(pPawn->m_nRenderMode() != kRenderNone)
		pPawn->m_nRenderMode = kRenderNone;
	if(pPawn->m_clrRender().a() != 0)
		pPawn->m_clrRender = Color(255, 255, 255, 0);

	// Dead for the game: round end counting, scoreboard, radar, chat/voice
	pPawn->m_lifeState() = LifeState_t::LIFE_DYING;
}

void RestorePawn(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if(!pController) return;
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if(!pPawn) return;

	if(g_Ghost[iSlot].bSavedCollision && pPawn->m_pCollision())
	{
		pPawn->m_pCollision()->m_collisionAttribute().m_nInteractsAs = g_Ghost[iSlot].nSavedInteractsAs;
		pPawn->CollisionRulesChanged();
	}
	pPawn->m_flShadowStrength() = 1.0f;
	pPawn->m_fEffects = pPawn->m_fEffects() & ~(uint32)(EF_NODRAW | EF_NOSHADOW);
	pPawn->m_nRenderMode = kRenderNormal;
	pPawn->m_clrRender = Color(255, 255, 255, 255);
	pPawn->m_bTakesDamage(true);
}

// Forget the ghost state without touching the pawn (disconnect, map change).
void ResetSlot(int iSlot)
{
	if(!IsValidSlot(iSlot)) return;
	g_Ghost[iSlot].bActive = false;
	g_Ghost[iSlot].bSavedCollision = false;
	g_Ghost[iSlot].bSilentDeath = false;
}

// Leave ghost mode: player is dead again, silently.
void RemoveGhost(int iSlot, bool bKill)
{
	if(!IsGhost(iSlot)) return;
	RestorePawn(iSlot);
	g_Ghost[iSlot].bActive = false;
	g_Ghost[iSlot].bSavedCollision = false;
	if(!bKill) return;

	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if(!pController) return;
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if(!pPawn) return;
	pPawn->m_lifeState() = LifeState_t::LIFE_ALIVE;
	g_Ghost[iSlot].bSilentDeath = true;
	g_pPlayers->CommitSuicide(iSlot, false, true);
	g_Ghost[iSlot].bSilentDeath = false;
}

void MakeGhost(int iSlot)
{
	g_Ghost[iSlot].bActive = true;
	g_Ghost[iSlot].bSavedCollision = false;
	g_Ghost[iSlot].flCooldownUntil = gpGlobals->curtime + g_flCooldown;
	g_pPlayers->Respawn(iSlot);

	g_pUtils->NextFrame([iSlot]() {
		if(!IsGhost(iSlot)) return;
		CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
		if(!pController) return;
		CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
		if(!pPawn) return;
		if(!g_szModel.empty()) g_pUtils->SetEntityModel(pPawn, g_szModel.c_str());
		ApplyGhost(iSlot);
	});
}

///////////////////////////////////////
// Commands

bool OnGhostCommand(int iSlot, const char* szContent)
{
	if(!IsValidSlot(iSlot) || !g_bEnabled) return false;
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if(!pController) return false;

	if(IsGhost(iSlot))
	{
		RemoveGhost(iSlot, true);
		return false;
	}

	int iTeam = pController->m_iTeamNum();
	if(iTeam != 2 && iTeam != 3) return false;
	if(!g_bRoundActive)
	{
		g_pUtils->PrintToChat(iSlot, " \x02[Ghost]\x01 Раунд ещё не начался или уже закончился");
		return false;
	}
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if(!pPawn || pPawn->IsAlive()) return false;
	if(gpGlobals->curtime < g_Ghost[iSlot].flCooldownUntil)
	{
		g_pUtils->PrintToChat(iSlot, " \x02[Ghost]\x01 Подожди %.0f сек.", g_Ghost[iSlot].flCooldownUntil - gpGlobals->curtime);
		return false;
	}
	MakeGhost(iSlot);
	return false;
}

bool OnUnGhostCommand(int iSlot, const char* szContent)
{
	if(IsGhost(iSlot)) RemoveGhost(iSlot, true);
	return false;
}

bool OnGhostOffCommand(int iSlot, const char* szContent)
{
	if(iSlot != -1) return false; // server console only
	g_bEnabled = !g_bEnabled;
	if(!g_bEnabled)
		for(int i = 0; i < MAX_SLOTS; i++) RemoveGhost(i, true);
	Msg("[Ghost] %s\n", g_bEnabled ? "enabled" : "disabled");
	return false;
}

///////////////////////////////////////
// Events

void RemoveAllGhosts(bool bKill)
{
	for(int i = 0; i < MAX_SLOTS; i++)
	{
		if(IsGhost(i)) RemoveGhost(i, bKill);
		ResetSlot(i);
	}
}

void OnRoundStart(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	g_bRoundActive = true;
	RemoveAllGhosts(false);
}

void OnRoundEnd(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	g_bRoundActive = false;
	RemoveAllGhosts(true);
}

void OnPlayerLeaveGhost(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	if(!IsValidSlot(iSlot)) return;
	if(!strcmp(szName, "player_disconnect"))
	{
		ResetSlot(iSlot);
		g_Ghost[iSlot].flCooldownUntil = 0.0f;
		return;
	}
	if(!strcmp(szName, "player_death"))
	{
		ResetSlot(iSlot);
		return;
	}
	// player_team
	if(IsGhost(iSlot)) RemoveGhost(iSlot, false);
	ResetSlot(iSlot);
}

// Other plugins (NoBlock, skins) touch the pawn on spawn: re-apply after them.
void OnPlayerSpawn(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	if(!IsGhost(iSlot)) return;
	g_pUtils->NextFrame([iSlot]() {
		if(IsGhost(iSlot)) ApplyGhost(iSlot);
	});
}

bool OnTakeDamagePre(int iSlot, CTakeDamageInfo *pInfo)
{
	if(IsGhost(iSlot)) return false;
	int iAttackerSlot = GetSlotFromPawnEntity(pInfo->m_hAttacker().Get());
	if(IsGhost(iAttackerSlot)) return false;
	return true;
}

///////////////////////////////////////
// Config

void LoadConfig()
{
	KeyValues* hKv = new KeyValues("Ghost");
	const char *pszPath = "addons/configs/ghost.ini";

	if (!hKv->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		g_pUtils->ErrorLog("[Ghost] Failed to load %s", pszPath);
		delete hKv;
		return;
	}

	g_szModel = hKv->GetString("model", "");
	g_flCooldown = hKv->GetFloat("cooldown", 5.0f);
	g_szSigCanAcquire = hKv->GetString("sig_canacquire", g_szSigCanAcquire.c_str());
	g_szSigProcessUsercmds = hKv->GetString("sig_processusercmds", "");
	g_iUserCmdPbOffset = hKv->GetInt("usercmd_pb_offset", 0x10);
	g_iUserCmdSize = hKv->GetInt("usercmd_size", 0);
	delete hKv;
}

bool OnSetGloveSkinCallback(int iSlot, int iTeam, int& iGlove)
{
	return !IsGhost(iSlot);
}

bool OnSetGloveCallback(int iSlot, int iTeam, int& iGlove)
{
	return !IsGhost(iSlot);
}

bool OnSetKnifeCallback(int iSlot, int iTeam, int& iKnife)
{
	return !IsGhost(iSlot);
}

///////////////////////////////////////
// Plugin

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

	g_SMAPI->AddListener( this, this );

	return true;
}

bool ghost::Unload(char *error, size_t maxlen)
{
	for(int i = 0; i < MAX_SLOTS; i++) RemoveGhost(i, false);

	SH_REMOVE_HOOK(IGameEventSystem, PostEventAbstract, g_gameEventSystem, SH_MEMBER(this, &ghost::OnPostEvent), false);
	if(g_pGameEventManager)
		SH_REMOVE_HOOK(IGameEventManager2, FireEvent, g_pGameEventManager, SH_MEMBER(this, &ghost::OnFireEvent), false);
	if(m_CanAcquire)
	{
		funchook_uninstall(m_CanAcquire, 0);
		funchook_destroy(m_CanAcquire);
	}
	if(m_ProcessUsercmds)
	{
		funchook_uninstall(m_ProcessUsercmds, 0);
		funchook_destroy(m_ProcessUsercmds);
	}
	if(g_pUtils)
	{
		if(g_pReapplyTimer) g_pUtils->RemoveTimer(g_pReapplyTimer);
		g_pUtils->ClearAllHooks(g_PLID);
	}
	ConVar_Unregister();

	return true;
}

void InstallDetours()
{
	DynLibUtils::CModule libserver(g_pSource2Server);

	UTIL_CanAcquire = libserver.FindPattern(g_szSigCanAcquire.c_str()).RCast< decltype(UTIL_CanAcquire) >();
	if (!UTIL_CanAcquire)
		g_pUtils->ErrorLog("[Ghost] CanAcquire signature not found: pickup/buy are not blocked");
	else
	{
		m_CanAcquire = funchook_create();
		funchook_prepare(m_CanAcquire, (void**)&UTIL_CanAcquire, (void*)CanAcquire);
		funchook_install(m_CanAcquire, 0);
	}

	if(g_szSigProcessUsercmds.empty())
		g_pUtils->ErrorLog("[Ghost] sig_processusercmds is empty: +use is not blocked");
	else
	{
		UTIL_ProcessUsercmds = libserver.FindPattern(g_szSigProcessUsercmds.c_str()).RCast< decltype(UTIL_ProcessUsercmds) >();
		if(!UTIL_ProcessUsercmds)
			g_pUtils->ErrorLog("[Ghost] ProcessUsercmds signature not found: +use is not blocked");
		else
		{
			m_ProcessUsercmds = funchook_create();
			funchook_prepare(m_ProcessUsercmds, (void**)&UTIL_ProcessUsercmds, (void*)ProcessUsercmds);
			funchook_install(m_ProcessUsercmds, 0);
		}
	}
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

	LoadConfig();
	InstallDetours();

	g_pGameEventManager = g_pUtils->GetGameEventManager();
	if(g_pGameEventManager)
		SH_ADD_HOOK(IGameEventManager2, FireEvent, g_pGameEventManager, SH_MEMBER(this, &ghost::OnFireEvent), false);

	g_pUtils->StartupServer(g_PLID, StartupServer);
	g_pUtils->MapStartHook(g_PLID, [](const char* szMap) {
		g_bRoundActive = false;
		for(int i = 0; i < MAX_SLOTS; i++) { ResetSlot(i); g_Ghost[i].flCooldownUntil = 0.0f; }
	});
	g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStart);
	g_pUtils->HookEvent(g_PLID, "round_end", OnRoundEnd);
	g_pUtils->HookEvent(g_PLID, "player_team", OnPlayerLeaveGhost);
	g_pUtils->HookEvent(g_PLID, "player_death", OnPlayerLeaveGhost);
	g_pUtils->HookEvent(g_PLID, "player_disconnect", OnPlayerLeaveGhost);
	g_pUtils->HookEvent(g_PLID, "player_spawn", OnPlayerSpawn);
	g_pUtils->HookOnTakeDamagePre(g_PLID, OnTakeDamagePre);
	g_pUtils->RegCommand(g_PLID, { "mm_redie", "mm_ghost", "css_redie", "css_ghost" }, { "!redie", "!ghost" }, OnGhostCommand);
	g_pUtils->RegCommand(g_PLID, { "mm_unghost", "css_unghost" }, { "!unghost" }, OnUnGhostCommand);
	g_pUtils->RegCommand(g_PLID, { "mm_ghost_toggle" }, {}, OnGhostOffCommand);
	g_pSCApi->OnSetGlove(g_PLID, OnSetGloveCallback);
	g_pSCApi->OnSetGloveSkin(g_PLID, OnSetGloveSkinCallback);
	g_pSCApi->OnSetKnife(g_PLID, OnSetKnifeCallback);

	// Game may reset flags (weapons given, NoBlock, collision rules): re-apply every second.
	g_pReapplyTimer = g_pUtils->CreateTimer(1.0f, []() {
		for(int i = 0; i < MAX_SLOTS; i++)
			if(IsGhost(i)) ApplyGhost(i);
		return 1.0f;
	});
}

///////////////////////////////////////
const char* ghost::GetLicense()
{
	return "GPL";
}

const char* ghost::GetVersion()
{
	return "2.0";
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
