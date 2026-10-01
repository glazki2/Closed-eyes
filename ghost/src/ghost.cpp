// Ghost: mode for dead players in CS2 (Metamod:Source 2, KHook API).
// Invisible, silent, intangible, cannot use/pick up/trigger anything, does not affect the round.

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <functional>
#include <vector>
#include <string>

#include "ghost.h"
#include "iserver.h"
#include "eiface.h"
#include "icvar.h"
#include "tier1/convar.h"
#include "irecipientfilter.h"
#include "engine/igameeventsystem.h"
#include "networksystem/inetworkmessages.h"
#include "networksystem/inetworkserializer.h"
#include "entity2/entityidentity.h"
#include "entity2/entitysystem.h"
#include "schemasystem/schemasystem.h"
#include "filesystem.h"
#include "KeyValues.h"

#include "gameevents.pb.h"
#include "usermessages.pb.h"
#include "cs_usercmd.pb.h"
#include "cchecktransmitinfo.h"

#include "module.h"
#include "CCSPlayerController.h"
#include "CCSPlayerPawn.h"
#include "CGameRules.h"
#include "ctakedamageinfo.h"

///////////////////////////////////////
// Globals

GhostPlugin g_GhostPlugin;
PLUGIN_EXPOSE(GhostPlugin, g_GhostPlugin);

IVEngineServer2* engine = nullptr;
IServerGameDLL* g_pServerGameDLL = nullptr;
IServerGameClients* g_pServerGameClients = nullptr;
IGameEventSystem* g_gameEventSystem = nullptr;
INetworkMessages* g_pNetMessages = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;

#define MAX_SLOTS 64
#define CHAT_PREFIX " \x04[Ghost]\x01 "
#define HUD_PRINTTALK 3

#ifndef EF_NOSHADOW
#define EF_NOSHADOW 0x010
#endif
#ifndef EF_NODRAW
#define EF_NODRAW 0x020
#endif

#define GHOST_BLOCKED_BUTTONS (IN_ATTACK | IN_USE | IN_ATTACK2 | IN_RELOAD)

// LIFE_DYING right after respawn gives the player a black screen
#define GHOST_DYING_DELAY 0.25f
#define GHOST_REAPPLY_INTERVAL 0.5f

///////////////////////////////////////
// Config (addons/ghost/ghost.ini). Defaults are for the linux server binary.

struct GhostConfig
{
	float flCooldown = 5.0f;

	int iOffGameEntitySystem = 80;
	int iOffCollisionRulesChanged = 187;
	int iOffRespawn = 276;
	int iOffCommitSuicide = 387;
	int iOffStripWeapons = 28;

	std::string szSigSetPawn = "55 48 8D 87 ? ? ? ? 48 89 E5 41 57 41 56 41 89 CE 41 55 45 89 CD";
	std::string szSigTakeDamage = "55 66 0F EF C0 48 89 E5 41 57 41 56 41 55 49 89 FD 31 FF";
	std::string szSigCanAcquire = "55 48 89 E5 41 57 41 56 49 89 F6 41 55 41 54 49 89 CC 53 48 89 FB 48 83 EC ? 4C 8B 6F";
	std::string szSigFindUseEntity = "48 B9 ? ? ? ? ? ? ? ? 55 48 89 E5 41 57 49 89 FF";
	std::string szSigFireOutput = "55 48 89 E5 41 57 49 89 FF 41 56 41 55 41 54 49 89 D4 53 48 89 F3 48 81 EC ? ? ? ? 48 8D 05";
	std::string szSigProcessUsercmds = "";
	int iUserCmdPbOffset = 0x10;
	int iUserCmdSize = 0;
};
GhostConfig g_Config;

///////////////////////////////////////
// State

struct GhostState
{
	bool bActive = false;          // player is a ghost right now
	bool bPendingRestore = false;  // pawn still carries ghost state (hidden, intangible) until the next real spawn
	bool bExpectSpawn = false;     // our own Respawn() is in progress
	bool bSilentDeath = false;     // the following player_death is ours and must not reach clients
	bool bSavedCollision = false;
	uint64_t nSavedInteractsAs = 0;
	float flCooldownUntil = 0.0f;
	float flDyingAt = 0.0f;        // when LIFE_DYING may be set
	float flExpectSpawnUntil = 0.0f;
};

GhostState g_Ghost[MAX_SLOTS];
bool g_bRoundActive = true; // late load mid-round: allow until the next map start
bool g_bAnyGhostState = false;
bool g_bEnabled = true;
float g_flNextReapply = 0.0f;
std::vector<std::function<void()>> g_NextFrame;

///////////////////////////////////////
// Helpers

inline bool IsValidSlot(int iSlot) { return iSlot >= 0 && iSlot < MAX_SLOTS; }
inline bool IsGhost(int iSlot) { return IsValidSlot(iSlot) && g_Ghost[iSlot].bActive; }
// Ghost right now or a pawn that still carries ghost state: keep it hidden, silent and harmless.
inline bool IsGhostPawn(int iSlot) { return IsValidSlot(iSlot) && (g_Ghost[iSlot].bActive || g_Ghost[iSlot].bPendingRestore); }
inline float CurTime() { return gpGlobals ? gpGlobals->curtime : 0.0f; }

CGameEntitySystem* GameEntitySystem()
{
	if (!g_pGameResourceServiceServer)
		return nullptr;
	return *reinterpret_cast<CGameEntitySystem**>((uintptr_t)g_pGameResourceServiceServer + g_Config.iOffGameEntitySystem);
}

void RefreshGlobals()
{
	g_pEntitySystem = GameEntitySystem();
	gpGlobals = engine ? engine->GetServerGlobals() : nullptr;
}

void NextFrame(std::function<void()> fn)
{
	g_NextFrame.push_back(std::move(fn));
}

CCSPlayerPawn* GetPawn(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	return pController ? pController->GetPlayerPawn() : nullptr;
}

