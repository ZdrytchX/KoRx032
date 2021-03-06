/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2000-2006 Tim Angus

This file is part of Tremfusion.

Tremfusion is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Tremfusion is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Tremfusion; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "g_local.h"

/*
===============
G_DamageFeedback

Called just before a snapshot is sent to the given player.
Totals up all damage and generates both the player_state_t
damage values to that client for pain blends and kicks, and
global pain sound events for all clients.
===============
*/
void P_DamageFeedback( gentity_t *player )
{
  gclient_t *client;
  float     count;
  vec3_t    angles;

  client = player->client;
  if( !PM_Live( client->ps.pm_type ) )
    return;

  // total points of damage shot at the player this frame
  count = client->damage_blood + client->damage_armor;
  if( count == 0 )
    return;   // didn't take any damage

  if( count > 255 )
    count = 255;

  // send the information to the client

  // world damage (falling, slime, etc) uses a special code
  // to make the blend blob centered instead of positional
  if( client->damage_fromWorld )
  {
    client->ps.damagePitch = 255;
    client->ps.damageYaw = 255;

    client->damage_fromWorld = qfalse;
  }
  else
  {
    vectoangles( client->damage_from, angles );
    client->ps.damagePitch = angles[ PITCH ] / 360.0 * 256;
    client->ps.damageYaw = angles[ YAW ] / 360.0 * 256;
  }

  // play an apropriate pain sound
  if( ( level.time > player->pain_debounce_time ) && !( player->flags & FL_GODMODE ) )
  {
    player->pain_debounce_time = level.time + 700;
    G_AddEvent( player, EV_PAIN, player->health > 255 ? 255 : player->health );
    client->ps.damageEvent++;
  }


  client->ps.damageCount = count;

  //
  // clear totals
  //
  client->damage_blood = 0;
  client->damage_armor = 0;
  client->damage_knockback = 0;
}



/*
=============
P_WorldEffects

Check for lava / slime contents and drowning
=============
*/
void P_WorldEffects( gentity_t *ent )
{
  int       waterlevel;

  if( ent->client->noclip )
  {
    ent->client->airOutTime = level.time + 12000; // don't need air
    return;
  }

  waterlevel = ent->waterlevel;

  //
  // check for drowning
  //
  if( waterlevel == 3 )
  {
    // if out of air, start drowning
    if( ent->client->airOutTime < level.time)
    {
      // drown!
      ent->client->airOutTime += 1000;
      if( ent->health > 0 )
      {
        // take more damage the longer underwater
        ent->damage += 2;
        if( ent->damage > 15 )
          ent->damage = 15;

        // play a gurp sound instead of a normal pain sound
        if( ent->health <= ent->damage )
          G_Sound( ent, CHAN_VOICE, G_SoundIndex( "*drown.wav" ) );
        else if( rand( ) & 1 )
          G_Sound( ent, CHAN_VOICE, G_SoundIndex( "sound/player/gurp1.wav" ) );
        else
          G_Sound( ent, CHAN_VOICE, G_SoundIndex( "sound/player/gurp2.wav" ) );

        // don't play a normal pain sound
        ent->pain_debounce_time = level.time + 200;

        G_Damage( ent, NULL, NULL, NULL, NULL,
          ent->damage, DAMAGE_NO_ARMOR, MOD_WATER );
      }
    }
  }
  else
  {
    ent->client->airOutTime = level.time + 12000;
    ent->damage = 2;
  }

  //
  // check for sizzle damage (move to pmove?)
  //
  if( waterlevel &&
      ( ent->watertype & ( CONTENTS_LAVA | CONTENTS_SLIME ) ) )
  {
    if( ent->health > 0 &&
        ent->pain_debounce_time <= level.time  )
    {
      if( ent->watertype & CONTENTS_LAVA )
      {
        G_Damage( ent, NULL, NULL, NULL, NULL,
          30 * waterlevel, 0, MOD_LAVA );
      }

      if( ent->watertype & CONTENTS_SLIME )
      {
        G_Damage( ent, NULL, NULL, NULL, NULL,
          10 * waterlevel, 0, MOD_SLIME );
      }
    }
  }
}



/*
===============
G_SetClientSound
===============
*/
void G_SetClientSound( gentity_t *ent )
{
  if( ent->waterlevel && ( ent->watertype & ( CONTENTS_LAVA | CONTENTS_SLIME ) ) )
    ent->client->ps.loopSound = level.snd_fry;
  else
    ent->client->ps.loopSound = 0;
}



//==============================================================

/*
==============
ClientShove
==============
*/
static int GetClientMass( gentity_t *ent )
{
  int entMass = 100;

  if( ent->client->pers.teamSelection == TEAM_ALIENS )
    entMass = BG_Class( ent->client->pers.classSelection )->health;
  else if( ent->client->pers.teamSelection == TEAM_HUMANS )
  {
    if( BG_InventoryContainsUpgrade( UP_BATTLESUIT, ent->client->ps.stats ) )
      entMass *= 2;
  }
  else
    return 0;
  return entMass;
}

static void ClientShove( gentity_t *ent, gentity_t *victim )
{
  // FIXME: do this with class knockbackscale
  vec3_t dir, push;
  float force;
  int entMass, vicMass;

  //If a builder is placing a building, don't shove.
 	if ( ( victim->client->ps.weapon >= WP_ABUILD ) &&
       ( victim->client->ps.weapon <= WP_HBUILD ) &&
 	     ( victim->client->ps.stats[ STAT_BUILDABLE ] != BA_NONE ) )
    return;

  // Don't push if the entity is not trying to move
  if( !ent->client->pers.cmd.rightmove && !ent->client->pers.cmd.forwardmove &&
      !ent->client->pers.cmd.upmove )
    return;

  // Cannot push enemy players unless they are walking on the player
  if( !OnSameTeam( ent, victim ) &&
      victim->client->ps.groundEntityNum != ent - g_entities )
    return;      

  // Shove force is scaled by relative mass
  entMass = GetClientMass( ent );
  vicMass = GetClientMass( victim );
  if( vicMass <= 0 || entMass <= 0 )
    return;
  force = g_shove.value * entMass / vicMass;
  if( force < 0 )
    force = 0;
  if( force > 150 )
    force = 150;

  // Give the victim some shove velocity
  VectorSubtract( victim->r.currentOrigin, ent->r.currentOrigin, dir );
  VectorNormalizeFast( dir );
  VectorScale( dir, force, push );
  VectorAdd( victim->client->ps.velocity, push, victim->client->ps.velocity );

  // Set the pmove timer so that the other client can't cancel
  // out the movement immediately
  if( !victim->client->ps.pm_time )
  {
    int time;

    time = force * 2 + 0.5f;
    if( time < 50 )
      time = 50;
    if( time > 200 )
      time = 200;
    victim->client->ps.pm_time = time;
    victim->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;

    // Knock victim off if they are wallwalking on us
    if( ( victim->client->ps.groundEntityNum == ent - g_entities ) &&
        ( victim->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING ) )
      victim->client->ps.pm_flags |= PMF_TIME_KNOCKOFF;
  }
}

/*
==============
ClientImpacts
==============
*/
void ClientImpacts( gentity_t *ent, pmove_t *pm )
{
  int       i;
  trace_t   trace;
  gentity_t *other;

  // clear a fake trace struct for touch function
  memset( &trace, 0, sizeof( trace ) );

  for( i = 0; i < pm->numtouch; i++ )
  {
    other = &g_entities[ pm->touchents[ i ] ];

    // see G_UnlaggedDetectCollisions(), this is the inverse of that.
    // if our movement is blocked by another player's real position,
    // don't use the unlagged position for them because they are
    // blocking or server-side Pmove() from reaching it
    if( other->client && other->client->unlaggedCalc.used )
      other->client->unlaggedCalc.used = qfalse;

    // tyrant impact attacks
    if( ent->client->ps.weapon == WP_ALEVEL4 || 
        ent->client->ps.weapon == WP_ALEVEL4_UPG )
    {
      G_ChargeAttack( ent, other );
      G_CrushAttack( ent, other );
    }

    // shove players
    if( ent->client && other->client )
      ClientShove( ent, other );

    // touch triggers
    if( other->touch )
      other->touch( other, ent, &trace );
  }
}

/*
============
G_TouchTriggers

Find all trigger entities that ent's current position touches.
Spectators will only interact with teleporters.
============
*/
void  G_TouchTriggers( gentity_t *ent )
{
  int       i, num;
  int       touch[MAX_GENTITIES];
  gentity_t *hit;
  trace_t   trace;
  vec3_t    mins, maxs;
  vec3_t    pmins, pmaxs;
  static    vec3_t range = { 10, 10, 10 };

  if( !ent->client )
    return;

  // dead clients don't activate triggers!
  if( ent->client->ps.stats[ STAT_HEALTH ] <= 0 )
    return;

  BG_ClassBoundingBox( ent->client->ps.stats[ STAT_CLASS ],
                       pmins, pmaxs, NULL, NULL, NULL );

  VectorAdd( ent->client->ps.origin, pmins, mins );
  VectorAdd( ent->client->ps.origin, pmaxs, maxs );

  VectorSubtract( mins, range, mins );
  VectorAdd( maxs, range, maxs );

  num = trap_EntitiesInBox( mins, maxs, touch, MAX_GENTITIES );

  // can't use ent->absmin, because that has a one unit pad
  VectorAdd( ent->client->ps.origin, ent->r.mins, mins );
  VectorAdd( ent->client->ps.origin, ent->r.maxs, maxs );

  for( i = 0; i < num; i++ )
  {
    hit = &g_entities[ touch[ i ] ];

    if( !hit->touch && !ent->touch )
      continue;

    if( !( hit->r.contents & CONTENTS_TRIGGER ) )
      continue;

    // ignore most entities if a spectator
    if( ( ent->client->sess.spectatorState != SPECTATOR_NOT ) ||
        ( ent->client->ps.stats[ STAT_STATE ] & SS_HOVELING ) )
    {
      if( hit->s.eType != ET_TELEPORT_TRIGGER &&
          // this is ugly but adding a new ET_? type will
          // most likely cause network incompatibilities
          hit->touch != Touch_DoorTrigger )
      {
        //check for manually triggered doors
        manualTriggerSpectator( hit, ent );
        continue;
      }
    }

    if( !trap_EntityContact( mins, maxs, hit ) )
      continue;

    memset( &trace, 0, sizeof( trace ) );

    if( hit->touch )
      hit->touch( hit, ent, &trace );
  }
}

