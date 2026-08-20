/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
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

#include "otpch.h"

#include "protocolgame.h"

#include "outputmessage.h"

#include "player.h"
#include "databasetasks.h"
#include "configmanager.h"
#include "actions.h"
#include "game.h"
#include "iologindata.h"
#include "iomarket.h"
#include "waitlist.h"
#include "ban.h"
#include "scheduler.h"
#include "modules.h"

#include "store.h"

extern Game g_game;
extern ConfigManager g_config;
extern Actions actions;
extern CreatureEvents* g_creatureEvents;
extern Chat* g_chat;
extern Modules* g_modules;
extern Monsters g_monsters;
extern Prey g_prey;
extern Store g_store;

ProtocolGame::LiveCastsMap ProtocolGame::liveCasts;





bool ProtocolGame::startLiveCast(const std::string& password /*= ""*/)
{
	auto connection = getConnection();
	if (!g_config.getBoolean(ConfigManager::ENABLE_LIVE_CASTING) || isLiveCaster() || !player || player->isRemoved() || !connection || liveCasts.size() >= getMaxLiveCastCount()) {
		return false;
	}

	{
		std::lock_guard<decltype(liveCastLock)> lock {liveCastLock};
		//DO NOT do any send operations here
		liveCastName = player->getName();
		liveCastPassword = password;
		isCaster.store(true, std::memory_order_relaxed);
	}

	liveCasts.insert(std::make_pair(player, getThis()));

	registerLiveCast();
	//Send a "dummy" channel
	sendChannel(CHANNEL_CAST, LIVE_CAST_CHAT_NAME, nullptr, nullptr);
	return true;
}

bool ProtocolGame::stopLiveCast()
{
	//dispatcher
	if (!isLiveCaster()) {
		return false;
	}

	CastSpectatorVec spectators;

	{
		std::lock_guard<decltype(liveCastLock)> lock {liveCastLock};
		//DO NOT do any send operations here
		std::swap(this->spectators, spectators);
		isCaster.store(false, std::memory_order_relaxed);
	}

	liveCasts.erase(player);
	for (auto& spectator : spectators) {
		spectator->onLiveCastStop();
	}
	unregisterLiveCast();

	return true;
}

void ProtocolGame::clearLiveCastInfo()
{
	static std::once_flag flag;
	std::call_once(flag, []() {
			assert(g_game.getGameState() == GAME_STATE_INIT);
			std::ostringstream query;
			query << "TRUNCATE TABLE `live_casts`;";
			g_databaseTasks.addTask(query.str());
		});
}

void ProtocolGame::registerLiveCast()
{
	std::ostringstream query;
	query << "INSERT into `live_casts` (`player_id`, `cast_name`, `password`, `version`) VALUES (" << player->getGUID() << ", '"
		<< getLiveCastName() << "', " << isPasswordProtected() << ", " << player->getProtocolVersion() << ");";
	g_databaseTasks.addTask(query.str());
}

void ProtocolGame::unregisterLiveCast()
{
	std::ostringstream query;
	query << "DELETE FROM `live_casts` WHERE `player_id`=" << player->getGUID() << ";";
	g_databaseTasks.addTask(query.str());
}

void ProtocolGame::updateLiveCastInfo()
{
	std::ostringstream query;
	query << "UPDATE `live_casts` SET `cast_name`='" << getLiveCastName() << "', `password`="
		<< isPasswordProtected() << ", `spectators`=" << getSpectatorCount()
		<< ", `version` = " << player->getProtocolVersion() << " WHERE `player_id`=" << player->getGUID() << ";";
	g_databaseTasks.addTask(query.str());
}

void ProtocolGame::addSpectator(ProtocolSpectator_ptr spectatorClient)
{
	std::lock_guard<decltype(liveCastLock)> lock(liveCastLock);
	//DO NOT do any send operations here
	spectators.emplace_back(spectatorClient);
	updateLiveCastInfo();
	
	if (player) {
		std::string message = "view" + std::to_string(spectators.size()) + " joined!";
		sendChannelMessage("", message, TALKTYPE_CHANNEL_O, CHANNEL_CAST);
	}
}

void ProtocolGame::removeSpectator(ProtocolSpectator_ptr spectatorClient)
{
	std::lock_guard<decltype(liveCastLock)> lock(liveCastLock);
	//DO NOT do any send operations here
	auto it = std::find(spectators.begin(), spectators.end(), spectatorClient);
	if (it != spectators.end()) {
		spectators.erase(it);
		updateLiveCastInfo();
		
		if (player) {
			std::string message = "view left! Total: " + std::to_string(spectators.size());
			sendChannelMessage("", message, TALKTYPE_CHANNEL_O, CHANNEL_CAST);
		}
	}
}





// Parse methods







void ProtocolGame::parseToggleMount(NetworkMessage& msg)
{
	bool mount = msg.getByte() != 0;
	addGameTask(&Game::playerToggleMount, player->getID(), mount);
}










void ProtocolGame::parseQuickLoot(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	addGameTask(&Game::playerQuickLoot, player->getID(), pos, spriteId, stackpos, nullptr);
}

void ProtocolGame::parseLootContainer(NetworkMessage& msg)
{
	uint8_t action = msg.getByte();
	if (action == 0) {
		ObjectCategory_t category = (ObjectCategory_t)msg.getByte();
		Position pos = msg.getPosition();
		uint16_t spriteId = msg.get<uint16_t>();
		uint8_t stackpos = msg.getByte();
		addGameTask(&Game::playerSetLootContainer, player->getID(), category, pos, spriteId, stackpos);
	} else if (action == 1) {
		ObjectCategory_t category = (ObjectCategory_t)msg.getByte();
		addGameTask(&Game::playerClearLootContainer, player->getID(), category);
	} else if (action == 3) {
		bool useMainAsFallback = msg.getByte() == 1;
		addGameTask(&Game::playerSetQuickLootFallback, player->getID(), useMainAsFallback);
	}

//	sendLootContainers();
}

void ProtocolGame::parseQuickLootBlackWhitelist(NetworkMessage& msg)
{
	QuickLootFilter_t filter = (QuickLootFilter_t)msg.getByte();
	std::vector<uint16_t> listedItems;

	uint16_t size = msg.get<uint16_t>();
	listedItems.reserve(size);

	for (int i = 0; i < size; i++) {
		listedItems.push_back(msg.get<uint16_t>());
	}

	addGameTask(&Game::playerQuickLootBlackWhitelist, player->getID(), filter, listedItems);
}

