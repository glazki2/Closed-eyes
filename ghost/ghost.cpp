#include <stdio.h>
#include <string.h>
#include "ghost.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "cs_usercmd.pb.h"
#include "cchecktransmitinfo.h"

// Ghost (redie) for CS2.
// Based on Pisex's ghost/redie plugin. Ideas and signatures taken from:
//  - CS2Fixes (Source2ZE): CheckTransmit hiding, FindUseEntity, FireOutputInternal, ProcessUsercmds, CanAcquire
//  - exkludera-cssharp/redie: LIFE_DYING after 0.25s (no black screen), transparent pawn so no ragdoll on leave

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
#define CHAT_PREFIX " \x04[Ghost]\x01 "

#ifndef EF_NOSHADOW
#define EF_NOSHADOW 0x010
#endif
#ifndef EF_NODRAW
#define EF_NODRAW 0x020
#endif

#define GHOST_BLOCKED_BUTTONS (IN_ATTACK | IN_USE | IN_ATTACK2 | IN_RELOAD)

// LIFE_DYING right after respawn gives the player a black screen (exkludera/redie)
#define GHOST_DYING_DELAY 0.25f
#define GHOST_REAPPLY_INTERVAL 0.5f

struct GhostState
{
	bool bActive;          // player is a ghost right now
	bool bPendingRestore;  // pawn still carries ghost state (hidden, intangible) until the next real spawn
	bool bExpectSpawn;     // our own Respawn() is in progress
	bool bSilentDeath;     // the following player_death is ours and must not reach clients
	bool bSavedCollision;
	uint64_t nSavedInteractsAs;
	float flCooldownUntil;
	float flDyingAt;       // when LIFE_DYING may be set
	float flExpectSpawnUntil;
};

GhostState g_Ghost[MAX_SLOTS];
bool g_bRoundActive = true; // late load mid-round: allow until the next map start
bool g_bAnyGhostState = false;
float g_flNextReapply = 0.0f;

// config
std::string g_szModel;
float g_flCooldown = 5.0f;
bool g_bEnabled = true;
// Signatures from CS2Fixes gamedata/cs2fixes.jsonc (linux server.so)
std::string g_szSigCanAcquire = "55 48 89 E5 41 57 41 56 49 89 F6 41 55 41 54 49 89 CC 53 48 89 FB 48 83 EC ? 4C 8B 6F";
std::string g_szSigFindUseEntity = "48 B9 ? ? ? ? ? ? ? ? 55 48 89 E5 41 57 49 89 FF";
std::string g_szSigFireOutput = "55 48 89 E5 41 57 49 89 FF 41 56 41 55 41 54 49 89 D4 53 48 89 F3 48 81 EC ? ? ? ? 48 8D 05";
std::string g_szSigProcessUsercmds = ""; // optional, see ghost.ini
int g_iUserCmdPbOffset = 0x10;
int g_iUserCmdSize = 0;

funchook_t* m_CanAcquire = nullptr;
funchook_t* m_ProcessUsercmds = nullptr;
funchook_t* m_FindUseEntity = nullptr;
funchook_t* m_FireOutput = nullptr;

///////////////////////////////////////
// Helpers

inline bool IsValidSlot(int iSlot) { return iSlot >= 0 && iSlot < MAX_SLOTS; }
inline bool IsGhost(int iSlot) { return IsValidSlot(iSlot) && g_Ghost[iSlot].bActive; }
// Ghost right now or a pawn that still carries ghost state: keep it hidden, silent and harmless.
inline bool IsGhostPawn(int iSlot) { return IsValidSlot(iSlot) && (g_Ghost[iSlot].bActive || g_Ghost[iSlot].bPendingRestore); }

inline float CurTime() { return gpGlobals ? gpGlobals->curtime : 0.0f; }

CCSPlayerPawn* GetPawn(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	return pController ? pController->GetPlayerPawn() : nullptr;
}

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