// Returns slot of the player owning this pawn or -1 if the entity is not a player pawn.
int GetSlotFromPawnEntity(CEntityInstance* pEnt)
{
	if (!pEnt)
		return -1;
	const char* szClass = pEnt->GetClassname();
	if (!szClass || strcmp(szClass, "player"))
		return -1;
	CBasePlayerController* pController = ((CCSPlayerPawn*)pEnt)->GetController();
	if (!pController)
		return -1;
	int iSlot = pController->GetPlayerSlot();
	return IsValidSlot(iSlot) ? iSlot : -1;
}

void UpdateAnyGhostState()
{
	g_bAnyGhostState = false;
	for (int i = 0; i < MAX_SLOTS; i++)
		if (IsGhostPawn(i))
		{
			g_bAnyGhostState = true;
			return;
		}
}

class CSingleRecipientFilter : public IRecipientFilter
{
public:
	CSingleRecipientFilter(int iSlot) { m_Recipients.Set(iSlot); }
	NetChannelBufType_t GetNetworkBufType(void) const override { return BUF_RELIABLE; }
	bool IsInitMessage(void) const override { return false; }
	const CPlayerBitVec& GetRecipients(void) const override { return m_Recipients; }
	CPlayerSlot GetPredictedPlayerSlot(void) const override { return -1; }

private:
	CPlayerBitVec m_Recipients;
};

void PrintToChat(int iSlot, const char* fmt, ...)
{
	if (!IsValidSlot(iSlot) || !g_pNetMessages || !g_gameEventSystem)
		return;

	char buf[256];
	va_list args;
	va_start(args, fmt);
	V_vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	INetworkMessageInternal* pNetMsg = g_pNetMessages->FindNetworkMessagePartial("TextMsg");
	if (!pNetMsg)
		return;

	auto data = pNetMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	data->set_dest(HUD_PRINTTALK);
	data->add_param(buf);

	CSingleRecipientFilter filter(iSlot);
	g_gameEventSystem->PostEventAbstract(-1, false, &filter, pNetMsg, data, 0);

	delete data;
}

CCSGameRules* GetGameRules()
{
	auto pProxy = (CCSGameRulesProxy*)UTIL_FindEntityByClassname("cs_gamerules");
	return pProxy ? pProxy->m_pGameRules() : nullptr;
}

///////////////////////////////////////
// Engine calls (vtable offsets / signatures from the config)

void (*UTIL_SetPawn)(CBasePlayerController*, CCSPlayerPawn*, bool, bool, bool, bool) = nullptr;

void CollisionRulesChanged(CBaseEntity* pEnt)
{
	CALL_VIRTUAL(void, g_Config.iOffCollisionRulesChanged, pEnt);
}

void RespawnPlayer(CCSPlayerController* pController)
{
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if (!pPawn || pPawn->IsAlive())
		return;
	// switch the controller from the observer pawn back to the player pawn
	if (UTIL_SetPawn)
		UTIL_SetPawn(pController, pPawn, true, false, false, false);
	CALL_VIRTUAL(void, g_Config.iOffRespawn, pController);
}

void CommitSuicide(CCSPlayerPawn* pPawn, bool bExplode, bool bForce)
{
	CALL_VIRTUAL(void, g_Config.iOffCommitSuicide, pPawn, bExplode, bForce);
}

void StripWeapons(CCSPlayerPawn* pPawn)
{
	CCSPlayer_ItemServices* pItemServices = pPawn->m_pItemServices();
	if (pItemServices)
		CALL_VIRTUAL(void, g_Config.iOffStripWeapons, pItemServices, true);
}

///////////////////////////////////////
// Ghost apply / restore

void ApplyGhost(int iSlot)
{
	CCSPlayerPawn* pPawn = GetPawn(iSlot);
	if (!pPawn)
		return;

	// no weapons (spawn loadout included)
	StripWeapons(pPawn);

	if (pPawn->m_bTakesDamage())
		pPawn->m_bTakesDamage(false);

	// Intangible: knife/bullet/grenade traces and triggers ignore the pawn,
	// movement still collides with the world (m_nInteractsWith untouched).
	CCollisionProperty* pCollision = pPawn->m_pCollision();
	if (pCollision)
	{
		VPhysicsCollisionAttribute_t& attr = pCollision->m_collisionAttribute();
		uint64_t nInteractsAs = attr.m_nInteractsAs();
		if (nInteractsAs != 0)
		{
			if (!g_Ghost[iSlot].bSavedCollision)
			{
				g_Ghost[iSlot].nSavedInteractsAs = nInteractsAs;
				g_Ghost[iSlot].bSavedCollision = true;
			}
			attr.m_nInteractsAs = 0;
			CollisionRulesChanged(pPawn);
		}
	}

	// Invisible, no shadow (others do not even receive the pawn, see Hook_CheckTransmit)
	if (pPawn->m_flShadowStrength() != 0.0f)
		pPawn->m_flShadowStrength = 0.0f;
	if ((pPawn->m_fEffects() & (EF_NODRAW | EF_NOSHADOW)) != (EF_NODRAW | EF_NOSHADOW))
		pPawn->m_fEffects = pPawn->m_fEffects() | EF_NODRAW | EF_NOSHADOW;
	if (pPawn->m_nRenderMode() != kRenderNone)
		pPawn->m_nRenderMode = kRenderNone;
	if (pPawn->m_clrRender().a() != 0)
		pPawn->m_clrRender = Color(255, 255, 255, 0);

	// The pawn stays LIFE_ALIVE: any other life state gives the ghost a black screen.
	// Round end by elimination is handled in CheckTeamsEliminated().
}