void ProtocolGame::parseResquestLockItems()
{
	addGameTask(&Game::playerRequestLockFind, player->getID());
}






void ProtocolGame::parseEquipObject(NetworkMessage& msg)
{
	uint16_t spriteId = msg.get<uint16_t>();
	// msg.get<uint8_t>();

	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerEquipItem, player->getID(), spriteId);
}









void ProtocolGame::parseEditVip(NetworkMessage& msg)
{
	uint32_t guid = msg.get<uint32_t>();
	const std::string description = msg.getString();
	uint32_t icon = std::min<uint32_t>(10, msg.get<uint32_t>()); // 10 is max icon in 9.63
	bool notify = msg.getByte() != 0;
	addGameTask(&Game::playerRequestEditVip, player->getID(), guid, description, icon, notify);
}


void ProtocolGame::parseWrapItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerWrapItem, player->getID(), pos, stackpos, spriteId);
}


void ProtocolGame::parseNPCSay(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	addGameTask(&Game::playerNPCSay, player->getID(), creatureId);
}

void ProtocolGame::parseThankYou(NetworkMessage& msg)
{
	uint32_t a_statementId = msg.get<uint32_t>();

	addGameTask(&Game::playerSendThankYou, player->getID(), a_statementId);
}









void ProtocolGame::parseMarketLeave()
{
	addGameTask(&Game::playerLeaveMarket, player->getID());
}

void ProtocolGame::parseMarketBrowse(NetworkMessage& msg)
{
	uint16_t browseId = msg.get<uint16_t>();

	if (browseId == MARKETREQUEST_OWN_OFFERS) {
		addGameTask(&Game::playerBrowseMarketOwnOffers, player->getID());
	} else if (browseId == MARKETREQUEST_OWN_HISTORY) {
		addGameTask(&Game::playerBrowseMarketOwnHistory, player->getID());
	} else {
		addGameTask(&Game::playerBrowseMarket, player->getID(), browseId);
	}
}

void ProtocolGame::parseTransferCoins(NetworkMessage& msg) {
	std::string recipient = msg.getString();
	uint16_t amount = msg.get<uint16_t>();

	addGameTask(&Game::playerTransferCoins, player->getID(), recipient, amount);
}

void ProtocolGame::parseMarketCreateOffer(NetworkMessage& msg)
{
	uint8_t type = msg.getByte();
	uint16_t spriteId = msg.get<uint16_t>();
	uint16_t amount = msg.get<uint16_t>();
	uint32_t price = msg.get<uint32_t>();
	bool anonymous = (msg.getByte() != 0);
	if (amount > 0 && price > 0) {
		addGameTask(&Game::playerCreateMarketOffer, player->getID(), type, spriteId, amount, price, anonymous);
	}
}

void ProtocolGame::parseMarketCancelOffer(NetworkMessage& msg)
{
	uint32_t timestamp = msg.get<uint32_t>();
	uint16_t counter = msg.get<uint16_t>();
	if (counter > 0) {
		addGameTask(&Game::playerCancelMarketOffer, player->getID(), timestamp, counter);
	}

	updateCoinBalance();
}

void ProtocolGame::parseMarketAcceptOffer(NetworkMessage& msg)
{
	uint32_t timestamp = msg.get<uint32_t>();
	uint16_t counter = msg.get<uint16_t>();
	uint16_t amount = msg.get<uint16_t>();
	if (amount > 0 && counter > 0) {
		addGameTask(&Game::playerAcceptMarketOffer, player->getID(), timestamp, counter, amount);
	}

	updateCoinBalance();
}

void ProtocolGame::parseModalWindowAnswer(NetworkMessage& msg)
{
	uint32_t id = msg.get<uint32_t>();
	uint8_t button = msg.getByte();
	uint8_t choice = msg.getByte();
	addGameTask(&Game::playerAnswerModalWindow, player->getID(), id, button, choice);
}

void ProtocolGame::parseOpenStore()
{
	addGameTask(&Game::playerOpenStore, player->getID(), true, nullptr);
}

void ProtocolGame::parseRequestStoreOffers(NetworkMessage& msg)
{
	uint8_t actionType = msg.getByte();
	if (actionType == 0 && version >= 1150) {
		player->sendStoreHome();
		return;		
	}

	StoreOffers* offers = nullptr;
	if (version <= 1100 ) {
		std::string categoryName = msg.getString();
		offers = g_store.getOfferByName(categoryName);
	} else if (actionType == 0) {
		offers = g_store.getOfferByName(g_config.getString(ConfigManager::DEFAULT_OFFER));
	} else if (actionType == 2) {
		std::string categoryName = msg.getString();
		offers = g_store.getOfferByName(categoryName);
	} else if (actionType == 4) {
		uint32_t id = msg.get<uint32_t>();
		offers = g_store.getOffersByOfferId(id);
	} else {
		// std::cout << "teste 3" << std::endl;
		// std::string categoryName = msg.getString();
		// offers = g_store.getOfferByName(categoryName);
	}

	if (offers != nullptr) {
		addGameTask(&Game::playerOpenStore, player->getID(), false, offers);
	} else if (version >= 1150) {
		addGameTask(&Game::playerOpenStore, player->getID(), false, nullptr);
	}
}

void ProtocolGame::parseBuyStoreOffer(NetworkMessage& msg)
{
	uint32_t id = msg.get<uint32_t>();
	OfferBuyTypes_t productType = static_cast<OfferBuyTypes_t>(msg.getByte());
	std::string param;

	StoreOffer* offer = g_store.getOfferById(id);
	if (offer == nullptr) {
		return;
	}

	if (offer->getOfferType() == OFFER_TYPE_NAMECHANGE && productType != OFFER_BUY_TYPE_NAMECHANGE) {
		requestPurchaseData(id, OFFER_BUY_TYPE_NAMECHANGE);
		return;
	}

	if (offer->getOfferType() == OFFER_TYPE_NAMECHANGE) {
		param = msg.getString();
	}

	addGameTask(&Game::playerBuyStoreOffer, player->getID(), *offer, std::move(param));
}

void ProtocolGame::parseSendDescription(NetworkMessage& msg)
{
	uint32_t offerId = msg.get<uint32_t>();
	StoreOffer* storeOffer = g_store.getOfferById(offerId);
	if (storeOffer == nullptr) {
		return;
	}
	player->sendOfferDescription(offerId, storeOffer->getDescription(player));
}

