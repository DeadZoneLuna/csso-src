//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "cbase.h"
#include "weapon_csbasegun.h"


#if defined( CLIENT_DLL )

	#define CWeaponG3SG1 C_WeaponG3SG1
	#include "c_cs_player.h"

#else

	#include "cs_player.h"
	#include "KeyValues.h"

#endif


class CWeaponG3SG1 : public CWeaponCSBaseGun
{
public:
	DECLARE_CLASS( CWeaponG3SG1, CWeaponCSBaseGun );
	DECLARE_NETWORKCLASS(); 
	DECLARE_PREDICTABLE();
	
	CWeaponG3SG1();

	virtual void Spawn();
	virtual void SecondaryAttack();
	virtual void PrimaryAttack();
	virtual bool Reload();
	virtual bool Deploy();

	virtual CSWeaponID GetCSWeaponID( void ) const		{ return WEAPON_G3SG1; }

private:
	CWeaponG3SG1( const CWeaponG3SG1 & );

	float m_flLastFire;
};

IMPLEMENT_NETWORKCLASS_ALIASED( WeaponG3SG1, DT_WeaponG3SG1 )

BEGIN_NETWORK_TABLE( CWeaponG3SG1, DT_WeaponG3SG1 )
END_NETWORK_TABLE()

#if defined CLIENT_DLL
BEGIN_PREDICTION_DATA( CWeaponG3SG1 )
	DEFINE_FIELD( m_flLastFire, FIELD_FLOAT ),
END_PREDICTION_DATA()
#endif

LINK_ENTITY_TO_CLASS( weapon_g3sg1, CWeaponG3SG1 );
PRECACHE_WEAPON_REGISTER( weapon_g3sg1 );



CWeaponG3SG1::CWeaponG3SG1()
{
	m_flLastFire = gpGlobals->curtime;
}

void CWeaponG3SG1::Spawn()
{
	BaseClass::Spawn();
}


void CWeaponG3SG1::SecondaryAttack()
{
	const float kZoomTime = 0.10f;

	CCSPlayer *pPlayer = GetPlayerOwner();
	if ( !pPlayer )
		return;

	if ( pPlayer->GetFOV() == pPlayer->GetDefaultFOV() )
	{
		pPlayer->SetFOV( pPlayer, 40, kZoomTime );
		m_weaponMode = Secondary_Mode;
		m_fAccuracyPenalty += GetCSWpnData().m_fInaccuracyAltSwitch;
	}
	else if (pPlayer->GetFOV() == 40)
	{
		pPlayer->SetFOV( pPlayer, 15, kZoomTime );
		m_weaponMode = Secondary_Mode;
	}
	else if (pPlayer->GetFOV() == 15)
	{
		pPlayer->SetFOV( pPlayer, pPlayer->GetDefaultFOV(), kZoomTime );
		m_weaponMode = Primary_Mode;
	}

#ifndef CLIENT_DLL
	// If this isn't guarded, the sound will be emitted twice, once by the server and once by the client.
	// Let the server play it since if only the client plays it, it's liable to get played twice cause of
	// a prediction error. joy.
	
	//=============================================================================
	// HPE_BEGIN:
	// [tj] Playing this from the player so that we don't try to play the sound outside the level.
	//=============================================================================
	if ( GetPlayerOwner() )
	{
		GetPlayerOwner()->EmitSound( "Default.Zoom" ); // zoom sound
	}
	//=============================================================================
	// HPE_END
	//=============================================================================
	// let the bots hear the rifle zoom
	IGameEvent * event = gameeventmanager->CreateEvent( "weapon_zoom" );
	if( event )
	{
		event->SetInt( "userid", pPlayer->GetUserID() );
		gameeventmanager->FireEvent( event );
	}
#endif

	m_flNextSecondaryAttack = gpGlobals->curtime + 0.3f;   
	m_zoomFullyActiveTime = gpGlobals->curtime + 0.3; // The worst zoom time from above.  
}

void CWeaponG3SG1::PrimaryAttack()
{
	CCSPlayer *pPlayer = GetPlayerOwner();
	if ( !pPlayer )
		return;

	m_flLastFire = gpGlobals->curtime;

	if ( !CSBaseGunFire( GetCSWpnData().m_flCycleTime[m_weaponMode], m_weaponMode ) )
		return;
}


bool CWeaponG3SG1::Reload()
{
	bool ret = BaseClass::Reload();
	
	m_weaponMode = Primary_Mode;
	
	return ret;
}

bool CWeaponG3SG1::Deploy()
{
	bool ret = BaseClass::Deploy();
	
	m_weaponMode = Primary_Mode;
	
	return ret;
}
