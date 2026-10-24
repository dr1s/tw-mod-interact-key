/*
 * tw-mod-interact-key
 *
 * Tortoise-WoW port of AzerothCore mod-interact-key.
 * Provides a server-side `.interact` command that finds the best nearby
 * interactable target and triggers the appropriate interaction.
 */

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "ScriptObjects.h"
#include "Config/Config.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Unit.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace InteractKey
{
    // 3D range checks. The stock AllCreaturesInRange / AllGameObjectsInRange use
    // 2D distance, which can miss targets on slopes/stairs. We need 3D.
    class AnyCreatureInRangeCheck
    {
    public:
        AnyCreatureInRangeCheck(WorldObject const* obj, float range) : _obj(obj), _range(range) { }
        bool operator()(Creature* c) const
        {
            return c && _obj->IsWithinDistInMap(c, _range);
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
            return go && _obj->IsWithinDistInMap(go, _range);
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

    enum Strings
    {
        INTERACT_KEY_DISABLED       = 1,
        INTERACT_KEY_NOTHING_NEARBY = 2,
        INTERACT_KEY_ANNOUNCE       = 3,
    };

    char const* ModuleName = "tw-mod-interact-key";

    // ---- configuration ----
    bool   g_enabled = true;
    float  g_range = 5.5f;
    float  g_coneDegrees = 180.0f;
    uint32 g_cooldownMs = 250;
    bool   g_preferCurrentTarget = true;
    bool   g_targetHostile = false;
    bool   g_autoAttackHostile = true;
    bool   g_printFailMessage = true;
    bool   g_announceTarget = false;
    bool   g_debug = false;
    bool   g_serverSideAutoLoot = false;

    void DebugLog(const char* fmt, ...)
    {
        if (!g_debug)
            return;

        va_list ap;
        va_start(ap, fmt);
        char buf[1024];
        vsnprintf(buf, 1024, fmt, ap);
        va_end(ap);

        sLog.outString("[tw-mod-interact-key] %s", buf);
    }

    void LoadConfig()
    {
        g_enabled            = sConfig.GetBoolDefault("InteractKey.Enable", true);
        g_range              = std::clamp(sConfig.GetFloatDefault("InteractKey.Range", 5.5f), 1.0f, 30.0f);
        g_coneDegrees        = sConfig.GetFloatDefault("InteractKey.ConeDegrees", 180.0f);
        g_cooldownMs         = std::min<uint32>(5000u, static_cast<uint32>(sConfig.GetIntDefault("InteractKey.CooldownMs", 250)));
        g_preferCurrentTarget = sConfig.GetBoolDefault("InteractKey.PreferCurrentTarget", true);
        g_targetHostile      = sConfig.GetBoolDefault("InteractKey.TargetHostile", false);
        g_autoAttackHostile  = sConfig.GetBoolDefault("InteractKey.AutoAttackHostile", true);
        g_printFailMessage   = sConfig.GetBoolDefault("InteractKey.PrintFailMessage", true);
        g_announceTarget     = sConfig.GetBoolDefault("InteractKey.AnnounceTarget", false);
        g_debug              = sConfig.GetBoolDefault("InteractKey.Debug", false);
        g_serverSideAutoLoot = sConfig.GetBoolDefault("InteractKey.ServerSideAutoLoot", false);
    }

    bool IsEnabled()             { return g_enabled; }
    float GetRange()             { return g_range; }

    float GetConeRadians()
    {
        float const degrees = g_coneDegrees;
        if (degrees <= 0.0f || degrees >= 360.0f)
            return 0.0f; // 0 means "any direction"
        return float(degrees * M_PI_F / 180.0);
    }

    uint32 GetCooldownMs()       { return g_cooldownMs; }
    bool ShouldTargetHostile()   { return g_targetHostile; }
    bool ShouldAutoAttackHostile() { return g_autoAttackHostile; }
    bool ShouldPreferCurrentTarget() { return g_preferCurrentTarget; }
    bool ShouldPrintFail()       { return g_printFailMessage; }
    bool ShouldAnnounceTarget()  { return g_announceTarget; }

    // ---- per-player cooldown tracking (cleared on logout) ----
    std::unordered_map<uint64, uint32> g_lastInteractMs;
    std::mutex g_lastInteractMutex;

    bool CheckAndStampCooldown(Player* player)
    {
        uint32 const cd = GetCooldownMs();
        if (cd == 0)
            return true;

        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> lock(g_lastInteractMutex);
        auto it = g_lastInteractMs.find(player->GetGUID());
        if (it != g_lastInteractMs.end() && WorldTimer::getMSTimeDiff(it->second, now) < cd)
            return false;
        g_lastInteractMs[player->GetGUID()] = now;
        return true;
    }

    void ForgetPlayer(uint64 guid)
    {
        std::lock_guard<std::mutex> lock(g_lastInteractMutex);
        g_lastInteractMs.erase(guid);
    }

    // ---- classification ----
    bool IsCorpseLootableBy(Creature* creature, Player* player)
    {
        if (!creature->IsDead())
        {
            DebugLog("IsCorpseLootableBy: %s (entry %u) not dead", creature->GetName(), creature->GetEntry());
            return false;
        }
        if (creature->loot.isLooted())
        {
            DebugLog("IsCorpseLootableBy: %s (entry %u) loot already taken", creature->GetName(), creature->GetEntry());
            return false;
        }
        // Tappable check: the creature is tapped by this player or their group
        bool tapped = creature->IsTappedBy(player);
        DebugLog("IsCorpseLootableBy: %s (entry %u) IsTappedBy=%s", creature->GetName(), creature->GetEntry(), tapped ? "true" : "false");
        return tapped;
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
            // NOTE: MAILBOX, BARBER_CHAIR and GUILD_BANK are excluded.
            // Mailbox/barber/guild-bank UIs are driven by the client;
            // there are no server opcodes to open them.
            case GAMEOBJECT_TYPE_GUARDPOST:
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
        {
            DebugLog("ClassifyCreature: %s (entry %u) skipped (totem/trigger)", creature->GetName(), creature->GetEntry());
            return TargetKind::None;
        }
        if (creature->IsPet() && creature->GetOwnerGuid() != player->GetObjectGuid())
        {
            DebugLog("ClassifyCreature: %s (entry %u) skipped (other player's pet)", creature->GetName(), creature->GetEntry());
            return TargetKind::None;
        }

        // Dead -> only relevant if lootable
        if (creature->IsDead())
        {
            if (IsCorpseLootableBy(creature, player))
            {
                DebugLog("ClassifyCreature: %s (entry %u) -> LootableCorpse", creature->GetName(), creature->GetEntry());
                return TargetKind::LootableCorpse;
            }
            DebugLog("ClassifyCreature: %s (entry %u) dead but not lootable", creature->GetName(), creature->GetEntry());
            return TargetKind::None;
        }

        // Alive: decide friendly-functional vs hostile
        bool const friendly = creature->IsFriendlyTo(player);
        uint32 const npcFlags = creature->GetUInt32Value(UNIT_NPC_FLAGS);

        if (friendly && npcFlags != UNIT_NPC_FLAG_NONE)
        {
            if (npcFlags & UNIT_NPC_FLAG_QUESTGIVER)
            {
                DebugLog("ClassifyCreature: %s (entry %u) -> QuestGiver", creature->GetName(), creature->GetEntry());
                return TargetKind::QuestGiver;
            }

            uint32 const serviceMask =
                UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_BANKER |
                UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_AUCTIONEER |
                UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_STABLEMASTER | UNIT_NPC_FLAG_BATTLEMASTER |
                UNIT_NPC_FLAG_SPIRITHEALER | UNIT_NPC_FLAG_SPIRITGUIDE | UNIT_NPC_FLAG_TABARDDESIGNER |
                UNIT_NPC_FLAG_PETITIONER;
            if (npcFlags & serviceMask)
            {
                DebugLog("ClassifyCreature: %s (entry %u) -> ServiceNpc", creature->GetName(), creature->GetEntry());
                return TargetKind::ServiceNpc;
            }

            if (npcFlags & UNIT_NPC_FLAG_GOSSIP)
            {
                DebugLog("ClassifyCreature: %s (entry %u) -> GossipNpc", creature->GetName(), creature->GetEntry());
                return TargetKind::GossipNpc;
            }
        }

        // Hostile, attackable
        if (!friendly && player->IsValidAttackTarget(creature))
        {
            DebugLog("ClassifyCreature: %s (entry %u) -> HostileCreature", creature->GetName(), creature->GetEntry());
            return TargetKind::HostileCreature;
        }

        DebugLog("ClassifyCreature: %s (entry %u) not classified (friendly=%s, validAttack=%s, npcFlags=%u)",
            creature->GetName(), creature->GetEntry(),
            friendly ? "true" : "false",
            (!friendly && player->IsValidAttackTarget(creature)) ? "true" : "false",
            npcFlags);
        return TargetKind::None;
    }

    // ---- search ----
    void CollectCreatureCandidates(Player* player, float range, float coneRad, std::vector<Candidate>& out)
    {
        std::list<Creature*> creatures;
        AnyCreatureInRangeCheck check(player, range);
        MaNGOS::CreatureListSearcher<AnyCreatureInRangeCheck> searcher(creatures, check);
        Cell::VisitGridObjects(player, searcher, range);
        for (Creature* creature : creatures)
        {
            if (!creature)
                continue;
            if (coneRad > 0.0f && !player->HasInArc(creature, coneRad))
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
        MaNGOS::GameObjectListSearcher<AnyGameObjectInRangeCheck> searcher(gobs, check);
        Cell::VisitGridObjects(player, searcher, range);
        for (GameObject* go : gobs)
        {
            if (!go || !go->IsInWorld())
                continue;
            if (coneRad > 0.0f && !player->HasInArc(go, coneRad))
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
    void ServerSideLootCorpse(Player* player, Creature* creature)
    {
        if (!creature || creature->IsAlive())
            return;
        if (!player->IsWithinDistInMap(creature, player->GetMaxLootDistance(creature), true, SizeFactor::None))
            return;
        if (!creature->IsTappedBy(player))
            return;

        DebugLog("ServerSideLootCorpse: looting %s (entry %u) server-side", creature->GetName(), creature->GetEntry());

        // Set up the loot context exactly as a manual click would.
        player->SetLootGuid(creature->GetObjectGuid());
        player->SendLoot(creature->GetObjectGuid(), LOOT_CORPSE);

        // Take money.
        WorldPacket moneyPacket(CMSG_LOOT_MONEY, 0);
        player->GetSession()->HandleLootMoneyOpcode(moneyPacket);

        // Take items one slot at a time until nothing remains for this player.
        Loot& loot = creature->loot;
        uint32 maxSlot = loot.GetMaxSlotInLootFor(player->GetGUIDLow());
        for (uint32 slot = 0; slot < maxSlot; ++slot)
        {
            // Skip already-looted slots and slots that have nothing for this player.
            if (!loot.LootItemInSlot(slot, player->GetGUIDLow()))
                continue;

            WorldPacket itemPacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            itemPacket << uint8(slot);
            player->GetSession()->HandleAutostoreLootItemOpcode(itemPacket);
        }

        // Release the loot and clean up the corpse.
        player->GetSession()->DoLootRelease(creature->GetObjectGuid());
    }

    void OpenCreatureGossip(Player* player, Creature* creature)
    {
        if (sScriptMgr.OnGossipHello(player, creature))
            return;
        player->PrepareGossipMenu(creature, creature->GetDefaultGossipMenuId());
        player->SendPreparedGossip(creature);
    }

    void PerformInteract(Player* player, Candidate const& best)
    {
        if (!best.object)
            return;

        player->SetSelectionGuid(best.object->GetObjectGuid());

        switch (best.kind)
        {
            case TargetKind::LootableCorpse:
            {
                if (Creature* c = best.object->ToCreature())
                {
                    if (g_serverSideAutoLoot)
                    {
                        ServerSideLootCorpse(player, c);
                    }
                    else
                    {
                        DebugLog("PerformInteract: asking client to loot %s (entry %u, guid %s)",
                            c->GetName(), c->GetEntry(), c->GetObjectGuid().GetString().c_str());

                        // Vanilla 1.12 ignores server-initiated SMSG_LOOT_RESPONSE.
                        // Send the target GUID to the client addon so it can send CMSG_LOOT.
                        // Format must match UnitGUID(): "0xHHHHHHHHLLLLLLLL" (16 hex digits).
                        std::ostringstream oss;
                        oss << "0x" << std::hex << std::uppercase << std::setw(16) << std::setfill('0')
                            << c->GetObjectGuid().GetRawValue();
                        player->SendAddonMessage("TW_INTERACT", std::string("LOOT:") + oss.str() + ":" + c->GetName());
                    }
                }
                break;
            }
            case TargetKind::QuestGiver:
            case TargetKind::ServiceNpc:
            case TargetKind::GossipNpc:
            {
                if (Creature* c = best.object->ToCreature())
                    OpenCreatureGossip(player, c);
                break;
            }
            case TargetKind::UsableGameObject:
            {
                if (GameObject* go = best.object->ToGameObject())
                {
                    // Chests need direct loot sending; GameObject::Use() does not
                    // open the loot window for GAMEOBJECT_TYPE_CHEST.
                    if (go->GetGoType() == GAMEOBJECT_TYPE_CHEST)
                    {
                        go->Use(player); // trigger scripts / linked objects first
                        player->SendLoot(go->GetObjectGuid(), LOOT_CORPSE);
                    }
                    else
                    {
                        go->Use(player);
                    }
                }
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
    bool HandleInteractCommand(ChatHandler* handler, char* /*args*/)
    {
        if (!IsEnabled())
        {
            handler->PSendSysMessage(sObjectMgr.GetModuleString(ModuleName, INTERACT_KEY_DISABLED, handler->GetSession()->GetSessionDbLocaleIndex()));
            return true;
        }

        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return true;
        if (player->IsBeingTeleported() || player->IsTaxiFlying())
            return true;

        if (!CheckAndStampCooldown(player))
            return true;

        float const range = GetRange();
        float const coneRad = GetConeRadians();

        std::vector<Candidate> candidates;
        candidates.reserve(16);

        // Current target preference (bypasses cone check so you can interact with a tab-target behind you)
        if (ShouldPreferCurrentTarget())
        {
            ObjectGuid const targetGuid = player->GetSelectionGuid();
            if (!targetGuid.IsEmpty())
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
                else if (GameObject* go = player->GetMap()->GetGameObject(targetGuid))
                {
                    if (go->IsInWorld() && player->IsWithinDistInMap(go, range) && IsInteractableGameObjectType(go))
                    {
                        Candidate c;
                        c.object = go;
                        c.kind = TargetKind::UsableGameObject;
                        c.distance = player->GetDistance(go);
                        c.preferred = true;
                        candidates.push_back(c);
                    }
                }
            }
        }

        CollectCreatureCandidates(player, range, coneRad, candidates);
        CollectGameObjectCandidates(player, range, coneRad, candidates);

        DebugLog("HandleInteractCommand: collected %zu candidates", candidates.size());

        Candidate best = PickBest(candidates);
        if (best.kind == TargetKind::None)
        {
            DebugLog("HandleInteractCommand: no suitable target found");
            if (ShouldPrintFail())
                handler->PSendSysMessage(sObjectMgr.GetModuleString(ModuleName, INTERACT_KEY_NOTHING_NEARBY, handler->GetSession()->GetSessionDbLocaleIndex()), range);
            return true;
        }

        DebugLog("HandleInteractCommand: picked target %s (kind=%s, dist=%.1f)",
            best.object->GetName(), KindName(best.kind), best.distance);

        PerformInteract(player, best);

        if (ShouldAnnounceTarget())
        {
            handler->PSendSysMessage(sObjectMgr.GetModuleString(ModuleName, INTERACT_KEY_ANNOUNCE, handler->GetSession()->GetSessionDbLocaleIndex()),
                KindName(best.kind),
                best.object->GetName(),
                best.distance);
        }
        return true;
    }
}

// ---- script registration ----
class InteractKeyWorldScript : public WorldScript
{
public:
    InteractKeyWorldScript()
        : WorldScript("InteractKeyWorld", { WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_AFTER_CONFIG_LOAD })
    {
    }

    void OnBeforeWorldInitialized() override
    {
        sLog.outString("[tw-mod-interact-key] module loaded.");
        InteractKey::LoadConfig();
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        InteractKey::LoadConfig();
    }
};

class interact_key_commandscript : public CommandScript
{
public:
    interact_key_commandscript() : CommandScript("interact_key_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> table =
        {
            { "interact", SEC_PLAYER, false, nullptr, "", nullptr, 0, "", 0, InteractKey::HandleInteractCommand }
        };
        return table;
    }
};

class interact_key_playerscript : public PlayerScript
{
public:
    interact_key_playerscript() : PlayerScript("interact_key_playerscript", { PLAYERHOOK_ON_LOGOUT }) { }

    void OnLogout(Player* player) override
    {
        if (player)
            InteractKey::ForgetPlayer(player->GetGUID());
    }
};

void AddSC_mod_interact_key()
{
    new InteractKeyWorldScript();
    new interact_key_commandscript();
    new interact_key_playerscript();
}