void ProtocolGame::parseOpenTransactionHistory(NetworkMessage& msg)
{
	uint8_t entryPages = msg.getByte();
	player->setEntriesPerPage(entryPages);
	addGameTask(&Game::playerStoreTransactionHistory, player->getID(), 1, entryPages);
}
void ProtocolGame::parseRequestTransactionHistory(NetworkMessage& msg)
{
	uint32_t pages = msg.get<uint32_t>();
	addGameTask(&Game::playerStoreTransactionHistory, player->getID(), pages + 1, player->getEntriesPerPage());
}

void ProtocolGame::parseBrowseField(NetworkMessage& msg)
{
	const Position& pos = msg.getPosition();
	addGameTask(&Game::playerBrowseField, player->getID(), pos);
}

void ProtocolGame::parseSeekInContainer(NetworkMessage& msg)
{
	uint8_t containerId = msg.getByte();
	uint16_t index = msg.get<uint16_t>();
	addGameTask(&Game::playerSeekInContainer, player->getID(), containerId, index);
}

// Prey System
void ProtocolGame::parseRequestResourceData(NetworkMessage& msg) 
{
	ResourceType_t resourceType = static_cast<ResourceType_t>(msg.getByte());
	addGameTask(&Game::playerRequestResourceData, player->getID(), resourceType);
}

void ProtocolGame::parsePreyAction(NetworkMessage& msg)
{
	uint8_t preySlotId = msg.getByte();
	PreyAction_t preyAction = static_cast<PreyAction_t>(msg.getByte());
	uint8_t monsterIndex = 0;
	uint16_t raceId = 0;
	if (preyAction == PREY_ACTION_MONSTERSELECTION) {
		monsterIndex = msg.getByte();
	} else if (preyAction == NEW_BONUS_SELECTIONWILDCARD) {
		raceId = msg.get<uint16_t>();
	}

	addGameTask(&Game::playerPreyAction, player->getID(), preySlotId, preyAction, monsterIndex, raceId);
}

void ProtocolGame::sendResourceData(ResourceType_t resourceType, int64_t amount) 
{
	return;

	NetworkMessage msg;
	msg.addByte(0xEE);
	msg.addByte(resourceType);
	msg.add<int64_t>(amount);
	writeToOutputBuffer(msg);
}

void ProtocolGame::parseRequestItemDetail(NetworkMessage& msg)
{
	uint8_t type = msg.getByte(); //
	if (type == 0x03){
		uint16_t itemid = msg.get<uint16_t>();

		player->sendItemDetail(itemid);
	}
	
}

// Send methods

void ProtocolGame::sendChannelEvent(uint16_t channelId, const std::string& playerName, ChannelEvent_t channelEvent)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xF3);
	msg.add<uint16_t>(channelId);
	msg.addString(playerName);
	msg.addByte(channelEvent);
	writeToOutputBuffer(msg);
}





void ProtocolGame::sendCreatureType(const Creature* creature, uint8_t creatureType)
{
	return;

	NetworkMessage msg;
	msg.addByte(0x95);
	msg.add<uint32_t>(creature->getID());
	if (version >= 1120) {
		if (creatureType == CREATURETYPE_SUMMON_OTHERS) {
			creatureType = CREATURETYPE_SUMMON_OWN;
		}
		msg.addByte(creatureType);
		if (creatureType == CREATURETYPE_SUMMON_OWN) {
			const Creature* master = creature->getMaster();
			if (master) {
				msg.add<uint32_t>(master->getID());
			} else {
				msg.add<uint32_t>(0);
			}
		}
	} else {
		msg.addByte(creatureType);
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureHelpers(uint32_t creatureId, uint16_t helpers)
{
	return;

	if (version >= 1185) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x94);
	msg.add<uint32_t>(creatureId);
	msg.add<uint16_t>(helpers);
	writeToOutputBuffer(msg);
}




void ProtocolGame::sendMapManage(uint8_t action)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xDD);

	if (action == 9) {	
		msg.addByte(action);
	
		msg.add<uint64_t>(10000000);
		msg.addByte(20);
	
		msg.add<uint16_t>(4);
		msg.addByte(0);
		msg.add<uint64_t>(8188);
	
		msg.add<uint16_t>(27);
		msg.addByte(0);
		msg.add<uint64_t>(14100);
	
		msg.add<uint16_t>(16);
		msg.addByte(0);
		msg.add<uint64_t>(5001);
	
		msg.add<uint16_t>(0x5);
		msg.addByte(0);
		msg.add<uint64_t>(623);
	
		msg.add<uint16_t>(20);
		msg.addByte(0);
		msg.add<uint64_t>(10011);
	
		msg.add<uint16_t>(0x9);
		msg.addByte(0);
		msg.add<uint64_t>(6);
	
		msg.add<uint16_t>(0x3);
		msg.addByte(0);
		msg.add<uint64_t>(2456);
	
		msg.add<uint16_t>(22);
		msg.addByte(0);
		msg.add<uint64_t>(257900);
	
		msg.add<uint16_t>(11);
		msg.addByte(0);
		msg.add<uint64_t>(20708);
	
		msg.add<uint16_t>(19);
		msg.addByte(0);
		msg.add<uint64_t>(85808);
	
		msg.add<uint16_t>(17);
		msg.addByte(0);
		msg.add<uint64_t>(112008);
	
		msg.add<uint16_t>(0x7);
		msg.addByte(0);
		msg.add<uint64_t>(112712);
	
		msg.add<uint16_t>(23);
		msg.addByte(0);
		msg.add<uint64_t>(6680);
	
		msg.add<uint16_t>(0x1);
		msg.addByte(0);
		msg.add<uint64_t>(14);
	
		msg.add<uint16_t>(24);
		msg.addByte(0);
		msg.add<uint64_t>(430027);
	
		msg.add<uint16_t>(0x2);
		msg.addByte(0);
		msg.add<uint64_t>(180);
	
		msg.add<uint16_t>(25);
		msg.addByte(0);
		msg.add<uint64_t>(11662);
	
		msg.add<uint16_t>(355);
		msg.addByte(0);
		msg.add<uint64_t>(0);
	
		msg.add<uint16_t>(14);
		msg.addByte(0);
		msg.add<uint64_t>(192);
	
		msg.add<uint16_t>(0x8);
		msg.addByte(0);
		msg.add<uint64_t>(271101);
	} else {
		return;
	}
	writeToOutputBuffer(msg);
}








