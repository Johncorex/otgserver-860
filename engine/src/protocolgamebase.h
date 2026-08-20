/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019  Mark Samman <mark.samman@gmail.com>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef FS_PROTOCOLGAMEBASE_H_A28CB86652D0AF6760E43DCD9ACED40D
#define FS_PROTOCOLGAMEBASE_H_A28CB86652D0AF6760E43DCD9ACED40D

#include "protocol.h"
#include "chat.h"
#include "creature.h"
#include "tasks.h"
#include "outfit.h"

class NetworkMessage;
class Player;
class Game;
class House;
class Container;
class Tile;
class Connection;
class Quest;
class Shop;
class StoreOffers;
class StoreOffer;
class ModalWindow;
class ProtocolGame;

using ProtocolGame_ptr = std::shared_ptr<ProtocolGame>;

extern Game g_game;

/** \brief 8.6 protocol backend. Contains the methods and member variables common
 *         to both the game and spectator protocols.
 */
class ProtocolGameBase : public Protocol {
	public:
		// static protocol information
		enum {server_sends_first = true};
		enum {protocol_identifier = 0}; // Not required as we send first
		enum {use_checksum = true};
		static const char* protocol_name() {
			return "gameworld protocol";
		}

		explicit ProtocolGameBase(Connection_ptr connection) : Protocol(connection) {}

		uint16_t getVersion() const {
			return version;
		}

		// stats.h macros route addGameTask(...) -> addGameTaskWithStats(...) and addGameTaskTimed(...) -> addGameTaskTimedWithStats(...); those templates are declared below.

		// stats.h macros route addGameTask(...) -> addGameTaskWithStats(...) and
		// addGameTaskTimed(...) -> addGameTaskTimedWithStats(...); the base .cpp undefines those
		// macros and re-defines them to wrap the callable in std::function (non-dependent context).
		void addGameTaskWithStats(std::function<void()> task, const std::string& function_str, const std::string& extra_info) {
			g_dispatcher.addTask(createTaskWithStats(task, function_str, extra_info));
		}

		void addGameTaskTimedWithStats(uint32_t delay, std::function<void()> task, const std::string& function_str, const std::string& extra_info) {
			g_dispatcher.addTask(createTaskWithStats(delay, task, function_str, extra_info));
		}

		ProtocolGame_ptr getThis();

		void login(const std::string& name, uint32_t accountId, OperatingSystem_t operatingSystem);
		void logout(bool displayEffect, bool forced);
		void connect(uint32_t playerId, OperatingSystem_t operatingSystem);

		// serialization helpers (8.6)
		void AddItem(NetworkMessage& msg, const Item* item);
		void AddItem(NetworkMessage& msg, uint16_t id, uint8_t count);

		void checkCreatureAsKnown(uint32_t id, bool& known, uint32_t& removedKnown);
		void AddCreature(NetworkMessage& msg, const Creature* creature, bool known, uint32_t remove);
		void AddPlayerStats(NetworkMessage& msg);
		void AddPlayerSkills(NetworkMessage& msg);
		void AddOutfit(NetworkMessage& msg, const Outfit_t& outfit);

		void AddWorldLight(NetworkMessage& msg, LightInfo lightInfo);
		void AddCreatureLight(NetworkMessage& msg, const Creature* creature);

		void GetTileDescription(const Tile* tile, NetworkMessage& msg);
		void GetFloorDescription(NetworkMessage& msg, int32_t x, int32_t y, int32_t z,
		                         int32_t width, int32_t height, int32_t offset, int32_t& skip);
		void GetMapDescription(int32_t x, int32_t y, int32_t z,
		                       int32_t width, int32_t height, NetworkMessage& msg);
		void MoveUpCreature(NetworkMessage& msg, const Creature* creature, const Position& newPos, const Position& oldPos);
		void MoveDownCreature(NetworkMessage& msg, const Creature* creature, const Position& newPos, const Position& oldPos);

		static void RemoveTileThing(NetworkMessage& msg, const Position& pos, uint32_t stackpos);
		static void RemoveTileCreature(NetworkMessage& msg, const Creature* creature, const Position& pos, uint32_t stackpos);

