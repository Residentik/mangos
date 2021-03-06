/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Object.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "World.h"
#include "Creature.h"
#include "Player.h"
#include "Vehicle.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "UpdateData.h"
#include "UpdateMask.h"
#include "Util.h"
#include "MapManager.h"
#include "Log.h"
#include "Transports.h"
#include "TargetedMovementGenerator.h"
#include "WaypointMovementGenerator.h"
#include "VMapFactory.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectPosSelector.h"
#include "TemporarySummon.h"
#include "OutdoorPvP/OutdoorPvPMgr.h"
#include "movement/packet_builder.h"
#include "movement/MoveSplineInit.h"
#include "movement/MoveSpline.h"
#include "UpdateFieldFlags.h"
#include "Group.h"
#include "CreatureLinkingMgr.h"

#define TERRAIN_LOS_STEP_DISTANCE   3.0f        // sample distance for terrain LoS

UpdateFieldData::UpdateFieldData(Object const* object, Player* target)
{
    m_isSelf = object == target;
    m_isItemOwner = false;
    m_hasSpecialInfo = false;
    m_isPartyMember = false;

    switch(object->GetTypeId())
    {
        case TYPEID_ITEM:
        case TYPEID_CONTAINER:
            m_flags = ItemUpdateFieldFlags;
            m_isOwner = m_isItemOwner = ((Item*)object)->GetOwnerGuid() == target->GetObjectGuid();
            break;
        case TYPEID_UNIT:
        case TYPEID_PLAYER:
        {
            m_flags = UnitUpdateFieldFlags;
            m_isOwner = ((Unit*)object)->GetOwnerGuid() == target->GetObjectGuid();
            m_hasSpecialInfo = ((Unit*)object)->HasAuraTypeWithCaster(SPELL_AURA_EMPATHY, target->GetObjectGuid());
            if (Player* pPlayer = ((Unit*)object)->GetCharmerOrOwnerPlayerOrPlayerItself())
                m_isPartyMember = pPlayer->IsInSameGroupWith(target);
            break;
        }
        case TYPEID_GAMEOBJECT:
            m_flags = GameObjectUpdateFieldFlags;
            m_isOwner = ((GameObject*)object)->GetOwnerGuid() == target->GetObjectGuid();
            break;
        case TYPEID_DYNAMICOBJECT:
            m_flags = DynamicObjectUpdateFieldFlags;
            m_isOwner = ((DynamicObject*)object)->GetCasterGuid() == target->GetObjectGuid();
            break;
        case TYPEID_CORPSE:
            m_flags = CorpseUpdateFieldFlags;
            m_isOwner = ((Corpse*)object)->GetOwnerGuid() == target->GetObjectGuid();
            break;
    }
}

bool UpdateFieldData::IsUpdateFieldVisible(uint16 fieldIndex) const
{
    if (m_flags[fieldIndex] == UF_FLAG_NONE)
        return false;

    if (HasFlags(fieldIndex, UF_FLAG_PUBLIC) ||
        (HasFlags(fieldIndex, UF_FLAG_PRIVATE) && m_isSelf) ||
        (HasFlags(fieldIndex, UF_FLAG_OWNER) && m_isOwner) ||
        (HasFlags(fieldIndex, UF_FLAG_ITEM_OWNER) && m_isItemOwner) ||
        (HasFlags(fieldIndex, UF_FLAG_PARTY_MEMBER) && m_isPartyMember))
        return true;

    return false;
}

Object::Object()
{
    m_objectTypeId        = TYPEID_OBJECT;
    m_objectType          = TYPEMASK_OBJECT;

    m_uint32Values        = 0;
    m_valuesCount         = 0;
    m_fieldNotifyFlags    = UF_FLAG_DYNAMIC;

    m_inWorld             = false;
    m_objectUpdated       = false;
}

Object::~Object()
{
    if (IsInWorld())
    {
        ///- Do NOT call RemoveFromWorld here, if the object is a player it will crash
        sLog.outError("Object::~Object (%s type %u) deleted but still in world!!", GetObjectGuid() ? GetObjectGuid().GetString().c_str() : "<none>", GetTypeId());
        MANGOS_ASSERT(false);
    }

    if (m_objectUpdated)
    {
        sLog.outError("Object::~Object ((%s type %u) deleted but still have updated status!!", GetObjectGuid() ? GetObjectGuid().GetString().c_str() : "<none>", GetTypeId());
        MANGOS_ASSERT(false);
    }

    delete[] m_uint32Values;
}

void Object::_InitValues()
{
    m_uint32Values = new uint32[ m_valuesCount ];
    memset(m_uint32Values, 0, m_valuesCount*sizeof(uint32));

    m_changedValues.resize(m_valuesCount, false);

    m_objectUpdated = false;
}

void Object::_Create(ObjectGuid guid)
{
    if(!m_uint32Values)
        _InitValues();

    SetGuidValue(OBJECT_FIELD_GUID, guid);
    SetUInt32Value(OBJECT_FIELD_TYPE, m_objectType);
    m_PackGUID.Set(guid);
}

void Object::SetObjectScale(float newScale)
{
    SetFloatValue(OBJECT_FIELD_SCALE_X, newScale);
}

void Object::SendForcedObjectUpdate()
{
    if (!m_inWorld || !m_objectUpdated)
        return;

    UpdateDataMapType update_players;

    BuildUpdateData(update_players);
//    RemoveFromClientUpdateList();

    WorldPacket packet;                                     // here we allocate a std::vector with a size of 0x10000
    for(UpdateDataMapType::iterator iter = update_players.begin(); iter != update_players.end(); ++iter)
    {
        if (!iter->first || !iter->first.IsPlayer())
            continue;

        Player* pPlayer = ObjectAccessor::FindPlayer(iter->first);
        if (!pPlayer)
            continue;

        iter->second.BuildPacket(&packet);
        pPlayer->GetSession()->SendPacket(&packet);
        packet.clear();                                     // clean the string
    }
}

void Object::BuildMovementUpdateBlock(UpdateData * data, uint16 flags ) const
{
    ByteBuffer buf(500);

    buf << uint8(UPDATETYPE_MOVEMENT);
    buf << GetPackGUID();

    BuildMovementUpdate(&buf, flags);

    data->AddUpdateBlock(buf);
}

void Object::BuildCreateUpdateBlockForPlayer(UpdateData *data, Player *target) const
{
    if(!target)
        return;

    uint8  updatetype   = UPDATETYPE_CREATE_OBJECT;
    uint16 updateFlags  = m_updateFlag;

    /** lower flag1 **/
    if (target == this)                                      // building packet for yourself
        updateFlags |= UPDATEFLAG_SELF;

    if (updateFlags & UPDATEFLAG_HAS_POSITION)
    {
        // UPDATETYPE_CREATE_OBJECT2 dynamic objects, corpses...
        if (isType(TYPEMASK_DYNAMICOBJECT) || isType(TYPEMASK_CORPSE) || isType(TYPEMASK_PLAYER))
            updatetype = UPDATETYPE_CREATE_OBJECT2;

        // UPDATETYPE_CREATE_OBJECT2 for pets...
        if (target->GetPetGuid() == GetObjectGuid())
            updatetype = UPDATETYPE_CREATE_OBJECT2;

        // UPDATETYPE_CREATE_OBJECT2 for some gameobject types...
        if (isType(TYPEMASK_GAMEOBJECT))
        {
            switch(((GameObject*)this)->GetGoType())
            {
                case GAMEOBJECT_TYPE_TRAP:
                case GAMEOBJECT_TYPE_DUEL_ARBITER:
                case GAMEOBJECT_TYPE_FLAGSTAND:
                case GAMEOBJECT_TYPE_FLAGDROP:
                    updatetype = UPDATETYPE_CREATE_OBJECT2;
                    break;
                case GAMEOBJECT_TYPE_TRANSPORT:
                    updateFlags |= UPDATEFLAG_TRANSPORT;
                    break;
                default:
                    break;
            }
        }

        if (isType(TYPEMASK_UNIT))
        {
            if(((Unit*)this)->getVictim())
                updateFlags |= UPDATEFLAG_HAS_ATTACKING_TARGET;
        }
    }

    //DEBUG_LOG("BuildCreateUpdate: update-type: %u, object-type: %u got updateFlags: %X", updatetype, m_objectTypeId, updateFlags);

    ByteBuffer buf;
    buf << uint8(updatetype);
    buf << GetPackGUID();
    buf << uint8(m_objectTypeId);

    BuildMovementUpdate(&buf, updateFlags);

    UpdateMask updateMask;
    updateMask.SetCount(m_valuesCount);
    _SetCreateBits(&updateMask, target);
    BuildValuesUpdate(updatetype, &buf, &updateMask, target);
    data->AddUpdateBlock(buf);
}

void Object::SendCreateUpdateToPlayer(Player* player)
{
    // send create update to player
    UpdateData upd;
    WorldPacket packet;

    BuildCreateUpdateBlockForPlayer(&upd, player);
    upd.BuildPacket(&packet);
    player->GetSession()->SendPacket(&packet);
}

void Object::BuildValuesUpdateBlockForPlayer(UpdateData *data, Player *target) const
{
    ByteBuffer buf(500);

    buf << uint8(UPDATETYPE_VALUES);
    buf << GetPackGUID();

    UpdateMask updateMask;
    updateMask.SetCount(m_valuesCount);

    _SetUpdateBits(&updateMask, target);
    BuildValuesUpdate(UPDATETYPE_VALUES, &buf, &updateMask, target);

    data->AddUpdateBlock(buf);
}

void Object::BuildOutOfRangeUpdateBlock(UpdateData * data) const
{
    data->AddOutOfRangeGUID(GetObjectGuid());
}