void ProtocolGame::sendUnjustifiedPoints(const uint8_t& dayProgress, const uint8_t& dayLeft, const uint8_t& weekProgress, const uint8_t& weekLeft, const uint8_t& monthProgress, const uint8_t& monthLeft, const uint8_t& skullDuration)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xB7);
	msg.addByte(dayProgress);
	msg.addByte(dayLeft);
	msg.addByte(weekProgress);
	msg.addByte(weekLeft);
	msg.addByte(monthProgress);
	msg.addByte(monthLeft);
	msg.addByte(skullDuration);
	writeToOutputBuffer(msg);
}


void ProtocolGame::sendRestingAreaIcon(bool activate/*=false*/, bool activeResting/*=false*/) {
	return;

	NetworkMessage msg;
	msg.addByte(0xA9);

	uint8_t b1=0, b2=0;
	std::ostringstream ss;
	ss << "";
	if(activate) {
		b1=1;
		ss << "Within ";

		if(activeResting){
			b2 =1;
			ss << "Active ";
		}
		else{
			b2 = 0;
		}
		ss << "Resting Area";
	}

	msg.addByte(b1);
	msg.addByte(b2);
	msg.addString(ss.str());
	writeToOutputBuffer(msg);
}


void ProtocolGame::sendClientCheck()
{
	return;

	NetworkMessage msg;
	msg.addByte(0x63);
	msg.add<uint32_t>(1);
	msg.addByte(1);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendGameNews()
{
	return;

	NetworkMessage msg;
	msg.addByte(0x98);
	msg.add<uint32_t>(1); // unknown
	msg.addByte(1); //(0 = open | 1 = highlight)
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendResourceBalance(uint64_t money, uint64_t bank)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xEE);
	msg.addByte(0x00);
	msg.add<uint64_t>(bank);
	msg.addByte(0xEE);
	msg.addByte(0x01);
	msg.add<uint64_t>(money);
	writeToOutputBuffer(msg);
}


void ProtocolGame::sendMarketEnter(uint32_t depotId)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xF6);

	msg.add<uint64_t>(player->getBankBalance());
	msg.addByte(std::min<uint32_t>(IOMarket::getPlayerOfferCount(player->getGUID()), std::numeric_limits<uint8_t>::max()));

	DepotLocker* depotLocker = player->getDepotLocker(depotId);
	if (!depotLocker) {
		msg.add<uint16_t>(0x00);
		writeToOutputBuffer(msg);
		return;
	}

	player->setInMarket(true);

	std::map<uint16_t, uint32_t> depotItems;
	std::forward_list<Container*> containerList{depotLocker};

	do {
		Container* container = containerList.front();
		containerList.pop_front();

		for (Item* item : container->getItemList()) {
			Container* c = item->getContainer();
			if (c && !c->empty()) {
				containerList.push_front(c);
				continue;
			}

			const ItemType& itemType = Item::items[item->getID()];
			if (itemType.wareId == 0) {
				continue;
			}

			if (c && (!itemType.isContainer() || c->capacity() != itemType.maxItems)) {
				continue;
			}

			if (!item->hasMarketAttributes()) {
				continue;
			}

			depotItems[itemType.wareId] += Item::countByType(item, -1);
		}
	} while (!containerList.empty());

	uint16_t itemsToSend = std::min<size_t>(depotItems.size(), std::numeric_limits<uint16_t>::max());
	msg.add<uint16_t>(itemsToSend);

	uint16_t i = 0;
	for (std::map<uint16_t, uint32_t>::const_iterator it = depotItems.begin(); i < itemsToSend; ++it, ++i) {
		msg.add<uint16_t>(it->first);
		msg.add<uint16_t>(std::min<uint32_t>(0xFFFF, it->second));
	}

	writeToOutputBuffer(msg);

	updateCoinBalance();
	sendResourceBalance(player->getMoney(), player->getBankBalance());
}