/*
=================
SpectatorThink
=================
*/
void SpectatorThink( gentity_t *ent, usercmd_t *ucmd )
{
  pmove_t pm;
  gclient_t *client;
  int clientNum;
  qboolean attack1, attack3, following, queued;

  client = ent->client;

  client->oldbuttons = client->buttons;
  client->buttons = ucmd->buttons;

  attack1 = ( client->buttons & BUTTON_ATTACK ) &&
            !( client->oldbuttons & BUTTON_ATTACK );
  attack3 = ( client->buttons & BUTTON_USE_HOLDABLE ) &&
            !( client->oldbuttons & BUTTON_USE_HOLDABLE );
   
  // We are in following mode only if we are following a non-spectating client
  following = client->sess.spectatorState == SPECTATOR_FOLLOW;
  if( following )
  {
    clientNum = client->sess.spectatorClient;
    if( clientNum < 0 || clientNum > level.maxclients ||
        ( !g_entities[ clientNum ].client && !level.clients[ clientNum ].pers.demoClient ) ||
        level.clients[ clientNum ].sess.spectatorState != SPECTATOR_NOT )
      following = qfalse;
    // to avoid scoreboard issues
    client->ps.persistant[ PERS_KILLED ] = client->pers.savedDeaths;
  }
  
  // Check to see if we are in the spawn queue
  if( client->pers.teamSelection == TEAM_ALIENS )
    queued = G_SearchSpawnQueue( &level.alienSpawnQueue, ent - g_entities );
  else if( client->pers.teamSelection == TEAM_HUMANS )
    queued = G_SearchSpawnQueue( &level.humanSpawnQueue, ent - g_entities );
  else
    queued = qfalse;

  // Wants to get out of spawn queue
  if( attack1 && queued )
  {
    if( client->sess.spectatorState == SPECTATOR_FOLLOW )
      G_StopFollowing( ent );
    if( client->ps.stats[ STAT_TEAM ] == TEAM_ALIENS )
      G_RemoveFromSpawnQueue( &level.alienSpawnQueue, client->ps.clientNum );
    else if( client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS )
      G_RemoveFromSpawnQueue( &level.humanSpawnQueue, client->ps.clientNum );
    client->pers.classSelection = PCL_NONE;
    client->ps.stats[ STAT_CLASS ] = PCL_NONE;
    client->ps.pm_flags &= ~PMF_QUEUED;
    queued = qfalse;
  }

  // Wants to get into spawn queue
  else if( attack1 )
  {
    if( client->sess.spectatorState == SPECTATOR_FOLLOW )
      G_StopFollowing( ent );
    if( client->pers.teamSelection == TEAM_NONE )
      G_TriggerMenu( client->ps.clientNum, MN_TEAM );
    else if( client->pers.teamSelection == TEAM_ALIENS )
      G_TriggerMenu( client->ps.clientNum, MN_A_CLASS );
    else if( client->pers.teamSelection == TEAM_HUMANS )
      G_TriggerMenu( client->ps.clientNum, MN_H_SPAWN );
  }

  // We are either not following anyone or following a spectator
  if( !following )
  {
    if( client->sess.spectatorState == SPECTATOR_FOLLOW )
      client->ps.pm_type = PM_FREEZE;
    else if( client->noclip )
      client->ps.pm_type = PM_NOCLIP;
    else if( client->sess.spectatorState != SPECTATOR_LOCKED )
      client->ps.pm_type = PM_SPECTATOR;
    else if( client->sess.spectatorState == SPECTATOR_LOCKED )
      client->ps.pm_type = PM_HOVELING;
    //Not literally hoveling
    if( queued )
      client->ps.pm_flags |= PMF_QUEUED;

    client->ps.speed = client->pers.flySpeed;

    client->ps.stats[ STAT_STAMINA ] = 0;
    client->ps.stats[ STAT_MISC ] = 0;
    client->ps.stats[ STAT_BUILDABLE ] = 0;
    client->ps.stats[ STAT_CLASS ] = PCL_NONE;
    client->ps.weapon = WP_NONE;

    // Set up for pmove
    memset( &pm, 0, sizeof( pm ) );
    pm.ps = &client->ps;
    pm.cmd = *ucmd;
    pm.tracemask = MASK_DEADSOLID; // spectators can fly through bodies
    pm.trace = trap_Trace;
    pm.pointcontents = trap_PointContents;

    // Perform a pmove
    Pmove( &pm );

    // Save results of pmove
    VectorCopy( client->ps.origin, ent->s.origin );

    G_TouchTriggers( ent );
    trap_UnlinkEntity( ent );

    // Set the queue position and spawn count for the client side
    if( client->ps.pm_flags & PMF_QUEUED )
    {
      if( client->ps.stats[ STAT_TEAM ] == TEAM_ALIENS )
      {
        client->ps.persistant[ PERS_QUEUEPOS ] =
          G_GetPosInSpawnQueue( &level.alienSpawnQueue, client->ps.clientNum );
        client->ps.persistant[ PERS_SPAWNS ] = level.numAlienSpawns;
      }
      else if( client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS )
      {
        client->ps.persistant[ PERS_QUEUEPOS ] =
          G_GetPosInSpawnQueue( &level.humanSpawnQueue, client->ps.clientNum );
        client->ps.persistant[ PERS_SPAWNS ] = level.numHumanSpawns;
      }
    }
  }
}



/*
=================
ClientInactivityTimer

Returns qfalse if the client is dropped
=================
*/
qboolean ClientInactivityTimer( gclient_t *client )
{
  if( !g_inactivity.integer )
  {
    // give everyone some time, so if the operator sets g_inactivity during
    // gameplay, everyone isn't kicked
    client->inactivityTime = level.time + 60 * 1000;
    client->inactivityWarning = qfalse;
  }
  else if( client->pers.cmd.forwardmove ||
           client->pers.cmd.rightmove ||
           client->pers.cmd.upmove ||
           ( client->pers.cmd.buttons & BUTTON_ATTACK )
           || client->pers.floodTime > client->inactivityTime )
  {
    client->inactivityTime = level.time + g_inactivity.integer * 60 * 1000;
    client->inactivityWarning = qfalse;
  }
  else if( !client->pers.localClient )
  {
    if( level.time > client->inactivityTime )
    {
      trap_DropClient( client - level.clients, DROP_INACTIVITY );
      return qfalse;
    }

    if( level.time > client->inactivityTime - 30000
        && level.time < client->inactivityTime - 30500 )
      trap_SendServerCommand( client - level.clients, "cp \"Thirty seconds until inactivity drop!\n\"" );
    
    if( level.time > client->inactivityTime - 10000 && !client->inactivityWarning )
    {
      client->inactivityWarning = qtrue;
      trap_SendServerCommand( client - level.clients, "cp \"Ten seconds until inactivity drop!\n\"" );
    }
  }

  return qtrue;
}