void Object::DestroyForPlayer( Player *target, bool anim ) const
{
    MANGOS_ASSERT(target);

    WorldPacket data(SMSG_DESTROY_OBJECT, 9);
    data << GetObjectGuid();
    data << uint8(anim ? 1 : 0);                            // WotLK (bool), may be despawn animation
    target->GetSession()->SendPacket(&data);
}

void Object::BuildMovementUpdate(ByteBuffer * data, uint16 updateFlags) const
{

    *data << uint16(updateFlags);                           // update flags

    // 0x20
    if (updateFlags & UPDATEFLAG_LIVING)
    {
        Unit *unit = ((Unit*)this);

        if (unit->GetTransport() || unit->GetVehicle())
            unit->m_movementInfo.AddMovementFlag(MOVEFLAG_ONTRANSPORT);
        else
            unit->m_movementInfo.RemoveMovementFlag(MOVEFLAG_ONTRANSPORT);

        // Update movement info time
        unit->m_movementInfo.UpdateTime(WorldTimer::getMSTime());
        // Write movement info
        unit->m_movementInfo.Write(*data);

        // Unit speeds
        *data << float(unit->GetSpeed(MOVE_WALK));
        *data << float(unit->GetSpeed(MOVE_RUN));
        *data << float(unit->GetSpeed(MOVE_RUN_BACK));
        *data << float(unit->GetSpeed(MOVE_SWIM));
        *data << float(unit->GetSpeed(MOVE_SWIM_BACK));
        *data << float(unit->GetSpeed(MOVE_FLIGHT));
        *data << float(unit->GetSpeed(MOVE_FLIGHT_BACK));
        *data << float(unit->GetSpeed(MOVE_TURN_RATE));
        *data << float(unit->GetSpeed(MOVE_PITCH_RATE));

        // 0x08000000
        if (unit->m_movementInfo.GetMovementFlags() & MOVEFLAG_SPLINE_ENABLED)
            Movement::PacketBuilder::WriteCreate(*unit->movespline, *data);
    }
    else
    {
        if (updateFlags & UPDATEFLAG_POSITION)
        {
            ObjectGuid transportGuid;
            if (GetObjectGuid().IsUnit())
            {
                if (((Unit*)this)->m_movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
                    transportGuid = ((Unit*)this)->m_movementInfo.GetTransportGuid();
            }
            else if (Transport* transport = ((WorldObject*)this)->GetTransport())
                transportGuid = transport->GetObjectGuid();

            if (transportGuid)
                *data << transportGuid.WriteAsPacked();
            else
                *data << uint8(0);

            *data << float(((WorldObject*)this)->GetPositionX());
            *data << float(((WorldObject*)this)->GetPositionY());
            *data << float(((WorldObject*)this)->GetPositionZ());

            if (transportGuid)
            {
                *data << float(((WorldObject*)this)->GetTransOffsetX());
                *data << float(((WorldObject*)this)->GetTransOffsetY());
                *data << float(((WorldObject*)this)->GetTransOffsetZ());
            }
            else
            {
                *data << float(((WorldObject*)this)->GetPositionX());
                *data << float(((WorldObject*)this)->GetPositionY());
                *data << float(((WorldObject*)this)->GetPositionZ());
            }
            *data << float(((WorldObject*)this)->GetOrientation());

            if (GetTypeId() == TYPEID_CORPSE)
                *data << float(((WorldObject*)this)->GetOrientation());
            else
                *data << float(0);
        }
        else
        {
            // 0x40
            if (updateFlags & UPDATEFLAG_HAS_POSITION)
            {
                // 0x02
                if ((updateFlags & UPDATEFLAG_TRANSPORT) && ((GameObject*)this)->GetGoType() == GAMEOBJECT_TYPE_MO_TRANSPORT)
                {
                    *data << float(0);
                    *data << float(0);
                    *data << float(0);
                    *data << float(((WorldObject *)this)->GetOrientation());
                }
                else if (updateFlags & UPDATEFLAG_TRANSPORT)
                {
                    *data << float(((WorldObject*)this)->GetTransOffsetX());
                    *data << float(((WorldObject*)this)->GetTransOffsetY());
                    *data << float(((WorldObject*)this)->GetTransOffsetZ());
                    *data << float(((WorldObject*)this)->GetTransOffsetO());
                }
                else
                {
                    *data << float(((WorldObject *)this)->GetPositionX());
                    *data << float(((WorldObject *)this)->GetPositionY());
                    *data << float(((WorldObject *)this)->GetPositionZ());
                    *data << float(((WorldObject *)this)->GetOrientation());
                }
            }
        }
    }

    // 0x8
    if (updateFlags & UPDATEFLAG_LOWGUID)
    {
        switch(GetTypeId())
        {
            case TYPEID_OBJECT:
            case TYPEID_ITEM:
            case TYPEID_CONTAINER:
            case TYPEID_GAMEOBJECT:
            case TYPEID_DYNAMICOBJECT:
            case TYPEID_CORPSE:
                *data << uint32(GetGUIDLow());              // GetGUIDLow()
                break;
            case TYPEID_UNIT:
                *data << uint32(0x0000000B);                // unk, can be 0xB or 0xC
                break;
            case TYPEID_PLAYER:
                if (updateFlags & UPDATEFLAG_SELF)
                    *data << uint32(0x0000002F);            // unk, can be 0x15 or 0x22
                else
                    *data << uint32(0x00000008);            // unk, can be 0x7 or 0x8
                break;
            default:
                *data << uint32(0x00000000);                // unk
                break;
        }
    }

    // 0x10
    if (updateFlags & UPDATEFLAG_HIGHGUID)
    {
        switch(GetTypeId())
        {
            case TYPEID_OBJECT:
            case TYPEID_ITEM:
            case TYPEID_CONTAINER:
            case TYPEID_GAMEOBJECT:
            case TYPEID_DYNAMICOBJECT:
            case TYPEID_CORPSE:
                *data << uint32(GetObjectGuid().GetHigh()); // GetGUIDHigh()
                break;
            case TYPEID_UNIT:
                *data << uint32(0x0000000B);                // unk, can be 0xB or 0xC
                break;
            case TYPEID_PLAYER:
                if (updateFlags & UPDATEFLAG_SELF)
                    *data << uint32(0x0000002F);            // unk, can be 0x15 or 0x22
                else
                    *data << uint32(0x00000008);            // unk, can be 0x7 or 0x8
                break;
            default:
                *data << uint32(0x00000000);                // unk
                break;
        }
    }

    // 0x4
    if (updateFlags & UPDATEFLAG_HAS_ATTACKING_TARGET)       // packed guid (current target guid)
    {
        if (((Unit*)this)->getVictim())
            *data << ((Unit*)this)->getVictim()->GetPackGUID();
        else
            data->appendPackGUID(0);
    }

    // 0x2
    if (updateFlags & UPDATEFLAG_TRANSPORT)
    {
        *data << uint32(WorldTimer::getMSTime());                       // ms time
    }

    // 0x80
    if (updateFlags & UPDATEFLAG_VEHICLE)
    {
        *data << uint32(((Unit*)this)->GetVehicleInfo()->m_ID); // vehicle id
        *data << float(((WorldObject*)this)->GetOrientation());
    }

    // 0x200
    if (updateFlags & UPDATEFLAG_ROTATION)
    {
        *data << int64(((GameObject*)this)->GetPackedWorldRotation());
    }
}

void Object::BuildValuesUpdate(uint8 updatetype, ByteBuffer * data, UpdateMask *updateMask, Player *target) const
{
    if (!target)
        return;

    bool IsActivateToQuest = false;
    bool IsPerCasterAuraState = false;

    if (updatetype == UPDATETYPE_CREATE_OBJECT || updatetype == UPDATETYPE_CREATE_OBJECT2)
    {
        if (isType(TYPEMASK_GAMEOBJECT) && !((GameObject*)this)->IsDynTransport())
        {
            if (((GameObject*)this)->ActivateToQuest(target) || target->isGameMaster())
                IsActivateToQuest = true;
        }
        else if (isType(TYPEMASK_UNIT))
        {
            if (((Unit*)this)->HasAuraState(AURA_STATE_CONFLAGRATE))
            {
                IsPerCasterAuraState = true;
                updateMask->SetBit(UNIT_FIELD_AURASTATE);
            }
        }
    }
    else                                                    // case UPDATETYPE_VALUES
    {
        if (isType(TYPEMASK_GAMEOBJECT) && !((GameObject*)this)->IsDynTransport())
        {
            if (((GameObject*)this)->ActivateToQuest(target) || target->isGameMaster())
                IsActivateToQuest = true;
            updateMask->SetBit(GAMEOBJECT_BYTES_1);         // why do we need this here?
        }
        else if (isType(TYPEMASK_UNIT))
        {
            if (((Unit*)this)->HasAuraState(AURA_STATE_CONFLAGRATE))
            {
                IsPerCasterAuraState = true;
                updateMask->SetBit(UNIT_FIELD_AURASTATE);
            }
        }
    }

    MANGOS_ASSERT(updateMask && updateMask->GetCount() == m_valuesCount);

    *data << (uint8)updateMask->GetBlockCount();
    data->append(updateMask->GetMask(), updateMask->GetLength());

    // 2 specialized loops for speed optimization in non-unit case
    if (isType(TYPEMASK_UNIT))                              // unit (creature/player) case
    {
        for(uint16 index = 0; index < m_valuesCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                if (index == UNIT_NPC_FLAGS)
                {
                    uint32 appendValue = m_uint32Values[index];

                    if (GetTypeId() == TYPEID_UNIT)
                    {
                        if (!target->canSeeSpellClickOn((Creature*)this))
                            appendValue &= ~UNIT_NPC_FLAG_SPELLCLICK;

                        if (appendValue & UNIT_NPC_FLAG_TRAINER)
                        {
                            if (!((Creature*)this)->IsTrainerOf(target, false))
                                appendValue &= ~(UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS | UNIT_NPC_FLAG_TRAINER_PROFESSION);
                        }

                        if (appendValue & UNIT_NPC_FLAG_STABLEMASTER)
                        {
                            if (target->getClass() != CLASS_HUNTER)
                                appendValue &= ~UNIT_NPC_FLAG_STABLEMASTER;
                        }
                    }

                    *data << uint32(appendValue);
                }
                else if (index == UNIT_FIELD_AURASTATE)
                {
                    if (IsPerCasterAuraState)
                    {
                        // IsPerCasterAuraState set if related pet caster aura state set already
                        if (((Unit*)this)->HasAuraStateForCaster(AURA_STATE_CONFLAGRATE, target->GetObjectGuid()))
                            *data << m_uint32Values[index];
                        else
                            *data << (m_uint32Values[index] & ~(1 << (AURA_STATE_CONFLAGRATE-1)));
                    }
                    else
                        *data << m_uint32Values[index];
                }
                // FIXME: Some values at server stored in float format but must be sent to client in uint32 format
                else if (index >= UNIT_FIELD_BASEATTACKTIME && index <= UNIT_FIELD_RANGEDATTACKTIME)
                {
                    // convert from float to uint32 and send
                    *data << uint32(m_floatValues[index] < 0 ? 0 : m_floatValues[index]);
                }

                // there are some float values which may be negative or can't get negative due to other checks
                else if ((index >= UNIT_FIELD_NEGSTAT0 && index <= UNIT_FIELD_NEGSTAT4) ||
                    (index >= UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE  && index <= (UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE + 6)) ||
                    (index >= UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE  && index <= (UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE + 6)) ||
                    (index >= UNIT_FIELD_POSSTAT0 && index <= UNIT_FIELD_POSSTAT4))
                {
                    *data << uint32(m_floatValues[index]);
                }

                // Gamemasters should be always able to select units - remove not selectable flag
                else if (index == UNIT_FIELD_FLAGS && target->isGameMaster())
                {
                    *data << (m_uint32Values[index] & ~UNIT_FLAG_NOT_SELECTABLE);
                }
                // hide lootable animation for unallowed players
                else if (index == UNIT_DYNAMIC_FLAGS && GetTypeId() == TYPEID_UNIT)
                {
                    if (!target->isAllowedToLoot((Creature*)this))
                        *data << (m_uint32Values[index] & ~(UNIT_DYNFLAG_LOOTABLE | UNIT_DYNFLAG_TAPPED_BY_PLAYER));
                    else
                    {
                        // flag only for original loot recipent
                        if (target->GetObjectGuid() == ((Creature*)this)->GetLootRecipientGuid())
                            *data << m_uint32Values[index];
                        else
                            *data << (m_uint32Values[index] & ~(UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER));
                    }
                }
                // hide RAF flag if need
                else if (index == UNIT_DYNAMIC_FLAGS && GetTypeId() == TYPEID_PLAYER)
                {
                    if (!((Player*)this)->IsReferAFriendLinked(target))
                        *data << (m_uint32Values[index] & ~UNIT_DYNFLAG_REFER_A_FRIEND);
                    else
                        *data << m_uint32Values[index];
                }
                // Frozen Mod
                else if (index == UNIT_FIELD_BYTES_2 || index == UNIT_FIELD_FACTIONTEMPLATE)
                {
                    bool ch = false;

                    if((GetTypeId() == TYPEID_PLAYER || GetTypeId() == TYPEID_UNIT) && target != this)
                    {
                        bool forcefriendly = false; // bool for pets/totems to offload more code from the big if below

                        if (GetTypeId() == TYPEID_UNIT && ((Creature*)this)->GetOwner())
                        {
                            forcefriendly = (((Creature*)this)->IsTotem() || ((Creature*)this)->IsPet())
                            && (((Creature*)this)->GetOwner()->GetTypeId() == TYPEID_PLAYER
                                && ((Creature*)this)->GetOwner()->IsFriendlyTo(target)
                                && ((Creature*)this)->GetOwner() != target
                                && (target->IsInSameGroupWith((Player*)((Creature*)this)->GetOwner()) || target->IsInSameRaidWith((Player*)((Creature*)this)->GetOwner())));
                        }

                        if(((Unit*)this)->IsSpoofSamePlayerFaction() || forcefriendly || (target->GetTypeId() == TYPEID_PLAYER && GetTypeId() == TYPEID_PLAYER && (target->IsInSameGroupWith((Player*)this) || target->IsInSameRaidWith((Player*)this))))
                        {
                            if (index == UNIT_FIELD_BYTES_2)
                            {
                                DEBUG_LOG("-- VALUES_UPDATE: Sending '%s' the blue-group-fix from '%s' (flag)", target->GetName(), ((Unit*)this)->GetName());
                                *data << ( m_uint32Values[ index ] & (UNIT_BYTE2_FLAG_SANCTUARY << 8) ); // this flag is at uint8 offset 1 !!
                                ch = true;
                            }
                            else if (index == UNIT_FIELD_FACTIONTEMPLATE)
                            {
                                FactionTemplateEntry const *ft1, *ft2;
                                ft1 = ((Unit*)this)->getFactionTemplateEntry();
                                ft2 = ((Unit*)target)->getFactionTemplateEntry();

                                if (ft1 && ft2 && (!ft1->IsFriendlyTo(*ft2) || ((Unit*)this)->IsSpoofSamePlayerFaction()))
                                {
                                    uint32 faction = ((Player*)target)->getFaction(); // pretend that all other HOSTILE players have own faction, to allow follow, heal, rezz (trade wont work)
                                    DEBUG_LOG("-- VALUES_UPDATE: Sending '%s' the blue-group-fix from '%s' (faction %u)", target->GetName(), ((Unit*)this)->GetName(), faction);
                                    *data << uint32(faction);
                                    ch = true;
                                }
                            }
                        }
                    }

                    if(!ch)
                        *data << m_uint32Values[ index ];
                }
                // Frozen Mod
                else
                {
                    // send in current format (float as float, uint32 as uint32)
                    *data << m_uint32Values[index];
                }
            }
        }
    }
    else if (isType(TYPEMASK_GAMEOBJECT))                   // gameobject case
    {
        for(uint16 index = 0; index < m_valuesCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                // send in current format (float as float, uint32 as uint32)
                if (index == GAMEOBJECT_DYNAMIC)
                {
                    // GAMEOBJECT_TYPE_DUNGEON_DIFFICULTY can have lo flag = 2
                    //      most likely related to "can enter map" and then should be 0 if can not enter

                    if (IsActivateToQuest)
                    {
                        switch(((GameObject*)this)->GetGoType())
                        {
                            case GAMEOBJECT_TYPE_QUESTGIVER:
                                // GO also seen with GO_DYNFLAG_LO_SPARKLE explicit, relation/reason unclear (192861)
                                *data << uint16(GO_DYNFLAG_LO_ACTIVATE);
                                *data << uint16(-1);
                                break;
                            case GAMEOBJECT_TYPE_CHEST:
                            case GAMEOBJECT_TYPE_GENERIC:
                            case GAMEOBJECT_TYPE_SPELL_FOCUS:
                            case GAMEOBJECT_TYPE_GOOBER:
                                *data << uint16(GO_DYNFLAG_LO_ACTIVATE | GO_DYNFLAG_LO_SPARKLE);
                                *data << uint16(-1);
                                break;
                            default:
                                // unknown, not happen.
                                *data << uint16(0);
                                *data << uint16(-1);
                                break;
                        }
                    }
                    else
                    {
                        // disable quest object
                        *data << uint16(0);
                        *data << uint16(-1);
                    }
                }
                else
                    *data << m_uint32Values[index];         // other cases
            }
        }
    }
    else                                                    // other objects case (no special index checks)
    {
        for(uint16 index = 0; index < m_valuesCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                // send in current format (float as float, uint32 as uint32)
                *data << m_uint32Values[index];
            }
        }
    }
}