void UpdateAnyGhostState()
{
	g_bAnyGhostState = false;
	for(int i = 0; i < MAX_SLOTS; i++)
		if(IsGhostPawn(i)) { g_bAnyGhostState = true; return; }
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
	CCSPlayerPawn* pPawn = pService ? pService->GetPawn() : nullptr;
	int iSlot = GetSlotFromPawnEntity(pPawn);
	// a ghost body (alive ghost or a hidden body after round end), but never a real respawned player
	if(IsGhostPawn(iSlot) && (g_Ghost[iSlot].bActive || pPawn->m_lifeState() == LifeState_t::LIFE_DYING))
		return AcquireResult::NotAllowedByMode;
	return UTIL_CanAcquire(pService, pItemView, eType, pLimit);
}

///////////////////////////////////////
// +use target search: a ghost never finds anything to use (doors, buttons, weapons, hostages)

class CCSPlayer_UseServices;
CBaseEntity* (*UTIL_FindUseEntity)(CCSPlayer_UseServices* pThis, float flUnk, int64_t nUnk) = nullptr;

CBaseEntity* FindUseEntity(CCSPlayer_UseServices* pThis, float flUnk, int64_t nUnk)
{
	CCSPlayerPawn* pPawn = pThis ? ((CPlayerPawnComponent*)pThis)->GetPawn() : nullptr;
	if(g_bAnyGhostState && pPawn && IsGhostPawn(GetSlotFromPawnEntity(pPawn))) return nullptr;
	return UTIL_FindUseEntity(pThis, flUnk, nUnk);
}

///////////////////////////////////////
// Entity outputs (triggers, buttons, doors...) activated by a ghost are not fired

class CEntityIOOutput;
struct CPulseArgumentPack;
struct CPulseInputParamMap;
void (*UTIL_FireOutputInternal)(CEntityIOOutput* pThis, CEntityInstance* pActivator, CEntityInstance* pCaller, CPulseArgumentPack* pArgs, float flDelay, CPulseInputParamMap* pParamMap, const void* pValue) = nullptr;

void FireOutputInternal(CEntityIOOutput* pThis, CEntityInstance* pActivator, CEntityInstance* pCaller, CPulseArgumentPack* pArgs, float flDelay, CPulseInputParamMap* pParamMap, const void* pValue)
{
	if(g_bAnyGhostState && IsGhostPawn(GetSlotFromPawnEntity(pActivator)))
		return;
	UTIL_FireOutputInternal(pThis, pActivator, pCaller, pArgs, flDelay, pParamMap, pValue);
}

///////////////////////////////////////
// Input (optional): strip +use / attacks from user commands

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
	if(g_bAnyGhostState && pController && IsGhostPawn(pController->GetPlayerSlot()))
	{
		// CUserCmd layout from CS2Fixes detours.cpp: pad 0x10, CSGOUserCmdPB, pad 0x38
		int iStride = g_iUserCmdSize > 0 ? g_iUserCmdSize : (int)(g_iUserCmdPbOffset + sizeof(CSGOUserCmdPB) + 0x38);
		for(int i = 0; i < numcmds; i++)
			StripButtons((CSGOUserCmdPB*)((uint8*)cmds + i * iStride + g_iUserCmdPbOffset));
	}
	return UTIL_ProcessUsercmds(pController, cmds, numcmds, paused, margin);
}

///////////////////////////////////////
// SourceHook declarations

SH_DECL_HOOK8_void(IGameEventSystem, PostEventAbstract, SH_NOATTRIB, 0, CSplitScreenSlot, bool, int, const uint64*, INetworkMessageInternal*, const CNetMessage*, unsigned long, NetChannelBufType_t)
SH_DECL_HOOK2(IGameEventManager2, FireEvent, SH_NOATTRIB, 0, bool, IGameEvent*, bool);
SH_DECL_HOOK3_void(ISource2Server, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK7_void(ISource2GameEntities, CheckTransmit, SH_NOATTRIB, 0, CCheckTransmitInfo**, int, CBitVec<16384>&, CBitVec<16384>&, const Entity2Networkable_t**, const uint16*, int);

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();
	g_ghost.HookGameEvents();
}

///////////////////////////////////////
// Ghost apply / restore