/*
==================
ClientTimerActions

Actions that happen once a second
==================
*/
void ClientTimerActions( gentity_t *ent, int msec )
{
  gclient_t *client;
  usercmd_t *ucmd;
  int       aForward, aRight, aUp;
  qboolean  walking = qfalse, stopped = qfalse,
            crouched = qfalse, jumping = qfalse,
            strafing = qfalse;
  int       i;

  ucmd = &ent->client->pers.cmd;

  aForward  = abs( ucmd->forwardmove );
  aRight    = abs( ucmd->rightmove );
  aUp       = abs( ucmd->upmove );

  if( aForward == 0 && aRight == 0 )
    stopped = qtrue;
  else if( aForward <= 64 && aRight <= 64 )
    walking = qtrue;

  if( aRight > 0 )
    strafing = qtrue;

  if( ucmd->upmove > 0 )
    jumping = qtrue;
  else if( ent->client->ps.pm_flags & PMF_DUCKED )
    crouched = qtrue;

  client = ent->client;
  client->time100 += msec;
  client->time1000 += msec;
  client->time10000 += msec;
  
  // spitpack activation detection
  if( client->ps.weapon == WP_SPITFIRE && ucmd->buttons & BUTTON_DODGE
      && !( client->oldbuttons & BUTTON_DODGE ) )
  {
    if( client->ps.pm_type == PM_SPITPACK )
    {
      client->ps.pm_type = PM_NORMAL;
      client->ps.eFlags &= ~EF_SPITPACK;
    }
    else
    {
      client->ps.pm_type = PM_SPITPACK;
      client->ps.eFlags |= EF_SPITPACK;   
    }
  }
  
  // zoom detection
  if( BG_GetPlayerWeapon( &client->ps ) == WP_MASS_DRIVER 
      || BG_GetPlayerWeapon( &client->ps ) == WP_LAS_GUN )
  {
    if( ( ucmd->buttons & BUTTON_ATTACK2 ) && !( ent->client->ps.eFlags & EF_ZOOM ) )
      client->ps.eFlags |= EF_ZOOM;      
    else if( !( ucmd->buttons & BUTTON_ATTACK2 ) && ( ent->client->ps.eFlags & EF_ZOOM ) )
      client->ps.eFlags &= ~EF_ZOOM;
  }
  if( ( ent->health <= 0 ) || ( client->ps.stats[ STAT_TEAM ] == TEAM_NONE ) )  {
    // ... ?
  }
  else if( level.vesd )
  {
    // damage them
    if( !g_tkmap.integer && (ent->nextRegenTime <= level.time) )
    {     
      G_Damage( ent, NULL, NULL, NULL, NULL, 1, 0, MOD_VESD );
      ent->nextRegenTime
        += ( round( ( (float) VAMPIRIC_ESD_TTD )
        / ( (float) client->ps.stats[ STAT_MAX_HEALTH ]) ) );
    }
    
    // vesd ammo regen (humans)
    if( client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS
        && ent->health > 0 && ent->client->nextAmmoRegen < level.time
        && !BG_Weapon( client->ps.weapon )->infiniteAmmo )
    {
      int offset = 1000;
      int maxammo = BG_Weapon( client->ps.weapon )->maxAmmo * ( BG_Weapon( client->ps.weapon )->maxClips + 1 );
      int curammo = client->ps.ammo * ( client->ps.clips + 1 );
      
      if( BG_Weapon( client->ps.weapon )->usesEnergy && (
          BG_InventoryContainsUpgrade( UP_BATTPACK, client->ps.stats ) ||
          BG_InventoryContainsUpgrade( UP_BATTLESUIT, client->ps.stats ) ) )
        maxammo = (int)( (float)BG_Weapon( client->ps.weapon )->maxAmmo 
                  * BATTPACK_MODIFIER );
      else if( !BG_Weapon( client->ps.weapon )->usesEnergy && (
          BG_InventoryContainsUpgrade( UP_AMMOPACK, client->ps.stats ) ||
          BG_InventoryContainsUpgrade( UP_BATTLESUIT, client->ps.stats ) ) )
        maxammo = (int)( (float)BG_Weapon( client->ps.weapon )->maxAmmo ) 
                  * ( 1 + (float) BG_Weapon( client->ps.weapon )->maxClips 
                      * AMMOPACK_MODIFIER );
      
      if( BG_Weapon( client->ps.weapon )->usesEnergy
          && curammo < maxammo )
      {
        offset = VAMPIRIC_ESD_TAR/maxammo;
        client->ps.ammo++;
      }

      ent->client->nextAmmoRegen = level.time + offset;
    }
  }
  else if( ent->health > client->ps.stats[ STAT_MAX_HEALTH ] )
  {
    // they got health in a Vampire Zone, take it away slowly
    if( ent->nextRegenTime <= level.time )
    {
      int dmg = (ent->health / client->ps.stats[ STAT_MAX_HEALTH ]) + 1;
      G_Damage( ent, NULL, NULL, NULL, NULL, dmg, 0, MOD_VESD );
      ent->nextRegenTime
        += ( round( ( (float) VAMPIRIC_ESD_TTD )
        / ( (float) client->ps.stats[ STAT_MAX_HEALTH ]) ) );
      if( ent->nextRegenTime < level.time )
        ent->nextRegenTime = level.time;
    }
  }
  // smooth alien regeneration
  else if( client->ps.stats[ STAT_TEAM ] == TEAM_ALIENS &&
      level.surrenderTeam != TEAM_ALIENS && 
      ( ent->lastDamageTime + ALIEN_REGEN_DAMAGE_TIME < level.time ) )
  {
    if( client->healRate > 0 &&
        ent->health > 0 &&
        ( ent->health < client->ps.stats[ STAT_MAX_HEALTH ] ) )
    {
      while( ent->nextRegenTime < level.time && !level.vesd )
      {
        ent->health += 1;
        ent->nextRegenTime += client->healRate;
        
        //take away one tk credit
        for( i = 0; i < MAX_CLIENTS; i++ )
        {
          if( ent->client->tkcredits[ i ] > 0 )
            ent->client->tkcredits[ i ]--;
        }
      }
      if( ent->health >= client->ps.stats[ STAT_MAX_HEALTH ] )
      {
        int i;
        
        ent->health = client->ps.stats[ STAT_MAX_HEALTH ];
        for( i = 0; i < MAX_CLIENTS; i++ )
        {
          ent->credits[ i ] = 0;
          //zero all tk accounts
          ent->client->tkcredits[ i ] = 0;
        }
      }
    }
  }
  
  // 'smooth' human regeneration
  else if( client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS &&
    level.surrenderTeam != TEAM_HUMANS && 
    ( ent->lastDamageTime + HUMAN_REGEN_DAMAGE_TIME < level.time ) 
    && ( ent->health < client->ps.stats[ STAT_MAX_HEALTH ] )
    && ent->health > 0 && ( ent->nextRegenTime < level.time ) )
  {
    //calculate regen rate
    int regen = REGEN_HEALTH_RATE;
    if( BG_InventoryContainsUpgrade( UP_REGEN, client->ps.stats ) )
    {
      regen *= 2;
      //Regen some stamina
      if( client->ps.stats[ STAT_STAMINA ] + REGEN_STAMINA_RATE <= MAX_STAMINA )
        client->ps.stats[ STAT_STAMINA ] += REGEN_STAMINA_RATE;
      else
        client->ps.stats[ STAT_STAMINA ] = MAX_STAMINA;
    }
    if( BG_InventoryContainsUpgrade( UP_BATTLESUIT, client->ps.stats ) )
      regen *= 2;
      
    if( ent->nextRegenTime < level.time && regen > 0 && !level.vesd )
    {
      ent->health += 1;
      ent->nextRegenTime = level.time + 1000/regen;
      
      //take away one tk credit
      for( i = 0; i < MAX_CLIENTS; i++ )
      {
        if( ent->client->tkcredits[ i ] > 0 )
          ent->client->tkcredits[ i ]--;
      }
    }
    
    if( ent->health >= client->ps.stats[ STAT_MAX_HEALTH ] )
    {
      int i;
      
      if( !level.vesd )
        ent->health = client->ps.stats[ STAT_MAX_HEALTH ];
      for( i = 0; i < MAX_CLIENTS; i++ )
      {
        ent->credits[ i ] = 0;
        ent->client->tkcredits[ i ] = 0;
      }
    }
  }
  
  if( ent->client->jetpackActive != BG_UpgradeIsActive( UP_JETPACK, ent->client->ps.stats ) )
  {
    ent->client->jetpackActive = BG_UpgradeIsActive( UP_JETPACK, ent->client->ps.stats );
    if( !BG_UpgradeIsActive( UP_JETPACK, ent->client->ps.stats ) )
      ent->client->jetpackStopTime = level.time;
  }
  
  while ( client->time100 >= 100 )
  {
    weapon_t weapon = BG_GetPlayerWeapon( &client->ps );

    client->time100 -= 100;

    // Restore or subtract stamina
    if( stopped || client->ps.pm_type == PM_JETPACK )
      client->ps.stats[ STAT_STAMINA ] += STAMINA_STOP_RESTORE;
    else if( client->ps.stats[ STAT_STATE ] & SS_SPEEDBOOST )
    {
      if( BG_InventoryContainsUpgrade( UP_LIGHTARMOUR, client->ps.stats ) )
      {
        client->ps.stats[ STAT_STAMINA ] -= STAMINA_LARMOUR_SPRINT_TAKE;
      }
      else
      {
        client->ps.stats[ STAT_STAMINA ] -= STAMINA_SPRINT_TAKE;
      }
    }
    else if( walking || crouched )
      client->ps.stats[ STAT_STAMINA ] += STAMINA_WALK_RESTORE;

    // Check stamina limits
    if( client->ps.stats[ STAT_STAMINA ] > MAX_STAMINA )
      client->ps.stats[ STAT_STAMINA ] = MAX_STAMINA;
    else if( client->ps.stats[ STAT_STAMINA ] < -MAX_STAMINA )
      client->ps.stats[ STAT_STAMINA ] = -MAX_STAMINA;

    // Update build timer
    if( weapon == WP_ABUILD || weapon == WP_ABUILD2 || weapon == WP_ABUILD3 ||
        BG_InventoryContainsWeapon( WP_HBUILD, client->ps.stats ) )
    {
        if( client->ps.stats[ STAT_MISC ] > 0 )
          client->ps.stats[ STAT_MISC ] -= 100;
        if( client->ps.stats[ STAT_MISC ] < 0 )
          client->ps.stats[ STAT_MISC ] = 0;
    }
    
    if( BG_InventoryContainsUpgrade( UP_CLOAK, client->ps.stats ) )
    {	
      if( ent->client->cloakReady == qfalse 
          && ( client->ps.stats[ STAT_CLOAK ] <= 0 || ent->health <= 0 ) )
      {
        ent->client->ps.eFlags &= ~EF_MOVER_STOP;
        ent->client->ps.stats[ STAT_CLOAK ] = 0;
      }
    }
  
    if( BG_InventoryContainsUpgrade( UP_CLOAK, client->ps.stats ) )
    {
      if( client->cloakReady == qtrue )
        client->ps.stats[ STAT_CLOAK ] = CLOAK_TIME;
      else if( client->lastcloaktime + 1000 <= level.time
               && client->ps.stats[ STAT_CLOAK ] > 0 )
      {
        client->ps.stats[ STAT_CLOAK ]--;
        client->lastcloaktime = level.time;
      }
    }
    else
      client->ps.stats[ STAT_CLOAK ] = 0;
      
    //client is not moving or is boosted
    if( client->ps.weapon == WP_ALEVEL1_UPG )
    {
      client->ps.eFlags &= ~EF_MOVER_STOP;
      if( aForward <= 5 && aRight <= 5 && aUp <= 5 && !( ucmd->buttons & BUTTON_ATTACK ) && level.overmindPresent )
        client->ps.eFlags |= EF_MOVER_STOP;
      else if(client->ps.stats[ STAT_STATE ] & SS_BOOSTED)
        client->ps.eFlags |= EF_MOVER_STOP;
      else if( !( level.overmindPresent) && ((level.time/1000) % (rand()%100 +2)) != 0  && ( aForward <= 5 && aRight <= 5 && aUp <= 5 && !( ucmd->buttons & BUTTON_ATTACK ) ) )  //Overly complicated way to make the lisk flash 'randomly'
        client->ps.eFlags |= EF_MOVER_STOP;
    }

    switch( weapon )
    {
      case WP_ABUILD:
      case WP_ABUILD2:
      case WP_ABUILD3:
      case WP_HBUILD:
      
        // Set validity bit on buildable
        if( ( client->ps.stats[ STAT_BUILDABLE ] & ~SB_VALID_TOGGLEBIT ) > BA_NONE )
        {
          int     dist = BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->buildDist;
          vec3_t  dummy;

          if( G_CanBuild( ent, client->ps.stats[ STAT_BUILDABLE ] & ~SB_VALID_TOGGLEBIT,
                          dist, dummy ) == IBE_NONE )
            client->ps.stats[ STAT_BUILDABLE ] |= SB_VALID_TOGGLEBIT;
          else
            client->ps.stats[ STAT_BUILDABLE ] &= ~SB_VALID_TOGGLEBIT;

          // Let the client know which buildables will be removed by building
          for( i = 0; i < MAX_MISC; i++ )
          {
            if( i < level.numBuildablesForRemoval )
              client->ps.misc[ i ] = level.markedBuildables[ i ]->s.number;
            else
              client->ps.misc[ i ] = 0;
          }
        }
        else
        {
          for( i = 0; i < MAX_MISC; i++ )
            client->ps.misc[ i ] = 0;
        }
        break;

      default:
        break;
    }

    if( ent->client->pers.teamSelection == TEAM_HUMANS && 
        ( client->ps.stats[ STAT_STATE ] & SS_HEALING_2X ) )
    {
      int remainingStartupTime = MEDKIT_STARTUP_TIME - ( level.time - client->lastMedKitTime );

      // set this every frame to ensure they won't get poisoned while healing
      client->poisonImmunityTime = level.time + MEDKIT_POISON_IMMUNITY_TIME;

      if( remainingStartupTime < 0 )
      {
        if( ent->health < ent->client->ps.stats[ STAT_MAX_HEALTH ] &&
            ent->client->medKitHealthToRestore &&
            ent->client->ps.pm_type != PM_DEAD )
        {
          ent->client->medKitHealthToRestore--;
          ent->health++;
        }
        else
          ent->client->ps.stats[ STAT_STATE ] &= ~SS_HEALING_2X;
      }
      else
      {
        if( ent->health < ent->client->ps.stats[ STAT_MAX_HEALTH ] &&
            ent->client->medKitHealthToRestore &&
            ent->client->ps.pm_type != PM_DEAD )
        {
          //partial increase
          if( level.time > client->medKitIncrementTime )
          {
            ent->client->medKitHealthToRestore--;
            ent->health++;

            client->medKitIncrementTime = level.time +
              ( remainingStartupTime / MEDKIT_STARTUP_SPEED );
          }
        }
        else
          ent->client->ps.stats[ STAT_STATE ] &= ~SS_HEALING_2X;
      }
    }
  }

  while( client->time1000 >= 1000 )
  {
    client->time1000 -= 1000;

    //client is poisoned
    if( client->ps.stats[ STAT_STATE ] & SS_POISONED )
    {
      int damage = ALIEN_POISON_DMG;

      if( client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS )
      {
        if( BG_InventoryContainsUpgrade( UP_BATTLESUIT, client->ps.stats ) )
          damage -= BSUIT_POISON_PROTECTION;
        if( BG_InventoryContainsUpgrade( UP_HELMET, client->ps.stats ) )
          damage -= HELMET_POISON_PROTECTION;
        if( BG_InventoryContainsUpgrade( UP_LIGHTARMOUR, client->ps.stats ) )
          damage -= LIGHTARMOUR_POISON_PROTECTION;
      }

      if( client->lastPoisonClient->s.eType == ET_BUILDABLE )
        G_Damage( ent, client->lastPoisonClient, client->lastPoisonClient, NULL,
          0, damage, 0, MOD_BOOSTER );
      else
        G_Damage( ent, client->lastPoisonClient, client->lastPoisonClient, NULL,
          0, damage, 0, MOD_POISON );
    }

    //recalulate alien heal rate
    if( client->ps.stats[ STAT_TEAM ] == TEAM_ALIENS &&
      level.surrenderTeam != TEAM_ALIENS )
    {
      int       entityList[ MAX_GENTITIES ];
      vec3_t    range = { LEVEL4_REGEN_RANGE, LEVEL4_REGEN_RANGE, LEVEL4_REGEN_RANGE };
      vec3_t    mins, maxs;
      int       i, num;
      gentity_t *boostEntity;
      float     modifier;
      float     advRantModifier = 1.0f;
      float     basiModifier = 1.0f;
      float     boostModifier = 1.0f;
      float     creepModifier = 1.0f;
      qboolean  creep;

      VectorAdd( client->ps.origin, range, maxs );
      VectorSubtract( client->ps.origin, range, mins );

      num = trap_EntitiesInBox( mins, maxs, entityList, MAX_GENTITIES );
      for( i = 0; i < num; i++ )
      {
        boostEntity = &g_entities[ entityList[ i ] ];

        if( boostEntity->s.eType == ET_BUILDABLE &&
            boostEntity->s.modelindex == BA_A_BOOSTER &&
            boostEntity->spawned && boostEntity->health > 0 &&
            G_FindOvermind( boostEntity ) )
          boostModifier = BOOSTER_REGEN_MOD;
        else if( boostEntity->client && boostEntity->health > 0 &&
                 boostEntity->client->pers.teamSelection == TEAM_ALIENS )
        {
          //no self boosting.
          if( boostEntity->client->ps.clientNum == client->ps.clientNum )
            continue;
          if( boostEntity->client->pers.classSelection == PCL_ALIEN_LEVEL1_UPG )
            basiModifier = LEVEL1_REGEN_MOD;
          if( boostEntity->client->pers.classSelection == PCL_ALIEN_LEVEL4_UPG )
            advRantModifier = LEVEL4_UPG_REGEN_MOD;
          else if( boostEntity->client->pers.classSelection == PCL_ALIEN_LEVEL1 )
            basiModifier = LEVEL1_REGEN_MOD;
        }
      }

      creep = G_FindCreep( ent );
      if( creep )
        creepModifier = CREEP_REGEN_MOD;

      modifier = 1.0f*advRantModifier*basiModifier*boostModifier*creepModifier;

      if( modifier > ALIEN_MAX_REGEN_MOD )
        modifier = ALIEN_MAX_REGEN_MOD;

      client->healRate = (int)((float)BG_Class( client->ps.stats[ STAT_CLASS ] )->regenRateMsec/modifier);

      // Transmit heal rate to the client so the HUD can display it properly
      client->ps.stats[ STAT_STATE ] &= ~( SS_HEALING_2X | SS_HEALING_3X );
      if( modifier >= 3.0f )
        client->ps.stats[ STAT_STATE ] |= SS_HEALING_3X;
      else if( modifier >= 2.0f )
        client->ps.stats[ STAT_STATE ] |= SS_HEALING_2X;
      if( creep || modifier != 1.0f )
        client->ps.stats[ STAT_STATE ] |= SS_HEALING_ACTIVE;
      else
        client->ps.stats[ STAT_STATE ] &= ~SS_HEALING_ACTIVE;
    }

    // if alive and we have a jetpack
    if( ent->client->ps.stats[ STAT_HEALTH ] > 0 
        && BG_InventoryContainsUpgrade( UP_JETPACK, ent->client->ps.stats ) )
    { 
      // take away one charge unit per second if the jetpack is active
      if( BG_UpgradeIsActive( UP_JETPACK, ent->client->ps.stats ) 
          && ent->client->ps.stats[ STAT_JET_CHARGE ] > 0 )
        ent->client->ps.stats[ STAT_JET_CHARGE ]--;
      // if the jetpack is not active and at less than full charge, process the recharge code
      else if( !BG_UpgradeIsActive( UP_JETPACK, ent->client->ps.stats ) 
                && ent->client->ps.stats[ STAT_JET_CHARGE ] < JETPACK_CHARGE_CAPACITY )
      {
        // if the level time is beyond: (jetpack's last deactivation time + standard charge delay)
        if( ent->client->jetpackStopTime + JETPACK_STD_CHARGE_DELAY < level.time )
        {
          // if increasing the charge will exceed maximum charge, set the charge to maximum            
          if( ent->client->ps.stats[ STAT_JET_CHARGE ] + JETPACK_STD_CHARGE_RATE > JETPACK_CHARGE_CAPACITY )
            ent->client->ps.stats[ STAT_JET_CHARGE ] = JETPACK_CHARGE_CAPACITY;
          // if not yet at full, give a standard charge
          else
            ent->client->ps.stats[ STAT_JET_CHARGE ] += JETPACK_STD_CHARGE_RATE;
        }
        // if the reactor is up and the level time is beyond: (jetpack's last deactivation time + rc charge delay)
        if( ent->client->jetpackStopTime + JETPACK_RC_CHARGE_DELAY < level.time && level.reactorPresent )
        {
          // if increasing the charge will exceed maximum charge, set the charge to maximum
          if( ent->client->ps.stats[ STAT_JET_CHARGE ] + JETPACK_RC_CHARGE_RATE > JETPACK_CHARGE_CAPACITY )
            ent->client->ps.stats[ STAT_JET_CHARGE ] = JETPACK_CHARGE_CAPACITY;
          // if not yet at full, keep giving a bonus charge
          else
            ent->client->ps.stats[ STAT_JET_CHARGE ] += JETPACK_RC_CHARGE_RATE;
        }
      }
    }

    if( ent->client->ps.stats[ STAT_HEALTH ] > 0 && ent->client->ps.stats[ STAT_TEAM ] == TEAM_ALIENS )
    {
      ent->client->pers.statscounters.timealive++;
      level.alienStatsCounters.timealive++;
      if( G_BuildableRange( ent->client->ps.origin, 900, BA_A_OVERMIND ) )
      {
        ent->client->pers.statscounters.timeinbase++;
        level.alienStatsCounters.timeinbase++;
      }
      if( BG_ClassHasAbility( ent->client->ps.stats[ STAT_CLASS ], SCA_WALLCLIMBER )  )
      {
        ent->client->pers.statscounters.dretchbasytime++;
        level.alienStatsCounters.dretchbasytime++;
        if( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING  || ent->client->ps.eFlags & EF_WALLCLIMBCEILING) 
        {
          ent->client->pers.statscounters.jetpackusewallwalkusetime++;
          level.alienStatsCounters.jetpackusewallwalkusetime++;
        }
      }
    }
    else if( ent->client->ps.stats[ STAT_HEALTH ] > 0 && ent->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS )
    {
      ent->client->pers.statscounters.timealive++;
      level.humanStatsCounters.timealive++;
      if( G_BuildableRange( ent->client->ps.origin, 900, BA_H_REACTOR ) )
      {
        ent->client->pers.statscounters.timeinbase++;
        level.humanStatsCounters.timeinbase++;
      }
      if( BG_InventoryContainsUpgrade( UP_JETPACK, client->ps.stats ) )
      {
        if( client->ps.pm_type == PM_JETPACK ) 
        {
          ent->client->pers.statscounters.jetpackusewallwalkusetime++;
          level.humanStatsCounters.jetpackusewallwalkusetime++;
        }
      }
    }
   
    // turn off life support when a team admits defeat
    if( client->ps.stats[ STAT_TEAM ] == TEAM_ALIENS &&
      level.surrenderTeam == TEAM_ALIENS )
    {
      G_Damage( ent, NULL, NULL, NULL, NULL,
        BG_Class( client->ps.stats[ STAT_CLASS ] )->regenRate,
        DAMAGE_NO_ARMOR, MOD_SUICIDE );
    }
    else if( client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS &&
      level.surrenderTeam == TEAM_HUMANS )
    {
      G_Damage( ent, NULL, NULL, NULL, NULL, 5, DAMAGE_NO_ARMOR, MOD_SUICIDE );
    }

    //calculate resistance to infection ('aids')
    ent->client->pers.aidresistance = 0;
    if( BG_InventoryContainsUpgrade( UP_REGEN, ent->client->ps.stats ) )
      ent->client->pers.aidresistance += BIOKIT_IFEC_PROTECTION;
    if( BG_InventoryContainsUpgrade( UP_LIGHTARMOUR, ent->client->ps.stats ) )
      ent->client->pers.aidresistance += LIGHTARMOUR_INFEC_PROTECTION;
    if( BG_InventoryContainsUpgrade( UP_BATTLESUIT, ent->client->ps.stats ) )
      ent->client->pers.aidresistance += BSUIT_INFEC_PROTECTION;
    if( BG_InventoryContainsUpgrade( UP_HELMET, ent->client->ps.stats ) )
      ent->client->pers.aidresistance += HELMET_INFEC_PROTECTION;

    //infection - stay away from teh infected ones!!! :)
    if( client->infected && !OnSameTeam( client->infector , ent ) )
    {
      //infect the others and suffer a bit
      if( client->infectionTime + 20000 > level.time )
      {
        int i, num, entityList[ MAX_GENTITIES ];
        vec3_t range = { 75, 75, 75 }, mins, maxs;
        gentity_t *target;

        VectorAdd( ent->s.origin, range, maxs );
        VectorSubtract( ent->s.origin, range, mins );
        num = trap_EntitiesInBox( mins, maxs, entityList, MAX_GENTITIES );
        for( i = 0; i < num; i++ )
        {
          srand( level.time );
          target = &g_entities[ entityList[ i ] ];

          // it must a client + not infected + human + visible + not same + lives
          if( target->client && !(target->client->infected) &&
              target->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS &&
              G_Visible( ent, target, CONTENTS_SOLID ) && ent != target 
              && target->health > 0 
              && ( rand( ) % 100 ) > target->client->pers.aidresistance )
          {
            target->client->infected = qtrue;
            target->client->infectionTime = level.time;
            target->client->infector = client->infector;
          }
        }
        G_Damage( ent, client->infector, client->infector, NULL, NULL, 1,
                  DAMAGE_NO_ARMOR, MOD_INFECTION );
      }
      // it is over until the next one
      else
        client->infected = qfalse;
    }
    // lose some voice enthusiasm
    if( client->voiceEnthusiasm > 0.0f )
      client->voiceEnthusiasm -= VOICE_ENTHUSIASM_DECAY;
    else
      client->voiceEnthusiasm = 0.0f;
      
    if( !ent->client->pers.muted && G_admin_permission( ent, ADMF_PERMMUTED ) )
      ent->client->pers.muted = qtrue;
    
    if( !ent->client->pers.denyBuild && G_admin_permission( ent, ADMF_PERMDENYBUILD ) )
      ent->client->pers.denyBuild = qtrue;
      
    if( !ent->client->pers.specd && G_admin_permission( ent, ADMF_PERMFORCESPEC ) )
      ent->client->pers.specd = qtrue;
  }

  // Regenerate Adv. Mara barbs
  if( client->ps.weapon == WP_ALEVEL2_UPG )
  {
    if( client->ps.ammo < BG_Weapon( WP_ALEVEL2_UPG )->maxAmmo )
    {
      if( ent->timestamp + LEVEL2_BOUNCEBALL_REGEN < level.time )
      {
        client->ps.ammo++;
        ent->timestamp = level.time;
      }
    }
    else
      ent->timestamp = level.time;
  }
  else if( client->ps.weapon == WP_SPITFIRE )
  {
    if( client->ps.stats[ STAT_STATE ] & SS_BOOSTED && client->ps.ammo < BG_Weapon( WP_SPITFIRE )->maxAmmo )
    {
      if( ent->timestamp + SPITFIRE_SPITBOMB_REGEN < level.time )
      {
        client->ps.ammo++;
        ent->timestamp = level.time;
      }
    }
    else
      ent->timestamp = level.time;
  }
  else if( client->ps.weapon == WP_ALEVEL3_UPG )
  {
    if( client->ps.ammo < BG_Weapon( WP_ALEVEL3_UPG )->maxAmmo )
    {
      if( ent->timestamp + LEVEL3_BOUNCEBALL_REGEN < level.time )
      {
        client->ps.ammo++;
        ent->timestamp = level.time;
      }
    }
    else
      ent->timestamp = level.time;
  }// Regenerate Adv Tyrant Acid Balls
  else if( client->ps.weapon == WP_ALEVEL4_UPG )
  {
    if( client->ps.stats[ STAT_STATE ] & SS_BOOSTED && client->ps.ammo < BG_Weapon( WP_ALEVEL4_UPG )->maxAmmo )
    {
      if( ent->timestamp + LEVEL4_EBLOB_REGEN < level.time )
      {
        client->ps.ammo++;
        ent->timestamp = level.time;
      }
    }
    else
      ent->timestamp = level.time;
  }
}

