#pragma once
#include "ehandle.h"
#include "CBaseEntity.h"
#include "CBaseModelEntity.h"
#include "services.h"

class CBasePlayerController;

class CBasePlayerPawn : public CBaseModelEntity
{
public:
	DECLARE_SCHEMA_CLASS(CBasePlayerPawn);

	SCHEMA_FIELD(CPlayer_MovementServices*, m_pMovementServices)
	SCHEMA_FIELD(CCSPlayer_WeaponServices*, m_pWeaponServices)
	SCHEMA_FIELD(CCSPlayer_ItemServices*, m_pItemServices)
	SCHEMA_FIELD(CPlayer_ObserverServices*, m_pObserverServices)
	SCHEMA_FIELD(CPlayer_CameraServices*, m_pCameraServices)
	SCHEMA_FIELD(CHandle<CBasePlayerController>, m_hController)
	SCHEMA_FIELD(QAngle, v_angle)

	CBasePlayerController* GetController() { return m_hController().Get(); }
};