void ApplyGhost(int iSlot)
{
	CCSPlayerPawn* pPawn = GetPawn(iSlot);
	if(!pPawn) return;

	// no weapons (spawn loadout included)
	CCSPlayer_ItemServices* pItemServices = pPawn->m_pItemServices();
	if(pItemServices) pItemServices->RemoveWeapons();

	if(pPawn->m_bTakesDamage())
		pPawn->m_bTakesDamage(false);

	// Intangible: knife/bullet/grenade traces and triggers ignore the pawn,
	// movement still collides with the world (m_nInteractsWith untouched).
	CCollisionProperty* pCollision = pPawn->m_pCollision();
	if(pCollision)
	{
		VPhysicsCollisionAttribute_t& attr = pCollision->m_collisionAttribute();
		uint64_t nInteractsAs = attr.m_nInteractsAs();
		if(nInteractsAs != 0)
		{
			if(!g_Ghost[iSlot].bSavedCollision)
			{
				g_Ghost[iSlot].nSavedInteractsAs = nInteractsAs;
				g_Ghost[iSlot].bSavedCollision = true;
			}
			attr.m_nInteractsAs = 0;
			g_pUtils->CollisionRulesChanged(pPawn);
		}
	}

	// Invisible, no shadow (others do not even receive the pawn, see OnCheckTransmit)
	if(pPawn->m_flShadowStrength() != 0.0f)
		pPawn->m_flShadowStrength = 0.0f;
	if((pPawn->m_fEffects() & (EF_NODRAW | EF_NOSHADOW)) != (EF_NODRAW | EF_NOSHADOW))
		pPawn->m_fEffects = pPawn->m_fEffects() | EF_NODRAW | EF_NOSHADOW;
	if(pPawn->m_nRenderMode() != kRenderNone)
		pPawn->m_nRenderMode = kRenderNone;
	if(pPawn->m_clrRender().a() != 0)
		pPawn->m_clrRender = Color(255, 255, 255, 0);

	// Dead for the game: round end counting, scoreboard, radar, chat/voice, bots
	if(CurTime() >= g_Ghost[iSlot].flDyingAt && pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
		pPawn->m_lifeState = LifeState_t::LIFE_DYING;
}

// Player interaction layers must never stay 0 on a normal player: that would make him unhittable.
uint64_t GetNormalInteractsAs(int iSlot)
{
	if(g_Ghost[iSlot].bSavedCollision && g_Ghost[iSlot].nSavedInteractsAs != 0)
		return g_Ghost[iSlot].nSavedInteractsAs;
	for(int i = 0; i < MAX_SLOTS; i++)
	{
		if(i == iSlot || IsGhostPawn(i)) continue;
		CCSPlayerPawn* pOther = GetPawn(i);
		if(!pOther || !pOther->IsAlive() || !pOther->m_pCollision()) continue;
		uint64_t nValue = pOther->m_pCollision()->m_collisionAttribute().m_nInteractsAs();
		if(nValue != 0) return nValue;
	}
	return 0;
}

void RestorePawn(int iSlot)
{
	CCSPlayerPawn* pPawn = GetPawn(iSlot);
	if(pPawn)
	{
		CCollisionProperty* pCollision = pPawn->m_pCollision();
		if(pCollision && pCollision->m_collisionAttribute().m_nInteractsAs() == 0)
		{
			uint64_t nValue = GetNormalInteractsAs(iSlot);
			if(nValue != 0)
			{
				pCollision->m_collisionAttribute().m_nInteractsAs = nValue;
				g_pUtils->CollisionRulesChanged(pPawn);
			}
			else
				g_pUtils->ErrorLog("[Ghost] Could not restore interaction layers for slot %d", iSlot);
		}
		pPawn->m_flShadowStrength = 1.0f;
		pPawn->m_fEffects = pPawn->m_fEffects() & ~(uint32)(EF_NODRAW | EF_NOSHADOW);
		pPawn->m_nRenderMode = kRenderNormal;
		pPawn->m_clrRender = Color(255, 255, 255, 255);
		pPawn->m_bTakesDamage(true);
	}
	g_Ghost[iSlot].bPendingRestore = false;
	g_Ghost[iSlot].bSavedCollision = false;
}

// Forget everything about the slot (disconnect, map change).
void ResetSlot(int iSlot)
{
	if(!IsValidSlot(iSlot)) return;
	g_Ghost[iSlot] = GhostState();
}

// Stop ghost logic. The body stays hidden and intangible until the game respawns the player
// (then OnGameFrame restores it). No kill, no death in stats.
void DeactivateGhost(int iSlot)
{
	if(!IsGhost(iSlot)) return;
	g_Ghost[iSlot].bActive = false;
	g_Ghost[iSlot].bExpectSpawn = false;
	g_Ghost[iSlot].bPendingRestore = true;
}

// Leave ghost mode: the player is dead again. Silent: no kill feed, deaths/score restored.
// The pawn keeps its transparent render so no ragdoll shows up (exkludera/redie).
void RemoveGhost(int iSlot)
{
	if(!IsGhost(iSlot)) return;
	DeactivateGhost(iSlot);

	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	CCSPlayerPawn* pPawn = pController ? pController->GetPlayerPawn() : nullptr;
	if(!pController || !pPawn || pPawn->m_lifeState() == LifeState_t::LIFE_DEAD) return;

	int iScore = pController->m_iScore();
	CCSPlayerController_ActionTrackingServices* pStats = pController->m_pActionTrackingServices();
	int iDeaths = pStats ? pStats->m_matchStats().m_iDeaths() : 0;

	pPawn->m_lifeState = LifeState_t::LIFE_ALIVE;
	g_Ghost[iSlot].bSilentDeath = true;
	g_pPlayers->CommitSuicide(iSlot, false, true);
	g_Ghost[iSlot].bSilentDeath = false;

	if(pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
	{
		// suicide did not happen: never leave a "revived" player behind, stay a ghost
		g_pUtils->ErrorLog("[Ghost] CommitSuicide failed for slot %d, player stays a ghost", iSlot);
		g_Ghost[iSlot].bActive = true;
		g_Ghost[iSlot].bPendingRestore = false;
		pPawn->m_lifeState = LifeState_t::LIFE_DYING;
		return;
	}

	if(pController->m_iScore() != iScore)
		pController->m_iScore = iScore;
	if(pStats)
		pStats->m_matchStats().m_iDeaths() = iDeaths;
}

void MakeGhost(int iSlot)
{
	GhostState& st = g_Ghost[iSlot];
	st.bActive = true;
	st.bPendingRestore = false;
	st.bSilentDeath = false;
	st.flCooldownUntil = CurTime() + g_flCooldown;
	st.flDyingAt = CurTime() + GHOST_DYING_DELAY;
	st.bExpectSpawn = true;
	st.flExpectSpawnUntil = CurTime() + 1.0f;
	g_bAnyGhostState = true;

	g_pPlayers->Respawn(iSlot);

	g_pUtils->NextFrame([iSlot]() {
		if(!IsGhost(iSlot)) return;
		CCSPlayerPawn* pPawn = GetPawn(iSlot);
		if(!pPawn) return;
		if(!g_szModel.empty()) g_pUtils->SetEntityModel(pPawn, g_szModel.c_str());
		ApplyGhost(iSlot);
	});
}

///////////////////////////////////////
// Hooks

// Others do not receive the ghost pawn at all: no model, shadow, radar, footsteps, client-side effects.
// Same approach as CS2Fixes "hide" and exkludera/redie. Spectators are left alone (pawn is NODRAW/LIFE_DYING anyway).
void ghost::OnCheckTransmit(CCheckTransmitInfo** ppInfoList, int infoCount, CBitVec<16384>& unionTransmitEdicts, CBitVec<16384>&, const Entity2Networkable_t** pNetworkables, const uint16* pEntityIndicies, int nEntities)
{
	if(!g_bAnyGhostState || !g_pEntitySystem || !gpGlobals) RETURN_META(MRES_IGNORED);

	int iHidden[MAX_SLOTS];
	int nHidden = 0;
	for(int j = 0; j < MAX_SLOTS; j++)
	{
		if(!IsGhostPawn(j)) continue;
		CCSPlayerPawn* pPawn = GetPawn(j);
		if(pPawn) iHidden[nHidden++] = j;
	}
	if(!nHidden) RETURN_META(MRES_IGNORED);

	for(int i = 0; i < infoCount; i++)
	{
		auto pInfo = (CCheckTransmitInfoExtended*)ppInfoList[i];
		int iViewer = pInfo->m_nPlayerSlot.Get();
		CCSPlayerController* pViewer = CCSPlayerController::FromSlot(iViewer);
		if(!pViewer || pViewer->GetPawnState() == STATE_OBSERVER_MODE) continue;

		for(int k = 0; k < nHidden; k++)
		{
			int j = iHidden[k];
			if(j == iViewer) continue; // always transmit to themselves
			CCSPlayerPawn* pPawn = GetPawn(j);
			if(!pPawn) continue;
			int iIndex = pPawn->entindex();
			pInfo->m_pTransmitEntity->Clear(iIndex);
			pInfo->m_pTransmitNonPlayers->Set(iIndex);
		}
	}
	RETURN_META(MRES_IGNORED);
}

// Footsteps / jump / land sounds of a ghost are sent only to the ghost himself.
void ghost::OnPostEvent(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients, INetworkMessageInternal* pEvent, const CNetMessage* pData, unsigned long nSize, NetChannelBufType_t bufType)
{
	if(!g_bAnyGhostState || !clients) RETURN_META(MRES_IGNORED);
	NetMessageInfo_t *info = pEvent->GetNetMessageInfo();
	if(!info || !info->m_pBinding) RETURN_META(MRES_IGNORED);
	const char* szName = info->m_pBinding->GetName();
	if(!szName || strcmp(szName, "CMsgSosStartSoundEvent")) RETURN_META(MRES_IGNORED);

	auto msg = const_cast<CNetMessage*>(pData)->ToPB<CMsgSosStartSoundEvent>();
	int iSlot = GetSlotFromPawnEntity(UTIL_GetEntityByIndex(msg->source_entity_index()));
	if(!IsGhostPawn(iSlot)) RETURN_META(MRES_IGNORED);

	uint64* pClients = const_cast<uint64*>(clients);
	*pClients &= (1ull << iSlot);
	if(*pClients == 0) RETURN_META(MRES_SUPERCEDE);
	RETURN_META(MRES_HANDLED);
}

// The ghost's deaths are never sent to clients: no kill feed entry.
// The event is not freed/superseded: other plugins still see it.
bool ghost::OnFireEvent(IGameEvent* pEvent, bool bDontBroadcast)
{
	if(!pEvent || bDontBroadcast) RETURN_META_VALUE(MRES_IGNORED, false);
	if(strcmp(pEvent->GetName(), "player_death")) RETURN_META_VALUE(MRES_IGNORED, false);

	int iSlot = pEvent->GetPlayerSlot("userid").Get();
	if(IsValidSlot(iSlot) && (g_Ghost[iSlot].bSilentDeath || IsGhostPawn(iSlot)))
		RETURN_META_VALUE_NEWPARAMS(MRES_IGNORED, false, &IGameEventManager2::FireEvent, (pEvent, true));
	RETURN_META_VALUE(MRES_IGNORED, false);
}

// Re-apply ghost state (game, NoBlock and other plugins may reset it), set LIFE_DYING after the delay,
// restore players that the game respawned.
void ghost::OnGameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	if(!g_bAnyGhostState || !gpGlobals || !g_pEntitySystem) RETURN_META(MRES_IGNORED);
	float flNow = CurTime();
	bool bReapply = flNow >= g_flNextReapply;
	if(bReapply) g_flNextReapply = flNow + GHOST_REAPPLY_INTERVAL;

	for(int i = 0; i < MAX_SLOTS; i++)
	{
		GhostState& st = g_Ghost[i];
		if(st.bActive)
		{
			if(st.bExpectSpawn && flNow > st.flExpectSpawnUntil)
				st.bExpectSpawn = false;
			CCSPlayerPawn* pPawn = GetPawn(i);
			if(!pPawn) continue;
			bool bNeedDying = flNow >= st.flDyingAt && pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE;
			if(bReapply || bNeedDying)
				ApplyGhost(i);
		}
		else if(st.bPendingRestore)
		{
			CCSPlayerPawn* pPawn = GetPawn(i);
			if(!pPawn) continue;
			// ghost body after round end / unghost is DYING or DEAD; ALIVE means the game respawned the player
			if(pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
				RestorePawn(i);
			else if(bReapply && pPawn->m_lifeState() == LifeState_t::LIFE_DYING)
			{
				CCSPlayer_ItemServices* pItemServices = pPawn->m_pItemServices();
				if(pItemServices) pItemServices->RemoveWeapons();
			}
		}
	}
	UpdateAnyGhostState();
	RETURN_META(MRES_IGNORED);
}

///////////////////////////////////////
// Commands

bool OnGhostCommand(int iSlot, const char* szContent)
{
	if(!IsValidSlot(iSlot)) return false;
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if(!pController) return false;

	if(IsGhost(iSlot))
	{
		RemoveGhost(iSlot);
		g_pUtils->PrintToChat(iSlot, CHAT_PREFIX "Ты вышел из режима призрака");
		return false;
	}
	if(!g_bEnabled)
	{
		g_pUtils->PrintToChat(iSlot, CHAT_PREFIX "Режим призрака отключён");
		return false;
	}

	int iTeam = pController->m_iTeamNum();
	if(iTeam != 2 && iTeam != 3)
	{
		g_pUtils->PrintToChat(iSlot, CHAT_PREFIX "Только для игроков за T или CT");
		return false;
	}
	CCSGameRules* pRules = g_pUtils->GetCCSGameRules();
	if(!g_bRoundActive || (pRules && pRules->m_bWarmupPeriod()))
	{
		g_pUtils->PrintToChat(iSlot, CHAT_PREFIX "Сейчас нельзя: разминка или раунд не идёт");
		return false;
	}
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if(!pPawn || pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
	{
		g_pUtils->PrintToChat(iSlot, CHAT_PREFIX "Только для мёртвых игроков");
		return false;
	}
	if(CurTime() < g_Ghost[iSlot].flCooldownUntil)
	{
		g_pUtils->PrintToChat(iSlot, CHAT_PREFIX "Подожди %.0f сек.", g_Ghost[iSlot].flCooldownUntil - CurTime() + 0.5f);
		return false;
	}
	MakeGhost(iSlot);
	g_pUtils->PrintToChat(iSlot, CHAT_PREFIX "Ты призрак. Выйти: !unghost");
	return false;
}

bool OnUnGhostCommand(int iSlot, const char* szContent)
{
	if(IsGhost(iSlot))
	{
		RemoveGhost(iSlot);
		g_pUtils->PrintToChat(iSlot, CHAT_PREFIX "Ты вышел из режима призрака");
	}
	return false;
}

bool OnGhostToggleCommand(int iSlot, const char* szContent)
{
	if(iSlot != -1) return false; // server console only
	g_bEnabled = !g_bEnabled;
	if(!g_bEnabled)
		for(int i = 0; i < MAX_SLOTS; i++) RemoveGhost(i);
	Msg("[Ghost] %s\n", g_bEnabled ? "enabled" : "disabled");
	return false;
}

///////////////////////////////////////
// Events

void OnRoundStart(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	g_bRoundActive = true;
	for(int i = 0; i < MAX_SLOTS; i++)
	{
		DeactivateGhost(i);
		CCSPlayerPawn* pPawn = g_Ghost[i].bPendingRestore ? GetPawn(i) : nullptr;
		if(pPawn && pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
			RestorePawn(i);
	}
}

void OnRoundEnd(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	g_bRoundActive = false;
	for(int i = 0; i < MAX_SLOTS; i++) DeactivateGhost(i);
}

void OnPlayerDeath(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetPlayerSlot("userid").Get();
	// a ghost killed by something else: keep the body hidden, restore on the next spawn
	if(IsGhost(iSlot)) DeactivateGhost(iSlot);
}

void OnPlayerTeam(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetPlayerSlot("userid").Get();
	if(!IsGhost(iSlot)) return;
	// the game does not kill a "dead" (LIFE_DYING) pawn on team change: do it ourselves
	g_pUtils->NextFrame([iSlot]() { RemoveGhost(iSlot); });
}

void OnPlayerDisconnect(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetPlayerSlot("userid").Get();
	ResetSlot(iSlot);
	UpdateAnyGhostState();
}

// Respawn by the game or another plugin (admin respawn, warmup...) turns a ghost back into a normal player.
void OnPlayerSpawn(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetPlayerSlot("userid").Get();
	if(!IsGhost(iSlot)) return;
	if(g_Ghost[iSlot].bExpectSpawn) // ours (may come more than once); the flag expires in OnGameFrame
	{
		g_pUtils->NextFrame([iSlot]() { if(IsGhost(iSlot)) ApplyGhost(iSlot); });
		return;
	}
	DeactivateGhost(iSlot); // OnGameFrame restores the alive pawn
}

bool OnTakeDamagePre(int iSlot, CTakeDamageInfo *pInfo)
{
	if(!g_bAnyGhostState) return true;
	if(IsGhostPawn(iSlot)) return false;
	if(pInfo && IsGhostPawn(GetSlotFromPawnEntity(pInfo->m_hAttacker().Get()))) return false;
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
		g_pUtils->ErrorLog("[Ghost] Failed to load %s, using defaults", pszPath);
		delete hKv;
		return;
	}

	g_szModel = hKv->GetString("model", "");
	g_flCooldown = hKv->GetFloat("cooldown", 5.0f);
	g_szSigCanAcquire = hKv->GetString("sig_canacquire", g_szSigCanAcquire.c_str());
	g_szSigFindUseEntity = hKv->GetString("sig_findusentity", g_szSigFindUseEntity.c_str());
	g_szSigFireOutput = hKv->GetString("sig_fireoutput", g_szSigFireOutput.c_str());
	g_szSigProcessUsercmds = hKv->GetString("sig_processusercmds", g_szSigProcessUsercmds.c_str());
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
// Detours

void InstallDetour(funchook_t*& pHook, const std::string& szSig, void** ppOriginal, void* pDetour, const char* szName, const char* szWhatBreaks)
{
	if(szSig.empty())
	{
		Msg("[Ghost] %s disabled in config\n", szName);
		return;
	}
	DynLibUtils::CModule libserver(g_pSource2Server);
	*ppOriginal = libserver.FindPattern(szSig.c_str()).RCast<void*>();
	if(!*ppOriginal)
	{
		g_pUtils->ErrorLog("[Ghost] %s signature not found: %s", szName, szWhatBreaks);
		return;
	}
	pHook = funchook_create();
	if(funchook_prepare(pHook, ppOriginal, pDetour) != 0 || funchook_install(pHook, 0) != 0)
	{
		g_pUtils->ErrorLog("[Ghost] %s hook failed: %s", szName, funchook_error_message(pHook));
		funchook_destroy(pHook);
		pHook = nullptr;
		*ppOriginal = nullptr;
	}
}

void RemoveDetour(funchook_t*& pHook)
{
	if(!pHook) return;
	funchook_uninstall(pHook, 0);
	funchook_destroy(pHook);
	pHook = nullptr;
}

void InstallDetours()
{
	InstallDetour(m_CanAcquire, g_szSigCanAcquire, (void**)&UTIL_CanAcquire, (void*)CanAcquire, "CanAcquire", "pickup/buy are not blocked");
	InstallDetour(m_FindUseEntity, g_szSigFindUseEntity, (void**)&UTIL_FindUseEntity, (void*)FindUseEntity, "FindUseEntity", "+use is not blocked");
	InstallDetour(m_FireOutput, g_szSigFireOutput, (void**)&UTIL_FireOutputInternal, (void*)FireOutputInternal, "FireOutputInternal", "triggers/buttons activated by a ghost still fire");
	InstallDetour(m_ProcessUsercmds, g_szSigProcessUsercmds, (void**)&UTIL_ProcessUsercmds, (void*)ProcessUsercmds, "ProcessUsercmds", "buttons are not stripped from user commands");
}

///////////////////////////////////////
// Plugin

bool ghost::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameEntities, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_gameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	SH_ADD_HOOK(IGameEventSystem, PostEventAbstract, g_gameEventSystem, SH_MEMBER(this, &ghost::OnPostEvent), false);
	SH_ADD_HOOK(ISource2Server, GameFrame, g_pSource2Server, SH_MEMBER(this, &ghost::OnGameFrame), true);
	SH_ADD_HOOK(ISource2GameEntities, CheckTransmit, g_pSource2GameEntities, SH_MEMBER(this, &ghost::OnCheckTransmit), true);

	g_SMAPI->AddListener( this, this );

	return true;
}

bool ghost::Unload(char *error, size_t maxlen)
{
	// leave nobody as a ghost and nobody hidden/unhittable
	if(g_pUtils && g_pPlayers)
	{
		for(int i = 0; i < MAX_SLOTS; i++)
		{
			RemoveGhost(i);
			if(g_Ghost[i].bPendingRestore) RestorePawn(i);
			ResetSlot(i);
		}
	}
	g_bAnyGhostState = false;

	SH_REMOVE_HOOK(IGameEventSystem, PostEventAbstract, g_gameEventSystem, SH_MEMBER(this, &ghost::OnPostEvent), false);
	SH_REMOVE_HOOK(ISource2Server, GameFrame, g_pSource2Server, SH_MEMBER(this, &ghost::OnGameFrame), true);
	SH_REMOVE_HOOK(ISource2GameEntities, CheckTransmit, g_pSource2GameEntities, SH_MEMBER(this, &ghost::OnCheckTransmit), true);
	if(g_pGameEventManager)
		SH_REMOVE_HOOK(IGameEventManager2, FireEvent, g_pGameEventManager, SH_MEMBER(this, &ghost::OnFireEvent), false);

	RemoveDetour(m_CanAcquire);
	RemoveDetour(m_FindUseEntity);
	RemoveDetour(m_FireOutput);
	RemoveDetour(m_ProcessUsercmds);

	if(g_pUtils)
		g_pUtils->ClearAllHooks(g_PLID);

	return true;
}

void ghost::HookGameEvents()
{
	if(g_pGameEventManager) return;
	g_pGameEventManager = g_pUtils->GetGameEventManager();
	if(g_pGameEventManager)
		SH_ADD_HOOK(IGameEventManager2, FireEvent, g_pGameEventManager, SH_MEMBER(this, &ghost::OnFireEvent), false);
}

void ghost::AllPluginsLoaded()
{
	char error[64];
	int ret;
	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pUtils = nullptr;
		g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pPlayers = nullptr;
		g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pSCApi = (ISkinChangerApi *)g_SMAPI->MetaFactory(SkinChanger_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pSCApi = nullptr;
		ConColorMsg(Color(255, 255, 0, 255), "[%s] SkinChanger not found: ghosts may get gloves/knife skins\n", GetLogTag());
	}

	LoadConfig();
	InstallDetours();
	HookGameEvents();

	g_pUtils->StartupServer(g_PLID, StartupServer);
	g_pUtils->MapStartHook(g_PLID, [](const char* szMap) {
		g_bRoundActive = false;
		for(int i = 0; i < MAX_SLOTS; i++) ResetSlot(i);
		g_bAnyGhostState = false;
		g_flNextReapply = 0.0f;
	});
	g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStart);
	g_pUtils->HookEvent(g_PLID, "round_end", OnRoundEnd);
	g_pUtils->HookEvent(g_PLID, "player_team", OnPlayerTeam);
	g_pUtils->HookEvent(g_PLID, "player_death", OnPlayerDeath);
	g_pUtils->HookEvent(g_PLID, "player_disconnect", OnPlayerDisconnect);
	g_pUtils->HookEvent(g_PLID, "player_spawn", OnPlayerSpawn);
	g_pUtils->HookOnTakeDamagePre(g_PLID, OnTakeDamagePre);
	g_pUtils->RegCommand(g_PLID, { "mm_redie", "mm_ghost", "css_redie", "css_ghost" }, { "!redie", "!ghost" }, OnGhostCommand);
	g_pUtils->RegCommand(g_PLID, { "mm_unghost", "css_unghost" }, { "!unghost" }, OnUnGhostCommand);
	g_pUtils->RegCommand(g_PLID, { "mm_ghost_toggle" }, {}, OnGhostToggleCommand);
	if(g_pSCApi)
	{
		g_pSCApi->OnSetGlove(g_PLID, OnSetGloveCallback);
		g_pSCApi->OnSetGloveSkin(g_PLID, OnSetGloveSkinCallback);
		g_pSCApi->OnSetKnife(g_PLID, OnSetKnifeCallback);
	}
}

///////////////////////////////////////
const char* ghost::GetLicense()
{
	return "GPL";
}

const char* ghost::GetVersion()
{
	return "2.1";
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
	return "Pisex, glazki2";
}

const char* ghost::GetDescription()
{
	return "Ghost mode for dead players: invisible, silent, intangible, no interaction";
}

const char* ghost::GetName()
{
	return "Ghost";
}

const char* ghost::GetURL()
{
	return "https://github.com/glazki2/Closed-eyes";
}