/*
====================
ClientIntermissionThink
====================
*/
void ClientIntermissionThink( gclient_t *client )
{
  client->ps.eFlags &= ~EF_FIRING;
  client->ps.eFlags &= ~EF_FIRING2;

  // the level will exit when everyone wants to or after timeouts

  // swap and latch button actions
  client->oldbuttons = client->buttons;
  client->buttons = client->pers.cmd.buttons;
  if( client->buttons & ( BUTTON_ATTACK | BUTTON_USE_HOLDABLE ) & ( client->oldbuttons ^ client->buttons ) )
    client->readyToExit = 1;
}


/*
================
ClientEvents

Events will be passed on to the clients for presentation,
but any server game effects are handled here
================
*/
void ClientEvents( gentity_t *ent, int oldEventSequence )
{
  int       i;
  int       event;
  gclient_t *client;
  int       damage;
  vec3_t    dir;
  vec3_t    point, mins;
  float     fallDistance;
  class_t   class;

  client = ent->client;
  class = client->ps.stats[ STAT_CLASS ];

  if( oldEventSequence < client->ps.eventSequence - MAX_PS_EVENTS )
    oldEventSequence = client->ps.eventSequence - MAX_PS_EVENTS;

  for( i = oldEventSequence; i < client->ps.eventSequence; i++ )
  {
    event = client->ps.events[ i & ( MAX_PS_EVENTS - 1 ) ];

    switch( event )
    {
      case EV_FALL_MEDIUM:
      case EV_FALL_FAR:
        if( ent->s.eType != ET_PLAYER )
          break;    // not in the player model

        fallDistance = ( (float)client->ps.stats[ STAT_FALLDIST ] - MIN_FALL_DISTANCE ) /
                         ( MAX_FALL_DISTANCE - MIN_FALL_DISTANCE );

        if( fallDistance < 0.0f )
          fallDistance = 0.0f;
        else if( fallDistance > 1.0f )
          fallDistance = 1.0f;

        damage = (int)( (float)BG_Class( class )->health *
                 BG_Class( class )->fallDamage * fallDistance );

        VectorSet( dir, 0, 0, 1 );
        BG_ClassBoundingBox( class, mins, NULL, NULL, NULL, NULL );
        mins[ 0 ] = mins[ 1 ] = 0.0f;
        VectorAdd( client->ps.origin, mins, point );

        ent->pain_debounce_time = level.time + 200; // no normal pain sound
        G_Damage( ent, NULL, NULL, dir, point, damage, DAMAGE_NO_LOCDAMAGE, MOD_FALLING );
        break;

      case EV_FIRE_WEAPON:
        FireWeapon( ent );
        break;

      case EV_FIRE_WEAPON2:
        FireWeapon2( ent );
        break;

      case EV_FIRE_WEAPON3:
        FireWeapon3( ent );
        break;

      case EV_NOAMMO:
        break;

      default:
        break;
    }
  }
}


