//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Implements shared baseplayer class functionality
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"
#include "movevars_shared.h"
#include "util_shared.h"
#include "datacache/imdlcache.h"
#include "cs_ammodef.h"
#if defined ( TF_DLL ) || defined ( TF_CLIENT_DLL )
#include "tf_gamerules.h"
#endif

#if defined( CLIENT_DLL )

	#include "iclientvehicle.h"
	#include "prediction.h"
	#include "c_basedoor.h"
	#include "c_world.h"
	#include "view.h"
	#include "client_virtualreality.h"
	#define CRecipientFilter C_RecipientFilter
	#include "sourcevr/isourcevirtualreality.h"

#else

	#include "iservervehicle.h"
	#include "trains.h"
	#include "world.h"
	#include "doors.h"
	#include "ai_basenpc.h"
	#include "env_zoom.h"

	extern int TrainSpeed(int iSpeed, int iMax);
	
#endif

#if defined( CSTRIKE_DLL )
#include "weapon_c4.h"
#endif // CSTRIKE_DLL

#include "in_buttons.h"
#include "engine/IEngineSound.h"
#include "tier0/vprof.h"
#include "SoundEmitterSystem/isoundemittersystembase.h"
#include "decals.h"
#include "obstacle_pushaway.h"
#ifdef SIXENSE
#include "sixense/in_sixense.h"
#endif

// NVNT haptic utils
#include "haptics/haptic_utils.h"
// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#if defined(GAME_DLL) && !defined(_XBOX)
	extern ConVar sv_pushaway_max_force;
	extern ConVar sv_pushaway_force;
	extern ConVar sv_turbophysics;

	class CUsePushFilter : public CTraceFilterEntitiesOnly
	{
	public:
		bool ShouldHitEntity( IHandleEntity *pHandleEntity, int contentsMask )
		{
			CBaseEntity *pEntity = EntityFromEntityHandle( pHandleEntity );

			// Static prop case...
			if ( !pEntity )
				return false;

			// Only impact on physics objects
			if ( !pEntity->VPhysicsGetObject() )
				return false;

#if defined( CSTRIKE_DLL )
			// don't push the bomb!
			if ( dynamic_cast<CC4*>( pEntity ) )
				return false;
#endif // CSTRIKE_DLL

			return g_pGameRules->CanEntityBeUsePushed( pEntity );
		}
	};
#endif

ConVar sv_infinite_ammo( "sv_infinite_ammo", "0", FCVAR_REPLICATED, "Player's active weapon will never run out of ammo. If set to 2 then player has infinite total ammo but still has to reload the magazine." );

ConVar view_punch_decay( "view_punch_decay", "18", FCVAR_CHEAT | FCVAR_REPLICATED, "Decay factor exponent for view punch" );
ConVar view_recoil_tracking( "view_recoil_tracking", "0.45", FCVAR_CHEAT | FCVAR_REPLICATED, "How closely the view tracks with the aim punch from weapon recoil" );

#ifdef CLIENT_DLL
ConVar mp_usehwmmodels( "mp_usehwmmodels", "0", NULL, "Enable the use of the hw morph models. (-1 = never, 1 = always, 0 = based upon GPU)" ); // -1 = never, 0 = if hasfastvertextextures, 1 = always
#endif

bool UseHWMorphModels()
{
// #ifdef CLIENT_DLL 
// 	if ( mp_usehwmmodels.GetInt() == 0 )
// 		return g_pMaterialSystemHardwareConfig->HasFastVertexTextures();
// 
// 	return mp_usehwmmodels.GetInt() > 0;
// #else
// 	return false;
// #endif
	return false;
}