void Object::ClearUpdateMask(bool remove)
{
    if (m_uint32Values)
    {
        for (uint16 index = 0; index < m_valuesCount; ++index)
            m_changedValues[index] = false;
    }

    if (m_objectUpdated)
    {
        if (remove)
            RemoveFromClientUpdateList();
        m_objectUpdated = false;
    }
}

bool Object::LoadValues(const char* data)
{
    if (!m_uint32Values)
        _InitValues();

    Tokens tokens(data, ' ');

    if (tokens.size() != m_valuesCount)
        return false;

    for (uint16 index = 0; index < m_valuesCount; ++index)
    {
        m_uint32Values[index] = atol(tokens[index]);
    }

    return true;
}

void Object::_SetUpdateBits(UpdateMask* updateMask, Player* target) const
{
    UpdateFieldData ufd(this, target);

    for (uint16 index = 0; index < m_valuesCount; ++index)
    {
        if (ufd.IsUpdateNeeded(index, m_fieldNotifyFlags) ||
            (m_changedValues[index] && ufd.IsUpdateFieldVisible(index)))
            updateMask->SetBit(index);
    }
}

void Object::_SetCreateBits(UpdateMask* updateMask, Player* target) const
{
    UpdateFieldData ufd(this, target);

    for (uint16 index = 0; index < m_valuesCount; ++index)
    {
        if (ufd.IsUpdateNeeded(index, m_fieldNotifyFlags) ||
            ((GetUInt32Value(index) != 0) && ufd.IsUpdateFieldVisible(index)))
            updateMask->SetBit(index);
    }
}

