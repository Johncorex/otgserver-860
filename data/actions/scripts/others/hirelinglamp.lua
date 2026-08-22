function onUse(player, item, fromPosition, target, toPosition, isHotkey)
	-- Hireling system not available in 8.6
	player:sendTextMessage(MESSAGE_INFO_DESCR, "Hireling system is not available in this version.")
	return false
end
