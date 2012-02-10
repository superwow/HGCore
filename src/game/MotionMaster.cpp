/*
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * Copyright (C) 2008-2009 Trinity <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "MotionMaster.h"
#include "CreatureAISelector.h"
#include "Creature.h"

#include "ConfusedMovementGenerator.h"
#include "FleeingMovementGenerator.h"
#include "HomeMovementGenerator.h"
#include "IdleMovementGenerator.h"
#include "PointMovementGenerator.h"
#include "TargetedMovementGenerator.h"
#include "WaypointMovementGenerator.h"
#include "RandomMovementGenerator.h"

#include "movement/MoveSpline.h"
#include "movement/MoveSplineInit.h"

#include <cassert>

inline bool isStatic(MovementGenerator *mv)
{
    return (mv == &si_idleMovement);
}

void MotionMaster::Initialize()
{
    // stop current move
    if (!m_owner->IsStopped())
        m_owner->StopMoving();

    // clear ALL movement generators (including default)
    Clear(false,true);

    // set new default movement generator
    if (m_owner->GetTypeId() == TYPEID_UNIT && !m_owner->hasUnitState(UNIT_STAT_POSSESSED))
    {
        MovementGenerator* movement = FactorySelector::selectMovementGenerator((Creature*)m_owner);
        push(movement == NULL ? &si_idleMovement : movement);
        top()->Initialize(*m_owner);
    }
    else
        push(&si_idleMovement);
}

MotionMaster::~MotionMaster()
{
    // just deallocate movement generator, but do not Finalize since it may access to already deallocated owner's memory
    while(!empty())
    {
        MovementGenerator * m = top();
        pop();
        if (!isStatic(m))
            delete m;
    }
}

void MotionMaster::UpdateMotion(uint32 diff)
{
    if (m_owner->hasUnitState(UNIT_STAT_CAN_NOT_MOVE))
        return;

    ASSERT( !empty() );
    m_cleanFlag |= MMCF_UPDATE;

    if (!top()->Update(*m_owner, diff))
    {
        m_cleanFlag &= ~MMCF_UPDATE;
        MovementExpired();
    }
    else
        m_cleanFlag &= ~MMCF_UPDATE;

    if (m_expList)
    {
        for (size_t i = 0; i < m_expList->size(); ++i)
        {
            MovementGenerator* mg = (*m_expList)[i];
            if (!isStatic(mg))
                delete mg;
        }

        delete m_expList;
        m_expList = NULL;

        if (empty())
            Initialize();

        if (m_cleanFlag & MMCF_RESET)
        {
            top()->Reset(*m_owner);
            m_cleanFlag &= ~MMCF_RESET;
        }
    }
}

void MotionMaster::DirectClean(bool reset, bool all)
{
    while (all ? !empty() : size() > 1)
    {
        MovementGenerator *curr = top();
        pop();
        curr->Finalize(*m_owner);

        if (!isStatic(curr))
            delete curr;
    }

    if (!all && reset)
    {
        ASSERT(!empty());
        top()->Reset(*m_owner);
    }
}

void MotionMaster::DelayedClean(bool reset, bool all)
{
    if (reset)
        m_cleanFlag |= MMCF_RESET;
    else
        m_cleanFlag &= ~MMCF_RESET;

    if (empty() || (!all && size() == 1))
        return;

    if (!m_expList)
        m_expList = new ExpireList();

    while (all ? !empty() : size() > 1)
    {
        MovementGenerator *curr = top();
        pop();
        curr->Finalize(*m_owner);

        if (!isStatic(curr))
            m_expList->push_back(curr);
    }
}

void MotionMaster::DirectExpire(bool reset)
{
    if (empty() || size() == 1)
        return;

    MovementGenerator *curr = top();
    pop();

    // also drop stored under top() targeted motions
    while (!empty() && (top()->GetMovementGeneratorType() == CHASE_MOTION_TYPE || top()->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE))
    {
        MovementGenerator *temp = top();
        pop();
        temp->Finalize(*m_owner);
        delete temp;
    }

    // Store current top MMGen, as Finalize might push a new MMGen
    MovementGenerator* nowTop = empty() ? NULL : top();
    // it can add another motions instead
    curr->Finalize(*m_owner);

    if (!isStatic(curr))
        delete curr;

    if (empty())
        Initialize();

    // Prevent reseting possible new pushed MMGen
    if (reset && top() == nowTop)
        top()->Reset(*m_owner);
}

void MotionMaster::DelayedExpire(bool reset)
{
    if (reset)
        m_cleanFlag |= MMCF_RESET;
    else
        m_cleanFlag &= ~MMCF_RESET;

    if (empty() || size() == 1)
        return;

    MovementGenerator *curr = top();
    pop();

    if (!m_expList)
        m_expList = new ExpireList();

    // also drop stored under top() targeted motions
    while (!empty() && (top()->GetMovementGeneratorType() == CHASE_MOTION_TYPE || top()->GetMovementGeneratorType() == FOLLOW_MOTION_TYPE))
    {
        MovementGenerator *temp = top();
        pop();
        temp ->Finalize(*m_owner);
        m_expList->push_back(temp );
    }

    curr->Finalize(*m_owner);

    if (!isStatic(curr))
        m_expList->push_back(curr);
}

void MotionMaster::MoveIdle()
{
    if (empty() || !isStatic(top()))
        push(&si_idleMovement);
}

void MotionMaster::MoveRandom(float spawndist)
{
    if (m_owner->GetTypeId() == TYPEID_UNIT)
        Mutate(new RandomMovementGenerator<Creature>(spawndist));
}

void MotionMaster::MoveTargetedHome()
{
    if (m_owner->hasUnitState(UNIT_STAT_LOST_CONTROL))
        return;

    Clear(false);

    if (m_owner->GetTypeId()==TYPEID_UNIT && !((Creature*)m_owner)->GetCharmerOrOwnerGUID())
    {
        Mutate(new HomeMovementGenerator<Creature>());
    }
    else if(m_owner->GetTypeId()==TYPEID_UNIT && ((Creature*)m_owner)->GetCharmerOrOwnerGUID())
    {
        Unit *target = ((Creature*)m_owner)->GetCharmerOrOwner();
        if(target)
        {
            Mutate(new FollowMovementGenerator<Creature>(*target,PET_FOLLOW_DIST,PET_FOLLOW_ANGLE));
        }
    }
    else
    {
        sLog.outError("Player (GUID: %u) attempt targeted home", m_owner->GetGUIDLow());
    }
}

void MotionMaster::MoveConfused()
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        Mutate(new ConfusedMovementGenerator<Player>());
    else
        Mutate(new ConfusedMovementGenerator<Creature>());
}

void MotionMaster::MoveChase(Unit* target, float dist, float angle)
{
    // ignore movement request if target not exist
    if (!target)
        return;

    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        Mutate(new ChaseMovementGenerator<Player>(*target,dist,angle));
    else
        Mutate(new ChaseMovementGenerator<Creature>(*target,dist,angle));
}

void MotionMaster::MoveFollow(Unit* target, float dist, float angle)
{
    if (m_owner->hasUnitState(UNIT_STAT_LOST_CONTROL))
        return;

    Clear();

    // ignore movement request if target not exist
    if (!target)
        return;

    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        Mutate(new FollowMovementGenerator<Player>(*target,dist,angle));
    else
        Mutate(new FollowMovementGenerator<Creature>(*target,dist,angle));
}

void MotionMaster::MovePoint(uint32 id, float x, float y, float z)
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        Mutate(new PointMovementGenerator<Player>(id,x,y,z));
    else
        Mutate(new PointMovementGenerator<Creature>(id,x,y,z));
}

void MotionMaster::MoveCharge(float x, float y, float z, float speed, uint32 id)
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        Mutate(new PointMovementGenerator<Player>(id,x,y,z,speed));
    else
        Mutate(new PointMovementGenerator<Creature>(id,x,y,z,speed));
}

void MotionMaster::MoveSeekAssistance(float x, float y, float z)
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        sLog.outError("Player (GUID: %u) attempt to seek assistance", m_owner->GetGUIDLow());
    else
    {
        m_owner->AttackStop();
        m_owner->ToCreature()->SetReactState(REACT_PASSIVE);

        Mutate(new AssistanceMovementGenerator(x,y,z));
    }
}

void MotionMaster::MoveSeekAssistanceDistract(uint32 time)
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        sLog.outError("Player (GUID: %u) attempt to call distract after assistance", m_owner->GetGUIDLow());
    else
        Mutate(new AssistanceDistractMovementGenerator(time));
}

void MotionMaster::MoveFleeing(Unit* enemy, uint32 time)
{
    if (!enemy)
        return;

    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        Mutate(new FleeingMovementGenerator<Player>(enemy->GetObjectGuid()));
    else
    {
        if (time)
            Mutate(new TimedFleeingMovementGenerator(enemy->GetGUID(), time));
        else
            Mutate(new FleeingMovementGenerator<Creature>(enemy->GetGUID()));
    }
}

void MotionMaster::MoveTaxiFlight(uint32 path, uint32 pathnode)
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
        Mutate(new FlightPathMovementGenerator(sTaxiPathNodesByPath[path],pathnode));
    else
        sLog.outError("Creature (Entry: %u GUID: %u) attempt taxi to (Path %u node %u)", m_owner->GetEntry(), m_owner->GetGUIDLow(), path, pathnode);
}

void MotionMaster::MoveDistract(uint32 timer)
{
    Mutate(new DistractMovementGenerator(timer));
}

void MotionMaster::MoveRotate(uint32 time, RotateDirection direction)
{
    if (!time)
        return;

    Mutate(new RotateMovementGenerator(time, direction));
}

void MotionMaster::MoveFall(uint32 id)
{
    // use larger distance for vmap height search than in most other cases
    float tz = m_owner->GetTerrain()->GetHeight(m_owner->GetPositionX(), m_owner->GetPositionY(), m_owner->GetPositionZ(), true, MAX_FALL_DISTANCE);
    if (tz <= INVALID_HEIGHT)
    {
        DEBUG_LOG("MotionMaster::MoveFall: unable retrive a proper height at map %u (x: %f, y: %f, z: %f).",
            m_owner->GetMap()->GetId(), m_owner->GetPositionX(), m_owner->GetPositionX(), m_owner->GetPositionZ());
        return;
    }

    // Abort too if the ground is very near
    if (fabs(m_owner->GetPositionZ() - tz) < 0.1f)
        return;

    Movement::MoveSplineInit init(*m_owner);
    init.MoveTo(m_owner->GetPositionX(),m_owner->GetPositionY(),tz);
    init.SetFall();
    init.Launch();
    Mutate(new EffectMovementGenerator(id));
}

void MotionMaster::Mutate(MovementGenerator *m)
{
    if (!empty())
    {
        switch (top()->GetMovementGeneratorType())
        {
            // HomeMovement is not that important, delete it if meanwhile a new comes
            case HOME_MOTION_TYPE:
            // DistractMovement interrupted by any other movement
            case EFFECT_MOTION_TYPE:
            case DISTRACT_MOTION_TYPE:
                MovementExpired(false);
            default:
                break;
        }

        if (!empty())
            top()->Interrupt(*m_owner);
    }

    m->Initialize(*m_owner);
    push(m);
}

void MotionMaster::MovePath(uint32 path_id, bool repeatable)
{
    if (!path_id)
        return;

    Mutate(new WaypointMovementGenerator<Creature>(path_id, repeatable));
}

void MotionMaster::propagateSpeedChange()
{
    Impl::container_type::iterator it = Impl::c.begin();
    for ( ;it != end(); ++it)
    {
        (*it)->unitSpeedChanged();
    }
}

MovementGeneratorType MotionMaster::GetCurrentMovementGeneratorType() const
{
    if (empty())
        return IDLE_MOTION_TYPE;

    return top()->GetMovementGeneratorType();
}

bool MotionMaster::GetDestination(float &x, float &y, float &z)
{
    if (m_owner->movespline->Finalized())
        return false;

    const G3D::Vector3& dest = m_owner->movespline->FinalDestination();
    x = dest.x;
    y = dest.y;
    z = dest.z;
    return true;
}

void MotionMaster::UpdateFinalDistanceToTarget(float fDistance)
{
    if (!empty())
        top()->UpdateFinalDistance(fDistance);
}