/*
==============
SendPendingPredictableEvents
==============
*/
void SendPendingPredictableEvents( playerState_t *ps )
{
  gentity_t *t;
  int       event, seq;
  int       extEvent, number;

  // if there are still events pending
  if( ps->entityEventSequence < ps->eventSequence )
  {
    // create a temporary entity for this event which is sent to everyone
    // except the client who generated the event
    seq = ps->entityEventSequence & ( MAX_PS_EVENTS - 1 );
    event = ps->events[ seq ] | ( ( ps->entityEventSequence & 3 ) << 8 );
    // set external event to zero before calling BG_PlayerStateToEntityState
    extEvent = ps->externalEvent;
    ps->externalEvent = 0;
    // create temporary entity for event
    t = G_TempEntity( ps->origin, event );
    number = t->s.number;
    BG_PlayerStateToEntityState( ps, &t->s, qtrue );
    t->s.number = number;
    t->s.eType = ET_EVENTS + event;
    t->s.eFlags |= EF_PLAYER_EVENT;
    t->s.otherEntityNum = ps->clientNum;
    // send to everyone except the client who generated the event
    t->r.svFlags |= SVF_NOTSINGLECLIENT;
    t->r.singleClient = ps->clientNum;
    // set back external event
    ps->externalEvent = extEvent;
  }
}

/*
==============
 G_UnlaggedStore

 Called on every server frame.  Stores position data for the client at that
 into client->unlaggedHist[] and the time into level.unlaggedTimes[].
 This data is used by G_UnlaggedCalc()
==============
*/
void G_UnlaggedStore( void )
{
  int i = 0;
  gentity_t *ent;
  unlagged_t *save;

  if( !g_unlagged.integer )
    return;
    
  level.unlaggedIndex++;
  if( level.unlaggedIndex >= MAX_UNLAGGED_MARKERS )
    level.unlaggedIndex = 0;

  level.unlaggedTimes[ level.unlaggedIndex ] = level.time;

  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    save = &ent->client->unlaggedHist[ level.unlaggedIndex ];
    save->used = qfalse;
    if( !ent->r.linked || !( ent->r.contents & CONTENTS_BODY ) )
      continue;
    if( ent->client->pers.connected != CON_CONNECTED )
      continue;
    VectorCopy( ent->r.mins, save->mins );
    VectorCopy( ent->r.maxs, save->maxs );
    VectorCopy( ent->s.pos.trBase, save->origin );
    save->used = qtrue;
  }
}

/*
==============
 G_UnlaggedClear

 Mark all unlaggedHist[] markers for this client invalid.  Useful for
 preventing teleporting and death.
==============
*/
void G_UnlaggedClear( gentity_t *ent )
{
  int i;

  for( i = 0; i < MAX_UNLAGGED_MARKERS; i++ )
    ent->client->unlaggedHist[ i ].used = qfalse;
}