void ProtocolGame::sendMarketLeave()
{
	return;

	NetworkMessage msg;
	msg.addByte(0xF7);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketBrowseItem(uint16_t itemId, const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
{
	return;

	NetworkMessage msg;

	msg.addByte(0xF9);
	msg.addItemId(itemId);

	msg.add<uint32_t>(buyOffers.size());
	for (const MarketOffer& offer : buyOffers) {
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint32_t>(offer.price);
		msg.addString(offer.playerName);
	}

	msg.add<uint32_t>(sellOffers.size());
	for (const MarketOffer& offer : sellOffers) {
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint32_t>(offer.price);
		msg.addString(offer.playerName);
	}

	updateCoinBalance();
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketAcceptOffer(const MarketOfferEx& offer)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.addItemId(offer.itemId);

	if (offer.type == MARKETACTION_BUY) {
		msg.add<uint32_t>(0x01);
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint32_t>(offer.price);
		msg.addString(offer.playerName);
		msg.add<uint32_t>(0x00);
	} else {
		msg.add<uint32_t>(0x00);
		msg.add<uint32_t>(0x01);
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint32_t>(offer.price);
		msg.addString(offer.playerName);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketBrowseOwnOffers(const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.add<uint16_t>(MARKETREQUEST_OWN_OFFERS);

	msg.add<uint32_t>(buyOffers.size());
	for (const MarketOffer& offer : buyOffers) {
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.addItemId(offer.itemId);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint32_t>(offer.price);
	}

	msg.add<uint32_t>(sellOffers.size());
	for (const MarketOffer& offer : sellOffers) {
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.addItemId(offer.itemId);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint32_t>(offer.price);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketCancelOffer(const MarketOfferEx& offer)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.add<uint16_t>(MARKETREQUEST_OWN_OFFERS);

	if (offer.type == MARKETACTION_BUY) {
		msg.add<uint32_t>(0x01);
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.addItemId(offer.itemId);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint32_t>(offer.price);
		msg.add<uint32_t>(0x00);
	} else {
		msg.add<uint32_t>(0x00);
		msg.add<uint32_t>(0x01);
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.addItemId(offer.itemId);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint32_t>(offer.price);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketBrowseOwnHistory(const HistoryMarketOfferList& buyOffers, const HistoryMarketOfferList& sellOffers)
{
	return;

	uint32_t i = 0;
	std::map<uint32_t, uint16_t> counterMap;
	uint32_t buyOffersToSend = std::min<uint32_t>(buyOffers.size(), 810 + std::max<int32_t>(0, 810 - sellOffers.size()));
	uint32_t sellOffersToSend = std::min<uint32_t>(sellOffers.size(), 810 + std::max<int32_t>(0, 810 - buyOffers.size()));

	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.add<uint16_t>(MARKETREQUEST_OWN_HISTORY);

	msg.add<uint32_t>(buyOffersToSend);
	for (auto it = buyOffers.begin(); i < buyOffersToSend; ++it, ++i) {
		msg.add<uint32_t>(it->timestamp);
		msg.add<uint16_t>(counterMap[it->timestamp]++);
		msg.addItemId(it->itemId);
		msg.add<uint16_t>(it->amount);
		msg.add<uint32_t>(it->price);
		msg.addByte(it->state);
	}

	counterMap.clear();
	i = 0;

	msg.add<uint32_t>(sellOffersToSend);
	for (auto it = sellOffers.begin(); i < sellOffersToSend; ++it, ++i) {
		msg.add<uint32_t>(it->timestamp);
		msg.add<uint16_t>(counterMap[it->timestamp]++);
		msg.addItemId(it->itemId);
		msg.add<uint16_t>(it->amount);
		msg.add<uint32_t>(it->price);
		msg.addByte(it->state);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketDetail(uint16_t itemId)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xF8);
	msg.addItemId(itemId);

	const ItemType& it = Item::items[itemId];
	if (it.armor != 0) {
		msg.addString(std::to_string(it.armor));
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (it.attack != 0) {
		// TODO: chance to hit, range
		// example:
		// "attack +x, chance to hit +y%, z fields"
		if (it.abilities && it.abilities->elementType != COMBAT_NONE && it.abilities->elementDamage != 0) {
			std::ostringstream ss;
			ss << it.attack << " physical +" << it.abilities->elementDamage << ' ' << getCombatName(it.abilities->elementType);
			msg.addString(ss.str());
		} else {
			msg.addString(std::to_string(it.attack));
		}
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (it.isContainer()) {
		msg.addString(std::to_string(it.maxItems));
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (it.defense != 0) {
		if (it.extraDefense != 0) {
			std::ostringstream ss;
			ss << it.defense << ' ' << std::showpos << it.extraDefense << std::noshowpos;
			msg.addString(ss.str());
		} else {
			msg.addString(std::to_string(it.defense));
		}
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (!it.description.empty()) {
		const std::string& descr = it.description;
		if (descr.back() == '.') {
			msg.addString(std::string(descr, 0, descr.length() - 1));
		} else {
			msg.addString(descr);
		}
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (it.decayTime != 0) {
		std::ostringstream ss;
		ss << it.decayTime << " seconds";
		msg.addString(ss.str());
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (it.abilities) {
		std::ostringstream ss;
		bool separator = false;

		for (size_t i = 0; i < COMBAT_COUNT; ++i) {
			if (it.abilities->absorbPercent[i] == 0) {
				continue;
			}

			if (separator) {
				ss << ", ";
			} else {
				separator = true;
			}

			ss << getCombatName(indexToCombatType(i)) << ' ' << std::showpos << it.abilities->absorbPercent[i] << std::noshowpos << '%';
		}

		msg.addString(ss.str());
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (it.minReqLevel != 0) {
		msg.addString(std::to_string(it.minReqLevel));
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (it.minReqMagicLevel != 0) {
		msg.addString(std::to_string(it.minReqMagicLevel));
	} else {
		msg.add<uint16_t>(0x00);
	}

	msg.addString(it.vocationString);

	msg.addString(it.runeSpellName);

	if (it.abilities) {
		std::ostringstream ss;
		bool separator = false;

		for (uint8_t i = SKILL_FIRST; i <= SKILL_FISHING; i++) {
			if (!it.abilities->skills[i]) {
				continue;
			}

			if (separator) {
				ss << ", ";
			} else {
				separator = true;
			}

			ss << getSkillName(i) << ' ' << std::showpos << it.abilities->skills[i] << std::noshowpos;
		}

		for (uint8_t i = SKILL_CRITICAL_HIT_CHANCE; i <= SKILL_LAST; i++) {
			if (!it.abilities->skills[i]) {
				continue;
			}

			if (separator) {
				ss << ", ";
			}
			else {
				separator = true;
			}

			ss << getSkillName(i) << ' ' << std::showpos << it.abilities->skills[i] << std::noshowpos << '%';
		}

		if (it.abilities->stats[STAT_MAGICPOINTS] != 0) {
			if (separator) {
				ss << ", ";
			} else {
				separator = true;
			}

			ss << "magic level " << std::showpos << it.abilities->stats[STAT_MAGICPOINTS] << std::noshowpos;
		}

		if (it.abilities->speed != 0) {
			if (separator) {
				ss << ", ";
			}

			ss << "speed " << std::showpos << (it.abilities->speed >> 1) << std::noshowpos;
		}

		msg.addString(ss.str());
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (it.charges != 0) {
		msg.addString(std::to_string(it.charges));
	} else {
		msg.add<uint16_t>(0x00);
	}

	std::string weaponName = getWeaponName(it.weaponType);

	if (it.slotPosition & SLOTP_TWO_HAND) {
		if (!weaponName.empty()) {
			weaponName += ", two-handed";
		} else {
			weaponName = "two-handed";
		}
	}

	msg.addString(weaponName);

	if (it.weight != 0) {
		std::ostringstream ss;
		if (it.weight < 10) {
			ss << "0.0" << it.weight;
		} else if (it.weight < 100) {
			ss << "0." << it.weight;
		} else {
			std::string weightString = std::to_string(it.weight);
			weightString.insert(weightString.end() - 2, '.');
			ss << weightString;
		}
		ss << " oz";
		msg.addString(ss.str());
	} else {
		msg.add<uint16_t>(0x00);
	}

	if (version > 1099) {
		uint8_t slot = Item::items[itemId].imbuingSlots;
		if(slot > 0) {
			msg.addString(std::to_string(slot));
		} else {
			msg.add<uint16_t>(0x00);
		}
	}

	MarketStatistics* statistics = IOMarket::getInstance().getPurchaseStatistics(itemId);
	if (statistics) {
		msg.addByte(0x01);
		msg.add<uint32_t>(statistics->numTransactions);
		msg.add<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), statistics->totalPrice));
		msg.add<uint32_t>(statistics->highestPrice);
		msg.add<uint32_t>(statistics->lowestPrice);
	} else {
		msg.addByte(0x00);
	}

	statistics = IOMarket::getInstance().getSaleStatistics(itemId);
	if (statistics) {
		msg.addByte(0x01);
		msg.add<uint32_t>(statistics->numTransactions);
		msg.add<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), statistics->totalPrice));
		msg.add<uint32_t>(statistics->highestPrice);
		msg.add<uint32_t>(statistics->lowestPrice);
	} else {
		msg.addByte(0x00);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendItemDetail(uint16_t itemCID)
{
	return;

	NetworkMessage msg;
	msg.addByte(0x76);

	const ItemType& it = Item::items.getItemIdByClientId(itemCID);
	msg.addByte(0x00); // ??
	if(version >= 1220) {
		msg.addByte(0x01);
	}

	msg.addByte(0x01); // name
	msg.addString(it.name);

	msg.add<uint16_t>(itemCID);
	if (it.stackable || it.isFluidContainer()) {
		msg.addByte(0x01);
	} else if (it.isContainer()) {
		msg.addByte(0x00);
	}
	if (it.isAnimation) {
		msg.addByte(0xFE);
	}
	msg.addByte(0x00);

	uint8_t count = 1;
	bool hasCombat = false;
	bool hasSkill = false;
	if (!it.isRune()) {
		if (it.armor != 0) {
			count++;
		}
		if (it.attack != 0) {
			count++;
		}	
		if (it.defense != 0) {
			count++;
		}

		if (it.abilities) {
			for (uint8_t i = SKILL_FIRST; i <= SKILL_FISHING; i++) {
				if (!it.abilities->skills[i]) {
					continue;
				}

				if (hasSkill) {
					break;
				}
				hasSkill = true;
				count++;
			}

			for (uint8_t i = SKILL_CRITICAL_HIT_CHANCE; i <= SKILL_LAST; i++) {
				if (!it.abilities->skills[i]) {
					continue;
				}

				if (hasSkill) {
					break;
				}
				hasSkill = true;
				count++;
			}

			if (it.abilities->stats[STAT_MAGICPOINTS] != 0) {
				if (!hasSkill) {
					hasSkill = true;
					count++;
				}
			}

			if (it.abilities->speed != 0) {
				if (!hasSkill) {
					hasSkill = true;
					count++;
				}
			}
		}

		if (it.abilities) {
			for (size_t i = 0; i < COMBAT_COUNT; ++i) {
				if (it.abilities->absorbPercent[i] == 0) {
					continue;
				}

				if (hasCombat) {
					break;
				}
				hasCombat = true;
				count++;

			}
		}

		if(it.imbuingSlots > 0) {
			count++;
		}
		if (!it.description.empty()) {
			count++;
		}
	} else {
		count = 0x05;
	}

	if(!it.vocationString.empty()) {
		count++;
	}

	msg.addByte(count);
	if (!it.isRune()) {
		if (it.armor != 0) {
			msg.addString("Armor");
			msg.addString(std::to_string(it.armor));
		}
		if (it.attack != 0) {
			msg.addString("Attack");
			msg.addString(std::to_string(it.attack));
		}	
		if (it.defense != 0) {
			msg.addString("Defense");
			if (it.extraDefense != 0) {
				std::ostringstream ss;
				ss << it.defense << ' ' << std::showpos << it.extraDefense << std::noshowpos;
				msg.addString(ss.str());
			} else {
				msg.addString(std::to_string(it.defense));
			}
			
		}
		if (it.abilities && hasSkill) {
			std::ostringstream ss;
			bool separator = false;
			for (uint8_t i = SKILL_FIRST; i <= SKILL_FISHING; i++) {
				if (!it.abilities->skills[i]) {
					continue;
				}

				if (separator) {
					ss << ", ";
				} else {
					separator = true;
				}

				ss << getSkillName(i) << ' ' << std::showpos << it.abilities->skills[i] << std::noshowpos;
			}

			for (uint8_t i = SKILL_CRITICAL_HIT_CHANCE; i <= SKILL_LAST; i++) {
				if (!it.abilities->skills[i]) {
					continue;
				}

				if (separator) {
					ss << ", ";
				} else {
					separator = true;
				}

				ss << getSkillName(i) << ' ' << std::showpos << it.abilities->skills[i] << std::noshowpos << '%';
			}

			if (it.abilities->stats[STAT_MAGICPOINTS] != 0) {
				if (separator) {
					ss << ", ";
				}
				separator = true;
				ss << "magic level " << std::showpos << it.abilities->stats[STAT_MAGICPOINTS] << std::noshowpos;
			}

			if (it.abilities->speed != 0) {
				if (separator) {
					ss << ", ";
				}
				ss << "speed " << std::showpos << (it.abilities->speed >> 1) << std::noshowpos;
			}
			msg.addString("Skills");
			msg.addString(ss.str());
		}

		if (it.abilities && hasCombat) {
			std::ostringstream ss;
			bool separator = false;
			for (size_t i = 0; i < COMBAT_COUNT; ++i) {
				if (it.abilities->absorbPercent[i] == 0) {
					continue;
				}
				if (separator) {
					ss << ", ";
				} else {
					separator = true;
				}
				ss << getCombatName(indexToCombatType(i)) << ' ' << std::showpos << it.abilities->absorbPercent[i] << std::noshowpos << '%';
			}
		msg.addString("Protection");
		msg.addString(ss.str());
	}

	if(!it.vocationString.empty()) {
			msg.addString("Professions");
			msg.addString(it.vocationString);
		}

		if (!it.description.empty()) {
			msg.addString("Description");
			const std::string& descr = it.description;
			if (descr.back() == '.') {
				msg.addString(std::string(descr, 0, descr.length() - 1));
			} else {
				msg.addString(descr);
			}
		}
	} else {
		msg.addString("Spell");
		if(it.runeSpellName.empty()) {
			msg.add<uint16_t>(0x00);
		} else {
			msg.addString(it.runeSpellName);
		}

		msg.addString("Required Level");
		msg.addString(std::to_string(it.runeLevel));

		msg.addString("Required Magic Level");
		msg.addString(std::to_string(it.runeMagLevel));

		if(!it.vocationString.empty()) {
			msg.addString("Professions");
			msg.addString(it.vocationString);
		}
	}

	msg.addString(it.stackable ? "Total Weight" : "Weight");
	if (it.weight != 0) {
		std::ostringstream ss;
		if (it.weight < 10) {
			ss << "0.0" << it.weight;
		} else if (it.weight < 100) {
			ss << "0." << it.weight;
		} else {
			std::string weightString = std::to_string(it.weight);
			weightString.insert(weightString.end() - 2, '.');
			ss << weightString;
		}
		ss << " oz";
		msg.addString(ss.str());
	} else {
		msg.addString("0.00 oz");
	}

	if (it.isRune()){
		msg.addString("Tradeable");
		msg.addString("Yes");
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCoinBalance() {
	return;

	NetworkMessage msg;
	msg.addByte(0xF2); // updating balance
	msg.addByte(0x01);

	msg.addByte(0xDF); // coins balance
	msg.addByte(0x01);

	msg.add<uint32_t>(player->getCoinBalance(COIN_TYPE_DEFAULT)); //total coins
	msg.add<uint32_t>(player->getCoinBalance(COIN_TYPE_TRANSFERABLE)); //transferable coins
	if (version >= 1220)
		msg.add<uint32_t>(player->getCoinBalance(COIN_TYPE_TOURNAMENT)); //transferable coins

	writeToOutputBuffer(msg);
}

void ProtocolGame::updateCoinBalance() {
	NetworkMessage msg;
	msg.addByte(0xF2);
	msg.addByte(0x00);

	writeToOutputBuffer(msg);

	g_dispatcher.addTask(
		createTask(std::bind([](uint32_t playerId) {
			Player* player = g_game.getPlayerByID(playerId);
			if (player != nullptr) {
				auto coinBalance = IOAccount::getCoinBalance(player->getAccount());
				auto tournamentCoinBalance = IOAccount::getCoinBalance(player->getAccount(), COIN_TYPE_TOURNAMENT);
				player->coinBalance = coinBalance;
				player->tournamentCoinBalance = tournamentCoinBalance;

				player->sendCoinBalance();
			}
	}, player->getID()))
	);
}

void ProtocolGame::sendQuestTracker()
{
	return;

	NetworkMessage msg;
	msg.addByte(0xD0); // byte quest tracker
	msg.addByte(1); // send quests of quest log ??
	msg.add<uint16_t>(1); // unknown
	writeToOutputBuffer(msg);
}















//tile













void ProtocolGame::sendSpellCooldown(uint8_t spellId, uint32_t time)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xA4);
	if (player->getProtocolVersion() < 1120 && spellId >= 170) {
		spellId = 150;
	}
	msg.addByte(spellId);
	msg.add<uint32_t>(time);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendSpellGroupCooldown(SpellGroup_t groupId, uint32_t time)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xA5);
	msg.addByte(groupId);
	msg.add<uint32_t>(time);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendModalWindow(const ModalWindow& modalWindow)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xFA);

	msg.add<uint32_t>(modalWindow.id);
	msg.addString(modalWindow.title);
	msg.addString(modalWindow.message);

	msg.addByte(modalWindow.buttons.size());
	for (const auto& it : modalWindow.buttons) {
		msg.addString(it.first);
		msg.addByte(it.second);
	}

	msg.addByte(modalWindow.choices.size());
	for (const auto& it : modalWindow.choices) {
		msg.addString(it.first);
		msg.addByte(it.second);
	}

	msg.addByte(modalWindow.defaultEscapeButton);
	msg.addByte(modalWindow.defaultEnterButton);
	msg.addByte(modalWindow.priority ? 0x01 : 0x00);

	writeToOutputBuffer(msg);
}

// OTCv8
void ProtocolGame::sendFeatures()
{
	return;

	if(!otclientV8) 
		return;

	std::map<GameFeature, bool> features;
	// place for non-standard OTCv8 features
	features[GameExtendedOpcode] = true;

	if(features.empty())
		return;

	NetworkMessage msg;
	msg.addByte(0x43);
	msg.add<uint16_t>(features.size());
	for(auto& feature : features) {
		msg.addByte((uint8_t)feature.first);
		msg.addByte(feature.second ? 1 : 0);
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPlayerMana(const Player* target)
{
	return;

	if (version < 1230) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x8B);
	msg.add<uint32_t>(target->getID());
	msg.addByte(11);
	msg.addByte(std::ceil((static_cast<double>(target->getMana()) / std::max<int32_t>(target->getMaxMana(), 1)) * 100));
	writeToOutputBuffer(msg);
}

void ProtocolGame::requestPurchaseData(uint32_t offerId, uint8_t offerType)
{
	NetworkMessage msg;
	msg.addByte(0xE1);
	msg.add<uint32_t>(offerId);
	msg.addByte(offerType);

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendStoreHistory(uint32_t totalPages, uint32_t pages, std::vector<StoreHistory> filter)
{
	return;

	NetworkMessage msg;
	msg.addByte(0xFD);
	msg.add<uint32_t>(totalPages > 0 ? pages - 1 : 0x0); //-- current page
	msg.add<uint32_t>(totalPages > 0 ? totalPages : 0x0); //-- total page
	msg.addByte(filter.size());

	for (auto currentHistory = filter.begin(), end = filter.end(); currentHistory != end; ++currentHistory) {
		if (version >= 1220)
			msg.add<uint32_t>(0);

		msg.add<uint32_t>((*currentHistory).time);
		msg.addByte((*currentHistory).mode);
		msg.add<int32_t>((*currentHistory).cust);
		if (version >= 1200)
    		msg.addByte((*currentHistory).coinMode); //0 = transferable tibia coin, 1 = normal tibia coin

		msg.addString((*currentHistory).description);
		if (version >= 1220)
			msg.addByte(0); //-- details
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendLockerItems(std::map<uint16_t, uint16_t> itemMap, uint16_t count)
{
	return;

	NetworkMessage msg;
	msg.addByte(0x94);

	msg.add<uint16_t>(count);
	for (const auto& it : itemMap) {
		msg.addItemId(it.first);
		msg.add<uint16_t>(it.second);
	}

	writeToOutputBuffer(msg);
}

// === 11.x send definitions (used by the engine; signatures match OTG) ===

// Delegation wrappers: route common 8.6 sends to ProtocolGameBase (8.6 serialization).
void ProtocolGame::sendTextMessage(const TextMessage& message) { ProtocolGameBase::sendTextMessage(message); }
void ProtocolGame::sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxlen, bool canWrite) { ProtocolGameBase::sendTextWindow(windowTextId, item, maxlen, canWrite); }
void ProtocolGame::sendChannelMessage(const std::string& author, const std::string& text, SpeakClasses type, uint16_t channel) { ProtocolGameBase::sendChannelMessage(author, text, type, channel); }
void ProtocolGame::sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId) { ProtocolGameBase::sendToChannel(creature, type, text, channelId); }
void ProtocolGame::sendCreatureSkull(const Creature* creature) { ProtocolGameBase::sendCreatureSkull(creature); }
void ProtocolGame::sendRemoveTileThing(const Position& pos, uint32_t stackpos) { ProtocolGameBase::sendRemoveTileThing(pos, stackpos); }
void ProtocolGame::sendCreatureTurn(const Creature* creature, uint32_t stackpos) { ProtocolGameBase::sendCreatureTurn(creature, stackpos); }
void ProtocolGame::sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text, const Position* pos) { ProtocolGameBase::sendCreatureSay(creature, type, text, pos); }
void ProtocolGame::sendPrivateMessage(const Player* speaker, SpeakClasses type, const std::string& text) { ProtocolGameBase::sendPrivateMessage(speaker, type, text); }
void ProtocolGame::sendCreatureSquare(const Creature* creature, SquareColor_t color, uint8_t length) { ProtocolGameBase::sendCreatureSquare(creature, color, length); }
void ProtocolGame::sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit) { ProtocolGameBase::sendCreatureOutfit(creature, outfit); }
void ProtocolGame::sendCreatureWalkthrough(const Creature* creature, bool walkthrough) { ProtocolGameBase::sendCreatureWalkthrough(creature, walkthrough); }
void ProtocolGame::sendCreatureShield(const Creature* creature) { ProtocolGameBase::sendCreatureShield(creature); }
void ProtocolGame::sendCancelTarget() { ProtocolGameBase::sendCancelTarget(); }
void ProtocolGame::sendChangeSpeed(const Creature* creature, uint32_t speed) { ProtocolGameBase::sendChangeSpeed(creature, speed); }
void ProtocolGame::sendCreatureHealth(const Creature* creature) { ProtocolGameBase::sendCreatureHealth(creature); }
void ProtocolGame::sendDistanceShoot(const Position& from, const Position& to, uint8_t type) { ProtocolGameBase::sendDistanceShoot(from, to, type); }
void ProtocolGame::sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName) { ProtocolGameBase::sendCreatePrivateChannel(channelId, channelName); }
void ProtocolGame::sendSaleItemList(const std::list<ShopInfo>& shop) { ProtocolGameBase::sendSaleItemList(shop); }
void ProtocolGame::sendTradeItemRequest(const std::string& traderName, const Item* item, bool ack) { ProtocolGameBase::sendTradeItemRequest(traderName, item, ack); }
void ProtocolGame::sendCloseTrade() { ProtocolGameBase::sendCloseTrade(); }
void ProtocolGame::sendChannelsDialog() { ProtocolGameBase::sendChannelsDialog(); }
void ProtocolGame::sendOpenPrivateChannel(const std::string& receiver) { ProtocolGameBase::sendOpenPrivateChannel(receiver); }
void ProtocolGame::sendOutfitWindow() { ProtocolGameBase::sendOutfitWindow(); }
void ProtocolGame::sendCloseContainer(uint8_t cid) { ProtocolGameBase::sendCloseContainer(cid); }
void ProtocolGame::sendQuestLog() { ProtocolGameBase::sendQuestLog(); }
void ProtocolGame::sendQuestLine(const Quest* quest) { ProtocolGameBase::sendQuestLine(quest); }
void ProtocolGame::sendFYIBox(const std::string& message) { ProtocolGameBase::sendFYIBox(message); }
void ProtocolGame::sendTutorial(uint8_t tutorialId) { ProtocolGameBase::sendTutorial(tutorialId); }
void ProtocolGame::sendAddMarker(const Position& pos, uint8_t markType, const std::string& desc) { ProtocolGameBase::sendAddMarker(pos, markType, desc); }
void ProtocolGame::sendMoveCreature(const Creature* creature, const Position& newPos, int32_t newStackPos, const Position& oldPos, int32_t oldStackPos, bool teleport) { ProtocolGameBase::sendMoveCreature(creature, newPos, newStackPos, oldPos, oldStackPos, teleport); }
void ProtocolGame::sendIcons(uint16_t icons) { ProtocolGameBase::sendIcons(icons); }
void ProtocolGame::sendReLoginWindow(uint8_t unfairFightReduction) { ProtocolGameBase::sendReLoginWindow(unfairFightReduction); }
void ProtocolGame::sendShop(Npc* npc, const ShopInfoList& itemList) { ProtocolGameBase::sendShop(npc, itemList); }
void ProtocolGame::sendHouseWindow(uint32_t windowTextId, const std::string& text) { ProtocolGameBase::sendHouseWindow(windowTextId, text); }
void ProtocolGame::sendAddContainerItem(uint8_t cid, uint16_t slot, const Item* item) { ProtocolGameBase::sendAddContainerItem(cid, item); }
void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint16_t slot, const Item* item) { ProtocolGameBase::sendUpdateContainerItem(cid, slot, item); }
void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint16_t slot, const Item* lastItem) { ProtocolGameBase::sendRemoveContainerItem(cid, slot); }
void ProtocolGame::sendUpdatedVIPStatus(uint32_t guid, VipStatus_t newStatus) { ProtocolGameBase::sendUpdatedVIPStatus(guid, newStatus); }
void ProtocolGame::sendClosePrivate(uint16_t channelId) { ProtocolGameBase::sendClosePrivate(channelId); }
void ProtocolGame::sendAddTileItem(const Position& pos, uint32_t stackpos, const Item* item) { ProtocolGameBase::sendAddTileItem(pos, stackpos, item); }
void ProtocolGame::sendUpdateTileItem(const Position& pos, uint32_t stackpos, const Item* item) { ProtocolGameBase::sendUpdateTileItem(pos, stackpos, item); }
void ProtocolGame::sendCloseShop() { ProtocolGameBase::sendCloseShop(); }

// 3-arg text-window overload (11.x house/quest flow); no 8.6 equivalent - no-op for the 8.6 client.
void ProtocolGame::sendTextWindow(uint32_t windowTextId, uint16_t maxlen, const std::string& text) {
	// 8.6 client has no text-window-with-string packet; intentionally empty.
}