		// Send functions (8.6)
		void sendUpdateTile(const Tile* tile, const Position& pos);
		void sendContainer(uint8_t cid, const Container* container, bool hasParent, uint16_t firstIndex);
		void sendChannel(uint16_t channelId, const std::string& channelName);
		void sendChannel(uint16_t channelId, const std::string& channelName, const UsersMap* channelUsers, const InvitedMap* invitedUsers);
		void sendAddCreature(const Creature* creature, const Position& pos, int32_t stackpos, bool isLogin);
		void sendMagicEffect(const Position& pos, uint8_t type);
		void sendStats();
		void sendInventoryItem(slots_t slot, const Item* item);
		void sendSkills();
		void sendCreatureLight(const Creature* creature);
		void sendWorldLight(LightInfo lightInfo);
		void sendMapDescription(const Position& pos);
		void sendVIP(uint32_t guid, const std::string& name, const std::string& description, uint32_t icon, bool notify, VipStatus_t status);
		void sendVIP(uint32_t guid, const std::string& name, VipStatus_t status);
		void sendCancelWalk();
		void sendPing();
		void sendPingBack();

		void sendChannelMessage(const std::string& author, const std::string& text, SpeakClasses type, uint16_t channel);
		void sendClosePrivate(uint16_t channelId);
		void sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName);
		void sendChannelsDialog();
		void sendOpenPrivateChannel(const std::string& receiver);
		void sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId);
		void sendPrivateMessage(const Player* speaker, SpeakClasses type, const std::string& text);
		void sendChannelEvent(uint16_t channelId, const std::string& playerName, ChannelEvent_t channelEvent);
		void sendIcons(uint16_t icons);
		void sendFYIBox(const std::string& message);

		void sendDistanceShoot(const Position& from, const Position& to, uint8_t type);
		void sendCreatureHealth(const Creature* creature);
		void sendCreatureTurn(const Creature* creature, uint32_t stackpos);
		void sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text, const Position* pos = nullptr);

		void sendQuestLog();
		void sendQuestLine(const Quest* quest);

		void sendCancelTarget();
		void sendChangeSpeed(const Creature* creature, uint32_t speed);
		void sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit);
		void sendTextMessage(const TextMessage& message);
		void sendReLoginWindow(uint8_t unfairFightReduction);

		void sendTutorial(uint8_t tutorialId);
		void sendAddMarker(const Position& pos, uint8_t markType, const std::string& desc);

		void sendCreatureWalkthrough(const Creature* creature, bool walkthrough);
		void sendCreatureShield(const Creature* creature);
		void sendCreatureSkull(const Creature* creature);
		void sendCreatureSquare(const Creature* creature, SquareColor_t color);
		void sendCreatureSquare(const Creature* creature, SquareColor_t color, uint8_t length) {
			sendCreatureSquare(creature, color);
		}

		void sendShop(Npc* npc, const ShopInfoList& itemList);
		void sendCloseShop();
		void sendSaleItemList(const std::list<ShopInfo>& shop);
		void sendTradeItemRequest(const std::string& traderName, const Item* item, bool ack);
		void sendCloseTrade();

		void sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxlen, bool canWrite);
		void sendTextWindow(uint32_t windowTextId, uint32_t itemId, const std::string& text);
		void sendHouseWindow(uint32_t windowTextId, const std::string& text);
		void sendOutfitWindow();

		void sendUpdatedVIPStatus(uint32_t guid, VipStatus_t newStatus);

		void sendFightModes();

		void sendAnimatedText(const std::string& message, const Position& pos, TextColor_t color);

		void sendAddTileItem(const Position& pos, uint32_t stackpos, const Item* item);
		void sendUpdateTileItem(const Position& pos, uint32_t stackpos, const Item* item);
		void sendRemoveTileThing(const Position& pos, uint32_t stackpos);
		void sendUpdateTileCreature(const Position& pos, uint32_t stackpos, const Creature* creature);
		void sendRemoveTileCreature(const Creature* creature, const Position& pos, uint32_t stackpos);

		void sendMoveCreature(const Creature* creature, const Position& newPos, int32_t newStackPos,
		                      const Position& oldPos, int32_t oldStackPos, bool teleport);

		void sendAddContainerItem(uint8_t cid, const Item* item);
		void sendUpdateContainerItem(uint8_t cid, uint16_t slot, const Item* item);
		void sendRemoveContainerItem(uint8_t cid, uint16_t slot);

		void sendCloseContainer(uint8_t cid);

		void AddShopItem(NetworkMessage& msg, const ShopInfo& item);

		void parseExtendedOpcode(NetworkMessage& msg);

		bool canSee(int32_t x, int32_t y, int32_t z) const;
		bool canSee(const Creature*) const;
		bool canSee(const Position& pos) const;

		void disconnectClient(const std::string& message) const;
		void release() override;
		void onRecvFirstMessage(NetworkMessage& msg) override;
		void parsePacket(NetworkMessage& msg) override;

		virtual void writeToOutputBuffer(const NetworkMessage& msg, bool broadcast = true);

		void onConnect() final;

		Player* player = nullptr;
		bool loggedIn = false;
		bool shouldAddExivaRestrictions = false;

		uint32_t eventConnect = 0;
		uint32_t challengeTimestamp = 0;
		uint16_t version = CLIENT_VERSION_MIN;
		uint32_t clientVersion = 0;
		bool supportsExtendedMagicEffects = false;

		uint8_t challengeRandom = 0;

		bool debugAssertSent = false;
		bool acceptPackets = false;

		std::unordered_set<uint32_t> knownCreatureSet;

		// ---- 8.6 packet parsers (private) ----
		private:
			void parseChannelInvite(NetworkMessage& msg);
			void parseChannelExclude(NetworkMessage& msg);
			void parseOpenChannel(NetworkMessage& msg);
			void parseCloseChannel(NetworkMessage& msg);
			void parseOpenPrivateChannel(NetworkMessage& msg);
			void parseAutoWalk(NetworkMessage& msg);
			void parseSetOutfit(NetworkMessage& msg);
			void parseUseItem(NetworkMessage& msg);
			void parseUseItemEx(NetworkMessage& msg);
			void parseUseWithCreature(NetworkMessage& msg);
			void parseCloseContainer(NetworkMessage& msg);
			void parseUpArrowContainer(NetworkMessage& msg);
			void parseUpdateContainer(NetworkMessage& msg);
			void parseThrow(NetworkMessage& msg);
			void parseLookAt(NetworkMessage& msg);
			void parseLookInBattleList(NetworkMessage& msg);
			void parseSay(NetworkMessage& msg);
			void parseFightModes(NetworkMessage& msg);
			void parseAttack(NetworkMessage& msg);
			void parseFollow(NetworkMessage& msg);
			void parseTextWindow(NetworkMessage& msg);
			void parseHouseWindow(NetworkMessage& msg);
			void parseLookInShop(NetworkMessage& msg);
			void parsePlayerPurchase(NetworkMessage& msg);
			void parsePlayerSale(NetworkMessage& msg);
			void parseRequestTrade(NetworkMessage& msg);
			void parseLookInTrade(NetworkMessage& msg);
			void parseAddVip(NetworkMessage& msg);
			void parseRemoveVip(NetworkMessage& msg);
			void parseRotateItem(NetworkMessage& msg);
			void parseRuleViolationReport(NetworkMessage& msg);
			void parseBugReport(NetworkMessage& msg);
			void parseDebugAssert(NetworkMessage& msg);
			void parseInviteToParty(NetworkMessage& msg);
			void parseJoinParty(NetworkMessage& msg);
			void parseRevokePartyInvite(NetworkMessage& msg);
			void parsePassPartyLeadership(NetworkMessage& msg);
			void parseEnableSharedPartyExperience(NetworkMessage& msg);
			void parseQuestLine(NetworkMessage& msg);

		public:
		// ---- 11.x-only sends kept as no-op stubs for 8.6 (called by Player/Game) ----
		void sendPremiumTrigger() {}
		void sendBlessStatus() {}
		void sendStoreHighlight() {}
		void sendBasicData() {}
		void sendPendingStateEntered() {}
		void sendEnterWorld() {}
		void sendInventoryClientIds() {}
		void sendItemsPrice() {}
		void sendPreyData(uint8_t) {}
		void sendRerollPrice(uint32_t) {}
		void sendFreeListRerollAvailability(uint8_t, uint16_t) {}
		void sendPreyTimeLeft(uint8_t, uint16_t) {}
		void sendMessageDialog(MessageDialog_t, const std::string&) {}
		void sendPvpSituations() {}
		void sendLootContainers() {}
		void sendLootStats(Item*) {}
		void sendTibiaTime(int32_t) {}
		void sendShowStoreOffers(StoreOffers*) {}
		void sendShowStoreOffers10(StoreOffers*) {}
		void sendShowStoreOffers11(StoreOffers*) {}
		void sendOfferDescription(uint32_t, std::string) {}
		void sendStoreHome() {}
		void sendStoreError(uint8_t, std::string) {}
		void sendStorePurchaseSuccessful(const std::string&, const uint32_t) {}
		void openStore() {}
		void sendInventory() {}
		void sendMapManage(uint8_t) {}
		void sendSpellCooldown(uint8_t, uint32_t) {}
		void sendSpellGroupCooldown(SpellGroup_t, uint32_t) {}
		void sendUpdatePartyInfo(uint32_t, uint8_t) {}
};

#endif