/*
==============
 G_UnlaggedCalc

 Loops through all active clients and calculates their predicted position
 for time then stores it in client->unlaggedCalc
==============
*/
void G_UnlaggedCalc( int time, gentity_t *rewindEnt )
{
  int i = 0;
  gentity_t *ent;
  int startIndex = level.unlaggedIndex;
  int stopIndex = -1;
  int frameMsec = 0;
  float lerp = 0.5f;

  if( !g_unlagged.integer )
    return;

  // clear any calculated values from a previous run
  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    ent->client->unlaggedCalc.used = qfalse;
  }

  for( i = 0; i < MAX_UNLAGGED_MARKERS; i++ )
  {
    if( level.unlaggedTimes[ startIndex ] <= time )
      break;
    stopIndex = startIndex;
    if( --startIndex < 0 )
      startIndex = MAX_UNLAGGED_MARKERS - 1;
  }
  if( i == MAX_UNLAGGED_MARKERS )
  {
    // if we searched all markers and the oldest one still isn't old enough
    // just use the oldest marker with no lerping
    lerp = 0.0f;
  }

  // client is on the current frame, no need for unlagged
  if( stopIndex == -1 )
    return;

  // lerp between two markers
  frameMsec = level.unlaggedTimes[ stopIndex ] -
    level.unlaggedTimes[ startIndex ];
  if( frameMsec > 0 )
  {
    lerp = ( float )( time - level.unlaggedTimes[ startIndex ] ) /
      ( float )frameMsec;
  }

  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    if( ent == rewindEnt )
      continue;
    if( !ent->r.linked || !( ent->r.contents & CONTENTS_BODY ) )
      continue;
    if( ent->client->pers.connected != CON_CONNECTED )
      continue;
    //if( ent->client->ps.stats[ STAT_STATE ] & SS_GRABBED )
    //  continue;
    if( !ent->client->unlaggedHist[ startIndex ].used )
      continue;
    if( !ent->client->unlaggedHist[ stopIndex ].used )
      continue;

    // between two unlagged markers
    VectorLerp( lerp, ent->client->unlaggedHist[ startIndex ].mins,
      ent->client->unlaggedHist[ stopIndex ].mins,
      ent->client->unlaggedCalc.mins );
    VectorLerp( lerp, ent->client->unlaggedHist[ startIndex ].maxs,
      ent->client->unlaggedHist[ stopIndex ].maxs,
      ent->client->unlaggedCalc.maxs );
    VectorLerp( lerp, ent->client->unlaggedHist[ startIndex ].origin,
      ent->client->unlaggedHist[ stopIndex ].origin,
      ent->client->unlaggedCalc.origin );

    ent->client->unlaggedCalc.used = qtrue;
  }
}

/*
==============
 G_UnlaggedOff

 Reverses the changes made to all active clients by G_UnlaggedOn()
==============
*/
void G_UnlaggedOff( void )
{
  int i = 0;
  gentity_t *ent;

  if( !g_unlagged.integer )
    return;

  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    if( !ent->client->unlaggedBackup.used )
      continue;
    VectorCopy( ent->client->unlaggedBackup.mins, ent->r.mins );
    VectorCopy( ent->client->unlaggedBackup.maxs, ent->r.maxs );
    VectorCopy( ent->client->unlaggedBackup.origin, ent->r.currentOrigin );
    ent->client->unlaggedBackup.used = qfalse;
    trap_LinkEntity( ent );
  }
}

/*
==============
 G_UnlaggedOn

 Called after G_UnlaggedCalc() to apply the calculated values to all active
 clients.  Once finished tracing, G_UnlaggedOff() must be called to restore
 the clients' position data

 As an optimization, all clients that have an unlagged position that is
 not touchable at "range" from "muzzle" will be ignored.  This is required
 to prevent a huge amount of trap_LinkEntity() calls per user cmd.
==============
*/

void G_UnlaggedOn( gentity_t *attacker, vec3_t muzzle, float range )
{
  int i = 0;
  gentity_t *ent;
  unlagged_t *calc;
  
  if( !g_unlagged.integer )
    return;

  if( !attacker->client->pers.useUnlagged )
    return;
  
  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    calc = &ent->client->unlaggedCalc;

    if( !calc->used )
      continue;
    if( ent->client->unlaggedBackup.used )
      continue;
    if( !ent->r.linked || !( ent->r.contents & CONTENTS_BODY ) )
      continue;
    if( VectorCompare( ent->r.currentOrigin, calc->origin ) )
      continue;  
    if( muzzle )
    {
      float r1 = Distance( calc->origin, calc->maxs );
      float r2 = Distance( calc->origin, calc->mins );
      float maxRadius = ( r1 > r2 ) ? r1 : r2;

      if( Distance( muzzle, calc->origin ) > range + maxRadius )
        continue;
    }

    // create a backup of the real positions
    VectorCopy( ent->r.mins, ent->client->unlaggedBackup.mins );
    VectorCopy( ent->r.maxs, ent->client->unlaggedBackup.maxs );
    VectorCopy( ent->r.currentOrigin, ent->client->unlaggedBackup.origin );
    ent->client->unlaggedBackup.used = qtrue;

    // move the client to the calculated unlagged position
    VectorCopy( calc->mins, ent->r.mins );
    VectorCopy( calc->maxs, ent->r.maxs );
    VectorCopy( calc->origin, ent->r.currentOrigin );
    trap_LinkEntity( ent );
  }
}
/*
==============
 G_UnlaggedDetectCollisions

 cgame prediction will predict a client's own position all the way up to
 the current time, but only updates other player's positions up to the
 postition sent in the most recent snapshot.

 This allows player X to essentially "move through" the position of player Y
 when player X's cmd is processed with Pmove() on the server.  This is because
 player Y was clipping player X's Pmove() on his client, but when the same
 cmd is processed with Pmove on the server it is not clipped.

 Long story short (too late): don't use unlagged positions for players who
 were blocking this player X's client-side Pmove().  This makes the assumption
 that if player X's movement was blocked in the client he's going to still
 be up against player Y when the Pmove() is run on the server with the
 same cmd.

 NOTE: this must be called after Pmove() and G_UnlaggedCalc()
==============
*/
static void G_UnlaggedDetectCollisions( gentity_t *ent )
{
  unlagged_t *calc;
  trace_t tr;
  float r1, r2;
  float range;

  if( !g_unlagged.integer )
    return;
  if( !ent->client->pers.useUnlagged )
    return;
  
  calc = &ent->client->unlaggedCalc;

  // if the client isn't moving, this is not necessary
  if( VectorCompare( ent->client->oldOrigin, ent->client->ps.origin ) )
    return;

  range = Distance( ent->client->oldOrigin, ent->client->ps.origin );

  // increase the range by the player's largest possible radius since it's
  // the players bounding box that collides, not their origin
  r1 = Distance( calc->origin, calc->mins );
  r2 = Distance( calc->origin, calc->maxs );
  range += ( r1 > r2 ) ? r1 : r2;

  G_UnlaggedOn( ent, ent->client->oldOrigin, range );

  trap_Trace(&tr, ent->client->oldOrigin, ent->r.mins, ent->r.maxs,
    ent->client->ps.origin, ent->s.number,  MASK_PLAYERSOLID );
  if( tr.entityNum >= 0 && tr.entityNum < MAX_CLIENTS )
    g_entities[ tr.entityNum ].client->unlaggedCalc.used = qfalse;

  G_UnlaggedOff( );
}

static void G_CheckZap( gentity_t *ent )
{
  int i;

  if( !ent->zapping )
  {
    // clear out established targets
    for( i = 0; i < LEVEL2_AREAZAP_MAX_TARGETS; i++ )
    {
      ent->zapTargets[ i ] = -1;
    }
    ent->zapDmg = 0.0f;
  }
  ent->wasZapping = ent->zapping;
  ent->zapping = qfalse;

  if( ent->client->ps.weapon == WP_ALEVEL2_UPG && ( ent->client->pers.cmd.buttons & BUTTON_ATTACK2 ) )
    ent->zapping = qtrue;
  else if( ent->client->ps.weapon == WP_SPITFIRE && ( ent->client->pers.cmd.buttons & BUTTON_ATTACK ) )
    ent->zapping = qtrue;

  if( ent->wasZapping && !ent->zapping )
    ent->client->ps.weaponTime = LEVEL2_AREAZAP_REPEAT;
}

