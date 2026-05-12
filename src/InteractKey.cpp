/*
 * Interact Key module for AzerothCore (3.3.5a)
 *
 * Adds a `.interact` chat command that finds the nearest interactable target
 * around the player and triggers the appropriate interaction (gossip, vendor,
 * loot, quest, gather, etc.) as if they had right-clicked it. Designed to be
 * bound to a key via an in-game macro for retail-style interact-on-keypress.
 */

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Unit.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace InteractKey
{
    // Range-only checks. The stock AllCreaturesOfEntryInRange / AllGameObjectsWithEntryInRange
    // do a strict GetEntry() == m_uiEntry comparison, so entry 0 matches nothing. We need to
    // see every nearby creature/gameobject regardless of entry, so we provide our own checks.
    class AnyCreatureInRangeCheck
    {
    public:
        AnyCreatureInRangeCheck(WorldObject const* obj, float range) : _obj(obj), _range(range) { }
        bool operator()(Creature* c) const
        {
            return c && _obj->IsWithinDist(c, _range, false);
        }
    private:
        WorldObject const* _obj;
        float _range;
    };

    class AnyGameObjectInRangeCheck
    {
    public:
        AnyGameObjectInRangeCheck(WorldObject const* obj, float range) : _obj(obj), _range(range) { }
        bool operator()(GameObject* go) const
        {
            return go && _obj->IsWithinDist(go, _range, false);
        }
    private:
        WorldObject const* _obj;
        float _range;
    };

    enum class TargetKind : uint8
    {
        None,
        LootableCorpse,
        QuestGiver,
        ServiceNpc,        // Vendor, Trainer, Banker, FlightMaster, Innkeeper, Auctioneer, Repair
        GossipNpc,
        UsableGameObject,
        HostileCreature
    };

    // Lower priority value = preferred when targets tie on distance bucket
    uint8 PriorityFor(TargetKind kind)
    {
        switch (kind)
        {
            case TargetKind::LootableCorpse:   return 1;
            case TargetKind::QuestGiver:       return 2;
            case TargetKind::ServiceNpc:       return 3;
            case TargetKind::GossipNpc:        return 4;
            case TargetKind::UsableGameObject: return 5;
            case TargetKind::HostileCreature:  return 6;
            default:                           return 255;
        }
    }

    struct Candidate
    {
        WorldObject* object = nullptr;
        TargetKind kind = TargetKind::None;
        float distance = 0.0f;
        bool preferred = false; // current target gets a flag to win ties
    };

    bool IsEnabled()
    {
        return sConfigMgr->GetOption<bool>("InteractKey.Enable", true);
    }

    float GetRange()
    {
        return std::clamp(sConfigMgr->GetOption<float>("InteractKey.Range", 5.5f), 1.0f, 30.0f);
    }

    float GetConeRadians()
    {
        float const degrees = sConfigMgr->GetOption<float>("InteractKey.ConeDegrees", 180.0f);
        if (degrees <= 0.0f || degrees >= 360.0f)
            return 0.0f; // 0 means "any direction"
        return float(degrees * M_PI / 180.0);
    }

    uint32 GetCooldownMs()
    {
        return std::min<uint32>(5000, sConfigMgr->GetOption<uint32>("InteractKey.CooldownMs", 250));
    }

    bool ShouldTargetHostile()
    {
        return sConfigMgr->GetOption<bool>("InteractKey.TargetHostile", false);
    }

    bool ShouldAutoAttackHostile()
    {
        return sConfigMgr->GetOption<bool>("InteractKey.AutoAttackHostile", true);
    }

    bool ShouldPreferCurrentTarget()
    {
        return sConfigMgr->GetOption<bool>("InteractKey.PreferCurrentTarget", true);
    }

    bool ShouldPrintFail()
    {
        return sConfigMgr->GetOption<bool>("InteractKey.PrintFailMessage", true);
    }

    // ---- per-player cooldown tracking (cleared on logout) ----
    std::unordered_map<ObjectGuid, uint32> g_lastInteractMs;
    std::mutex g_lastInteractMutex;

    bool CheckAndStampCooldown(Player* player)
    {
        uint32 const cd = GetCooldownMs();
        if (cd == 0)
            return true;

        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> lock(g_lastInteractMutex);
        auto it = g_lastInteractMs.find(player->GetGUID());
        if (it != g_lastInteractMs.end() && getMSTimeDiff(it->second, now) < cd)
            return false;
        g_lastInteractMs[player->GetGUID()] = now;
        return true;
    }

    void ForgetPlayer(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_lastInteractMutex);
        g_lastInteractMs.erase(guid);
    }

    // ---- classification ----
    bool IsCorpseLootableBy(Creature* creature, Player* player)
    {
        if (!creature->isDead())
            return false;
        if (creature->loot.isLooted())
            return false;
        // Tappable check: the creature is tapped by this player or their group
        return creature->isTappedBy(player) || (creature->GetLootRecipientGroup() && player->GetGroup() == creature->GetLootRecipientGroup());
    }

    bool IsInteractableGameObjectType(GameObject* go)
    {
        switch (go->GetGoType())
        {
            case GAMEOBJECT_TYPE_DOOR:
            case GAMEOBJECT_TYPE_BUTTON:
            case GAMEOBJECT_TYPE_QUESTGIVER:
            case GAMEOBJECT_TYPE_CHEST:
            case GAMEOBJECT_TYPE_BINDER:
            case GAMEOBJECT_TYPE_CHAIR:
            case GAMEOBJECT_TYPE_GOOBER:
            case GAMEOBJECT_TYPE_SPELLCASTER:
            case GAMEOBJECT_TYPE_MEETINGSTONE:
            case GAMEOBJECT_TYPE_MAILBOX:
            case GAMEOBJECT_TYPE_GUARDPOST:
            case GAMEOBJECT_TYPE_BARBER_CHAIR:
            case GAMEOBJECT_TYPE_GUILD_BANK:
            case GAMEOBJECT_TYPE_FISHINGHOLE:
                return true;
            default:
                return false;
        }
    }

    TargetKind ClassifyCreature(Creature* creature, Player* player)
    {
        if (!creature || !creature->IsInWorld())
            return TargetKind::None;

        if (creature->IsTotem() || creature->IsTrigger())
            return TargetKind::None;
        if (creature->IsPet() && creature->GetOwnerGUID() != player->GetGUID())
            return TargetKind::None;

        // Dead -> only relevant if lootable
        if (creature->isDead())
        {
            if (IsCorpseLootableBy(creature, player))
                return TargetKind::LootableCorpse;
            return TargetKind::None;
        }

        // Alive: decide friendly-functional vs hostile
        bool const friendly = creature->IsFriendlyTo(player);
        uint32 const npcFlags = creature->GetUInt32Value(UNIT_NPC_FLAGS);

        if (friendly && npcFlags != UNIT_NPC_FLAG_NONE)
        {
            if (npcFlags & UNIT_NPC_FLAG_QUESTGIVER)
                return TargetKind::QuestGiver;

            uint32 const serviceMask =
                UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_BANKER |
                UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_AUCTIONEER |
                UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_STABLEMASTER | UNIT_NPC_FLAG_BATTLEMASTER |
                UNIT_NPC_FLAG_SPIRITHEALER | UNIT_NPC_FLAG_SPIRITGUIDE | UNIT_NPC_FLAG_TABARDDESIGNER |
                UNIT_NPC_FLAG_PETITIONER;
            if (npcFlags & serviceMask)
                return TargetKind::ServiceNpc;

            if (npcFlags & UNIT_NPC_FLAG_GOSSIP)
                return TargetKind::GossipNpc;
        }

        // Hostile, attackable
        if (!friendly && player->IsValidAttackTarget(creature))
            return TargetKind::HostileCreature;

        return TargetKind::None;
    }

    // ---- search ----
    void CollectCreatureCandidates(Player* player, float range, float coneRad, std::vector<Candidate>& out)
    {
        std::list<Creature*> creatures;
        AnyCreatureInRangeCheck check(player, range);
        Acore::CreatureListSearcher<AnyCreatureInRangeCheck> searcher(player, creatures, check);
        Cell::VisitObjects(player, searcher, range);
        for (Creature* creature : creatures)
        {
            if (!creature || !player->IsWithinDistInMap(creature, range))
                continue;
            if (coneRad > 0.0f && !player->HasInArc(coneRad, creature))
                continue;

            TargetKind const kind = ClassifyCreature(creature, player);
            if (kind == TargetKind::None)
                continue;
            if (kind == TargetKind::HostileCreature && !ShouldTargetHostile())
                continue;

            Candidate c;
            c.object = creature;
            c.kind = kind;
            c.distance = player->GetDistance(creature);
            out.push_back(c);
        }
    }

    void CollectGameObjectCandidates(Player* player, float range, float coneRad, std::vector<Candidate>& out)
    {
        std::list<GameObject*> gobs;
        AnyGameObjectInRangeCheck check(player, range);
        Acore::GameObjectListSearcher<AnyGameObjectInRangeCheck> searcher(player, gobs, check);
        Cell::VisitObjects(player, searcher, range);
        for (GameObject* go : gobs)
        {
            if (!go || !go->IsInWorld())
                continue;
            if (!player->IsWithinDistInMap(go, range))
                continue;
            if (coneRad > 0.0f && !player->HasInArc(coneRad, go))
                continue;
            if (!IsInteractableGameObjectType(go))
                continue;

            Candidate c;
            c.object = go;
            c.kind = TargetKind::UsableGameObject;
            c.distance = player->GetDistance(go);
            out.push_back(c);
        }
    }

    Candidate PickBest(std::vector<Candidate>& candidates)
    {
        Candidate best;
        best.kind = TargetKind::None;

        for (Candidate const& c : candidates)
        {
            if (best.kind == TargetKind::None)
            {
                best = c;
                continue;
            }

            // Preferred (current target) always wins if it's a valid interact
            if (c.preferred && !best.preferred)
            {
                best = c;
                continue;
            }
            if (best.preferred && !c.preferred)
                continue;

            uint8 const pBest = PriorityFor(best.kind);
            uint8 const pCand = PriorityFor(c.kind);
            if (pCand < pBest)
            {
                best = c;
                continue;
            }
            if (pCand == pBest && c.distance < best.distance)
                best = c;
        }
        return best;
    }

    // ---- perform ----
    void OpenCreatureGossip(Player* player, Creature* creature)
    {
        if (sScriptMgr->OnGossipHello(player, creature))
            return;
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
        player->SendPreparedGossip(creature);
        creature->AI()->sGossipHello(player);
    }

    void PerformInteract(Player* player, Candidate const& best)
    {
        if (!best.object)
            return;

        player->SetSelection(best.object->GetGUID());

        switch (best.kind)
        {
            case TargetKind::LootableCorpse:
            {
                if (Creature* c = best.object->ToCreature())
                    player->SendLoot(c->GetGUID(), LOOT_CORPSE);
                break;
            }
            case TargetKind::QuestGiver:
            case TargetKind::ServiceNpc:
            case TargetKind::GossipNpc:
            {
                if (Creature* c = best.object->ToCreature())
                {
                    // Stop the creature briefly so the window stays open (matches right-click flow)
                    c->PauseMovementForInteraction();
                    OpenCreatureGossip(player, c);
                }
                break;
            }
            case TargetKind::UsableGameObject:
            {
                if (GameObject* go = best.object->ToGameObject())
                    go->Use(player);
                break;
            }
            case TargetKind::HostileCreature:
            {
                if (Creature* c = best.object->ToCreature())
                {
                    if (ShouldAutoAttackHostile() && player->IsAlive())
                        player->Attack(c, true);
                }
                break;
            }
            default:
                break;
        }
    }

    char const* KindName(TargetKind kind)
    {
        switch (kind)
        {
            case TargetKind::LootableCorpse:   return "loot";
            case TargetKind::QuestGiver:       return "quest giver";
            case TargetKind::ServiceNpc:       return "service NPC";
            case TargetKind::GossipNpc:        return "NPC";
            case TargetKind::UsableGameObject: return "object";
            case TargetKind::HostileCreature:  return "hostile";
            default:                           return "?";
        }
    }

    // ---- command entry point ----
    bool HandleInteractCommand(ChatHandler* handler)
    {
        if (!IsEnabled())
        {
            handler->PSendSysMessage("The interact command is disabled on this server.");
            return true;
        }

        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return true;
        if (player->IsBeingTeleported() || player->IsInFlight())
            return true;
        if (!player->IsAlive())
        {
            // Dead players can still loot their corpse-runback target, but not much else.
            // Let the search proceed; if nothing interactable is found, the fail message handles it.
        }

        if (!CheckAndStampCooldown(player))
            return true;

        float const range = GetRange();
        float const coneRad = GetConeRadians();

        std::vector<Candidate> candidates;
        candidates.reserve(16);

        // Current target preference (bypasses cone check so you can interact with a tab-target behind you)
        if (ShouldPreferCurrentTarget())
        {
            if (ObjectGuid const targetGuid = player->GetTarget())
            {
                if (Unit* target = ObjectAccessor::GetUnit(*player, targetGuid))
                {
                    if (Creature* creature = target->ToCreature())
                    {
                        if (player->IsWithinDistInMap(creature, range))
                        {
                            TargetKind const kind = ClassifyCreature(creature, player);
                            if (kind != TargetKind::None)
                            {
                                // For current target, allow hostile even if config disables auto-pick
                                Candidate c;
                                c.object = creature;
                                c.kind = kind;
                                c.distance = player->GetDistance(creature);
                                c.preferred = true;
                                candidates.push_back(c);
                            }
                        }
                    }
                }
            }
        }

        CollectCreatureCandidates(player, range, coneRad, candidates);
        CollectGameObjectCandidates(player, range, coneRad, candidates);

        Candidate best = PickBest(candidates);
        if (best.kind == TargetKind::None)
        {
            if (ShouldPrintFail())
                handler->PSendSysMessage("Nothing to interact with within {:.1f} yards.", range);
            return true;
        }

        PerformInteract(player, best);

        if (sConfigMgr->GetOption<bool>("InteractKey.AnnounceTarget", false))
        {
            handler->PSendSysMessage("Interacting with {}: {} ({:.1f}y)",
                KindName(best.kind),
                best.object->GetName(),
                best.distance);
        }
        return true;
    }
}

// ---- script registration ----
class interact_key_commandscript : public CommandScript
{
public:
    interact_key_commandscript() : CommandScript("interact_key_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "interact", InteractKey::HandleInteractCommand, SEC_PLAYER, Console::No }
        };
        return table;
    }
};

class interact_key_playerscript : public PlayerScript
{
public:
    interact_key_playerscript() : PlayerScript("interact_key_playerscript", { PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            InteractKey::ForgetPlayer(player->GetGUID());
    }
};

void AddSC_mod_interact_key()
{
    new interact_key_commandscript();
    new interact_key_playerscript();
}