void CopySoundNameWithModifierToken( char *pchDest, const char *pchSource, int nMaxLenInChars, const char *pchToken )
{
	// Copy the sound name
	int nSource = 0;
	int nDest = 0;
	bool bFoundPeriod = false;

	while ( pchSource[ nSource ] != '\0' && nDest < nMaxLenInChars - 2 )
	{
		pchDest[ nDest ] = pchSource[ nSource ];
		nDest++;
		nSource++;

		if ( !bFoundPeriod && pchSource[ nSource - 1 ] == '.' )
		{
			// Insert special token after the period
			bFoundPeriod = true;

			int nToken = 0;

			while ( pchToken[ nToken ] != '\0' && nDest < nMaxLenInChars - 2 )
			{
				pchDest[ nDest ] = pchToken[ nToken ];
				nDest++;
				nToken++;
			}
		}
	}

	pchDest[ nDest ] = '\0';
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : float
//-----------------------------------------------------------------------------
float CBasePlayer::GetTimeBase( void ) const
{
	return m_nTickBase * TICK_INTERVAL;
}

float CBasePlayer::GetPlayerMaxSpeed()
{
	// player max speed is the lower limit of m_flMaxSpeed and sv_maxspeed
	float fMaxSpeed = sv_maxspeed.GetFloat();
	if ( MaxSpeed() > 0.0f && MaxSpeed() < fMaxSpeed )
		fMaxSpeed = MaxSpeed();

	return fMaxSpeed;
}

//-----------------------------------------------------------------------------
// Purpose: Called every usercmd by the player PreThink
//-----------------------------------------------------------------------------
void CBasePlayer::ItemPreFrame()
{
	// Handle use events
	PlayerUse();

	CBaseCombatWeapon *pActive = GetActiveWeapon();

	// Allow all the holstered weapons to update
	for ( int i = 0; i < WeaponCount(); ++i )
	{
		CBaseCombatWeapon *pWeapon = GetWeapon( i );

		if ( pWeapon == NULL )
			continue;

		if ( pActive == pWeapon )
			continue;

		pWeapon->ItemHolsterFrame();
	}

    if ( gpGlobals->curtime < m_flNextAttack )
		return;

	if (!pActive)
		return;

#if defined( CLIENT_DLL )
	// Not predicting this weapon
	if ( !pActive->IsPredicted() )
		return;
#endif

	pActive->ItemPreFrame();
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CBasePlayer::UsingStandardWeaponsInVehicle( void )
{
	Assert( IsInAVehicle() );
#if !defined( CLIENT_DLL )
	IServerVehicle *pVehicle = GetVehicle();
#else
	IClientVehicle *pVehicle = GetVehicle();
#endif
	Assert( pVehicle );
	if ( !pVehicle )
		return true;

	// NOTE: We *have* to do this before ItemPostFrame because ItemPostFrame
	// may dump us out of the vehicle
	int nRole = pVehicle->GetPassengerRole( this );
	bool bUsingStandardWeapons = pVehicle->IsPassengerUsingStandardWeapons( nRole );

	// Fall through and check weapons, etc. if we're using them 
	if (!bUsingStandardWeapons )
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Called every usercmd by the player PostThink
//-----------------------------------------------------------------------------
void CBasePlayer::ItemPostFrame()
{
	VPROF( "CBasePlayer::ItemPostFrame" );

	// Put viewmodels into basically correct place based on new player origin
	CalcViewModelView( EyePosition(), EyeAngles() );

	// Don't process items while in a vehicle.
	if ( GetVehicle() )
	{
#if defined( CLIENT_DLL )
		IClientVehicle *pVehicle = GetVehicle();
#else
		IServerVehicle *pVehicle = GetVehicle();
#endif

		bool bUsingStandardWeapons = UsingStandardWeaponsInVehicle();

#if defined( CLIENT_DLL )
		if ( pVehicle->IsPredicted() )
#endif
		{
			pVehicle->ItemPostFrame( this );
		}

		if (!bUsingStandardWeapons || !GetVehicle())
			return;
	}


	// check if the player is using something
	if ( m_hUseEntity != NULL )
	{
#if !defined( CLIENT_DLL )
		Assert( !IsInAVehicle() );
		ImpulseCommands();// this will call playerUse
#endif
		return;
	}

    if ( gpGlobals->curtime < m_flNextAttack )
	{
		if ( GetActiveWeapon() )
		{
			GetActiveWeapon()->ItemBusyFrame();
		}
	}
	else
	{
		if ( GetActiveWeapon() && (!IsInAVehicle() || UsingStandardWeaponsInVehicle()) )
		{
#if defined( CLIENT_DLL )
			// Not predicting this weapon
			if ( GetActiveWeapon()->IsPredicted() )
#endif

			{
				GetActiveWeapon()->ItemPostFrame( );
			}
		}
	}

#if !defined( CLIENT_DLL )
	ImpulseCommands();
#else
	// NOTE: If we ever support full impulse commands on the client,
	// remove this line and call ImpulseCommands instead.
	m_nImpulse = 0;
#endif

	extern ConVar sv_infinite_ammo;
	if ( (sv_infinite_ammo.GetInt() == 1) && (GetActiveWeapon() != NULL) )
	{
		CBaseCombatWeapon *pWeapon = GetActiveWeapon();

		pWeapon->m_iClip1 = pWeapon->GetMaxClip1();
		pWeapon->m_iClip2 = pWeapon->GetMaxClip2();

#if defined( GAME_DLL )

		if ( pWeapon->GetWpnData().iFlags & ITEM_FLAG_EXHAUSTIBLE )
		{
			int iPrimaryAmmoType = pWeapon->GetPrimaryAmmoType();
			if ( iPrimaryAmmoType >= 0 )
				SetAmmoCount( GetAmmoDef()->MaxCarry( iPrimaryAmmoType, this ), iPrimaryAmmoType );

			int iSecondaryAmmoType = pWeapon->GetSecondaryAmmoType();
			if ( iSecondaryAmmoType >= 0 )
				SetAmmoCount( GetAmmoDef()->MaxCarry( iSecondaryAmmoType, this ), iSecondaryAmmoType );
		}
		else
		{
			pWeapon->SetReserveAmmoCount( AMMO_POSITION_PRIMARY, pWeapon->GetReserveAmmoMax( AMMO_POSITION_PRIMARY ), true );
			pWeapon->SetReserveAmmoCount( AMMO_POSITION_SECONDARY, pWeapon->GetReserveAmmoMax( AMMO_POSITION_SECONDARY ), true );
		}
#endif
	}
}


//-----------------------------------------------------------------------------
// Eye angles
//-----------------------------------------------------------------------------
const QAngle &CBasePlayer::EyeAngles( )
{
	// NOTE: Viewangles are measured *relative* to the parent's coordinate system
	CBaseEntity *pMoveParent = const_cast<CBasePlayer*>(this)->GetMoveParent();

	if ( !pMoveParent )
	{
		return pl.v_angle;
	}

	// FIXME: Cache off the angles?
	matrix3x4_t eyesToParent, eyesToWorld;
	AngleMatrix( pl.v_angle, eyesToParent );
	ConcatTransforms( pMoveParent->EntityToWorldTransform(), eyesToParent, eyesToWorld );

	static QAngle angEyeWorld;
	MatrixAngles( eyesToWorld, angEyeWorld );
	return angEyeWorld;
}


const QAngle &CBasePlayer::LocalEyeAngles()
{
	return pl.v_angle;
}

//-----------------------------------------------------------------------------
// Actual Eye position + angles
//-----------------------------------------------------------------------------
Vector CBasePlayer::EyePosition( )
{
	if ( GetVehicle() != NULL )
	{
		// Return the cached result
		CacheVehicleView();
		return m_vecVehicleViewOrigin;
	}
	else
	{
#ifdef CLIENT_DLL
		if ( IsObserver() )
		{
			if ( GetObserverMode() == OBS_MODE_CHASE || GetObserverMode() == OBS_MODE_POI )
			{
				if ( IsLocalPlayer() )
				{
					return MainViewOrigin();
				}
			}
		}
#endif
		return BaseClass::EyePosition();
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : 
// Output : const Vector
//-----------------------------------------------------------------------------
const Vector CBasePlayer::GetPlayerMins( void ) const
{
	if ( IsObserver() )
	{
		return VEC_OBS_HULL_MIN_SCALED( this );	
	}
	else
	{
		if ( GetFlags() & FL_DUCKING )
		{
			return VEC_DUCK_HULL_MIN_SCALED( this );
		}
		else
		{
			return VEC_HULL_MIN_SCALED( this );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : 
// Output : const Vector
//-----------------------------------------------------------------------------
const Vector CBasePlayer::GetPlayerMaxs( void ) const
{	
	if ( IsObserver() )
	{
		return VEC_OBS_HULL_MAX_SCALED( this );	
	}
	else
	{
		if ( GetFlags() & FL_DUCKING )
		{
			return VEC_DUCK_HULL_MAX_SCALED( this );
		}
		else
		{
			return VEC_HULL_MAX_SCALED( this );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Update the vehicle view, or simply return the cached position and angles
//-----------------------------------------------------------------------------
void CBasePlayer::CacheVehicleView( void )
{
	// If we've calculated the view this frame, then there's no need to recalculate it
	if ( m_nVehicleViewSavedFrame == gpGlobals->framecount )
		return;

#ifdef CLIENT_DLL
	IClientVehicle *pVehicle = GetVehicle();
#else
	IServerVehicle *pVehicle = GetVehicle();
#endif

	if ( pVehicle != NULL )
	{		
		int nRole = pVehicle->GetPassengerRole( this );

		// Get our view for this frame
		pVehicle->GetVehicleViewPosition( nRole, &m_vecVehicleViewOrigin, &m_vecVehicleViewAngles, &m_flVehicleViewFOV );
		m_nVehicleViewSavedFrame = gpGlobals->framecount;

#ifdef CLIENT_DLL
		if( UseVR() )
		{
			C_BaseAnimating *pVehicleAnimating = dynamic_cast<C_BaseAnimating *>( pVehicle );
			if( pVehicleAnimating )
			{
				int eyeAttachmentIndex = pVehicleAnimating->LookupAttachment( "vehicle_driver_eyes" );

				Vector vehicleEyeOrigin;
				QAngle vehicleEyeAngles;
				pVehicleAnimating->GetAttachment( eyeAttachmentIndex, vehicleEyeOrigin, vehicleEyeAngles );

				g_ClientVirtualReality.OverrideTorsoTransform( vehicleEyeOrigin, vehicleEyeAngles );
			}
		}
#endif
	}
}

//-----------------------------------------------------------------------------
// Returns eye vectors
//-----------------------------------------------------------------------------
void CBasePlayer::EyeVectors( Vector *pForward, Vector *pRight, Vector *pUp )
{
	if ( GetVehicle() != NULL )
	{
		// Cache or retrieve our calculated position in the vehicle
		CacheVehicleView();
		AngleVectors( m_vecVehicleViewAngles, pForward, pRight, pUp );
	}
	else
	{
		AngleVectors( EyeAngles(), pForward, pRight, pUp );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Returns the eye position and angle vectors.
//-----------------------------------------------------------------------------
void CBasePlayer::EyePositionAndVectors( Vector *pPosition, Vector *pForward,
										 Vector *pRight, Vector *pUp )
{
	// Handle the view in the vehicle
	if ( GetVehicle() != NULL )
	{
		CacheVehicleView();
		AngleVectors( m_vecVehicleViewAngles, pForward, pRight, pUp );
		
		if ( pPosition != NULL )
		{
			*pPosition = m_vecVehicleViewOrigin;
		}
	}
	else
	{
		VectorCopy( BaseClass::EyePosition(), *pPosition );
		AngleVectors( EyeAngles(), pForward, pRight, pUp );
	}
}

#ifdef CLIENT_DLL
surfacedata_t * CBasePlayer::GetFootstepSurface( const Vector &origin, const char *surfaceName )
{
	return physprops->GetSurfaceData( physprops->GetSurfaceIndex( surfaceName ) );
}
#endif

surfacedata_t *CBasePlayer::GetLadderSurface( const Vector &origin )
{
#ifdef CLIENT_DLL
	return GetFootstepSurface( origin, "ladder" );
#else
	return physprops->GetSurfaceData( physprops->GetSurfaceIndex( "ladder" ) );
#endif
}

void CBasePlayer::UpdateStepSound( surfacedata_t *psurface, const Vector &vecOrigin, const Vector &vecVelocity )
{
	bool bWalking;
	float fvol;
	Vector knee;
	Vector feet;
	float height;
	float speed;
	float velrun;
	float velwalk;
	bool fLadder;

	if ( m_flStepSoundTime > 0 )
	{
		m_flStepSoundTime -= 1000.0f * gpGlobals->frametime;
		if ( m_flStepSoundTime < 0 )
		{
			m_flStepSoundTime = 0;
		}
	}

	if ( m_flStepSoundTime > 0 )
		return;

	if ( GetFlags() & (FL_FROZEN|FL_ATCONTROLS))
		return;

	if ( GetMoveType() == MOVETYPE_NOCLIP || GetMoveType() == MOVETYPE_OBSERVER )
		return;

	if ( !sv_footsteps.GetFloat() )
		return;

	speed = VectorLength( vecVelocity );
	float groundspeed = Vector2DLength( vecVelocity.AsVector2D() );

	// determine if we are on a ladder
	fLadder = ( GetMoveType() == MOVETYPE_LADDER );

	GetStepSoundVelocities( &velwalk, &velrun );

	bool onground = ( GetFlags() & FL_ONGROUND );
	bool movingalongground = ( groundspeed > 0.0001f );
	bool moving_fast_enough =  ( speed >= velwalk );



	// To hear step sounds you must be either on a ladder or moving along the ground AND
	// You must be moving fast enough

	if ( !moving_fast_enough || !(fLadder || ( onground && movingalongground )) )
			return;

//	MoveHelper()->PlayerSetAnimation( PLAYER_WALK );

	bWalking = speed < velrun;

	VectorCopy( vecOrigin, knee );
	VectorCopy( vecOrigin, feet );

	height = GetPlayerMaxs()[ 2 ] - GetPlayerMins()[ 2 ];

	knee[2] = vecOrigin[2] + 0.2 * height;

	// find out what we're stepping in or on...
	if ( fLadder )
	{
		psurface = GetLadderSurface(vecOrigin);
		fvol = 0.5;

		SetStepSoundTime( STEPSOUNDTIME_ON_LADDER, bWalking );
	}
#ifdef CSTRIKE_DLL
	else if ( enginetrace->GetPointContents( knee ) & MASK_WATER )  // we want to use the knee for Cstrike, not the waist
#else
	else if ( GetWaterLevel() == WL_Waist )
#endif // CSTRIKE_DLL
	{
		static int iSkipStep = 0;

		if ( iSkipStep == 0 )
		{
			iSkipStep++;
			return;
		}

		if ( iSkipStep++ == 3 )
		{
			iSkipStep = 0;
		}
		psurface = physprops->GetSurfaceData( physprops->GetSurfaceIndex( "wade" ) );
		fvol = 0.65;
		SetStepSoundTime( STEPSOUNDTIME_WATER_KNEE, bWalking );
	}
	else if ( GetWaterLevel() == WL_Feet )
	{
		psurface = physprops->GetSurfaceData( physprops->GetSurfaceIndex( "water" ) );
		fvol = bWalking ? 0.2 : 0.5;

		SetStepSoundTime( STEPSOUNDTIME_WATER_FOOT, bWalking );
	}
	else
	{
		if ( !psurface )
			return;

		SetStepSoundTime( STEPSOUNDTIME_NORMAL, bWalking );

		switch ( psurface->game.material )
		{
		default:
		case CHAR_TEX_CONCRETE:						
			fvol = bWalking ? 0.2 : 0.5;
			break;

		case CHAR_TEX_METAL:	
			fvol = bWalking ? 0.2 : 0.5;
			break;

		case CHAR_TEX_DIRT:
			fvol = bWalking ? 0.25 : 0.55;
			break;

		case CHAR_TEX_VENT:	
			fvol = bWalking ? 0.4 : 0.7;
			break;

		case CHAR_TEX_GRATE:
			fvol = bWalking ? 0.2 : 0.5;
			break;

		case CHAR_TEX_TILE:	
			fvol = bWalking ? 0.2 : 0.5;
			break;

		case CHAR_TEX_SLOSH:
			fvol = bWalking ? 0.2 : 0.5;
			break;
		}
	}
	
	// play the sound
	// 65% volume if ducking
	if ( GetFlags() & FL_DUCKING )
	{
		fvol *= 0.65;
	}

#ifdef CSTRIKE_DLL
	fvol *= 0.5; // vanilla source 2013 soundsystem sucks
#endif
	PlayStepSound( feet, psurface, fvol, false );
}

ConVar sv_max_distance_transmit_footsteps( "sv_max_distance_transmit_footsteps", "1250.0", FCVAR_REPLICATED, "Maximum distance to transmit footstep sound effects." );

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : step - 
//			fvol - 
//			force - force sound to play
//-----------------------------------------------------------------------------
void CBasePlayer::PlayStepSound( Vector &vecOrigin, surfacedata_t *psurface, float fvol, bool bForce )
{
	if ( gpGlobals->maxClients > 1 && !sv_footsteps.GetFloat() )
		return;

#if defined( CLIENT_DLL )
	// during prediction play footstep sounds only once
	if ( prediction->InPrediction() && !prediction->IsFirstTimePredicted() )
		return;
#endif

	if ( !psurface )
		return;

	int nSide = m_Local.m_nStepside;
	unsigned short stepSoundName = nSide ? psurface->sounds.stepleft : psurface->sounds.stepright;
	if ( !stepSoundName )
		return;

	m_Local.m_nStepside = !nSide;

	CSoundParameters params;

	Assert( nSide == 0 || nSide == 1 );

	if ( m_StepSoundCache[ nSide ].m_usSoundNameIndex == stepSoundName )
	{
		params = m_StepSoundCache[ nSide ].m_SoundParameters;
	}
	else
	{
		IPhysicsSurfaceProps *physprops = MoveHelper()->GetSurfaceProps();
		
		// footstep sounds
#ifdef CSTRIKE_DLL
		const char *pRawSoundName = physprops->GetString( stepSoundName );
		const char *pSoundName = NULL;
		int const nStepCopyLen = V_strlen(pRawSoundName) + 4;
		char *szStep = ( char * ) stackalloc( nStepCopyLen );
		if ( GetTeamNumber() == TEAM_CT )
		{
			Q_snprintf(szStep, nStepCopyLen, "ct_%s", pRawSoundName);
		}
		else
		{
			Q_snprintf(szStep, nStepCopyLen, "t_%s", pRawSoundName);
		}

		pSoundName = szStep;
		if ( !CBaseEntity::GetParametersForSound( pSoundName, params, NULL ) )
		{
			DevMsg( "Can't find specific footstep sound! (%s) - Using the default instead. (%s)\n", pSoundName, pRawSoundName );
			pSoundName = pRawSoundName;
		}
#else
		const char *pSoundName = physprops->GetString( stepSoundName );
#endif
		if ( !CBaseEntity::GetParametersForSound( pSoundName, params, NULL ) )
			return;

		// Only cache if there's one option.  Otherwise we'd never here any other sounds
		if ( params.count == 1 )
		{
			m_StepSoundCache[ nSide ].m_usSoundNameIndex = stepSoundName;
			m_StepSoundCache[ nSide ].m_SoundParameters = params;
		}
	}

	CRecipientFilter filter;
	
#if defined( CLIENT_DLL )
	// make sure we hear our own jump
	filter.AddRecipient( this );
	if ( prediction->InPrediction() && !bForce )
	{
		// Only use these rules when in prediction.
		filter.UsePredictionRules();
	}
#endif

	if( !bForce )
	{
		filter.AddRecipientsByPAS( vecOrigin );
	}

#ifndef CLIENT_DLL
	// in MP, server removes all players in the vecOrigin's PVS, these players generate the footsteps client side
	if ( gpGlobals->maxClients > 1 && !bForce )
	{
		filter.RemoveRecipientsByPVS( vecOrigin );
	}
	
	if( bForce )
	{
		filter.AddAllPlayers();
	}
	// the client plays it's own sound
	filter.RemoveRecipient( this );

	// Don't transmit footsteps if they are outside maximum footstep transmission range.
	for ( int i = 0; i < filter.GetRecipientCount(); ++i )
	{
		int entIndex = filter.GetRecipientIndex( i );
		IHandleEntity* entity = gEntList.LookupEntityByNetworkIndex( entIndex );
		if ( entity )
		{
			CBasePlayer* player = dynamic_cast<CBasePlayer*>( gEntList.GetBaseEntity( entity->GetRefEHandle() ) );
			if ( player != NULL )
			{
				float dist = vecOrigin.DistTo( player->EyePosition() );
				if ( dist > sv_max_distance_transmit_footsteps.GetFloat() )
				{
					filter.RemoveRecipient( player );
				}
			}
		}
	}
#endif

// #if defined( CLIENT_DLL )
// 	Msg( "CLIENT_DLL: (PlayStepSound) filter recipients = %d\n", filter.GetRecipientCount() );
// #else
// 	Msg( "GAME_DLL: (PlayStepSound) filter recipients = %d\n", filter.GetRecipientCount() );
// #endif

	EmitSound_t ep;
	ep.m_nChannel = CHAN_BODY;
	ep.m_pSoundName = params.soundname;
	ep.m_flVolume = fvol;
	ep.m_SoundLevel = params.soundlevel;
	ep.m_nFlags = 0;
	ep.m_nPitch = params.pitch;
	ep.m_pOrigin = &vecOrigin;

	EmitSound( filter, entindex(), ep );

#ifdef CSTRIKE_DLL
	CSoundParameters paramsSuitSound;
	if (!CBaseEntity::GetParametersForSound((GetTeamNumber() == TEAM_CT) ? "CT_Default.Suit" : "T_Default.Suit", paramsSuitSound, NULL))
		return;

	EmitSound_t epSuitSound;
	epSuitSound.m_nChannel = CHAN_AUTO;
	epSuitSound.m_pSoundName = paramsSuitSound.soundname;
	epSuitSound.m_flVolume = fvol * 0.1; // vanilla source 2013 soundsystem sucks
	epSuitSound.m_SoundLevel = paramsSuitSound.soundlevel;
	epSuitSound.m_nFlags = 0;
	epSuitSound.m_nPitch = paramsSuitSound.pitch;
	epSuitSound.m_pOrigin = &vecOrigin;

	EmitSound(filter, entindex(), epSuitSound);
#endif
}

void CBasePlayer::UpdateButtonState( int nUserCmdButtonMask )
{
	// Track button info so we can detect 'pressed' and 'released' buttons next frame
	m_afButtonLast = m_nButtons;

	// Get button states
	m_nButtons = nUserCmdButtonMask;
 	int buttonsChanged = m_afButtonLast ^ m_nButtons;
	
	// Debounced button codes for pressed/released
	// UNDONE: Do we need auto-repeat?
	m_afButtonPressed =  buttonsChanged & m_nButtons;		// The changed ones still down are "pressed"
	m_afButtonReleased = buttonsChanged & (~m_nButtons);	// The ones not down are "released"
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CBasePlayer::GetStepSoundVelocities( float *velwalk, float *velrun )
{
	// UNDONE: need defined numbers for run, walk, crouch, crouch run velocities!!!!	
	if ( ( GetFlags() & FL_DUCKING) || ( GetMoveType() == MOVETYPE_LADDER ) )
	{
		*velwalk = 60;		// These constants should be based on cl_movespeedkey * cl_forwardspeed somehow
		*velrun = 80;		
	}
	else
	{
		*velwalk = 90;
		*velrun = 220;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CBasePlayer::SetStepSoundTime( stepsoundtimes_t iStepSoundTime, bool bWalking )
{
	switch ( iStepSoundTime )
	{
	case STEPSOUNDTIME_NORMAL:
	case STEPSOUNDTIME_WATER_FOOT:
		m_flStepSoundTime = bWalking ? 400 : 300;
		break;

	case STEPSOUNDTIME_ON_LADDER:
		m_flStepSoundTime = 350;
		break;

	case STEPSOUNDTIME_WATER_KNEE:
		m_flStepSoundTime = 600;
		break;

	default:
		Assert(0);
		break;
	}

	// UNDONE: need defined numbers for run, walk, crouch, crouch run velocities!!!!	
	if ( ( GetFlags() & FL_DUCKING) || ( GetMoveType() == MOVETYPE_LADDER ) )
	{
		m_flStepSoundTime += 100;
	}
}

Vector CBasePlayer::Weapon_ShootPosition( )
{
	return EyePosition();
}

void CBasePlayer::SetAnimationExtension( const char *pExtension )
{
	Q_strncpy( m_szAnimExtension, pExtension, sizeof(m_szAnimExtension) );
}


//-----------------------------------------------------------------------------
// Purpose: Set the weapon to switch to when the player uses the 'lastinv' command
//-----------------------------------------------------------------------------
void CBasePlayer::Weapon_SetLast( CBaseCombatWeapon *pWeapon )
{
	m_hLastWeapon = pWeapon;
}

//-----------------------------------------------------------------------------
// Purpose: Override to clear dropped weapon from the hud
//-----------------------------------------------------------------------------
void CBasePlayer::Weapon_Drop( CBaseCombatWeapon *pWeapon, const Vector *pvecTarget /* = NULL */, const Vector *pVelocity /* = NULL */ )
{
	bool bWasActiveWeapon = false;
	if ( pWeapon == GetActiveWeapon() )
	{
		bWasActiveWeapon = true;
	}

	if ( pWeapon )
	{
		if ( bWasActiveWeapon )
		{
			pWeapon->SendWeaponAnim( ACT_VM_IDLE );
		}
	}

#if defined( GAME_DLL )
	BaseClass::Weapon_Drop( pWeapon, pvecTarget, pVelocity );
#endif

	if ( bWasActiveWeapon )
	{
		if (!SwitchToNextBestWeapon( NULL ))
		{
			CBaseViewModel *vm = GetViewModel();
			if ( vm )
				vm->AddEffects( EF_NODRAW );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Override base class so player can reset autoaim
// Input  :
// Output :
//-----------------------------------------------------------------------------
bool CBasePlayer::Weapon_Switch( CBaseCombatWeapon *pWeapon, int viewmodelindex /*=0*/ ) 
{
	CBaseCombatWeapon *pLastWeapon = GetActiveWeapon();

	if ( BaseClass::Weapon_Switch( pWeapon, viewmodelindex ))
	{
		if ( pLastWeapon && Weapon_ShouldSetLast( pLastWeapon, GetActiveWeapon() ) )
		{
			Weapon_SetLast( pLastWeapon->GetLastWeapon() );
		}

		CBaseViewModel *pViewModel = GetViewModel( viewmodelindex );
		Assert( pViewModel );
		if ( pViewModel )
			pViewModel->RemoveEffects( EF_NODRAW );
		ResetAutoaim( );
		return true;
	}
	return false;
}

void CBasePlayer::SelectLastItem(void)
{
	if ( m_hLastWeapon.Get() == NULL )
		return;

	if ( GetActiveWeapon() && !GetActiveWeapon()->CanHolster() )
		return;

	SelectItem( m_hLastWeapon.Get()->GetClassname(), m_hLastWeapon.Get()->GetSubType() );
}


//-----------------------------------------------------------------------------
// Purpose: Abort any reloads we're in
//-----------------------------------------------------------------------------
void CBasePlayer::AbortReload( void )
{
	if ( GetActiveWeapon() )
	{
		GetActiveWeapon()->AbortReload();
	}
}

#if !defined( NO_ENTITY_PREDICTION )
void CBasePlayer::AddToPlayerSimulationList( CBaseEntity *other )
{
	CHandle< CBaseEntity > h;
	h = other;
	// Already in list
	if ( m_SimulatedByThisPlayer.Find( h ) != m_SimulatedByThisPlayer.InvalidIndex() )
		return;

	Assert( other->IsPlayerSimulated() );

	m_SimulatedByThisPlayer.AddToTail( h );
}

//-----------------------------------------------------------------------------
// Purpose: Fixme, this should occur if the player fails to drive simulation
//  often enough!!!
// Input  : *other - 
//-----------------------------------------------------------------------------
void CBasePlayer::RemoveFromPlayerSimulationList( CBaseEntity *other )
{
	if ( !other )
		return;

	Assert( other->IsPlayerSimulated() );
	Assert( other->GetSimulatingPlayer() == this );


	CHandle< CBaseEntity > h;
	h = other;

	m_SimulatedByThisPlayer.FindAndRemove( h );
}

void CBasePlayer::SimulatePlayerSimulatedEntities( void )
{
	int c = m_SimulatedByThisPlayer.Count();
	int i;

	for ( i = c - 1; i >= 0; i-- )
	{
		CHandle< CBaseEntity > h;
		
		h = m_SimulatedByThisPlayer[ i ];
		CBaseEntity *e = h;

		if ( !e || !e->IsPlayerSimulated() )
		{
			m_SimulatedByThisPlayer.Remove( i );
			continue;
		}

#if defined( CLIENT_DLL )
		if ( e->IsClientCreated() && prediction->InPrediction() && !prediction->IsFirstTimePredicted() )
		{
			continue;
		}
#endif
		Assert( e->IsPlayerSimulated() );
		Assert( e->GetSimulatingPlayer() == this );

		e->PhysicsSimulate();
	}

	// Loop through all entities again, checking their untouch if flagged to do so
	c = m_SimulatedByThisPlayer.Count();

	for ( i = c - 1; i >= 0; i-- )
	{
		CHandle< CBaseEntity > h;
		
		h = m_SimulatedByThisPlayer[ i ];

		CBaseEntity *e = h;
		if ( !e || !e->IsPlayerSimulated() )
		{
			m_SimulatedByThisPlayer.Remove( i );
			continue;
		}

#if defined( CLIENT_DLL )
		if ( e->IsClientCreated() && prediction->InPrediction() && !prediction->IsFirstTimePredicted() )
		{
			continue;
		}
#endif

		Assert( e->IsPlayerSimulated() );
		Assert( e->GetSimulatingPlayer() == this );

		if ( !e->GetCheckUntouch() )
			continue;

		e->PhysicsCheckForEntityUntouch();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CBasePlayer::ClearPlayerSimulationList( void )
{
	int c = m_SimulatedByThisPlayer.Size();
	int i;

	for ( i = c - 1; i >= 0; i-- )
	{
		CHandle< CBaseEntity > h;
		
		h = m_SimulatedByThisPlayer[ i ];
		CBaseEntity *e = h;
		if ( e )
		{
			e->UnsetPlayerSimulated();
		}
	}

	m_SimulatedByThisPlayer.RemoveAll();
}
#endif

//-----------------------------------------------------------------------------
// Purpose: Return true if we should allow selection of the specified item
//-----------------------------------------------------------------------------
bool CBasePlayer::Weapon_ShouldSelectItem( CBaseCombatWeapon *pWeapon )
{
	return ( pWeapon != GetActiveWeapon() );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CBasePlayer::SelectItem( const char *pstr, int iSubType )
{
	if (!pstr)
		return;

	CBaseCombatWeapon *pItem = Weapon_OwnsThisType( pstr, iSubType );

	if (!pItem)
		return;

	if( GetObserverMode() != OBS_MODE_NONE )
		return;// Observers can't select things.

	if ( !Weapon_ShouldSelectItem( pItem ) )
		return;

	// FIX, this needs to queue them up and delay
	// Make sure the current weapon can be holstered
	if ( GetActiveWeapon() )
	{
		if ( !GetActiveWeapon()->CanHolster() && !pItem->ForceWeaponSwitch() )
			return;

		ResetAutoaim( );
	}

	Weapon_Switch( pItem );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
ConVar sv_debug_player_use( "sv_debug_player_use", "0", FCVAR_REPLICATED, "Visualizes +use logic. Green cross=trace success, Red cross=trace too far, Green box=radius success" );
float IntervalDistance( float x, float x0, float x1 )
{
	// swap so x0 < x1
	if ( x0 > x1 )
	{
		float tmp = x0;
		x0 = x1;
		x1 = tmp;
	}

	if ( x < x0 )
		return x0-x;
	else if ( x > x1 )
		return x - x1;
	return 0;
}

CBaseEntity *CBasePlayer::FindUseEntity()
{
	Vector forward, up;
	// NOTE: This doesn't handle the case when the player is in a vehicle.
	AngleVectors( GetFinalAimAngle(), &forward, NULL, &up );

	trace_t tr;
	// Search for objects in a sphere (tests for entities that are not solid, yet still useable)
	Vector searchCenter = EyePosition();

	// NOTE: Some debris objects are useable too, so hit those as well
	// A button, etc. can be made out of clip brushes, make sure it's +useable via a traceline, too.
	int useableContents = MASK_SOLID | CONTENTS_DEBRIS | CONTENTS_PLAYERCLIP;

#ifdef CSTRIKE_DLL
	useableContents = (MASK_NPCSOLID_BRUSHONLY | MASK_OPAQUE_AND_NPCS) & ~CONTENTS_OPAQUE;
#endif

#ifndef CLIENT_DLL
	CBaseEntity *pFoundByTrace = NULL;
#endif

	// UNDONE: Might be faster to just fold this range into the sphere query
	CBaseEntity *pObject = NULL;

	float nearestDist = FLT_MAX;
	// try the hit entity if there is one, or the ground entity if there isn't.
	CBaseEntity *pNearest = NULL;

	const int NUM_TANGENTS = 8;

#if defined( CSTRIKE_DLL ) && defined( GAME_DLL )
	const int NUM_TRACES = 1;
#else
	const int NUM_TRACES = NUM_TANGENTS;
#endif
	// trace a box at successive angles down
	//							forward, 45 deg, 30 deg, 20 deg, 15 deg, 10 deg, -10, -15
	const float tangents[NUM_TANGENTS] = { 0, 1, 0.57735026919f, 0.3639702342f, 0.267949192431f, 0.1763269807f, -0.1763269807f, -0.267949192431f };
	for ( int i = 0; i < NUM_TRACES; i++ )
	{
		if ( i == 0 )
		{
			UTIL_TraceLine( searchCenter, searchCenter + forward * 1024, useableContents, this, COLLISION_GROUP_NONE, &tr );
		}
		else
		{
			Vector down = forward - tangents[i]*up;
			VectorNormalize(down);
			UTIL_TraceHull( searchCenter, searchCenter + down * 72, -Vector(16,16,16), Vector(16,16,16), useableContents, this, COLLISION_GROUP_NONE, &tr );
		}
		pObject = tr.m_pEnt;

#ifndef CLIENT_DLL
		pFoundByTrace = pObject;
#endif
		bool bUsable = IsUseableEntity(pObject, 0);
		while ( pObject && !bUsable && pObject->GetMoveParent() )
		{
			pObject = pObject->GetMoveParent();
			bUsable = IsUseableEntity(pObject, 0);
		}

		if ( bUsable )
		{
			Vector delta = tr.endpos - tr.startpos;
			float centerZ = CollisionProp()->WorldSpaceCenter().z;
			delta.z = IntervalDistance( tr.endpos.z, centerZ + CollisionProp()->OBBMins().z, centerZ + CollisionProp()->OBBMaxs().z );
			float dist = delta.Length();
#if defined( CSTRIKE_DLL )
			CCSPlayer *pPlayer = dynamic_cast<CCSPlayer*>( pObject );
			if ( (pPlayer && pPlayer->IsBot() && dist < PLAYER_USE_BOT_RADIUS) || dist < PLAYER_USE_RADIUS )
			{
#else
			if ( dist < PLAYER_USE_RADIUS )
			{
#endif
#ifndef CLIENT_DLL

				if ( sv_debug_player_use.GetBool() )
				{
					NDebugOverlay::Line( searchCenter, tr.endpos, 0, 255, 0, true, 30 );
					NDebugOverlay::Cross3D( tr.endpos, 16, 0, 255, 0, true, 30 );
				}

				if ( pObject->MyNPCPointer() && pObject->MyNPCPointer()->IsPlayerAlly( this ) )
				{
					// If about to select an NPC, do a more thorough check to ensure
					// that we're selecting the right one from a group.
					pObject = DoubleCheckUseNPC( pObject, searchCenter, forward );
				}
#endif
				if ( sv_debug_player_use.GetBool() )
				{
					Msg( "Trace using: %s\n", pObject ? pObject->GetDebugName() : "no usable entity found" );
				}

				pNearest = pObject;
				
				// if this is directly under the cursor just return it now
				if ( i == 0 )
					return pObject;
			}
		}
	}

	// check ground entity first
	// if you've got a useable ground entity, then shrink the cone of this search to 45 degrees
	// otherwise, search out in a 90 degree cone (hemisphere)
	if ( GetGroundEntity() && IsUseableEntity(GetGroundEntity(), FCAP_USE_ONGROUND) )
	{
		pNearest = GetGroundEntity();
	}
	if ( pNearest )
	{
		// estimate nearest object by distance from the view vector
		Vector point;
		pNearest->CollisionProp()->CalcNearestPoint( searchCenter, &point );
		nearestDist = CalcDistanceToLine( point, searchCenter, forward );
		if ( sv_debug_player_use.GetBool() )
		{
			Msg("Trace found %s, dist %.2f\n", pNearest->GetClassname(), nearestDist );
		}
	}

#if defined( CSTRIKE_DLL ) && defined( GAME_DLL )
	CCSPlayer* pPlayer = ToCSPlayer( this );
	const float MIN_DOT_FOR_WEAPONS = 0.99f;
#endif

	for ( CEntitySphereQuery sphere( searchCenter, PLAYER_USE_RADIUS ); ( pObject = sphere.GetCurrentEntity() ) != NULL; sphere.NextEntity() )
	{
		if ( !pObject )
			continue;

		if ( !IsUseableEntity( pObject, FCAP_USE_IN_RADIUS ) )
			continue;

		// see if it's more roughly in front of the player than previous guess
		Vector point;
		pObject->CollisionProp()->CalcNearestPoint( searchCenter, &point );

		float fMinimumDot = 0.8f; // pObject->GetUseLookAtAngle()

#if defined( CSTRIKE_DLL ) && defined( GAME_DLL )
		CWeaponCSBase *pWeapon = dynamic_cast<CWeaponCSBase*>( pObject );
		CSWeaponType nWepType = WEAPONTYPE_UNKNOWN;
		if ( pWeapon )
		{
			nWepType = pWeapon->GetWeaponType();

			if ( pPlayer->IsPrimaryOrSecondaryWeapon( nWepType ) )
				fMinimumDot = MIN_DOT_FOR_WEAPONS;
		}
#endif

		Vector dir = point - searchCenter;
		VectorNormalize(dir);
		float dot = DotProduct( dir, forward );

		// Need to be looking at the object more or less
		if ( dot < fMinimumDot )
			continue;

		float dist = CalcDistanceToLine( point, searchCenter, forward );

		if ( sv_debug_player_use.GetBool() )
		{
			Msg("Radius found %s, dist %.2f\n", pObject->GetClassname(), dist );
		}

		if ( dist < nearestDist )
		{
			// Since this has purely been a radius search to this point, we now
			// make sure the object isn't behind glass or a grate.
			trace_t trCheckOccluded;
			UTIL_TraceLine( searchCenter, point, useableContents, this, COLLISION_GROUP_NONE, &trCheckOccluded );

			if ( trCheckOccluded.fraction == 1.0 || trCheckOccluded.m_pEnt == pObject )
			{
				pNearest = pObject;
				nearestDist = dist;
			}
		}
	}

#ifndef CLIENT_DLL
	if ( !pNearest )
	{
		// Haven't found anything near the player to use, nor any NPC's at distance.
		// Check to see if the player is trying to select an NPC through a rail, fence, or other 'see-though' volume.
		trace_t trAllies;
		UTIL_TraceLine( searchCenter, searchCenter + forward * PLAYER_USE_RADIUS, MASK_OPAQUE_AND_NPCS, this, COLLISION_GROUP_NONE, &trAllies );

		if ( trAllies.m_pEnt && IsUseableEntity( trAllies.m_pEnt, 0 ) && trAllies.m_pEnt->MyNPCPointer() && trAllies.m_pEnt->MyNPCPointer()->IsPlayerAlly( this ) )
		{
			// This is an NPC, take it!
			pNearest = trAllies.m_pEnt;
		}
	}

	if ( pNearest && pNearest->MyNPCPointer() && pNearest->MyNPCPointer()->IsPlayerAlly( this ) )
	{
		pNearest = DoubleCheckUseNPC( pNearest, searchCenter, forward );
	}

	if ( sv_debug_player_use.GetBool() )
	{
		if ( !pNearest )
		{
			NDebugOverlay::Line( searchCenter, tr.endpos, 255, 0, 0, true, 30 );
			NDebugOverlay::Cross3D( tr.endpos, 16, 255, 0, 0, true, 30 );
		}
		else if ( pNearest == pFoundByTrace )
		{
			NDebugOverlay::Line( searchCenter, tr.endpos, 0, 255, 0, true, 30 );
			NDebugOverlay::Cross3D( tr.endpos, 16, 0, 255, 0, true, 30 );
		}
		else
		{
			NDebugOverlay::Box( pNearest->WorldSpaceCenter(), Vector(-8, -8, -8), Vector(8, 8, 8), 0, 255, 0, true, 30 );
		}
	}
#endif

	if ( sv_debug_player_use.GetBool() )
	{
		Msg( "Radial using: %s\n", pNearest ? pNearest->GetDebugName() : "no usable entity found" );
	}

	return pNearest;
}

//-----------------------------------------------------------------------------
// Purpose: Handles USE keypress
//-----------------------------------------------------------------------------
void CBasePlayer::PlayerUse ( void )
{
#ifdef GAME_DLL
	// Was use pressed or released?
	if ( ! ((m_nButtons | m_afButtonPressed | m_afButtonReleased) & IN_USE) )
		return;

	if ( IsObserver() )
	{
		// do special use operation in oberserver mode
		if ( m_afButtonPressed & IN_USE )
			ObserverUse( true );
		else if ( m_afButtonReleased & IN_USE )
			ObserverUse( false );
		
		return;
	}

#if !defined(_XBOX)
	// push objects in turbo physics mode
	if ( (m_nButtons & IN_USE) && sv_turbophysics.GetBool() )
	{
		Vector forward, up;
		EyeVectors( &forward, NULL, &up );

		trace_t tr;
		// Search for objects in a sphere (tests for entities that are not solid, yet still useable)
		Vector searchCenter = EyePosition();

		CUsePushFilter filter;

		UTIL_TraceLine( searchCenter, searchCenter + forward * 96.0f, MASK_SOLID, &filter, &tr );

		// try the hit entity if there is one, or the ground entity if there isn't.
		CBaseEntity *entity = tr.m_pEnt;

		if ( entity )
		{
			IPhysicsObject *pObj = entity->VPhysicsGetObject();

			if ( pObj )
			{
				Vector vPushAway = (entity->WorldSpaceCenter() - WorldSpaceCenter());
				vPushAway.z = 0;

				float flDist = VectorNormalize( vPushAway );
				flDist = MAX( flDist, 1 );

				float flForce = sv_pushaway_force.GetFloat() / flDist;
				flForce = MIN( flForce, sv_pushaway_max_force.GetFloat() );

				pObj->ApplyForceOffset( vPushAway * flForce, WorldSpaceCenter() );
			}
		}
	}
#endif

	if ( m_afButtonPressed & IN_USE )
	{
		// Controlling some latched entity?
		if ( ClearUseEntity() )
		{
			return;
		}
		else
		{
			if ( m_afPhysicsFlags & PFLAG_DIROVERRIDE )
			{
				m_afPhysicsFlags &= ~PFLAG_DIROVERRIDE;
				m_iTrain = TRAIN_NEW|TRAIN_OFF;
				return;
			}
			else
			{	// Start controlling the train!
				CBaseEntity *pTrain = GetGroundEntity();
				if ( pTrain && !(m_nButtons & IN_JUMP) && (GetFlags() & FL_ONGROUND) && (pTrain->ObjectCaps() & FCAP_DIRECTIONAL_USE) && pTrain->OnControls(this) )
				{
					m_afPhysicsFlags |= PFLAG_DIROVERRIDE;
					m_iTrain = TrainSpeed(pTrain->m_flSpeed, ((CFuncTrackTrain*)pTrain)->GetMaxSpeed());
					m_iTrain |= TRAIN_NEW;
					EmitSound( "Player.UseTrain" );
					return;
				}
			}
		}
	}

	CBaseEntity *pUseEntity = FindUseEntity();

#if defined( CSTRIKE_DLL )
	// in counterstrike, we need to allow the buy menu to open more easily
	// The old code defaulted to using whatever you were pointing at.
	// This code first checks to see if you're in a buy zone.  If that's true, 
	// then it ignores any weapon you may be pointing at. (the planted c4 is
	// not a weapon).

	if ( m_afButtonPressed & IN_USE )
	{
		CCSPlayer* pPlayer = ToCSPlayer( this );
		CWeaponCSBase *pWeapon = dynamic_cast<CWeaponCSBase*>(pUseEntity);
		CSWeaponType nWepType = WEAPONTYPE_UNKNOWN;
		if ( pWeapon )
			nWepType = pWeapon->GetWeaponType();

		bool bOpenBuyWithUse = true;
		if ( pPlayer )
		{
			const char *cl_useopensbuymenu = engine->GetClientConVarValue( ENTINDEX( pPlayer->edict() ), "cl_use_opens_buy_menu" );
			if ( cl_useopensbuymenu && atoi( cl_useopensbuymenu ) <= 0 )
				bOpenBuyWithUse = false;
		}

		if ( pWeapon && pPlayer->IsPrimaryOrSecondaryWeapon( nWepType ) )
		{
			bool bPickupIsSecondary = pWeapon->IsKindOf( WEAPONTYPE_PISTOL );
			CBaseCombatWeapon *pPlayerWeapon = NULL;

			if ( !bPickupIsSecondary )
			{
				pPlayerWeapon = pPlayer->Weapon_GetSlot( WEAPON_SLOT_RIFLE );
			}
			else
			{
				pPlayerWeapon = pPlayer->Weapon_GetSlot( WEAPON_SLOT_PISTOL );
			}

			// if the player has a weapon in the slot that occupies the weapon that they'd like to pick up
			// AND they are able to drop it, pick up the new weapon
			// OR if they don't have a weapon in that slot, go ahead and pick up the new weapon
			if ( (pPlayerWeapon && pPlayer->HandleDropWeapon( pPlayerWeapon, true )) || !pPlayerWeapon )
			{
				pWeapon->Touch( this );

				if ( pWeapon->IsPrimaryWeapon() )
				{
					// change the weapon to a picked up one if it's primary
					pPlayer->Weapon_Switch( pWeapon, 0 );
				}
			}
		}
		else if ( pPlayer && bOpenBuyWithUse && pPlayer->IsInBuyZone() && pPlayer->CanPlayerBuy( false ) )
		{
			bool bItemIsNullOrWeapon = true;

			if ( pUseEntity )
			{
				bItemIsNullOrWeapon = (pWeapon != 0);
			}

			if ( bItemIsNullOrWeapon )
			{
				engine->ClientCommand( edict(), "buymenu\n" );
				return;
			}
		}
		else if ( !pUseEntity && pPlayer && pPlayer->HasC4() && pPlayer->GetActiveCSWeapon() )
		{
			if ( pPlayer->m_bInBombZone && !(pPlayer->GetActiveCSWeapon()->IsA( WEAPON_C4 )) )
			{
				// we're in a bomb zone with C4, but it's not equipped.  Equip it.
				CWeaponCSBase	*pC4Weapon = NULL;

				//Search for the c4 weapon to use
				for ( int i = 0; i < pPlayer->WeaponCount(); i++ )
				{
					CBaseCombatWeapon* pWeaponBase = pPlayer->GetWeapon( i );
					CWeaponCSBase* pWeapon = dynamic_cast< CWeaponCSBase* > (pWeaponBase);

					if ( pWeapon == NULL )
					{
						continue;
					}


					if ( !pWeapon->IsA( WEAPON_C4 ) )
					{
						continue;
					}

					// Must be eligible for switching to.
					if ( !pPlayer->Weapon_CanSwitchTo( pWeapon ) )
					{
						continue;
					}

					pC4Weapon = pWeapon;
				}

				if ( pC4Weapon != NULL )
				{
					//pPlayer->SetLastWeaponBeforeAutoSwitchToC4( pPlayer->GetActiveCSWeapon() );
					pPlayer->Weapon_Switch( pC4Weapon, 0 );

					static_cast<CC4*>(pC4Weapon)->m_bIsPlantingViaUse = true;
				}
			}
			else if ( pPlayer->GetActiveCSWeapon()->IsA( WEAPON_C4 ) )
			{
				static_cast<CC4*>(pPlayer->GetActiveWeapon())->m_bIsPlantingViaUse = true;
			}
		}
		else if ( pUseEntity && pUseEntity->IsPlayer() )
		{
			// Bots can give their C4 to the requesting human.

			CCSPlayer* pPlayer = ToCSPlayer( this );
			CCSPlayer* pBot = ToCSPlayer( pUseEntity );

			if ( pPlayer && pPlayer->IsAlive() && pBot && pBot->IsBot() && (pBot->GetTeamNumber() == pPlayer->GetTeamNumber()) && pBot->HasC4() )
			{
				//distance check is implicit in +use, but check it anyway
				if ( (pBot->WorldSpaceCenter() - pPlayer->WorldSpaceCenter()).Length() < 200 )
				{
					CBaseCombatWeapon *pC4 = pBot->Weapon_OwnsThisType( "weapon_c4" );
					if ( pC4 )
					{
						pBot->SetBombDroppedTime( gpGlobals->curtime );
						pBot->CSWeaponDrop( pC4, WorldSpaceCenter(), false );
						pBot->Radio( "Radio.YouTakeThePoint", "#Cstrike_TitlesTXT_Game_afk_bomb_drop" );
					}
				}

			}
		}
	}

#endif

	// Found an object
	if ( pUseEntity )
	{

		//!!!UNDONE: traceline here to prevent +USEing buttons through walls			

		int caps = pUseEntity->ObjectCaps();
		variant_t emptyVariant;
		if ( ( (m_nButtons & IN_USE) && (caps & FCAP_CONTINUOUS_USE) ) || ( (m_afButtonPressed & IN_USE) && (caps & (FCAP_IMPULSE_USE|FCAP_ONOFF_USE)) ) )
		{
			if ( caps & FCAP_CONTINUOUS_USE )
			{
				m_afPhysicsFlags |= PFLAG_USING;
			}

			if ( pUseEntity->ObjectCaps() & FCAP_ONOFF_USE )
			{
				pUseEntity->AcceptInput( "Use", this, this, emptyVariant, USE_ON );
			}
			else
			{
				pUseEntity->AcceptInput( "Use", this, this, emptyVariant, USE_TOGGLE );
			}
		}
		// UNDONE: Send different USE codes for ON/OFF.  Cache last ONOFF_USE object to send 'off' if you turn away
		else if ( (m_afButtonReleased & IN_USE) && (pUseEntity->ObjectCaps() & FCAP_ONOFF_USE) )	// BUGBUG This is an "off" use
		{
			pUseEntity->AcceptInput( "Use", this, this, emptyVariant, USE_OFF );
		}
	}
	else if ( m_afButtonPressed & IN_USE )
	{
		PlayUseDenySound();
	}
#endif
}

ConVar	sv_suppress_viewpunch( "sv_suppress_viewpunch", "0", FCVAR_REPLICATED | FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY );

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CBasePlayer::ViewPunch( const QAngle &angleOffset )
{
	//See if we're suppressing the view punching
	if ( sv_suppress_viewpunch.GetBool() )
		return;

	// We don't allow view kicks in the vehicle
	if ( IsInAVehicle() )
		return;

	m_Local.m_viewPunchAngle += angleOffset;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CBasePlayer::ViewPunchReset( float tolerance )
{
	if ( tolerance != 0 )
	{
		tolerance *= tolerance;	// square
		float check = m_Local.m_viewPunchAngle->LengthSqr();
		if ( check > tolerance )
			return;
	}
	m_Local.m_viewPunchAngle = vec3_angle;
}

#if defined( CLIENT_DLL )

#include "iviewrender.h"
#include "ivieweffects.h"

#endif

static ConVar smoothstairs( "smoothstairs", "1", FCVAR_REPLICATED, "Smooth player eye z coordinate when traversing stairs." );

//-----------------------------------------------------------------------------
// Handle view smoothing when going up or down stairs
//-----------------------------------------------------------------------------
void CBasePlayer::SmoothViewOnStairs( Vector& eyeOrigin )
{
	CBaseEntity *pGroundEntity = GetGroundEntity();
	float flCurrentPlayerZ = GetLocalOrigin().z;
	float flCurrentPlayerViewOffsetZ = GetViewOffset().z;

	// Smooth out stair step ups
	// NOTE: Don't want to do this when the ground entity is moving the player
	if ( ( pGroundEntity != NULL && pGroundEntity->GetMoveType() == MOVETYPE_NONE ) && ( flCurrentPlayerZ != m_flOldPlayerZ ) && smoothstairs.GetBool() &&
		 m_flOldPlayerViewOffsetZ == flCurrentPlayerViewOffsetZ )
	{
		int dir = ( flCurrentPlayerZ > m_flOldPlayerZ ) ? 1 : -1;

		float steptime = gpGlobals->frametime;
		if (steptime < 0)
		{
			steptime = 0;
		}

		m_flOldPlayerZ += steptime * 150 * dir;

		const float stepSize = 18.0f;

		if ( dir > 0 )
		{
			if (m_flOldPlayerZ > flCurrentPlayerZ)
			{
				m_flOldPlayerZ = flCurrentPlayerZ;
			}
			if (flCurrentPlayerZ - m_flOldPlayerZ > stepSize)
			{
				m_flOldPlayerZ = flCurrentPlayerZ - stepSize;
			}
		}
		else
		{
			if (m_flOldPlayerZ < flCurrentPlayerZ)
			{
				m_flOldPlayerZ = flCurrentPlayerZ;
			}
			if (flCurrentPlayerZ - m_flOldPlayerZ < -stepSize)
			{
				m_flOldPlayerZ = flCurrentPlayerZ + stepSize;
			}
		}

		eyeOrigin[2] += m_flOldPlayerZ - flCurrentPlayerZ;
	}
	else
	{
		m_flOldPlayerZ = flCurrentPlayerZ;
		m_flOldPlayerViewOffsetZ = flCurrentPlayerViewOffsetZ;
	}
}

static bool IsWaterContents( int contents )
{
	if ( contents & MASK_WATER )
		return true;

//	if ( contents & CONTENTS_TESTFOGVOLUME )
//		return true;

	return false;
}

void CBasePlayer::ResetObserverMode()
{

	m_hObserverTarget.Set( 0 );
	m_iObserverMode = (int)OBS_MODE_NONE;

#ifndef CLIENT_DLL
	m_iObserverLastMode = OBS_MODE_ROAMING;
	m_bForcedObserverMode = false;
	m_afPhysicsFlags &= ~PFLAG_OBSERVER;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : eyeOrigin - 
//			eyeAngles - 
//			zNear - 
//			zFar - 
//			fov - 
//-----------------------------------------------------------------------------
void CBasePlayer::CalcView( Vector &eyeOrigin, QAngle &eyeAngles, float &zNear, float &zFar, float &fov )
{
#if defined( CLIENT_DLL )
	IClientVehicle *pVehicle; 
#else
	IServerVehicle *pVehicle;
#endif
	pVehicle = GetVehicle();

	if ( !pVehicle )
	{
#if defined( CLIENT_DLL )
		if( UseVR() )
			g_ClientVirtualReality.CancelTorsoTransformOverride();
#endif

		if ( IsObserver() )
		{
			CalcObserverView( eyeOrigin, eyeAngles, fov );
		}
		else
		{
			CalcPlayerView( eyeOrigin, eyeAngles, fov );
		}
	}
	else
	{
		CalcVehicleView( pVehicle, eyeOrigin, eyeAngles, zNear, zFar, fov );
	}
	// NVNT update fov on the haptics dll for input scaling.
#if defined( CLIENT_DLL )
	if(IsLocalPlayer() && haptics)
		haptics->UpdatePlayerFOV(fov);
#endif
}


void CBasePlayer::CalcViewModelView( const Vector& eyeOrigin, const QAngle& eyeAngles)
{
	for ( int i = 0; i < MAX_VIEWMODELS; i++ )
	{
		CBaseViewModel *vm = GetViewModel( i );
		if ( !vm )
			continue;
	
		vm->CalcViewModelView( this, eyeOrigin, eyeAngles );
	}
}

void CBasePlayer::CalcPlayerView( Vector& eyeOrigin, QAngle& eyeAngles, float& fov )
{
#if defined( CLIENT_DLL )
	if ( !prediction->InPrediction() )
	{
		// FIXME: Move into prediction
		view->DriftPitch();
	}
#endif

	VectorCopy( EyePosition(), eyeOrigin );
#ifdef SIXENSE
	if ( g_pSixenseInput->IsEnabled() )
	{
		VectorCopy( EyeAngles() + GetEyeAngleOffset(), eyeAngles );
	}
	else
	{
		VectorCopy( EyeAngles(), eyeAngles );
	}
#else
	VectorCopy( EyeAngles(), eyeAngles );
#endif

#if defined( CLIENT_DLL )
	if ( !prediction->InPrediction() )
#endif
	{
		SmoothViewOnStairs( eyeOrigin );
	}

	// Snack off the origin before bob + water offset are applied
	Vector vecBaseEyePosition = eyeOrigin;

	CalcViewRoll( eyeAngles );

	CalcAddViewmodelCameraAnimation( eyeOrigin, eyeAngles );
	
	// Apply punch angles
	VectorAdd( eyeAngles, m_Local.m_viewPunchAngle, eyeAngles );

	// TODO[pmf]: apply a scaling factor to this
	VectorAdd( eyeAngles, GetAimPunchAngle() * view_recoil_tracking.GetFloat(), eyeAngles );

#if defined( CLIENT_DLL )
	if ( !prediction->InPrediction() )
	{
		// Shake it up baby!
		vieweffects->CalcShake();
		vieweffects->ApplyShake( eyeOrigin, eyeAngles, 1.0 );
	}
#endif

#if defined( CLIENT_DLL )
	// Apply a smoothing offset to smooth out prediction errors.
	Vector vSmoothOffset;
	GetPredictionErrorSmoothingVector( vSmoothOffset );
	eyeOrigin += vSmoothOffset;
	m_flObserverChaseDistance = 0.0;
#endif

	// calc current FOV
	fov = GetFOV();
}

//-----------------------------------------------------------------------------
// Purpose: The main view setup function for vehicles
//-----------------------------------------------------------------------------
void CBasePlayer::CalcVehicleView( 
#if defined( CLIENT_DLL )
	IClientVehicle *pVehicle, 
#else
	IServerVehicle *pVehicle,
#endif
	Vector& eyeOrigin, QAngle& eyeAngles,
	float& zNear, float& zFar, float& fov )
{
	Assert( pVehicle );

	// Start with our base origin and angles
	CacheVehicleView();
	eyeOrigin = m_vecVehicleViewOrigin;
	eyeAngles = m_vecVehicleViewAngles;

#if defined( CLIENT_DLL )

	fov = GetFOV();

	// Allows the vehicle to change the clip planes
	pVehicle->GetVehicleClipPlanes( zNear, zFar );
#endif

	// Snack off the origin before bob + water offset are applied
	Vector vecBaseEyePosition = eyeOrigin;

	CalcViewRoll( eyeAngles );

	CalcAddViewmodelCameraAnimation( eyeOrigin, eyeAngles );

	// Apply punch angle
	VectorAdd( eyeAngles, m_Local.m_viewPunchAngle, eyeAngles );

#if defined( CLIENT_DLL )
	if ( !prediction->InPrediction() )
	{
		// Shake it up baby!
		vieweffects->CalcShake();
		vieweffects->ApplyShake( eyeOrigin, eyeAngles, 1.0 );
	}
#endif

}


void CBasePlayer::CalcObserverView( Vector& eyeOrigin, QAngle& eyeAngles, float& fov )
{
#if defined( CLIENT_DLL )
	switch ( GetObserverMode() )
	{

		case OBS_MODE_DEATHCAM	:	CalcDeathCamView( eyeOrigin, eyeAngles, fov );
									break;

		case OBS_MODE_ROAMING	:	// just copy current position without view offset
		case OBS_MODE_FIXED		:	CalcRoamingView( eyeOrigin, eyeAngles, fov );
									break;

		case OBS_MODE_IN_EYE	:	CalcInEyeCamView( eyeOrigin, eyeAngles, fov );
									break;

		case OBS_MODE_POI		: // PASSTIME
		case OBS_MODE_CHASE		:	CalcChaseCamView( eyeOrigin, eyeAngles, fov  );
									break;

		case OBS_MODE_FREEZECAM	:	CalcFreezeCamView( eyeOrigin, eyeAngles, fov  );
									break;
	}
#else
	// on server just copy target postions, final view positions will be calculated on client
	VectorCopy( EyePosition(), eyeOrigin );
	VectorCopy( EyeAngles(), eyeAngles );
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Compute roll angle for a particular lateral velocity
// Input  : angles - 
//			velocity - 
//			rollangle - 
//			rollspeed - 
// Output : float CViewRender::CalcRoll
//-----------------------------------------------------------------------------
float CBasePlayer::CalcRoll (const QAngle& angles, const Vector& velocity, float rollangle, float rollspeed)
{
    float   sign;
    float   side;
    float   value;
	
	Vector  forward, right, up;
	
    AngleVectors (angles, &forward, &right, &up);
	
	// Get amount of lateral movement
    side = DotProduct( velocity, right );
	// Right or left side?
    sign = side < 0 ? -1 : 1;
    side = fabs(side);
    
	value = rollangle;
	// Hit 100% of rollangle at rollspeed.  Below that get linear approx.
    if ( side < rollspeed )
	{
		side = side * value / rollspeed;
	}
    else
	{
		side = value;
	}

	// Scale by right/left sign
    return side*sign;
}

//-----------------------------------------------------------------------------
// Purpose: Allow the viewmodel to layer in artist-authored additive camera animation (to make some first-person anims 'punchier')
//-----------------------------------------------------------------------------
#define CAM_DRIVER_RETURN_TO_NORMAL 0.25f
#define CAM_DRIVER_RETURN_TO_NORMAL_GAIN 0.8f
void CBasePlayer::CalcAddViewmodelCameraAnimation( Vector& eyeOrigin, QAngle& eyeAngles )
{
#ifdef CLIENT_DLL
	CBaseViewModel *vm = GetViewModel();
	if ( vm && vm->GetModelPtr() )
	{
		float flTimeDelta = clamp( (gpGlobals->curtime - vm->m_flCamDriverAppliedTime), 0, CAM_DRIVER_RETURN_TO_NORMAL );
		if ( flTimeDelta < CAM_DRIVER_RETURN_TO_NORMAL )
		{
			vm->m_flCamDriverWeight = clamp( Gain( RemapValClamped( flTimeDelta, 0.0f, CAM_DRIVER_RETURN_TO_NORMAL, 1.0f, 0.0f ), CAM_DRIVER_RETURN_TO_NORMAL_GAIN ), 0, 1 );

			//eyeOrigin += (vm->m_vecCamDriverLastPos * vm->m_flCamDriverWeight);
			eyeAngles += (vm->m_angCamDriverLastAng * vm->m_flCamDriverWeight);
		}
		else
		{
			vm->m_flCamDriverWeight = 0;
		}
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Determine view roll, including data kick
//-----------------------------------------------------------------------------
void CBasePlayer::CalcViewRoll( QAngle& eyeAngles )
{
	if ( GetMoveType() == MOVETYPE_NOCLIP )
		return;

	float side = CalcRoll( GetAbsAngles(), GetAbsVelocity(), sv_rollangle.GetFloat(), sv_rollspeed.GetFloat() );
	eyeAngles[ROLL] += side;
}

#if defined( CLIENT_DLL )
extern ConVar cl_use_new_headbob;
//ConVar cl_headbob_freq( "cl_headbob_freq", "12", FCVAR_CLIENTDLL );
//ConVar cl_headbob_amp("cl_headbob_amp", "1.5", FCVAR_CLIENTDLL );
ConVar cl_headbob_land_dip_amt( "cl_headbob_land_dip_amt", "4", FCVAR_CLIENTDLL );
#endif 

void CBasePlayer::CalcViewBob( Vector& eyeOrigin )
{
#if defined( CLIENT_DLL )
	if ( cl_use_new_headbob.GetBool() == false )
		return;

	Vector vecBaseEyePosition = eyeOrigin;

	// if we just landed, dip the player's view
	float flOldFallVel = m_Local.m_flOldFallVelocity;
	float flFallVel = m_Local.m_flFallVelocity;
	//Msg("Fall Velocity: %f\n", flFallVel );

	if ( flFallVel <= 0.1f && flOldFallVel > 10.0f && flOldFallVel <= PLAYER_FATAL_FALL_SPEED && m_Local.m_bInLanding == false )
	{
		m_Local.m_bInLanding = true;
		m_Local.m_flLandingTime = gpGlobals->curtime;
	}

	// don't bob the view right now
	/*
	const float flMaxSpeed = sv_maxspeed.GetFloat();
	float flSpeedFactor;
	*/

	if ( m_Local.m_bInLanding == true )
	{
		float landseconds = MAX( gpGlobals->curtime - m_Local.m_flLandingTime, 0.0f );
		float landFraction = SimpleSpline( landseconds / 0.25f );
		clamp( landFraction, 0.0f, 1.0f );

		float flDipAmount = (1 / flOldFallVel) * 0.1f;

		int dipHighOffset = 64;
		int dipLowOffset = dipHighOffset - cl_headbob_land_dip_amt.GetInt();
		Vector temp = GetViewOffset();
		temp.z = ((dipLowOffset - flDipAmount) * landFraction) +
			(dipHighOffset * (1 - landFraction));

		if ( temp.z > dipHighOffset )
		{
			temp.z = dipHighOffset;
			m_Local.m_bInLanding = false;
		}
		eyeOrigin.z -= (dipHighOffset - temp.z);
		//SetViewOffset( temp );
	}
	else
	{
		// don't bob the view right now
		/*
		flSpeedFactor = GetAbsVelocity().Length() / flMaxSpeed;
		clamp( flSpeedFactor, 0.0f, 1.0f );
		eyeOrigin.z += flSpeedFactor * (sin(gpGlobals->curtime * cl_headbob_freq.GetFloat() ) * cl_headbob_amp.GetFloat());
		*/
	}

	// stop when our eyes get back to default
	if ( m_Local.m_bInLanding == true && ((eyeOrigin.z - 0.001f) >= vecBaseEyePosition.z) )
	{
		m_Local.m_bInLanding = false;
	}

	if ( m_Local.m_bInLanding == false )
	{
		// Set the old velocity to the new velocity, we check next frame to see if we hit the ground
		m_Local.m_flOldFallVelocity = m_Local.m_flFallVelocity;
	}
#endif
}


void CBasePlayer::DoMuzzleFlash()
{
	for ( int i = 0; i < MAX_VIEWMODELS; i++ )
	{
		CBaseViewModel *vm = GetViewModel( i );
		if ( !vm )
			continue;

		vm->DoMuzzleFlash();
	}

	BaseClass::DoMuzzleFlash();
}


float CBasePlayer::GetFOVDistanceAdjustFactor()
{
	float defaultFOV	= (float)GetDefaultFOV();
	float localFOV		= (float)GetFOV();

	if ( localFOV == defaultFOV || defaultFOV < 0.001f )
	{
		return 1.0f;
	}

	// If FOV is lower, then we're "zoomed" in and this will give a factor < 1 so apparent LOD distances can be
	//  shorted accordingly
	return localFOV / defaultFOV;

}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : &vecTracerSrc - 
//			&tr - 
//			iTracerType - 
//-----------------------------------------------------------------------------
void CBasePlayer::MakeTracer( const Vector &vecTracerSrc, const trace_t &tr, int iTracerType )
{
	if ( GetActiveWeapon() )
	{
		GetActiveWeapon()->MakeTracer( vecTracerSrc, tr, iTracerType );
		return;
	}

	BaseClass::MakeTracer( vecTracerSrc, tr, iTracerType );
}


void CBasePlayer::SharedSpawn()
{
	SetMoveType( MOVETYPE_WALK );
	SetSolid( SOLID_BBOX );
	AddSolidFlags( FSOLID_NOT_STANDABLE );
	SetFriction( 1.0f );

	pl.deadflag	= false;
	m_lifeState	= LIFE_ALIVE;
	m_iHealth = 100;
	m_takedamage		= DAMAGE_YES;

	m_Local.m_bDrawViewmodel = true;
	m_Local.m_flStepSize = sv_stepsize.GetFloat();
	m_Local.m_bAllowAutoMovement = true;

	m_nRenderFX = kRenderFxNone;
	m_flNextAttack	= gpGlobals->curtime;
	m_flMaxspeed		= 0.0f;

	MDLCACHE_CRITICAL_SECTION();
	SetSequence( SelectWeightedSequence( ACT_IDLE ) );

	if ( GetFlags() & FL_DUCKING ) 
		SetCollisionBounds( VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX );
	else
		SetCollisionBounds( VEC_HULL_MIN, VEC_HULL_MAX );

	// dont let uninitialized value here hurt the player
	m_Local.m_flFallVelocity = 0;

	SetBloodColor( BLOOD_COLOR_RED );
	// NVNT inform haptic dll we have just spawned local player
#ifdef CLIENT_DLL
	if(IsLocalPlayer() &&haptics)
		haptics->LocalPlayerReset();
#endif

	m_flDuckAmount = 0;
	m_flDuckSpeed = CS_PLAYER_DUCK_SPEED_IDEAL;
}


//-----------------------------------------------------------------------------
// Purpose: 
// Output : int
//-----------------------------------------------------------------------------
int CBasePlayer::GetDefaultFOV( void ) const
{
#if defined( CLIENT_DLL )
	if ( GetObserverMode() == OBS_MODE_IN_EYE )
	{
		C_BasePlayer *pTargetPlayer = dynamic_cast<C_BasePlayer*>( GetObserverTarget() );

		if ( pTargetPlayer && !pTargetPlayer->IsObserver() )
		{
			return pTargetPlayer->GetDefaultFOV();
		}
	}
#endif

	int iFOV = ( m_iDefaultFOV == 0 ) ? g_pGameRules->DefaultFOV() : m_iDefaultFOV;
	if ( iFOV > MAX_FOV )
		iFOV = MAX_FOV;

	return iFOV;
}

void CBasePlayer::AvoidPhysicsProps( CUserCmd *pCmd )
{
#ifndef _XBOX
	// Don't avoid if noclipping or in movetype none
	switch ( GetMoveType() )
	{
	case MOVETYPE_NOCLIP:
	case MOVETYPE_NONE:
	case MOVETYPE_OBSERVER:
		return;
	default:
		break;
	}

	if ( GetObserverMode() != OBS_MODE_NONE || !IsAlive() )
		return;

	AvoidPushawayProps( this, pCmd );
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : const char
//-----------------------------------------------------------------------------
const char *CBasePlayer::GetTracerType( void )
{
	if ( GetActiveWeapon() )
	{
		return GetActiveWeapon()->GetTracerType();
	}

	return BaseClass::GetTracerType();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CBasePlayer::ClearZoomOwner( void )
{
	m_hZoomOwner = NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Sets the FOV of the client, doing interpolation between old and new if requested
// Input  : FOV - New FOV
//			zoomRate - Amount of time (in seconds) to move between old and new FOV
//-----------------------------------------------------------------------------
bool CBasePlayer::SetFOV( CBaseEntity *pRequester, int FOV, float zoomRate, int iZoomStart /* = 0 */ )
{
	//NOTENOTE: You MUST specify who is requesting the zoom change
	assert( pRequester != NULL );
	if ( pRequester == NULL )
		return false;

	// If we already have an owner, we only allow requests from that owner
	if ( ( m_hZoomOwner.Get() != NULL ) && ( m_hZoomOwner.Get() != pRequester ) )
	{
#ifdef GAME_DLL
		if ( CanOverrideEnvZoomOwner( m_hZoomOwner.Get() ) == false )
#endif
			return false;
	}
	else
	{
		//FIXME: Maybe do this is as an accessor instead
		if ( FOV == 0 )
		{
			m_hZoomOwner = NULL;
		}
		else
		{
			m_hZoomOwner = pRequester;
		}
	}

	// Setup our FOV and our scaling time

	if ( iZoomStart > 0 )
	{
		m_iFOVStart = iZoomStart;
	}
	else
	{
		m_iFOVStart = GetFOV();
	}

	m_flFOVTime = gpGlobals->curtime;
	m_iFOV = FOV;

	m_Local.m_flFOVRate	= zoomRate;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CBasePlayer::UpdateUnderwaterState( void )
{
	if ( GetWaterLevel() == WL_Eyes )
	{
		if ( IsPlayerUnderwater() == false )
		{
			SetPlayerUnderwater( true );
		}
		return;
	}

	if ( IsPlayerUnderwater() )
	{
		SetPlayerUnderwater( false );
	}

	if ( GetWaterLevel() == 0 )
	{
		if ( GetFlags() & FL_INWATER )
		{
#ifndef CLIENT_DLL
			if ( m_iHealth > 0 && IsAlive() )
			{
				EmitSound( "Player.Wade" );
			}
#endif
			RemoveFlag( FL_INWATER );
		}
	}
	else if ( !(GetFlags() & FL_INWATER) )
	{
#ifndef CLIENT_DLL
		// player enter water sound
		if (GetWaterType() == CONTENTS_WATER)
		{
			EmitSound( "Player.Wade" );
		}
#endif

		AddFlag( FL_INWATER );
	}
}

//-----------------------------------------------------------------------------
// Purpose: data accessor
// ensure that for every emitsound there is a matching stopsound
//-----------------------------------------------------------------------------
void CBasePlayer::SetPlayerUnderwater( bool state )
{
	if ( m_bPlayerUnderwater != state )
	{
#if defined( WIN32 ) && !defined( _X360 ) 
		// NVNT turn on haptic drag when underwater
		if(state)
			HapticSetDrag(this,1);
		else
			HapticSetDrag(this,0);
#endif
		m_bPlayerUnderwater = state;

#ifdef CLIENT_DLL
		if ( state )
			EmitSound( "Player.AmbientUnderWater" );
		else
			StopSound( "Player.AmbientUnderWater" );		
#endif
	}
}


void CBasePlayer::SetPreviouslyPredictedOrigin( const Vector &vecAbsOrigin )
{
	m_vecPreviouslyPredictedOrigin = vecAbsOrigin;
}

const Vector &CBasePlayer::GetPreviouslyPredictedOrigin() const
{
	return m_vecPreviouslyPredictedOrigin;
}

bool fogparams_t::operator !=( const fogparams_t& other ) const
{
	if ( this->enable != other.enable ||
		this->blend != other.blend ||
		!VectorsAreEqual(this->dirPrimary, other.dirPrimary, 0.01f ) || 
		this->colorPrimary.Get() != other.colorPrimary.Get() ||
		this->colorSecondary.Get() != other.colorSecondary.Get() ||
		this->start != other.start ||
		this->end != other.end ||
		this->farz != other.farz ||
		this->maxdensity != other.maxdensity ||
		this->colorPrimaryLerpTo.Get() != other.colorPrimaryLerpTo.Get() ||
		this->colorSecondaryLerpTo.Get() != other.colorSecondaryLerpTo.Get() ||
		this->startLerpTo != other.startLerpTo ||
		this->endLerpTo != other.endLerpTo ||
		this->lerptime != other.lerptime ||
		this->duration != other.duration )
		return true;

	return false;
}