/*
==============
ClientThink

This will be called once for each client frame, which will
usually be a couple times for each server frame on fast clients.

If "g_synchronousClients 1" is set, this will be called exactly
once for each server frame, which makes for smooth demo recording.
==============
*/
void ClientThink_real( gentity_t *ent )
{
  gclient_t *client;
  pmove_t   pm;
  int       oldEventSequence;
  int       msec;
  usercmd_t *ucmd;

  client = ent->client;

  // don't think if the client is not yet connected (and thus not yet spawned in)
  if( client->pers.connected != CON_CONNECTED )
    return;

  // mark the time, so the connection sprite can be removed
  ucmd = &ent->client->pers.cmd;

  if( ent->client->pers.paused )
    ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = ucmd->buttons = 0;

  // sanity check the command time to prevent speedup cheating
  if( ucmd->serverTime > level.time + 200 )
  {
    ucmd->serverTime = level.time + 200;
//    G_Printf("serverTime <<<<<\n" );
  }

  if( ucmd->serverTime < level.time - 1000 )
  {
    ucmd->serverTime = level.time - 1000;
//    G_Printf("serverTime >>>>>\n" );
  }

  msec = ucmd->serverTime - client->ps.commandTime;
  // following others may result in bad times, but we still want
  // to check for follow toggles
  if( msec < 1 && client->sess.spectatorState != SPECTATOR_FOLLOW )
    return;

  if( msec > 200 )
    msec = 200;

  client->unlaggedTime = ucmd->serverTime;

  if( pmove_msec.integer < 8 )
    trap_Cvar_Set( "pmove_msec", "8" );
  else if( pmove_msec.integer > 33 )
    trap_Cvar_Set( "pmove_msec", "33" );

  if( pmove_fixed.integer || client->pers.pmoveFixed )
  {
    ucmd->serverTime = ( ( ucmd->serverTime + pmove_msec.integer - 1 ) / pmove_msec.integer ) * pmove_msec.integer;
    //if (ucmd->serverTime - client->ps.commandTime <= 0)
    //  return;
  }

  //
  // check for exiting intermission
  //
  if( level.intermissiontime )
  {
    ClientIntermissionThink( client );
    return;
  }

  G_UpdatePTRConnection( client );

  // spectators don't do much
  if( client->sess.spectatorState != SPECTATOR_NOT )
  {
    if( client->sess.spectatorState == SPECTATOR_SCOREBOARD )
      return;
      
    //To clean up teams during ESD
    if( g_extremeSuddenDeath.integer && g_smartesd.integer && !g_tkmap.integer
        && !level.intermissiontime && ent->lastDamageTime + 3000 <= level.time )
      G_ChangeTeam( ent, TEAM_NONE );

    SpectatorThink( ent, ucmd );
    return;
  }

  //forcespec evasion bug fix
  if( ent->client->pers.specd && ent->client->ps.stats[ STAT_TEAM ] != TEAM_NONE )
  {
    G_ChangeTeam( ent, TEAM_NONE );
    trap_SendServerCommand( ent - g_entities, "print \"^1You're forced to spectators, lets try to keep it that way.^7\"" );
    trap_SendServerCommand( ent - g_entities, "cp \"^1You're forcespeced!^7\"" );
    return;
  }
  
  // check for inactivity timer, but never drop the local client of a non-dedicated server
  // don't do this check if they've got the inactivity flag
  if( !ClientInactivityTimer( client ) 
      && !G_admin_permission( ent, ADMF_ACTIVITY ) )
    return;

  // calculate where ent is currently seeing all the other active clients
  G_UnlaggedCalc( ent->client->unlaggedTime, ent );

  if( client->noclip )
    client->ps.pm_type = PM_NOCLIP;
  else if( client->ps.stats[ STAT_HEALTH ] <= 0 )
    client->ps.pm_type = PM_DEAD;
  else if( client->ps.stats[ STAT_STATE ] & SS_HOVELING )
    client->ps.pm_type = PM_HOVELING;
  else if( client->ps.stats[ STAT_STATE ] & SS_BLOBLOCKED ||
           client->ps.stats[ STAT_STATE ] & SS_GRABBED )
    client->ps.pm_type = PM_GRABBED;
  else if( BG_InventoryContainsUpgrade( UP_JETPACK, client->ps.stats ) && BG_UpgradeIsActive( UP_JETPACK, client->ps.stats ) )
    client->ps.pm_type = PM_JETPACK;
  else if( client->ps.eFlags & EF_SPITPACK )
    client->ps.pm_type = PM_SPITPACK;
  else
    client->ps.pm_type = PM_NORMAL;

  if( ( client->ps.stats[ STAT_STATE ] & SS_GRABBED ) &&
      client->grabExpiryTime < level.time )
    client->ps.stats[ STAT_STATE ] &= ~SS_GRABBED;

  if( ( client->ps.stats[ STAT_STATE ] & SS_BLOBLOCKED ) &&
      client->lastLockTime + LOCKBLOB_LOCKTIME < level.time )
  {
    client->ps.stats[ STAT_STATE ] &= ~SS_BLOBLOCKED;
    client->blobs = 0;
  }
  if( ( client->ps.stats[ STAT_STATE ] & SS_SLOWLOCKED ) &&
      client->lastSlowTime + ABUILDER_BLOB_TIME < level.time )
  {
    client->ps.stats[ STAT_STATE ] &= ~SS_SLOWLOCKED;
    client->blobs = 0;
  }
  // Update boosted state flags
  client->ps.stats[ STAT_STATE ] &= ~SS_BOOSTEDWARNING;
  if( client->ps.stats[ STAT_STATE ] & SS_BOOSTED )
  {
    if( level.time - client->boostedTime >= BOOST_TIME )
      client->ps.stats[ STAT_STATE ] &= ~SS_BOOSTED;
    else if( level.time - client->boostedTime >= BOOST_WARN_TIME )
      client->ps.stats[ STAT_STATE ] |= SS_BOOSTEDWARNING;
  }

  // Check if poison cloud has worn off
  if( ( client->ps.eFlags & EF_POISONCLOUDED ) &&
      BG_PlayerPoisonCloudTime( &client->ps ) - level.time +
      client->lastPoisonCloudedTime <= 0 )
    client->ps.eFlags &= ~EF_POISONCLOUDED;

  if( client->ps.stats[ STAT_STATE ] & SS_POISONED &&
      client->lastPoisonTime + ALIEN_POISON_TIME < level.time )
    client->ps.stats[ STAT_STATE ] &= ~SS_POISONED;
    
  if( !client->pers.jgrab )
    client->ps.gravity = g_gravity.value;
  else
    client->ps.gravity = 0;
    
  if( BG_InventoryContainsUpgrade( UP_MEDKIT, client->ps.stats ) &&
      BG_UpgradeIsActive( UP_MEDKIT, client->ps.stats ) )
  {
    //if currently using a medkit or have no need for a medkit now
    if( client->ps.stats[ STAT_STATE ] & SS_HEALING_2X ||
        ( client->ps.stats[ STAT_HEALTH ] == client->ps.stats[ STAT_MAX_HEALTH ] &&
          !( client->ps.stats[ STAT_STATE ] & SS_POISONED ) ) )
    {
      BG_DeactivateUpgrade( UP_MEDKIT, client->ps.stats );
    }
    else if( client->ps.stats[ STAT_HEALTH ] > 0 )
    {
      //remove anti toxin
      BG_DeactivateUpgrade( UP_MEDKIT, client->ps.stats );
      if ( !client->pers.override )
      {
        BG_RemoveUpgradeFromInventory( UP_MEDKIT, client->ps.stats );
      }

      client->ps.stats[ STAT_STATE ] &= ~SS_POISONED;
      client->poisonImmunityTime = level.time + MEDKIT_POISON_IMMUNITY_TIME;

      client->ps.stats[ STAT_STATE ] |= SS_HEALING_2X;
      client->lastMedKitTime = level.time;
      client->medKitHealthToRestore =
        client->ps.stats[ STAT_MAX_HEALTH ] - client->ps.stats[ STAT_HEALTH ];
      client->medKitIncrementTime = level.time +
        ( MEDKIT_STARTUP_TIME / MEDKIT_STARTUP_SPEED );

      G_AddEvent( ent, EV_MEDKIT_USED, 0 );
    }
  }

  if( BG_InventoryContainsUpgrade( UP_CLOAK, client->ps.stats ) &&
      BG_UpgradeIsActive( UP_CLOAK, client->ps.stats ) )
  {
    if( client->cloakReady )
    {
      BG_DeactivateUpgrade( UP_CLOAK, client->ps.stats );
      client->cloakReady = qfalse;
      client->lastcloaktime = level.time;
      client->ps.eFlags |= EF_MOVER_STOP;
      client->ps.stats[ STAT_CLOAK ] = CLOAK_TIME;
    }
  }

  if( BG_InventoryContainsUpgrade( UP_GRENADE, client->ps.stats ) &&
      BG_UpgradeIsActive( UP_GRENADE, client->ps.stats ) )
  {
    int lastWeapon = ent->s.weapon;

    //remove grenade
    BG_DeactivateUpgrade( UP_GRENADE, client->ps.stats );
    BG_RemoveUpgradeFromInventory( UP_GRENADE, client->ps.stats );

    //M-M-M-M-MONSTER HACK
    ent->s.weapon = WP_GRENADE;
    FireWeapon( ent );
    ent->s.weapon = lastWeapon;
  }

  // set speed
  if( !client->pers.paused && !client->pers.jgrab )
  {
    if( client->ps.pm_type == PM_NOCLIP )
      client->ps.speed = client->pers.flySpeed;
    else
      client->ps.speed = g_speed.value *
        BG_Class( client->ps.stats[ STAT_CLASS ] )->speed;
  }
  else
    client->ps.speed = 0;

  if( client->lastCreepSlowTime + CREEP_TIMEOUT < level.time )
    client->ps.stats[ STAT_STATE ] &= ~SS_CREEPSLOWED;

  //randomly disable the jet pack if damaged
  if( BG_InventoryContainsUpgrade( UP_JETPACK, client->ps.stats ) &&
      BG_UpgradeIsActive( UP_JETPACK, client->ps.stats ) )
  {
    if( ent->lastDamageTime + JETPACK_DISABLE_TIME > level.time )
    {
      if( random( ) > JETPACK_DISABLE_CHANCE && !client->pers.override )
        client->ps.pm_type = PM_NORMAL;
    }

    //switch jetpack off if no reactor or underwater
    if( ( ent->waterlevel >= 3 ) && !client->pers.override )
    {
      BG_DeactivateUpgrade( UP_JETPACK, client->ps.stats );
      ent->client->jetpackStopTime = level.time;
    }
    
    //switch jetpack off if there is no fuel left
    if( client->ps.stats[ STAT_JET_CHARGE ] <= 0 )
    {
      BG_DeactivateUpgrade( UP_JETPACK, client->ps.stats );
      ent->client->jetpackStopTime = level.time;
    }

  }

  // set up for pmove
  oldEventSequence = client->ps.eventSequence;

  memset( &pm, 0, sizeof( pm ) );

  if( ent->flags & FL_FORCE_GESTURE )
  {
    ent->flags &= ~FL_FORCE_GESTURE;
    ent->client->pers.cmd.buttons |= BUTTON_GESTURE;
  }

  // clear fall velocity before every pmove
  client->pmext.fallVelocity = 0.f;

  pm.ps = &client->ps;
  pm.pmext = &client->pmext;
  pm.cmd = *ucmd;

  if( pm.ps->pm_type == PM_DEAD )
    pm.tracemask = MASK_DEADSOLID;

  if( pm.ps->stats[ STAT_STATE ] & SS_HOVELING )
    pm.tracemask = MASK_DEADSOLID;
  else
    pm.tracemask = MASK_PLAYERSOLID;

  pm.trace = trap_Trace;
  pm.pointcontents = trap_PointContents;
  pm.debugLevel = g_debugMove.integer;
  pm.noFootsteps = 0;

  pm.pmove_fixed = pmove_fixed.integer | client->pers.pmoveFixed;
  pm.pmove_msec = pmove_msec.integer;

  VectorCopy( client->ps.origin, client->oldOrigin );

  // moved from after Pmove -- potentially the cause of
  // future triggering bugs
  if( !ent->client->noclip )
    G_TouchTriggers( ent );

  Pmove( &pm );

  G_UnlaggedDetectCollisions( ent );

  // save results of pmove
  if( ent->client->ps.eventSequence != oldEventSequence )
    ent->eventTime = level.time;

  if( g_smoothClients.integer )
    BG_PlayerStateToEntityStateExtraPolate( &ent->client->ps, &ent->s, ent->client->ps.commandTime, qtrue );
  else
    BG_PlayerStateToEntityState( &ent->client->ps, &ent->s, qtrue );

  switch( client->ps.weapon )
  {
    case WP_ALEVEL0:
      if( !CheckVenomAttack( ent ) )
      {
        client->ps.weaponstate = WEAPON_READY;
      }
      else
      {
        client->ps.generic1 = WPM_PRIMARY;
        G_AddEvent( ent, EV_FIRE_WEAPON, 0 );
      }
      break;
    case WP_ALEVEL0_UPG:
      if( !CheckVenomAttack2( ent ) )
      {
        client->ps.weaponstate = WEAPON_READY;
      }
      else
      {
        client->ps.generic1 = WPM_PRIMARY;
        G_AddEvent( ent, EV_FIRE_WEAPON, 0 );
      }
      break;

    case WP_ALEVEL1:
    case WP_ALEVEL1_UPG:
      CheckGrabAttack( ent );
      break;

    case WP_ALEVEL3:
    case WP_ALEVEL3_UPG:
      if( !CheckPounceAttack( ent ) )
      {
        client->ps.weaponstate = WEAPON_READY;
      }
      else
      {
        client->ps.generic1 = WPM_SECONDARY;
        G_AddEvent( ent, EV_FIRE_WEAPON2, 0 );
      }
      break;

    default:
      break;
  }

  SendPendingPredictableEvents( &ent->client->ps );

  if( !( ent->client->ps.eFlags & EF_FIRING ) )
    client->fireHeld = qfalse;    // for grapple
  if( !( ent->client->ps.eFlags & EF_FIRING2 ) )
    client->fire2Held = qfalse;

  // use the snapped origin for linking so it matches client predicted versions
  VectorCopy( ent->s.pos.trBase, ent->r.currentOrigin );

  VectorCopy( pm.mins, ent->r.mins );
  VectorCopy( pm.maxs, ent->r.maxs );

  ent->waterlevel = pm.waterlevel;
  ent->watertype = pm.watertype;

  // touch other objects
  ClientImpacts( ent, &pm );

  G_CheckZap( ent );

  // execute client events
  ClientEvents( ent, oldEventSequence );

  // link entity now, after any personal teleporters have been used
  trap_LinkEntity( ent );

  // NOTE: now copy the exact origin over otherwise clients can be snapped into solid
  VectorCopy( ent->client->ps.origin, ent->r.currentOrigin );
  VectorCopy( ent->client->ps.origin, ent->s.origin );

  // save results of triggers and client events
  if( ent->client->ps.eventSequence != oldEventSequence )
    ent->eventTime = level.time;

  // Don't think anymore if dead
  if( client->ps.stats[ STAT_HEALTH ] <= 0 )
    return;

  // swap and latch button actions
  client->oldbuttons = client->buttons;
  client->buttons = ucmd->buttons;
  client->latched_buttons |= client->buttons & ~client->oldbuttons;

  if( ( client->buttons & BUTTON_USE_EVOLVE ) && !( client->oldbuttons & BUTTON_USE_EVOLVE ) &&
       client->ps.stats[ STAT_HEALTH ] > 0 )
  {
    trace_t   trace;
    vec3_t    view, point;
    gentity_t *traceEnt;

    if( client->ps.stats[ STAT_STATE ] & SS_HOVELING )
    {
      gentity_t *hovel = client->hovel;

      //only let the player out if there is room
      if( !AHovel_Blocked( hovel, ent, qtrue ) )
      {
        //prevent lerping
        client->ps.eFlags ^= EF_TELEPORT_BIT;
        client->ps.eFlags &= ~EF_NODRAW;
        G_UnlaggedClear( ent );

        //client leaves hovel
        client->ps.stats[ STAT_STATE ] &= ~SS_HOVELING;

        //hovel is empty
        G_SetBuildableAnim( hovel, BANIM_ATTACK2, qfalse );
        hovel->active = qfalse;
      }
      else
      {
        //exit is blocked
        G_TriggerMenu( ent->client->ps.clientNum, MN_A_HOVEL_BLOCKED );
      }
    }
    else
    {
#define USE_OBJECT_RANGE 64

      int       entityList[ MAX_GENTITIES ];
      vec3_t    range = { USE_OBJECT_RANGE, USE_OBJECT_RANGE, USE_OBJECT_RANGE };
      vec3_t    mins, maxs;
      int       i, num;

      // look for object infront of player
      AngleVectors( client->ps.viewangles, view, NULL, NULL );
      VectorMA( client->ps.origin, USE_OBJECT_RANGE, view, point );
      trap_Trace( &trace, client->ps.origin, NULL, NULL, point, ent->s.number, MASK_SHOT );

      traceEnt = &g_entities[ trace.entityNum ];

      if( traceEnt && traceEnt->buildableTeam == client->ps.stats[ STAT_TEAM ] && traceEnt->use )
        traceEnt->use( traceEnt, ent, ent ); //other and activator are the same in this context
      else
      {
        //no entity in front of player - do a small area search

        VectorAdd( client->ps.origin, range, maxs );
        VectorSubtract( client->ps.origin, range, mins );

        num = trap_EntitiesInBox( mins, maxs, entityList, MAX_GENTITIES );
        for( i = 0; i < num; i++ )
        {
          traceEnt = &g_entities[ entityList[ i ] ];

          if( traceEnt && traceEnt->buildableTeam == client->ps.stats[ STAT_TEAM ] && traceEnt->use )
          {
            traceEnt->use( traceEnt, ent, ent ); //other and activator are the same in this context
            break;
          }
        }

        if( i == num && client->ps.stats[ STAT_TEAM ] == TEAM_ALIENS )
        {
          if( BG_AlienCanEvolve( client->ps.stats[ STAT_CLASS ],
              client->pers.Credit, g_alienStage.integer ) )
          {
            //no nearby objects and alien - show class menu
            G_TriggerMenu( ent->client->ps.clientNum, MN_A_INFEST );
          }
          else
          {
            //flash frags
            G_AddEvent( ent, EV_ALIEN_EVOLVE_FAILED, 0 );
          }
        }
      }
    }
  }

  // Give clients some credit periodically
  if( ent->client->lastKillTime + FREEKILL_PERIOD < level.time )
  {
    if( level.suddenDeath )
    {
      //gotta love logic like this eh?
    }
    else if( ent->client->ps.stats[ STAT_TEAM ] == TEAM_ALIENS )
      G_AddCreditToClient( ent->client, FREEKILL_ALIEN, qtrue );
    else if( ent->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS )
      G_AddCreditToClient( ent->client, FREEKILL_HUMAN, qtrue );

    ent->client->lastKillTime = level.time;
  }

  // perform once-a-second actions
  ClientTimerActions( ent, msec );

  if( ent->suicideTime > 0 && ent->suicideTime < level.time )
  {
    ent->flags &= ~FL_GODMODE;
    ent->client->ps.stats[ STAT_HEALTH ] = ent->health = 0;
    player_die( ent, ent, ent, 100000, MOD_SUICIDE );

    ent->suicideTime = 0;
  }
  
  if( ( client->pers.jgrab && !(client->ps.stats[ STAT_STATE ] & SS_GRABBED ) ) )
    client->pers.jgrab = qfalse;
  
  if( !BG_UpgradeIsActive( UP_JETPACK, ent->client->ps.stats ) && client->pers.jgrab)
    client->pers.jgrab = qfalse;
}