void Object::SetInt32Value( uint16 index, int32 value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (m_int32Values[index] != value)
    {
        m_int32Values[index] = value;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetUInt32Value( uint16 index, uint32 value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (m_uint32Values[index] != value)
    {
        m_uint32Values[index] = value;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetUInt64Value( uint16 index, const uint64 &value )
{
    MANGOS_ASSERT(index + 1 < m_valuesCount || PrintIndexError(index, true));
    if (*((uint64*) & (m_uint32Values[index])) != value)
    {
        m_uint32Values[index] = *((uint32*)&value);
        m_uint32Values[index + 1] = *(((uint32*)&value) + 1);
        m_changedValues[index] = true;
        m_changedValues[index + 1] = true;
        MarkForClientUpdate();
    }
}

void Object::SetFloatValue( uint16 index, float value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (m_floatValues[index] != value)
    {
        m_floatValues[index] = value;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetByteValue( uint16 index, uint8 offset, uint8 value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (offset > 4)
    {
        sLog.outError("Object::SetByteValue: wrong offset %u", offset);
        return;
    }

    if (uint8(m_uint32Values[index] >> (offset * 8)) != value)
    {
        m_uint32Values[index] &= ~uint32(uint32(0xFF) << (offset * 8));
        m_uint32Values[index] |= uint32(uint32(value) << (offset * 8));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetUInt16Value( uint16 index, uint8 offset, uint16 value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (offset > 2)
    {
        sLog.outError("Object::SetUInt16Value: wrong offset %u", offset);
        return;
    }

    if (uint16(m_uint32Values[index] >> (offset * 16)) != value)
    {
        m_uint32Values[index] &= ~uint32(uint32(0xFFFF) << (offset * 16));
        m_uint32Values[index] |= uint32(uint32(value) << (offset * 16));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetStatFloatValue( uint16 index, float value)
{
    if (value < 0)
        value = 0.0f;

    SetFloatValue(index, value);
}

void Object::SetStatInt32Value( uint16 index, int32 value)
{
    if (value < 0)
        value = 0;

    SetUInt32Value(index, uint32(value));
}

void Object::ApplyModUInt32Value(uint16 index, int32 val, bool apply)
{
    int32 cur = GetUInt32Value(index);
    cur += (apply ? val : -val);
    if (cur < 0)
        cur = 0;
    SetUInt32Value(index, cur);
}

void Object::ApplyModInt32Value(uint16 index, int32 val, bool apply)
{
    int32 cur = GetInt32Value(index);
    cur += (apply ? val : -val);
    SetInt32Value(index, cur);
}

void Object::ApplyModSignedFloatValue(uint16 index, float  val, bool apply)
{
    float cur = GetFloatValue(index);
    cur += (apply ? val : -val);
    SetFloatValue(index, cur);
}

void Object::ApplyModPositiveFloatValue(uint16 index, float  val, bool apply)
{
    float cur = GetFloatValue(index);
    cur += (apply ? val : -val);
    if (cur < 0)
        cur = 0;
    SetFloatValue(index, cur);
}

void Object::SetFlag( uint16 index, uint32 newFlag )
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));
    uint32 oldval = m_uint32Values[index];
    uint32 newval = oldval | newFlag;

    if (oldval != newval)
    {
        m_uint32Values[index] = newval;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::RemoveFlag( uint16 index, uint32 oldFlag )
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));
    uint32 oldval = m_uint32Values[index];
    uint32 newval = oldval & ~oldFlag;

    if (oldval != newval)
    {
        m_uint32Values[index] = newval;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetByteFlag( uint16 index, uint8 offset, uint8 newFlag )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (offset > 4)
    {
        sLog.outError("Object::SetByteFlag: wrong offset %u", offset);
        return;
    }

    if (!(uint8(m_uint32Values[index] >> (offset * 8)) & newFlag))
    {
        m_uint32Values[index] |= uint32(uint32(newFlag) << (offset * 8));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::RemoveByteFlag( uint16 index, uint8 offset, uint8 oldFlag )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (offset > 4)
    {
        sLog.outError("Object::RemoveByteFlag: wrong offset %u", offset);
        return;
    }

    if (uint8(m_uint32Values[index] >> (offset * 8)) & oldFlag)
    {
        m_uint32Values[index] &= ~uint32(uint32(oldFlag) << (offset * 8));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetShortFlag(uint16 index, bool highpart, uint16 newFlag)
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (!(uint16(m_uint32Values[index] >> (highpart ? 16 : 0)) & newFlag))
    {
        m_uint32Values[index] |= uint32(uint32(newFlag) << (highpart ? 16 : 0));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::RemoveShortFlag(uint16 index, bool highpart, uint16 oldFlag)
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (uint16(m_uint32Values[index] >> (highpart ? 16 : 0)) & oldFlag)
    {
        m_uint32Values[index] &= ~uint32(uint32(oldFlag) << (highpart ? 16 : 0));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

bool Object::PrintIndexError(uint32 index, bool set) const
{
    sLog.outError("Attempt %s nonexistent value field: %u (count: %u) for object typeid: %u type mask: %u",(set ? "set value to" : "get value from"),index,m_valuesCount,GetTypeId(),m_objectType);

    // ASSERT must fail after function call
    return false;
}

bool Object::PrintEntryError(char const* descr) const
{
    sLog.outError("Object Type %u, Entry %u (lowguid %u) with invalid call for %s", GetTypeId(), GetEntry(), GetObjectGuid().GetCounter(), descr);

    // always false for continue assert fail
    return false;
}


void Object::BuildUpdateDataForPlayer(Player* player, UpdateDataMapType& update_players)
{
    if (!player)
        return;

    UpdateData& data = update_players[player->GetObjectGuid()];

    BuildValuesUpdateBlockForPlayer(&data, player);
}

void Object::AddToClientUpdateList()
{
    sLog.outError("Unexpected call of Object::AddToClientUpdateList for object (TypeId: %u Update fields: %u)",GetTypeId(), m_valuesCount);
    MANGOS_ASSERT(false);
}

void Object::RemoveFromClientUpdateList()
{
    sLog.outError("Unexpected call of Object::RemoveFromClientUpdateList for object (TypeId: %u Update fields: %u)",GetTypeId(), m_valuesCount);
    MANGOS_ASSERT(false);
}

void Object::BuildUpdateData( UpdateDataMapType& /*update_players */)
{
    sLog.outError("Unexpected call of Object::BuildUpdateData for object (TypeId: %u Update fields: %u)",GetTypeId(), m_valuesCount);
    MANGOS_ASSERT(false);
}

void Object::MarkForClientUpdate()
{
    if (m_inWorld)
    {
        if(!m_objectUpdated)
        {
            AddToClientUpdateList();
            m_objectUpdated = true;
        }
    }
}

WorldObject::WorldObject()
    : loot(this), m_groupLootTimer(0), m_groupLootId(0), m_lootGroupRecipientId(0), m_transportInfo(NULL), movespline(new Movement::MoveSpline()),
    m_currMap(NULL), m_position(WorldLocation()), m_viewPoint(*this), m_isActiveObject(false), m_LastUpdateTime(WorldTimer::getMSTime())
{
}

WorldObject::~WorldObject()
{
    delete movespline;
}

void WorldObject::CleanupsBeforeDelete()
{
    RemoveFromWorld(false);
    ClearUpdateMask(true);
}

void WorldObject::_Create(ObjectGuid guid, uint32 phaseMask)
{
    Object::_Create(guid);
    SetPhaseMask(phaseMask, false);
}

void WorldObject::AddToWorld()
{
    MANGOS_ASSERT(m_currMap);
    if (!IsInWorld())
        Object::AddToWorld();

    // Possible inserted object, already exists in object store. Not must cause any problem, but need check.
    GetMap()->InsertObject(this);
    GetMap()->AddUpdateObject(GetObjectGuid());
}

void WorldObject::RemoveFromWorld(bool remove)
{
    Map* map = GetMap();
    MANGOS_ASSERT(map);

    if (IsInWorld())
        Object::RemoveFromWorld(remove);

    map->RemoveUpdateObject(GetObjectGuid());

    if (remove)
    {
        ResetMap();
        map->EraseObject(GetObjectGuid());
    }
}

ObjectLockType& WorldObject::GetLock(MapLockType _lockType)
{
    return GetMap() ? GetMap()->GetLock(_lockType) : sWorld.GetLock(_lockType);
}

void WorldObject::Relocate(WorldLocation const& location)
{
    bool locationChanged    = !bool(location == m_position);
    bool orientationChanged = bool(fabs(location.o - m_position.o) > M_NULL_F);

    m_position = location;

    if (isType(TYPEMASK_UNIT))
    {
        if (locationChanged)
            ((Unit*)this)->m_movementInfo.ChangePosition(m_position.x, m_position.y, m_position.z, m_position.o);
        else if (orientationChanged)
            ((Unit*)this)->m_movementInfo.ChangeOrientation(m_position.o);
    }
}

void WorldObject::Relocate(float x, float y, float z, float orientation)
{
    m_position.x = x;
    m_position.y = y;
    m_position.z = z;
    m_position.o = orientation;

    if (isType(TYPEMASK_UNIT))
        ((Unit*)this)->m_movementInfo.ChangePosition(x, y, z, orientation);
}

void WorldObject::Relocate(float x, float y, float z)
{
    m_position.x = x;
    m_position.y = y;
    m_position.z = z;

    if (isType(TYPEMASK_UNIT))
        ((Unit*)this)->m_movementInfo.ChangePosition(x, y, z, GetOrientation());
}

void WorldObject::SetOrientation(float orientation)
{
    m_position.o = orientation;

    if (isType(TYPEMASK_UNIT))
        ((Unit*)this)->m_movementInfo.ChangeOrientation(orientation);
}

uint32 WorldObject::GetZoneId() const
{
    return GetTerrain()->GetZoneId(m_position.x, m_position.y, m_position.z);
}

uint32 WorldObject::GetAreaId() const
{
    return GetTerrain()->GetAreaId(m_position.x, m_position.y, m_position.z);
}

void WorldObject::GetZoneAndAreaId(uint32& zoneid, uint32& areaid) const
{
    GetTerrain()->GetZoneAndAreaId(zoneid, areaid, m_position.x, m_position.y, m_position.z);
}

InstanceData* WorldObject::GetInstanceData() const
{
    return GetMap()->GetInstanceData();
}

float WorldObject::GetDistance(const WorldObject* obj) const
{
    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();
    float dist = GetDistance(obj->GetPosition()) - sizefactor;
    return ( dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistance(WorldLocation const& loc) const
{
    float dist = GetPosition().GetDistance(loc) - GetObjectBoundingRadius();
    return (dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistance2d(float x, float y) const
{
    float sizefactor = GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(Location(x, y, GetPositionZ())) - sizefactor;
    return ( dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistance(float x, float y, float z) const
{
    float sizefactor = GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(Location(x, y, z)) - sizefactor;
    return ( dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistance2d(const WorldObject* obj) const
{
    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(Location(obj->GetPositionX(), obj->GetPositionY(), GetPositionZ())) - sizefactor;
    return ( dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistanceZ(const WorldObject* obj) const
{
    float dz = fabs(GetPositionZ() - obj->GetPositionZ());
    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();
    float dist = dz - sizefactor;
    return ( dist > M_NULL_F ? dist : 0.0f);
}

bool WorldObject::IsWithinDist3d(float x, float y, float z, float dist2compare) const
{
    return IsWithinDist3d(Location(x, y, z), dist2compare);
}

bool WorldObject::IsWithinDist3d(Location const& loc, float dist2compare) const
{
    float sizefactor = GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(loc);
    float maxdist = dist2compare + sizefactor;

    return dist < maxdist;
}

bool WorldObject::IsWithinDist2d(float x, float y, float dist2compare) const
{
    float dist = GetPosition().GetDistance(Location(x, y, GetPositionZ()));
    float sizefactor = GetObjectBoundingRadius();
    float maxdist = dist2compare + sizefactor;

    return dist < maxdist;
}

bool WorldObject::_IsWithinDist(WorldObject const* obj, float dist2compare, bool is3D) const
{

    float dist = is3D ?
        GetPosition().GetDistance(obj->GetPosition()) :
        GetPosition().GetDistance(Location(obj->GetPositionX(), obj->GetPositionY(), GetPositionZ()));

    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();
    float maxdist = dist2compare + sizefactor;

    return dist < maxdist;
}

bool WorldObject::IsWithinLOSInMap(const WorldObject* obj) const
{
    if (!IsInMap(obj))
        return false;

    float ox,oy,oz;
    obj->GetPosition(ox,oy,oz);
    return IsWithinLOS(ox, oy, oz );
}

bool WorldObject::IsWithinLOS(float ox, float oy, float oz) const
{
    float x,y,z;
    GetPosition(x,y,z);
    return GetMap()->IsInLineOfSight(x, y, z + 2.0f, ox, oy, oz + 2.0f, GetPhaseMask());
}

bool WorldObject::GetDistanceOrder(WorldObject const* obj1, WorldObject const* obj2, bool is3D /* = true */) const
{
    float dist1 = is3D ?
        GetPosition().GetDistance(obj1->GetPosition()) :
        GetPosition().GetDistance(Location(obj1->GetPositionX(), obj1->GetPositionY(), GetPositionZ()));

    float dist2 = is3D ?
        GetPosition().GetDistance(obj2->GetPosition()) :
        GetPosition().GetDistance(Location(obj2->GetPositionX(), obj2->GetPositionY(), GetPositionZ()));

    return dist1 < dist2;
}

bool WorldObject::IsInRange(WorldObject const* obj, float minRange, float maxRange, bool is3D /* = true */) const
{
    float dist = is3D ?
        GetPosition().GetDistance(obj->GetPosition()) :
        GetPosition().GetDistance(Location(obj->GetPositionX(), obj->GetPositionY(), GetPositionZ()));

    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();

    // check only for real range
    if (minRange > M_NULL_F)
    {
        float mindist = minRange + sizefactor;
        if (dist < mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return dist < maxdist;
}

bool WorldObject::IsInRange2d(float x, float y, float minRange, float maxRange) const
{
    float dist =  GetPosition().GetDistance(Location(x, y, GetPositionZ()));

    float sizefactor = GetObjectBoundingRadius();

    // check only for real range
    if (minRange > M_NULL_F)
    {
        float mindist = minRange + sizefactor;
        if (dist < mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return dist < maxdist;
}

bool WorldObject::IsInRange3d(float x, float y, float z, float minRange, float maxRange) const
{

    float dist = GetPosition().GetDistance(Location(x, y, z));
    float sizefactor = GetObjectBoundingRadius();

    // check only for real range
    if (minRange > M_NULL_F)
    {
        float mindist = minRange + sizefactor;
        if (dist < mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return dist < maxdist;
}

bool WorldObject::IsInBetween(const WorldObject *obj1, const WorldObject *obj2, float size) const
{
    if (GetPositionX() > std::max(obj1->GetPositionX(), obj2->GetPositionX())
        || GetPositionX() < std::min(obj1->GetPositionX(), obj2->GetPositionX())
        || GetPositionY() > std::max(obj1->GetPositionY(), obj2->GetPositionY())
        || GetPositionY() < std::min(obj1->GetPositionY(), obj2->GetPositionY()))
        return false;

    if (!size)
        size = GetObjectBoundingRadius() / 2;

    float angle = obj1->GetAngle(this) - obj1->GetAngle(obj2);
    return abs(sin(angle)) * GetExactDist2d(obj1->GetPositionX(), obj1->GetPositionY()) < size;
}

float WorldObject::GetAngle(const WorldObject* obj) const
{
    if (!obj)
        return 0.0f;

    // Rework the assert, when more cases where such a call can happen have been fixed
    //MANGOS_ASSERT(obj != this || PrintEntryError("GetAngle (for self)"));
    if (obj == this)
    {
        sLog.outError("WorldObject::GetAngle INVALID CALL for GetAngle for %s", obj->GetGuidStr().c_str());
        return 0.0f;
    }
    return GetAngle(obj->GetPositionX(), obj->GetPositionY());
}

// Return angle in range 0..2*pi
float WorldObject::GetAngle(const float x, const float y) const
{
    float dx = x - GetPositionX();
    float dy = y - GetPositionY();

    float ang = atan2(dy, dx);                              // returns value between -Pi..Pi
    ang = (ang >= 0) ? ang : 2 * M_PI_F + ang;
    return MapManager::NormalizeOrientation(ang);
}

bool WorldObject::HasInArc(const float arcangle, const WorldObject* obj) const
{
    // always have self in arc
    if (obj == this)
        return true;

    float arc = arcangle;

    // move arc to range 0.. 2*pi
    arc = MapManager::NormalizeOrientation(arc);

    float angle = GetAngle(obj);
    angle -= m_position.o;

    // move angle to range -pi ... +pi
    angle = MapManager::NormalizeOrientation(angle);
    if (angle > M_PI_F)
        angle -= 2.0f*M_PI_F;

    float lborder =  -1 * (arc/2.0f);                       // in range -pi..0
    float rborder = (arc/2.0f);                             // in range 0..pi
    return (( angle >= lborder ) && ( angle <= rborder ));
}

bool WorldObject::isInFrontInMap(WorldObject const* target, float distance,  float arc) const
{
    return IsWithinDistInMap(target, distance) && HasInArc( arc, target );
}

bool WorldObject::isInBackInMap(WorldObject const* target, float distance, float arc) const
{
    return IsWithinDistInMap(target, distance) && !HasInArc( 2 * M_PI_F - arc, target );
}

bool WorldObject::isInFront(WorldObject const* target, float distance,  float arc) const
{
    return IsWithinDist(target, distance) && HasInArc( arc, target );
}

bool WorldObject::isInBack(WorldObject const* target, float distance, float arc) const
{
    return IsWithinDist(target, distance) && !HasInArc( 2 * M_PI_F - arc, target );
}

void WorldObject::GetRandomPoint( float x, float y, float z, float distance, float &rand_x, float &rand_y, float &rand_z) const
{
    if (fabs(distance) < M_NULL_F)
    {
        rand_x = x;
        rand_y = y;
        rand_z = z;
        return;
    }

    // angle to face `obj` to `this`
    float angle = rand_norm_f()*2*M_PI_F;
    float new_dist = rand_norm_f()*distance;

    rand_x = x + new_dist * cos(angle);
    rand_y = y + new_dist * sin(angle);
    rand_z = z;

    MaNGOS::NormalizeMapCoord(rand_x);
    MaNGOS::NormalizeMapCoord(rand_y);
    UpdateGroundPositionZ(rand_x,rand_y,rand_z);            // update to LOS height if available
}

void WorldObject::UpdateGroundPositionZ(float x, float y, float &z) const
{
    float new_z = GetMap()->GetHeight(GetPhaseMask(), x, y, z);
    if (new_z > INVALID_HEIGHT)
        z = new_z + 0.05f;                                   // just to be sure that we are not a few pixel under the surface
}

void WorldObject::UpdateAllowedPositionZ(float x, float y, float &z) const
{
    switch (GetTypeId())
    {
        case TYPEID_UNIT:
        {
            Unit* pVictim = ((Creature const*)this)->getVictim();
            if (pVictim)
            {
                // anyway creature move to victim for thinly Z distance (shun some VMAP wrong ground calculating)
                if (fabs(GetPositionZ() - pVictim->GetPositionZ()) < 5.0f)
                    return;
            }

            if (((Creature const*)this)->IsLevitating())
            {
                float ground_z = GetMap()->GetHeight(GetPhaseMask(), x, y, z);
                z  = ground_z + ((Creature const*)this)->GetFloatValue(UNIT_FIELD_HOVERHEIGHT) + GetObjectBoundingRadius() * GetObjectScale();
            }
            // non fly unit don't must be in air
            // non swim unit must be at ground (mostly speedup, because it don't must be in water and water level check less fast
            else if (!((Creature const*)this)->CanFly())
            {
                bool canSwim = ((Creature const*)this)->CanSwim();
                float ground_z = z;
                float max_z = canSwim
                    ? GetTerrain()->GetWaterOrGroundLevel(x, y, z, &ground_z, !((Unit const*)this)->HasAuraType(SPELL_AURA_WATER_WALK))
                    : ((ground_z = GetMap()->GetHeight(GetPhaseMask(), x, y, z)));
                if (max_z > INVALID_HEIGHT)
                {
                    if (z > max_z)
                        z = max_z;
                    else if (z < ground_z)
                        z = ground_z;
                }
            }
            else
            {
                float ground_z = GetMap()->GetHeight(GetPhaseMask(), x, y, z);
                if (z < ground_z)
                    z = ground_z;
            }
            break;
        }
        case TYPEID_PLAYER:
        {
            // for server controlled moves player work same as creature (but it can always swim)
            if (!((Player const*)this)->CanFly())
            {
                float ground_z = z;
                float max_z = GetTerrain()->GetWaterOrGroundLevel(x, y, z, &ground_z, !((Unit const*)this)->HasAuraType(SPELL_AURA_WATER_WALK));
                if (max_z > INVALID_HEIGHT)
                {
                    if (max_z != ground_z && z > max_z)
                        z = max_z;
                    else if (z < ground_z)
                        z = ground_z;
                }
            }
            else
            {
                float ground_z = GetMap()->GetHeight(GetPhaseMask(), x, y, z);
                if (z < ground_z)
                    z = ground_z;
            }
            break;
        }
        default:
        {
            float ground_z = GetMap()->GetHeight(GetPhaseMask(), x, y, z);
            if (ground_z > INVALID_HEIGHT)
                z = ground_z;
            break;
        }
    }
}

bool WorldObject::IsPositionValid() const
{
    return MaNGOS::IsValidMapCoord(m_position.x,m_position.y,m_position.z,m_position.o);
}

void WorldObject::MonsterSay(const char* text, uint32 language, Unit* target)
{
    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data, GetObjectGuid(), CHAT_MSG_MONSTER_SAY, text, language, GetName(), target ? target->GetObjectGuid() : ObjectGuid(), target ? target->GetName() : "");
    SendMessageToSetInRange(&data,sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_SAY),true);
}

void WorldObject::MonsterYell(const char* text, uint32 language, Unit* target)
{
    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data, GetObjectGuid(), CHAT_MSG_MONSTER_YELL, text, language, GetName(), target ? target->GetObjectGuid() : ObjectGuid(), target ? target->GetName() : "");
    SendMessageToSetInRange(&data,sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_YELL),true);
}

void WorldObject::MonsterTextEmote(const char* text, Unit* target, bool IsBossEmote)
{
    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data, GetObjectGuid(), IsBossEmote ? CHAT_MSG_RAID_BOSS_EMOTE : CHAT_MSG_MONSTER_EMOTE, text, LANG_UNIVERSAL,
        GetName(), target ? target->GetObjectGuid() : ObjectGuid(), target ? target->GetName() : "");
    SendMessageToSetInRange(&data, sWorld.getConfig(IsBossEmote ? CONFIG_FLOAT_LISTEN_RANGE_YELL : CONFIG_FLOAT_LISTEN_RANGE_TEXTEMOTE), true);
}

void WorldObject::MonsterWhisper(const char* text, Unit* target, bool IsBossWhisper)
{
    if (!target || target->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data, GetObjectGuid(), IsBossWhisper ? CHAT_MSG_RAID_BOSS_WHISPER : CHAT_MSG_MONSTER_WHISPER, text, LANG_UNIVERSAL,
        GetName(), target->GetObjectGuid(), target->GetName());
    ((Player*)target)->GetSession()->SendPacket(&data);
}

namespace MaNGOS
{
    class MonsterChatBuilder
    {
        public:
            MonsterChatBuilder(WorldObject const& obj, ChatMsg msgtype, int32 textId, uint32 language, Unit* target)
                : i_object(obj), i_msgtype(msgtype), i_textId(textId), i_language(language), i_target(target) {}
            void operator()(WorldPacket& data, int32 loc_idx)
            {
                char const* text = sObjectMgr.GetMangosString(i_textId,loc_idx);

                WorldObject::BuildMonsterChat(&data, i_object.GetObjectGuid(), i_msgtype, text, i_language, i_object.GetNameForLocaleIdx(loc_idx), i_target ? i_target->GetObjectGuid() : ObjectGuid(), i_target ? i_target->GetNameForLocaleIdx(loc_idx) : "");
            }

        private:
            WorldObject const& i_object;
            ChatMsg i_msgtype;
            int32 i_textId;
            uint32 i_language;
            Unit* i_target;
    };
}                                                           // namespace MaNGOS

void WorldObject::MonsterSay(int32 textId, uint32 language, Unit* target)
{
    float range = sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_SAY);
    MaNGOS::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_SAY, textId, language, target);
    MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> say_do(say_build);
    MaNGOS::CameraDistWorker<MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> > say_worker(this, range, say_do);
    Cell::VisitWorldObjects(this, say_worker, range);
}

void WorldObject::MonsterYell(int32 textId, uint32 language, Unit* target)
{
    float range = sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_YELL);
    MaNGOS::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_YELL, textId, language, target);
    MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> say_do(say_build);
    MaNGOS::CameraDistWorker<MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> > say_worker(this,range,say_do);
    Cell::VisitWorldObjects(this, say_worker, range);
}

void WorldObject::MonsterYellToZone(int32 textId, uint32 language, Unit* target)
{
    MaNGOS::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_YELL, textId, language, target);
    MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> say_do(say_build);

    uint32 zoneid = GetZoneId();

    Map::PlayerList const& pList = GetMap()->GetPlayers();
    for(Map::PlayerList::const_iterator itr = pList.begin(); itr != pList.end(); ++itr)
        if (itr->getSource()->GetZoneId()==zoneid)
            say_do(itr->getSource());
}

void WorldObject::MonsterTextEmote(int32 textId, Unit* target, bool IsBossEmote)
{
    float range = sWorld.getConfig(IsBossEmote ? CONFIG_FLOAT_LISTEN_RANGE_YELL : CONFIG_FLOAT_LISTEN_RANGE_TEXTEMOTE);

    MaNGOS::MonsterChatBuilder say_build(*this, IsBossEmote ? CHAT_MSG_RAID_BOSS_EMOTE : CHAT_MSG_MONSTER_EMOTE, textId, LANG_UNIVERSAL, target);
    MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> say_do(say_build);
    MaNGOS::CameraDistWorker<MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> > say_worker(this,range,say_do);
    Cell::VisitWorldObjects(this, say_worker, range);
}

void WorldObject::MonsterWhisper(int32 textId, Unit* target, bool IsBossWhisper)
{
    if (!target || target->GetTypeId() != TYPEID_PLAYER)
        return;

    uint32 loc_idx = ((Player*)target)->GetSession()->GetSessionDbLocaleIndex();
    char const* text = sObjectMgr.GetMangosString(textId, loc_idx);

    WorldPacket data(SMSG_MESSAGECHAT, 200);
    BuildMonsterChat(&data, GetObjectGuid(), IsBossWhisper ? CHAT_MSG_RAID_BOSS_WHISPER : CHAT_MSG_MONSTER_WHISPER, text, LANG_UNIVERSAL,
        GetNameForLocaleIdx(loc_idx), target->GetObjectGuid(), "");

    ((Player*)target)->GetSession()->SendPacket(&data);
}

void WorldObject::BuildMonsterChat(WorldPacket *data, ObjectGuid senderGuid, uint8 msgtype, char const* text, uint32 language, char const* name, ObjectGuid targetGuid, char const* targetName)
{
    *data << uint8(msgtype);
    *data << uint32(language);
    *data << ObjectGuid(senderGuid);
    *data << uint32(0);                                     // 2.1.0
    *data << uint32(strlen(name)+1);
    *data << name;
    *data << ObjectGuid(targetGuid);                        // Unit Target
    if (targetGuid && !targetGuid.IsPlayer())
    {
        *data << uint32(strlen(targetName)+1);              // target name length
        *data << targetName;                                // target name
    }
    *data << uint32(strlen(text)+1);
    *data << text;
    *data << uint8(0);                                      // ChatTag
}

void WorldObject::SendMessageToSet(WorldPacket *data, bool /*bToSelf*/)
{
    //if object is in world, map for it already created!
    if (IsInWorld())
        GetMap()->MessageBroadcast(this, data);
}

void WorldObject::SendMessageToSetInRange(WorldPacket *data, float dist, bool /*bToSelf*/)
{
    //if object is in world, map for it already created!
    if (IsInWorld())
        GetMap()->MessageDistBroadcast(this, data, dist);
}

void WorldObject::SendMessageToSetExcept(WorldPacket *data, Player const* skipped_receiver)
{
    //if object is in world, map for it already created!
    if (IsInWorld())
    {
        MaNGOS::MessageDelivererExcept notifier(this, data, skipped_receiver);
        Cell::VisitWorldObjects(this, notifier, GetMap()->GetVisibilityDistance(this));
    }
}

void WorldObject::SendObjectDeSpawnAnim(ObjectGuid guid)
{
    WorldPacket data(SMSG_GAMEOBJECT_DESPAWN_ANIM, 8);
    data << ObjectGuid(guid);
    SendMessageToSet(&data, true);
}

void WorldObject::SendGameObjectCustomAnim(ObjectGuid guid, uint32 animId /*= 0*/)
{
    WorldPacket data(SMSG_GAMEOBJECT_CUSTOM_ANIM, 8+4);
    data << ObjectGuid(guid);
    data << uint32(animId);
    SendMessageToSet(&data, true);
}

void WorldObject::SetMap(Map * map)
{
    MANGOS_ASSERT(map);
    m_currMap = map;
    //lets save current map's Id/instanceId
    m_position.SetMapId(map->GetId());
    m_position.SetInstanceId(map->GetInstanceId());
}

TerrainInfo const* WorldObject::GetTerrain() const
{
    MANGOS_ASSERT(m_currMap);
    return m_currMap->GetTerrain();
}

void WorldObject::AddObjectToRemoveList()
{
    GetMap()->AddObjectToRemoveList(this);
}

void WorldObject::RemoveObjectFromRemoveList()
{
    GetMap()->RemoveObjectFromRemoveList(this);
}

Creature* WorldObject::SummonCreature(uint32 id, float x, float y, float z, float ang,TempSummonType spwtype,uint32 despwtime, bool asActiveObject)
{
    CreatureInfo const *cinfo = ObjectMgr::GetCreatureTemplate(id);
    if(!cinfo)
    {
        sLog.outErrorDb("WorldObject::SummonCreature: Creature (Entry: %u) not existed for summoner: %s. ", id, GetGuidStr().c_str());
        return NULL;
    }

    TemporarySummon* pCreature = new TemporarySummon(GetObjectGuid());

    Team team = TEAM_NONE;
    if (GetTypeId()==TYPEID_PLAYER)
        team = ((Player*)this)->GetTeam();

    CreatureCreatePos pos(GetMap(), x, y, z, ang, GetPhaseMask());

    if (fabs(x) < M_NULL_F && fabs(y) < M_NULL_F && fabs(z) < M_NULL_F)
        pos = CreatureCreatePos(this, GetOrientation(), CONTACT_DISTANCE, ang);

    if (!pCreature->Create(GetMap()->GenerateLocalLowGuid(cinfo->GetHighGuid()), pos, cinfo, team))
    {
        delete pCreature;
        return NULL;
    }

    pCreature->SetSummonPoint(pos);

    // Active state set before added to map
    pCreature->SetActiveObjectState(asActiveObject);

    pCreature->Summon(spwtype, despwtime);                  // Also initializes the AI and MMGen

    if (GetTypeId()==TYPEID_UNIT && ((Creature*)this)->AI())
        ((Creature*)this)->AI()->JustSummoned(pCreature);

    // Creature Linking, Initial load is handled like respawn
    if (pCreature->IsLinkingEventTrigger())
        GetMap()->GetCreatureLinkingHolder()->DoCreatureLinkingEvent(LINKING_EVENT_RESPAWN, pCreature);

    // return the creature therewith the summoner has access to it
    return pCreature;
}

GameObject* WorldObject::SummonGameobject(uint32 id, float x, float y, float z, float angle, uint32 despwtime)
{
    GameObject* pGameObj = new GameObject;

    Map *map = GetMap();

    if (!map)
        return NULL;

    if(!pGameObj->Create(map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT), id, map,
        GetPhaseMask(), x, y, z, angle))
    {
        delete pGameObj;
        return NULL;
    }

    pGameObj->SetRespawnTime(despwtime/IN_MILLISECONDS);

    map->Add(pGameObj);

    return pGameObj;
}

namespace MaNGOS
{
    class NearUsedPosDo
    {
        public:
            NearUsedPosDo(WorldObject const& obj, WorldObject const* searcher, float absAngle, ObjectPosSelector& selector)
                : i_object(obj), i_searcher(searcher), i_absAngle(MapManager::NormalizeOrientation(absAngle)), i_selector(selector) {}

            void operator()(Corpse*) const {}
            void operator()(DynamicObject*) const {}

            void operator()(Creature* c) const
            {
                // skip self or target
                if (c == i_searcher || c == &i_object)
                    return;

                float x, y, z;

                if (c->IsStopped() || !c->GetMotionMaster()->GetDestination(x, y, z))
                {
                    x = c->GetPositionX();
                    y = c->GetPositionY();
                }

                add(c,x,y);
            }

            template<class T>
            void operator()(T* u) const
            {
                // skip self or target
                if (u == i_searcher || u == &i_object)
                    return;

                float x,y;

                x = u->GetPositionX();
                y = u->GetPositionY();

                add(u,x,y);
            }

            // we must add used pos that can fill places around center
            void add(WorldObject* u, float x, float y) const
            {
                // dist include size of u and i_object
                float dx = i_object.GetPositionX() - x;
                float dy = i_object.GetPositionY() - y;
                float dist2d = sqrt((dx * dx) + (dy * dy));

                float delta = i_selector.m_searcherSize + u->GetObjectBoundingRadius();

                // u is too nearest/far away to i_object
                if (dist2d < i_selector.m_searcherDist - delta ||
                    dist2d >= i_selector.m_searcherDist + delta)
                    return;

                float angle = i_object.GetAngle(u) - i_absAngle;

                // move angle to range -pi ... +pi, range before is -2Pi..2Pi
                if (angle > M_PI_F)
                    angle -= 2.0f * M_PI_F;
                else if (angle < -M_PI_F)
                    angle += 2.0f * M_PI_F;

                i_selector.AddUsedArea(u->GetObjectBoundingRadius(), angle, dist2d);
            }
        private:
            WorldObject const& i_object;
            WorldObject const* i_searcher;
            float              i_absAngle;
            ObjectPosSelector& i_selector;
    };
}                                                           // namespace MaNGOS

//===================================================================================================

void WorldObject::GetNearPoint2D(float &x, float &y, float distance2d, float absAngle ) const
{
    x = GetPositionX() + (GetObjectBoundingRadius() + distance2d) * cos(absAngle);
    y = GetPositionY() + (GetObjectBoundingRadius() + distance2d) * sin(absAngle);

    MaNGOS::NormalizeMapCoord(x);
    MaNGOS::NormalizeMapCoord(y);
}

void WorldObject::GetNearPoint(WorldObject const* searcher, float &x, float &y, float &z, float searcher_bounding_radius, float distance2d, float absAngle) const
{
    GetNearPoint2D(x, y, distance2d + searcher_bounding_radius, absAngle);
    const float init_z = z = GetPositionZ();

    // if detection disabled, return first point
    if(!sWorld.getConfig(CONFIG_BOOL_DETECT_POS_COLLISION))
    {
        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);
        return;
    }

    // or remember first point
    float first_x = x;
    float first_y = y;
    bool first_los_conflict = false;                        // first point LOS problems

    const float dist = distance2d + searcher_bounding_radius + GetObjectBoundingRadius();

    // prepare selector for work
    ObjectPosSelector selector(GetPositionX(), GetPositionY(), dist, searcher_bounding_radius);

    // adding used positions around object
    {
        MaNGOS::NearUsedPosDo u_do(*this, searcher, absAngle, selector);
        MaNGOS::WorldObjectWorker<MaNGOS::NearUsedPosDo> worker(this, u_do);

        Cell::VisitAllObjects(this, worker, distance2d + searcher_bounding_radius);
    }

    // maybe can just place in primary position
    if (selector.CheckOriginalAngle())
    {
        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);

        if (fabs(init_z - z) < dist && IsWithinLOS(x, y, z))
            return;

        first_los_conflict = true;                          // first point have LOS problems
    }

    // set first used pos in lists
    selector.InitializeAngle();

    float angle;                                            // candidate of angle for free pos

    // select in positions after current nodes (selection one by one)
    while (selector.NextAngle(angle))                        // angle for free pos
    {
        GetNearPoint2D(x, y, distance2d + searcher_bounding_radius, absAngle + angle);
        z = GetPositionZ();

        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);

        if (fabs(init_z - z) < dist && IsWithinLOS(x, y, z))
            return;
    }

    // BAD NEWS: not free pos (or used or have LOS problems)
    // Attempt find _used_ pos without LOS problem
    if (!first_los_conflict)
    {
        x = first_x;
        y = first_y;

        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);
        return;
    }

    // set first used pos in lists
    selector.InitializeAngle();

    // select in positions after current nodes (selection one by one)
    while (selector.NextUsedAngle(angle))                   // angle for used pos but maybe without LOS problem
    {
        GetNearPoint2D(x, y, distance2d + searcher_bounding_radius, absAngle + angle);
        z = GetPositionZ();

        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);

        if (fabs(init_z - z) < dist && IsWithinLOS(x, y, z))
            return;
    }

    // BAD BAD NEWS: all found pos (free and used) have LOS problem :(
    x = first_x;
    y = first_y;

    if (searcher)
        searcher->UpdateAllowedPositionZ(x, y, z);          // update to LOS height if available
    else
        UpdateGroundPositionZ(x, y, z);
}

void WorldObject::SetPhaseMask(uint32 newPhaseMask, bool update)
{
    m_position.SetPhaseMask(newPhaseMask);

    if (update && IsInWorld())
        UpdateVisibilityAndView();
}

void WorldObject::PlayDistanceSound( uint32 sound_id, Player* target /*= NULL*/ )
{
    WorldPacket data(SMSG_PLAY_OBJECT_SOUND,4+8);
    data << uint32(sound_id);
    data << GetObjectGuid();
    if (target)
        target->SendDirectMessage( &data );
    else
        SendMessageToSet( &data, true );
}

void WorldObject::PlayDirectSound( uint32 sound_id, Player* target /*= NULL*/ )
{
    WorldPacket data(SMSG_PLAY_SOUND, 4);
    data << uint32(sound_id);
    if (target)
        target->SendDirectMessage( &data );
    else
        SendMessageToSet( &data, true );
}

GameObject* WorldObject::GetClosestGameObjectWithEntry(const WorldObject* pSource, uint32 uiEntry, float fMaxSearchRange)
{
    //return closest gameobject in grid, with range from pSource
    GameObject* pGameObject = NULL;

    CellPair p(MaNGOS::ComputeCellPair(pSource->GetPositionX(), pSource->GetPositionY()));
    Cell cell(p);
    cell.SetNoCreate();

    MaNGOS::NearestGameObjectEntryInObjectRangeCheck gobject_check(*pSource, uiEntry, fMaxSearchRange);
    MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck> searcher(pGameObject, gobject_check);

    TypeContainerVisitor<MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck>, GridTypeMapContainer> grid_gobject_searcher(searcher);

    cell.Visit(p, grid_gobject_searcher,*(pSource->GetMap()), *this, fMaxSearchRange);

    return pGameObject;
}

void WorldObject::GetGameObjectListWithEntryInGrid(std::list<GameObject*>& lList, uint32 uiEntry, float fMaxSearchRange)
{
    CellPair pair(MaNGOS::ComputeCellPair(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    MaNGOS::AllGameObjectsWithEntryInRange check(this, uiEntry, fMaxSearchRange);
    MaNGOS::GameObjectListSearcher<MaNGOS::AllGameObjectsWithEntryInRange> searcher(lList, check);
    TypeContainerVisitor<MaNGOS::GameObjectListSearcher<MaNGOS::AllGameObjectsWithEntryInRange>, GridTypeMapContainer> visitor(searcher);

    GetMap()->Visit(cell, visitor);
}

void WorldObject::UpdateVisibilityAndView()
{
    GetViewPoint().Call_UpdateVisibilityForOwner();
    UpdateObjectVisibility();
    GetViewPoint().Event_ViewPointVisibilityChanged();
}

Creature* WorldObject::GetClosestCreatureWithEntry(WorldObject* pSource, uint32 uiEntry, float fMaxSearchRange)
{
   Creature *p_Creature = NULL;
   CellPair p(MaNGOS::ComputeCellPair(pSource->GetPositionX(), pSource->GetPositionY()));

   Cell cell(p);
   cell.SetNoCreate();
   MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck u_check(*pSource,uiEntry,true,false,fMaxSearchRange);
   MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(p_Creature, u_check);
   TypeContainerVisitor<MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck>, GridTypeMapContainer >  grid_creature_searcher(searcher);
   cell.Visit(p, grid_creature_searcher, *pSource->GetMap(), *this, fMaxSearchRange);
   return p_Creature;
}

void WorldObject::UpdateObjectVisibility()
{
    CellPair p = MaNGOS::ComputeCellPair(GetPositionX(), GetPositionY());
    Cell cell(p);

    GetMap()->UpdateObjectVisibility(this, cell, p);
}

void WorldObject::AddToClientUpdateList()
{
    GetMap()->AddUpdateObject(GetObjectGuid());
}

void WorldObject::RemoveFromClientUpdateList()
{
    GetMap()->RemoveUpdateObject(GetObjectGuid());
}

struct WorldObjectChangeAccumulator
{
    UpdateDataMapType &i_updateDatas;
    WorldObject &i_object;
    WorldObjectChangeAccumulator(WorldObject &obj, UpdateDataMapType &d) : i_updateDatas(d), i_object(obj)
    {
        // send self fields changes in another way, otherwise
        // with new camera system when player's camera too far from player, camera wouldn't receive packets and changes from player
        if (i_object.isType(TYPEMASK_PLAYER))
            i_object.BuildUpdateDataForPlayer((Player*)&i_object, i_updateDatas);
    }

    void Visit(CameraMapType &m)
    {
        for(CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
        {
            Player* owner = iter->getSource()->GetOwner();
            if (owner && owner != &i_object && owner->HaveAtClient(&i_object))
                i_object.BuildUpdateDataForPlayer(owner, i_updateDatas);
        }
    }

    template<class SKIP> void Visit(GridRefManager<SKIP> &) {}
};

void WorldObject::BuildUpdateData( UpdateDataMapType & update_players)
{
    WorldObjectChangeAccumulator notifier(*this, update_players);
    Cell::VisitWorldObjects(this, notifier, GetMap()->GetVisibilityDistance(this));

    ClearUpdateMask(false);
}

bool WorldObject::IsControlledByPlayer() const
{
    switch (GetTypeId())
    {
        case TYPEID_GAMEOBJECT:
            return ((GameObject*)this)->GetOwnerGuid().IsPlayer();
        case TYPEID_UNIT:
        case TYPEID_PLAYER:
            return ((Unit*)this)->IsCharmerOrOwnerPlayerOrPlayerItself();
        case TYPEID_DYNAMICOBJECT:
            return ((DynamicObject*)this)->GetCasterGuid().IsPlayer();
        case TYPEID_CORPSE:
            return true;
        default:
            return false;
    }
}

void WorldObject::StartGroupLoot( Group* group, uint32 timer )
{
    m_groupLootId = group->GetId();
    m_groupLootTimer = timer;
}

void WorldObject::StopGroupLoot()
{
    if (!m_groupLootId)
        return;

    Group* group = sObjectMgr.GetGroupById(m_groupLootId);
    if (group)
        group->EndRoll();

    m_groupLootTimer = 0;
    m_groupLootId = 0;
}

/**
 * Return original player who tap creature, it can be different from player/group allowed to loot so not use it for loot code
 */
Player* WorldObject::GetOriginalLootRecipient() const
{
    return !m_lootRecipientGuid.IsEmpty() ? ObjectAccessor::FindPlayer(m_lootRecipientGuid) : NULL;
}

/**
 * Return group if player tap creature as group member, independent is player after leave group or stil be group member
 */
Group* WorldObject::GetGroupLootRecipient() const
{
    // original recipient group if set and not disbanded
    return m_lootGroupRecipientId ? sObjectMgr.GetGroupById(m_lootGroupRecipientId) : NULL;
}

/**
 * Return player who can loot tapped creature (member of group or single player)
 *
 * In case when original player tap creature as group member then group tap prefered.
 * This is for example important if player after tap leave group.
 * If group not exist or disbanded or player tap creature not as group member return player
 */
Player* WorldObject::GetLootRecipient() const
{
    // original recipient group if set and not disbanded
    Group* group = GetGroupLootRecipient();

    // original recipient player if online
    Player* player = GetOriginalLootRecipient();

    // if group not set or disbanded return original recipient player if any
    if (!group)
        return player;

    // group case

    // return player if it still be in original recipient group
    if (player && player->GetGroup() == group)
        return player;

    // find any in group
    for(GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
        if (Player *p = itr->getSource())
            return p;

    return NULL;
}

/**
 * Set player and group (if player group member) who tap creature
 */
void WorldObject::SetLootRecipient(Unit *unit)
{
    // set the player whose group should receive the right
    // to loot the creature after it dies
    // should be set to NULL after the loot disappears

    if (!unit)
    {
        m_lootRecipientGuid.Clear();
        m_lootGroupRecipientId = 0;
        return;
    }

    Player* player = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
    if(!player)                                             // normal creature, no player involved
        return;

    // set player for non group case or if group will disbanded
    m_lootRecipientGuid = player->GetObjectGuid();

    // set group for group existed case including if player will leave group at loot time
    if (Group* group = player->GetGroup())
        m_lootGroupRecipientId = group->GetId();

}

// Frozen Mod
void Object::ForceValuesUpdateAtIndex(uint16 index)
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError(index, true));

    m_changedValues[index] = true; // makes server think the field changed

    MarkForClientUpdate();
}
// Frozen Mod

bool WorldObject::PrintCoordinatesError(float x, float y, float z, char const* descr) const
{
    sLog.outError("%s with invalid %s coordinates: mapid = %uu, x = %f, y = %f, z = %f", GetGuidStr().c_str(), descr, GetMapId(), x, y, z);
    return false;                                           // always false for continue assert fail
}

void WorldObject::SetActiveObjectState(bool active)
{
    if (m_isActiveObject == active || (isType(TYPEMASK_PLAYER) && !active))  // player shouldn't became inactive, never
        return;

    if (IsInWorld() && !isType(TYPEMASK_PLAYER))
        // player's update implemented in a different from other active worldobject's way
        // it's considired to use generic way in future
    {
        if (isActiveObject() && !active)
            GetMap()->RemoveFromActive(this);
        else if (!isActiveObject() && active)
            GetMap()->AddToActive(this);
    }
    m_isActiveObject = active;
}

void WorldObject::UpdateWorldState(uint32 state, uint32 value)
{
    if (GetMap())
        sWorldStateMgr.SetWorldStateValueFor(GetMap(), state, value);
}

uint32 WorldObject::GetWorldState(uint32 stateId)
{
    return sWorldStateMgr.GetWorldStateValueFor(this, stateId);
}

WorldObjectEventProcessor* WorldObject::GetEvents()
{
    return &m_Events;
}

void WorldObject::KillAllEvents(bool force)
{
    MAPLOCK_WRITE(this, MAP_LOCK_TYPE_DEFAULT);
    GetEvents()->KillAllEvents(force);
}

void WorldObject::AddEvent(BasicEvent* Event, uint64 e_time, bool set_addtime)
{
    MAPLOCK_WRITE(this, MAP_LOCK_TYPE_DEFAULT);
    if (set_addtime)
        GetEvents()->AddEvent(Event, GetEvents()->CalculateTime(e_time), set_addtime);
    else
        GetEvents()->AddEvent(Event, e_time, set_addtime);
}

void WorldObject::UpdateEvents(uint32 update_diff, uint32 time)
{
    {
        MAPLOCK_READ(this, MAP_LOCK_TYPE_DEFAULT);
        GetEvents()->RenewEvents();
    }

    GetEvents()->Update(update_diff);
}