// Player interaction layers must never stay 0 on a normal player: that would make him unhittable.
uint64_t GetNormalInteractsAs(int iSlot)
{
	if (g_Ghost[iSlot].bSavedCollision && g_Ghost[iSlot].nSavedInteractsAs != 0)
		return g_Ghost[iSlot].nSavedInteractsAs;
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		if (i == iSlot || IsGhostPawn(i))
			continue;
		CCSPlayerPawn* pOther = GetPawn(i);
		if (!pOther || !pOther->IsAlive() || !pOther->m_pCollision())
			continue;
		uint64_t nValue = pOther->m_pCollision()->m_collisionAttribute().m_nInteractsAs();
		if (nValue != 0)
			return nValue;
	}
	return 0;
}

void RestorePawn(int iSlot)
{
	CCSPlayerPawn* pPawn = GetPawn(iSlot);
	if (pPawn)
	{
		CCollisionProperty* pCollision = pPawn->m_pCollision();
		if (pCollision && pCollision->m_collisionAttribute().m_nInteractsAs() == 0)
		{
			uint64_t nValue = GetNormalInteractsAs(iSlot);
			if (nValue != 0)
			{
				pCollision->m_collisionAttribute().m_nInteractsAs = nValue;
				CollisionRulesChanged(pPawn);
			}
			else
				Warning("[Ghost] Could not restore interaction layers for slot %d\n", iSlot);
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
	if (!IsValidSlot(iSlot))
		return;
	g_Ghost[iSlot] = GhostState();
}

// Stop ghost logic. The body stays hidden and intangible until the game respawns the player
// (then Hook_GameFrame restores it). No kill, no death in stats.
void DeactivateGhost(int iSlot)
{
	if (!IsGhost(iSlot))
		return;
	g_Ghost[iSlot].bActive = false;
	g_Ghost[iSlot].bExpectSpawn = false;
	g_Ghost[iSlot].bPendingRestore = true;
}

// Leave ghost mode: the player is dead again. Silent: no kill feed, deaths/score restored.
// The pawn keeps its transparent render so no ragdoll shows up.
void RemoveGhost(int iSlot)
{
	if (!IsGhost(iSlot))
		return;
	DeactivateGhost(iSlot);

	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	CCSPlayerPawn* pPawn = pController ? pController->GetPlayerPawn() : nullptr;
	if (!pController || !pPawn || pPawn->m_lifeState() == LifeState_t::LIFE_DEAD)
		return;

	int iScore = pController->m_iScore();
	CCSPlayerController_ActionTrackingServices* pStats = pController->m_pActionTrackingServices();
	int iDeaths = pStats ? pStats->m_matchStats().m_iDeaths() : 0;

	pPawn->m_lifeState = LifeState_t::LIFE_ALIVE;
	g_Ghost[iSlot].bSilentDeath = true;
	CommitSuicide(pPawn, false, true);
	g_Ghost[iSlot].bSilentDeath = false;

	if (pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
	{
		// suicide did not happen: never leave a "revived" player behind, stay a ghost
		Warning("[Ghost] CommitSuicide failed for slot %d, player stays a ghost\n", iSlot);
		g_Ghost[iSlot].bActive = true;
		g_Ghost[iSlot].bPendingRestore = false;
		return;
	}

	if (pController->m_iScore() != iScore)
		pController->m_iScore = iScore;
	if (pStats)
		pStats->m_matchStats().m_iDeaths() = iDeaths;
}

void MakeGhost(int iSlot)
{
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if (!pController)
		return;

	GhostState& st = g_Ghost[iSlot];
	st.bActive = true;
	st.bPendingRestore = false;
	st.bSilentDeath = false;
	st.flCooldownUntil = CurTime() + g_Config.flCooldown;
	st.flDyingAt = CurTime() + GHOST_DYING_DELAY;
	st.bExpectSpawn = true;
	st.flExpectSpawnUntil = CurTime() + 1.0f;
	g_bAnyGhostState = true;

	RespawnPlayer(pController);

	NextFrame([iSlot]() {
		if (IsGhost(iSlot))
			ApplyGhost(iSlot);
	});
}

///////////////////////////////////////
// Commands

void OnGhostCommand(int iSlot)
{
	Msg("[Ghost] !ghost from slot %d\n", iSlot);
	if (!IsValidSlot(iSlot))
		return;
	CCSPlayerController* pController = CCSPlayerController::FromSlot(iSlot);
	if (!pController)
		return;

	if (IsGhost(iSlot))
	{
		RemoveGhost(iSlot);
		PrintToChat(iSlot, CHAT_PREFIX "Ты вышел из режима призрака");
		return;
	}
	if (!g_bEnabled)
	{
		PrintToChat(iSlot, CHAT_PREFIX "Режим призрака отключён");
		return;
	}

	int iTeam = pController->m_iTeamNum();
	if (iTeam != 2 && iTeam != 3)
	{
		PrintToChat(iSlot, CHAT_PREFIX "Только для игроков за T или CT");
		return;
	}
	CCSGameRules* pRules = GetGameRules();
	if (pRules && pRules->m_bWarmupPeriod())
	{
		Msg("[Ghost] slot %d: refused, warmup\n", iSlot);
		PrintToChat(iSlot, CHAT_PREFIX "Сейчас нельзя: разминка или раунд не идёт");
		return;
	}
	CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
	if (!pPawn || pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
	{
		PrintToChat(iSlot, CHAT_PREFIX "Только для мёртвых игроков");
		return;
	}
	if (CurTime() < g_Ghost[iSlot].flCooldownUntil)
	{
		PrintToChat(iSlot, CHAT_PREFIX "Подожди %.0f сек.", g_Ghost[iSlot].flCooldownUntil - CurTime() + 0.5f);
		return;
	}
	MakeGhost(iSlot);
	PrintToChat(iSlot, CHAT_PREFIX "Ты призрак. Выйти: !unghost");
}

void OnUnGhostCommand(int iSlot)
{
	if (!IsGhost(iSlot))
		return;
	RemoveGhost(iSlot);
	PrintToChat(iSlot, CHAT_PREFIX "Ты вышел из режима призрака");
}

// returns true if the text (without ! or /) is a ghost command
bool RunChatCommand(int iSlot, const char* szCmd)
{
	char word[32];
	int i = 0;
	while (szCmd[i] && szCmd[i] != ' ' && szCmd[i] != '"' && i < (int)sizeof(word) - 1)
	{
		word[i] = szCmd[i];
		i++;
	}
	word[i] = '\0';

	if (!V_stricmp(word, "ghost") || !V_stricmp(word, "redie"))
	{
		NextFrame([iSlot]() { OnGhostCommand(iSlot); });
		return true;
	}
	if (!V_stricmp(word, "unghost"))
	{
		NextFrame([iSlot]() { OnUnGhostCommand(iSlot); });
		return true;
	}
	return false;
}

CON_COMMAND_F(mm_ghost_toggle, "Enable/disable ghost mode (server console)", FCVAR_NONE)
{
	if (context.GetPlayerSlot().Get() != -1)
		return;
	g_bEnabled = !g_bEnabled;
	if (!g_bEnabled)
		for (int i = 0; i < MAX_SLOTS; i++)
			RemoveGhost(i);
	Msg("[Ghost] %s\n", g_bEnabled ? "enabled" : "disabled");
}

CON_COMMAND_F(mm_ghost_status, "Show ghost plugin status (server console)", FCVAR_NONE)
{
	if (context.GetPlayerSlot().Get() != -1)
		return;
	int nGhosts = 0;
	for (int i = 0; i < MAX_SLOTS; i++)
		if (IsGhost(i))
			nGhosts++;
	Msg("[Ghost] %s, ghosts: %d, round active: %d\n", g_bEnabled ? "enabled" : "disabled", nGhosts, g_bRoundActive);
}

///////////////////////////////////////
// Event handlers (called from Hook_FireEvent)

void OnRoundStart()
{
	g_bRoundActive = true;
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		DeactivateGhost(i);
		CCSPlayerPawn* pPawn = g_Ghost[i].bPendingRestore ? GetPawn(i) : nullptr;
		if (pPawn && pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
		{
			RestorePawn(i);
			ResetSlot(i);
		}
		// otherwise stays pending: restored on this player's next spawn
	}
}

void OnRoundEnd()
{
	g_bRoundActive = false;
	// ghosts are alive for the game: they leave silently, otherwise they would "survive" the round
	for (int i = 0; i < MAX_SLOTS; i++)
		if (IsGhost(i))
			NextFrame([i]() { RemoveGhost(i); });
}

void OnPlayerDeath(int iSlot)
{
	// a ghost killed by something else: keep the body hidden, restore on the next spawn
	if (IsGhost(iSlot))
		DeactivateGhost(iSlot);
}

void OnPlayerTeam(int iSlot)
{
	if (!IsGhost(iSlot))
		return;
	// the game does not kill a "dead" (LIFE_DYING) pawn on team change: do it ourselves
	NextFrame([iSlot]() { RemoveGhost(iSlot); });
}

// Respawn by the game or another plugin (admin respawn, warmup...) turns a ghost back into a normal player.
void OnPlayerSpawn(int iSlot)
{
	if (!IsGhost(iSlot))
		return;
	if (g_Ghost[iSlot].bExpectSpawn) // ours (may come more than once); the flag expires in Hook_GameFrame
	{
		NextFrame([iSlot]() {
			if (IsGhost(iSlot))
				ApplyGhost(iSlot);
		});
		return;
	}
	DeactivateGhost(iSlot); // Hook_GameFrame restores the alive pawn
}

///////////////////////////////////////
// Hooks

// --- events: hooked on the CGameEventManager vtable, so no instance pointer is needed
KHook::Return<bool> Hook_FireEvent(IGameEventManager2* pThis, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (!pEvent)
		return {KHook::Action::Ignore, false};

	const char* szName = pEvent->GetName();
	if (!szName)
		return {KHook::Action::Ignore, false};

	static bool s_bLogged = false;
	if (!s_bLogged)
	{
		s_bLogged = true;
		Msg("[Ghost] FireEvent hook works (first event: %s)\n", szName);
	}

	if (!strcmp(szName, "player_death"))
	{
		int iSlot = pEvent->GetPlayerSlot("userid").Get();
		bool bHide = IsValidSlot(iSlot) && (g_Ghost[iSlot].bSilentDeath || IsGhostPawn(iSlot));
		OnPlayerDeath(iSlot);
		// the ghost's deaths never reach clients (no kill feed); other plugins still see the event
		if (bHide && !bDontBroadcast)
			return KHook::Recall<bool (IGameEventManager2::*)(IGameEvent*, bool)>(nullptr, {KHook::Action::Ignore, false}, pThis, pEvent, true);
	}
	else if (!strcmp(szName, "player_spawn"))
		OnPlayerSpawn(pEvent->GetPlayerSlot("userid").Get());
	else if (!strcmp(szName, "player_team"))
		OnPlayerTeam(pEvent->GetPlayerSlot("userid").Get());
	else if (!strcmp(szName, "round_start"))
		OnRoundStart();
	else if (!strcmp(szName, "round_end"))
		OnRoundEnd();

	return {KHook::Action::Ignore, false};
}
KHook::Virtual<IGameEventManager2, bool, IGameEvent*, bool> g_hkFireEvent(Hook_FireEvent, nullptr);
void* g_pGameEventManagerVTable = nullptr;

// --- chat: !ghost, !redie, !unghost (and /ghost etc. without showing the message)
KHook::Return<void> Hook_DispatchConCommand(ICvar* pThis, ConCommandRef cmd, const CCommandContext& ctx, const CCommand& args)
{
	int iSlot = ctx.GetPlayerSlot().Get();
	if (!IsValidSlot(iSlot) || args.ArgC() < 2)
		return {KHook::Action::Ignore};

	const char* szArg0 = args.Arg(0);
	if (V_stricmp(szArg0, "say") && V_stricmp(szArg0, "say_team"))
		return {KHook::Action::Ignore};

	const char* szText = args.Arg(1);
	while (*szText == '"' || *szText == ' ')
		szText++;
	if (*szText != '!' && *szText != '/')
		return {KHook::Action::Ignore};

	bool bSilent = *szText == '/';
	if (RunChatCommand(iSlot, szText + 1) && bSilent)
		return {KHook::Action::Supersede};

	return {KHook::Action::Ignore};
}
KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext&, const CCommand&> g_hkDispatchConCommand(Hook_DispatchConCommand, nullptr);

// --- client console: css_ghost / mm_ghost / css_redie / css_unghost ...
KHook::Return<void> Hook_ClientCommand(IServerGameClients* pThis, CPlayerSlot slot, const CCommand& args)
{
	int iSlot = slot.Get();
	if (!IsValidSlot(iSlot) || args.ArgC() < 1)
		return {KHook::Action::Ignore};

	const char* szCmd = args.Arg(0);
	if (!V_strnicmp(szCmd, "css_", 4) || !V_strnicmp(szCmd, "mm_", 3))
	{
		const char* szName = strchr(szCmd, '_') + 1;
		if (RunChatCommand(iSlot, szName))
			return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}
KHook::Virtual<IServerGameClients, void, CPlayerSlot, const CCommand&> g_hkClientCommand(Hook_ClientCommand, nullptr);

KHook::Return<void> Hook_ClientDisconnect(IServerGameClients* pThis, CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* pszName, uint64 xuid, const char* pszNetworkID)
{
	ResetSlot(slot.Get());
	UpdateAnyGhostState();
	return {KHook::Action::Ignore};
}
KHook::Virtual<IServerGameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*> g_hkClientDisconnect(nullptr, Hook_ClientDisconnect);

// --- map start: new entity system, reset everything
class GameSessionConfiguration_t
{};
class ISource2WorldSession;
KHook::Return<void> Hook_StartupServer(INetworkServerService* pThis, const GameSessionConfiguration_t& config, ISource2WorldSession* pSession, const char* szMap)
{
	RefreshGlobals();
	g_bRoundActive = false;
	for (int i = 0; i < MAX_SLOTS; i++)
		ResetSlot(i);
	g_bAnyGhostState = false;
	g_flNextReapply = 0.0f;
	g_NextFrame.clear();
	return {KHook::Action::Ignore};
}
KHook::Virtual<INetworkServerService, void, const GameSessionConfiguration_t&, ISource2WorldSession*, const char*> g_hkStartupServer(nullptr, Hook_StartupServer);

// Ghosts are alive for the game. When a team has no real (non-ghost) players alive,
// its ghosts leave silently so the game ends the round by elimination as usual.
void CheckTeamsEliminated()
{
	if (!g_bRoundActive)
		return;
	int nReal[4] = {}, nGhosts[4] = {};
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		CCSPlayerController* pController = CCSPlayerController::FromSlot(i);
		if (!pController)
			continue;
		int iTeam = pController->m_iTeamNum();
		if (iTeam != 2 && iTeam != 3)
			continue;
		if (IsGhost(i))
		{
			nGhosts[iTeam]++;
			continue;
		}
		CCSPlayerPawn* pPawn = pController->GetPlayerPawn();
		if (pPawn && pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE && !g_Ghost[i].bPendingRestore)
			nReal[iTeam]++;
	}
	for (int iTeam = 2; iTeam <= 3; iTeam++)
	{
		if (nGhosts[iTeam] == 0 || nReal[iTeam] > 0)
			continue;
		for (int i = 0; i < MAX_SLOTS; i++)
		{
			CCSPlayerController* pController = IsGhost(i) ? CCSPlayerController::FromSlot(i) : nullptr;
			if (pController && pController->m_iTeamNum() == iTeam)
			{
				RemoveGhost(i);
				PrintToChat(i, CHAT_PREFIX "В твоей команде никого не осталось, режим призрака выключен");
			}
		}
	}
}

// round_end without the event hook: the game sets the next round restart time when the round ends
void CheckRoundEndFallback()
{
	if (!g_bRoundActive)
		return;
	CCSGameRules* pRules = GetGameRules();
	if (pRules && pRules->m_flRestartRoundTime().GetTime() > CurTime())
		OnRoundEnd();
}

// --- per tick: next-frame queue, re-apply ghost state, LIFE_DYING after the delay, restore respawned players
KHook::Return<void> Hook_GameFrame(IServerGameDLL* pThis, bool simulating, bool bFirstTick, bool bLastTick)
{
	// the entity system is recreated on map change: never keep a stale pointer
	RefreshGlobals();
	if (!gpGlobals || !g_pEntitySystem)
		return {KHook::Action::Ignore};

	// round start fallback: works even if the FireEvent hook never fires
	static float s_flLastRoundStart = -1.0f;
	if (CCSGameRules* pRules = GetGameRules())
	{
		float t = pRules->m_fRoundStartTime().GetTime();
		if (t != s_flLastRoundStart)
		{
			bool bFirst = s_flLastRoundStart < 0.0f;
			s_flLastRoundStart = t;
			if (!bFirst && !g_bRoundActive)
				OnRoundStart();
			else if (bFirst)
				g_bRoundActive = true;
		}
	}

	if (!g_NextFrame.empty())
	{
		auto queue = std::move(g_NextFrame);
		g_NextFrame.clear();
		for (auto& fn : queue)
			fn();
	}

	if (!g_bAnyGhostState)
		return {KHook::Action::Ignore};

	CheckRoundEndFallback();
	CheckTeamsEliminated();

	float flNow = CurTime();
	bool bReapply = flNow >= g_flNextReapply;
	if (bReapply)
		g_flNextReapply = flNow + GHOST_REAPPLY_INTERVAL;

	for (int i = 0; i < MAX_SLOTS; i++)
	{
		GhostState& st = g_Ghost[i];
		if (st.bActive)
		{
			if (st.bExpectSpawn && flNow > st.flExpectSpawnUntil)
				st.bExpectSpawn = false;
			CCSPlayerPawn* pPawn = GetPawn(i);
			if (!pPawn)
				continue;
			if (bReapply || flNow < st.flDyingAt + 0.5f)
				ApplyGhost(i);
		}
		else if (st.bPendingRestore)
		{
			CCSPlayerPawn* pPawn = GetPawn(i);
			if (!pPawn)
				continue;
			// ghost body after round end / unghost is DYING or DEAD; ALIVE means the game respawned the player
			if (pPawn->m_lifeState() == LifeState_t::LIFE_ALIVE)
				RestorePawn(i);
			else if (bReapply && pPawn->m_lifeState() == LifeState_t::LIFE_DYING)
				StripWeapons(pPawn);
		}
	}
	UpdateAnyGhostState();
	return {KHook::Action::Ignore};
}
KHook::Virtual<IServerGameDLL, void, bool, bool, bool> g_hkGameFrame(nullptr, Hook_GameFrame);

// --- others do not receive the ghost pawn at all: no model, shadow, radar, footsteps.
// Spectators are left alone (pawn is NODRAW/LIFE_DYING anyway).
KHook::Return<void> Hook_CheckTransmit(ISource2GameEntities* pThis, CCheckTransmitInfo** ppInfoList, int infoCount, CBitVec<16384>& unionTransmitEdicts,
									   CBitVec<16384>& unk, const Entity2Networkable_t** pNetworkables, const uint16* pEntityIndicies, int nEntities)
{
	if (!g_bAnyGhostState || !g_pEntitySystem || !gpGlobals)
		return {KHook::Action::Ignore};

	int iHidden[MAX_SLOTS];
	int nHidden = 0;
	for (int j = 0; j < MAX_SLOTS; j++)
		if (IsGhostPawn(j) && GetPawn(j))
			iHidden[nHidden++] = j;
	if (!nHidden)
		return {KHook::Action::Ignore};

	for (int i = 0; i < infoCount; i++)
	{
		auto pInfo = (CCheckTransmitInfoExtended*)ppInfoList[i];
		int iViewer = pInfo->m_nPlayerSlot.Get();
		CCSPlayerController* pViewer = CCSPlayerController::FromSlot(iViewer);
		if (!pViewer || pViewer->GetPawnState() == STATE_OBSERVER_MODE)
			continue;

		for (int k = 0; k < nHidden; k++)
		{
			int j = iHidden[k];
			if (j == iViewer)
				continue; // always transmit to themselves
			CCSPlayerPawn* pPawn = GetPawn(j);
			if (!pPawn)
				continue;
			int iIndex = pPawn->entindex();
			pInfo->m_pTransmitEntity->Clear(iIndex);
			pInfo->m_pTransmitNonPlayers->Set(iIndex);
		}
	}
	return {KHook::Action::Ignore};
}
KHook::Virtual<ISource2GameEntities, void, CCheckTransmitInfo**, int, CBitVec<16384>&, CBitVec<16384>&, const Entity2Networkable_t**, const uint16*, int>
	g_hkCheckTransmit(nullptr, Hook_CheckTransmit);

// --- footsteps / jump / land sounds of a ghost are sent only to the ghost himself
KHook::Return<void> Hook_PostEventAbstract(IGameEventSystem* pThis, CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients,
										   INetworkMessageInternal* pEvent, const CNetMessage* pData, unsigned long nSize, NetChannelBufType_t bufType)
{
	if (!g_bAnyGhostState || !clients || !pEvent || !pData)
		return {KHook::Action::Ignore};
	NetMessageInfo_t* info = pEvent->GetNetMessageInfo();
	if (!info || !info->m_pBinding)
		return {KHook::Action::Ignore};
	const char* szName = info->m_pBinding->GetName();
	if (!szName || strcmp(szName, "CMsgSosStartSoundEvent"))
		return {KHook::Action::Ignore};

	auto msg = const_cast<CNetMessage*>(pData)->ToPB<CMsgSosStartSoundEvent>();
	int iSlot = GetSlotFromPawnEntity(UTIL_GetEntityByIndex(msg->source_entity_index()));
	if (!IsGhostPawn(iSlot))
		return {KHook::Action::Ignore};

	uint64* pClients = const_cast<uint64*>(clients);
	*pClients &= (1ull << iSlot);
	if (*pClients == 0)
		return {KHook::Action::Supersede};
	return {KHook::Action::Ignore};
}
KHook::Virtual<IGameEventSystem, void, CSplitScreenSlot, bool, int, const uint64*, INetworkMessageInternal*, const CNetMessage*, unsigned long, NetChannelBufType_t>
	g_hkPostEventAbstract(Hook_PostEventAbstract, nullptr);

///////////////////////////////////////
// Detours (signatures from the config)

// no damage to and from a ghost body
struct CTakeDamageResult;
KHook::Return<int64> Hook_TakeDamage(CBaseEntity* pThis, CTakeDamageInfo* pInfo, CTakeDamageResult* pResult)
{
	if (!g_bAnyGhostState)
		return {KHook::Action::Ignore, 0};
	if (IsGhostPawn(GetSlotFromPawnEntity(pThis)))
		return {KHook::Action::Supersede, 1};
	if (pInfo && IsGhostPawn(GetSlotFromPawnEntity(pInfo->m_hAttacker().Get())))
		return {KHook::Action::Supersede, 1};
	return {KHook::Action::Ignore, 0};
}
KHook::Member<CBaseEntity, int64, CTakeDamageInfo*, CTakeDamageResult*> g_hkTakeDamage(Hook_TakeDamage, nullptr);

// pickup / buy
enum class AcquireMethod : int
{
	PickUp,
	Buy,
};
enum class AcquireResult : int
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
KHook::Return<AcquireResult> Hook_CanAcquire(CCSPlayer_ItemServices* pThis, CEconItemView* pItem, AcquireMethod eMethod, uint64_t unk)
{
	if (!g_bAnyGhostState || !pThis)
		return {KHook::Action::Ignore, AcquireResult::Allowed};
	CCSPlayerPawn* pPawn = pThis->GetPawn();
	int iSlot = GetSlotFromPawnEntity(pPawn);
	// a ghost body (alive ghost or a hidden body after round end), but never a real respawned player
	if (IsGhostPawn(iSlot) && (g_Ghost[iSlot].bActive || pPawn->m_lifeState() == LifeState_t::LIFE_DYING))
		return {KHook::Action::Supersede, AcquireResult::NotAllowedByMode};
	return {KHook::Action::Ignore, AcquireResult::Allowed};
}
KHook::Member<CCSPlayer_ItemServices, AcquireResult, CEconItemView*, AcquireMethod, uint64_t> g_hkCanAcquire(Hook_CanAcquire, nullptr);

// +use target search: a ghost never finds anything to use (doors, buttons, weapons, C4, hostages)
class CCSPlayer_UseServices;
KHook::Return<CBaseEntity*> Hook_FindUseEntity(CCSPlayer_UseServices* pThis, float flUnk, int64_t nUnk)
{
	if (!g_bAnyGhostState || !pThis)
		return {KHook::Action::Ignore, nullptr};
	CCSPlayerPawn* pPawn = ((CPlayerPawnComponent*)pThis)->GetPawn();
	if (pPawn && IsGhostPawn(GetSlotFromPawnEntity(pPawn)))
		return {KHook::Action::Supersede, nullptr};
	return {KHook::Action::Ignore, nullptr};
}
KHook::Member<CCSPlayer_UseServices, CBaseEntity*, float, int64_t> g_hkFindUseEntity(Hook_FindUseEntity, nullptr);

// entity outputs (triggers, buttons, doors...) activated by a ghost are not fired
class CEntityIOOutput;
KHook::Return<void> Hook_FireOutputInternal(CEntityIOOutput* pThis, CEntityInstance* pActivator, CEntityInstance* pCaller, void* pArgs, float flDelay, void* pParamMap, const void* pValue)
{
	if (g_bAnyGhostState && IsGhostPawn(GetSlotFromPawnEntity(pActivator)))
		return {KHook::Action::Supersede};
	return {KHook::Action::Ignore};
}
KHook::Member<CEntityIOOutput, void, CEntityInstance*, CEntityInstance*, void*, float, void*, const void*> g_hkFireOutput(Hook_FireOutputInternal, nullptr);

// optional: strip +use / attacks from user commands
class CUserCmd;
void StripButtons(CSGOUserCmdPB* pCmd)
{
	if (!pCmd->has_base())
		return;
	CBaseUserCmdPB* pBase = pCmd->mutable_base();
	if (pBase->has_buttons_pb())
	{
		CInButtonStatePB* pButtons = pBase->mutable_buttons_pb();
		pButtons->set_buttonstate1(pButtons->buttonstate1() & ~(uint64)GHOST_BLOCKED_BUTTONS);
		pButtons->set_buttonstate2(pButtons->buttonstate2() & ~(uint64)GHOST_BLOCKED_BUTTONS);
		pButtons->set_buttonstate3(pButtons->buttonstate3() & ~(uint64)GHOST_BLOCKED_BUTTONS);
	}
	for (int i = 0; i < pBase->subtick_moves_size(); i++)
	{
		CSubtickMoveStep* pStep = pBase->mutable_subtick_moves(i);
		if (pStep->button() & GHOST_BLOCKED_BUTTONS)
			pStep->set_button(0);
	}
	pCmd->clear_attack1_start_history_index();
	pCmd->clear_attack2_start_history_index();
}
KHook::Return<void*> Hook_ProcessUsercmds(CCSPlayerController* pController, CUserCmd* cmds, int numcmds, bool paused, float margin)
{
	if (g_bAnyGhostState && pController && IsGhostPawn(pController->GetPlayerSlot()))
	{
		// CUserCmd layout: pad 0x10, CSGOUserCmdPB, pad 0x38
		int iStride = g_Config.iUserCmdSize > 0 ? g_Config.iUserCmdSize : (int)(g_Config.iUserCmdPbOffset + sizeof(CSGOUserCmdPB) + 0x38);
		for (int i = 0; i < numcmds; i++)
			StripButtons((CSGOUserCmdPB*)((uint8*)cmds + i * iStride + g_Config.iUserCmdPbOffset));
	}
	return {KHook::Action::Ignore, nullptr};
}
KHook::Member<CCSPlayerController, void*, CUserCmd*, int, bool, float> g_hkProcessUsercmds(Hook_ProcessUsercmds, nullptr);

///////////////////////////////////////
// Config / setup

void LoadConfig()
{
	KeyValues* hKv = new KeyValues("Ghost");
	const char* pszPath = "addons/ghost/ghost.ini";

	if (!hKv->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		Warning("[Ghost] Failed to load %s, using defaults\n", pszPath);
		delete hKv;
		return;
	}

	g_Config.flCooldown = hKv->GetFloat("cooldown", g_Config.flCooldown);
	g_Config.iOffGameEntitySystem = hKv->GetInt("offset_gameentitysystem", g_Config.iOffGameEntitySystem);
	g_Config.iOffCollisionRulesChanged = hKv->GetInt("offset_collisionruleschanged", g_Config.iOffCollisionRulesChanged);
	g_Config.iOffRespawn = hKv->GetInt("offset_respawn", g_Config.iOffRespawn);
	g_Config.iOffCommitSuicide = hKv->GetInt("offset_commitsuicide", g_Config.iOffCommitSuicide);
	g_Config.iOffStripWeapons = hKv->GetInt("offset_stripweapons", g_Config.iOffStripWeapons);
	g_Config.szSigSetPawn = hKv->GetString("sig_setpawn", g_Config.szSigSetPawn.c_str());
	g_Config.szSigTakeDamage = hKv->GetString("sig_takedamage", g_Config.szSigTakeDamage.c_str());
	g_Config.szSigCanAcquire = hKv->GetString("sig_canacquire", g_Config.szSigCanAcquire.c_str());
	g_Config.szSigFindUseEntity = hKv->GetString("sig_findusentity", g_Config.szSigFindUseEntity.c_str());
	g_Config.szSigFireOutput = hKv->GetString("sig_fireoutput", g_Config.szSigFireOutput.c_str());
	g_Config.szSigProcessUsercmds = hKv->GetString("sig_processusercmds", g_Config.szSigProcessUsercmds.c_str());
	g_Config.iUserCmdPbOffset = hKv->GetInt("usercmd_pb_offset", g_Config.iUserCmdPbOffset);
	g_Config.iUserCmdSize = hKv->GetInt("usercmd_size", g_Config.iUserCmdSize);
	delete hKv;
}

void* FindSignature(DynLibUtils::CModule& module, const std::string& szSig, const char* szName, const char* szWhatBreaks)
{
	if (szSig.empty())
	{
		Msg("[Ghost] %s disabled in config\n", szName);
		return nullptr;
	}
	void* pAddr = module.FindPattern(szSig.c_str()).RCast<void*>();
	if (!pAddr)
		Warning("[Ghost] %s signature not found: %s\n", szName, szWhatBreaks);
	return pAddr;
}

template <typename HOOK>
void SetupDetour(HOOK& hook, DynLibUtils::CModule& module, const std::string& szSig, const char* szName, const char* szWhatBreaks)
{
	void* pAddr = FindSignature(module, szSig, szName, szWhatBreaks);
	if (pAddr)
		hook.Configure(pAddr);
}

bool GhostPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_gameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pServerGameDLL, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_ANY(GetServerFactory, g_pServerGameClients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameEntities, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);

	g_SMAPI->AddListener(this, this);

	LoadConfig();

	DynLibUtils::CModule libserver(g_pServerGameDLL);

	// game events: hook FireEvent on the CGameEventManager vtable
	g_pGameEventManagerVTable = libserver.GetVirtualTableByName("CGameEventManager").RCast<void*>();
	if (!g_pGameEventManagerVTable)
	{
		V_snprintf(error, maxlen, "CGameEventManager vtable not found");
		return false;
	}
	g_hkFireEvent.Configure(&IGameEventManager2::FireEvent);
	g_hkFireEvent.AddGlobal((IGameEventManager2*)&g_pGameEventManagerVTable);

	g_hkGameFrame.Configure(&IServerGameDLL::GameFrame);
	g_hkGameFrame.Add(g_pServerGameDLL);
	g_hkClientCommand.Configure(&IServerGameClients::ClientCommand);
	g_hkClientCommand.Add(g_pServerGameClients);
	g_hkClientDisconnect.Configure(&IServerGameClients::ClientDisconnect);
	g_hkClientDisconnect.Add(g_pServerGameClients);
	g_hkStartupServer.Configure(&INetworkServerService::StartupServer);
	g_hkStartupServer.Add(g_pNetworkServerService);
	g_hkDispatchConCommand.Configure(&ICvar::DispatchConCommand);
	g_hkDispatchConCommand.Add(g_pCVar);
	g_hkCheckTransmit.Configure(&ISource2GameEntities::CheckTransmit);
	g_hkCheckTransmit.Add(g_pSource2GameEntities);
	g_hkPostEventAbstract.Configure(static_cast<void (IGameEventSystem::*)(CSplitScreenSlot, bool, int, const uint64*, INetworkMessageInternal*, const CNetMessage*, unsigned long, NetChannelBufType_t)>(&IGameEventSystem::PostEventAbstract));
	g_hkPostEventAbstract.Add(g_gameEventSystem);

	UTIL_SetPawn = (decltype(UTIL_SetPawn))FindSignature(libserver, g_Config.szSigSetPawn, "CBasePlayerController::SetPawn", "respawn from spectator may fail");
	SetupDetour(g_hkTakeDamage, libserver, g_Config.szSigTakeDamage, "CBaseEntity::TakeDamageOld", "damage to/from ghosts is not blocked");
	SetupDetour(g_hkCanAcquire, libserver, g_Config.szSigCanAcquire, "CanAcquire", "pickup/buy are not blocked");
	SetupDetour(g_hkFindUseEntity, libserver, g_Config.szSigFindUseEntity, "FindUseEntity", "+use is not blocked");
	SetupDetour(g_hkFireOutput, libserver, g_Config.szSigFireOutput, "FireOutputInternal", "triggers/buttons activated by a ghost still fire");
	SetupDetour(g_hkProcessUsercmds, libserver, g_Config.szSigProcessUsercmds, "ProcessUsercmds", "buttons are not stripped from user commands");

	META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);

	if (late)
		RefreshGlobals();

	Msg("[Ghost] loaded\n");
	return true;
}

bool GhostPlugin::Unload(char* error, size_t maxlen)
{
	// leave nobody as a ghost and nobody hidden/unhittable
	if (g_pEntitySystem)
	{
		for (int i = 0; i < MAX_SLOTS; i++)
		{
			RemoveGhost(i);
			if (g_Ghost[i].bPendingRestore)
				RestorePawn(i);
			ResetSlot(i);
		}
	}
	g_bAnyGhostState = false;

	g_hkFireEvent.RemoveGlobal((IGameEventManager2*)&g_pGameEventManagerVTable);
	g_hkGameFrame.Remove(g_pServerGameDLL);
	g_hkClientCommand.Remove(g_pServerGameClients);
	g_hkClientDisconnect.Remove(g_pServerGameClients);
	g_hkStartupServer.Remove(g_pNetworkServerService);
	g_hkDispatchConCommand.Remove(g_pCVar);
	g_hkCheckTransmit.Remove(g_pSource2GameEntities);
	g_hkPostEventAbstract.Remove(g_gameEventSystem);

	ConVar_Unregister();
	return true;
}

void GhostPlugin::AllPluginsLoaded()
{
}