/*
==================
ClientThink

A new command has arrived from the client
==================
*/
void ClientThink( int clientNum )
{
  gentity_t *ent;

  ent = g_entities + clientNum;
  trap_GetUsercmd( clientNum, &ent->client->pers.cmd );

  // mark the time we got info, so we can display the
  // phone jack if they don't get any for a while
  ent->client->lastCmdTime = level.time;

  if( !g_synchronousClients.integer )
    ClientThink_real( ent );
}


void G_RunClient( gentity_t *ent )
{
  if( !g_synchronousClients.integer )
    return;

  ent->client->pers.cmd.serverTime = level.time;
  ClientThink_real( ent );
}


/*
==================
SpectatorClientEndFrame

==================
*/
void SpectatorClientEndFrame( gentity_t *ent )
{
  gclient_t *cl;
  int       clientNum, flags;
  int       score, ping;

  // if we are doing a chase cam or a remote view, grab the latest info
  if( ent->client->sess.spectatorState == SPECTATOR_FOLLOW )
  {
    clientNum = ent->client->sess.spectatorClient;
    if( clientNum >= 0 )
    {
      cl = &level.clients[ clientNum ];
      if( cl->pers.connected == CON_CONNECTED || cl->pers.demoClient )
      {
        flags = ( cl->ps.eFlags & ~( EF_VOTED | EF_TEAMVOTED ) ) |
                ( ent->client->ps.eFlags & ( EF_VOTED | EF_TEAMVOTED ) );
        score = ent->client->ps.persistant[ PERS_KILLS ];
        ping = ent->client->ps.ping;
        ent->client->ps = cl->ps;
        ent->client->ps.persistant[ PERS_KILLS ] = score;
        ent->client->ps.ping = ping;
        ent->client->ps.pm_flags |= PMF_FOLLOW;
        ent->client->ps.pm_flags &= ~PMF_QUEUED;
        ent->client->ps.eFlags = flags;
      }
    }
  }
}

/*
==============
ClientEndFrame

Called at the end of each server frame for each connected client
A fast client will have multiple ClientThink for each ClientEdFrame,
while a slow client may have multiple ClientEndFrame between ClientThink.
==============
*/
void ClientEndFrame( gentity_t *ent )
{
  clientPersistant_t  *pers;

  if( ent->client->sess.spectatorState != SPECTATOR_NOT )
  {
    SpectatorClientEndFrame( ent );
    return;
  }

  pers = &ent->client->pers;

  //
  // If the end of unit layout is displayed, don't give
  // the player any normal movement attributes
  //
  if( level.intermissiontime )
    return;

  // burn from lava, etc
  P_WorldEffects( ent );

  // apply all the damage taken this frame
  P_DamageFeedback( ent );

  // add the EF_CONNECTION flag if we haven't gotten commands recently
  if( level.time - ent->client->lastCmdTime > 1000 )
    ent->s.eFlags |= EF_CONNECTION;
  else
    ent->s.eFlags &= ~EF_CONNECTION;

  ent->client->ps.stats[ STAT_HEALTH ] = ent->health; // FIXME: get rid of ent->health...

  // respawn if dead
  if( ent->client->ps.stats[ STAT_HEALTH ] <= 0 && level.time >= ent->client->respawnTime )
    respawn( ent );

  G_SetClientSound( ent );

  G_UpdateZaps( ent );
  
  // update the credits ...
  ent->client->ps.persistant[ PERS_CREDIT ] = pers->Credit;
  
  // set the latest infor
  if( g_smoothClients.integer )
    BG_PlayerStateToEntityStateExtraPolate( &ent->client->ps, &ent->s, ent->client->ps.commandTime, qtrue );
  else
    BG_PlayerStateToEntityState( &ent->client->ps, &ent->s, qtrue );

  SendPendingPredictableEvents( &ent->client->ps );
}


