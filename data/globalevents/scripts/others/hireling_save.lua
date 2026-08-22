function onShutdown()
    Game.sendConsoleMessage('>> Hireling system not available in 8.6', CONSOLEMESSAGE_TYPE_STARTUP)
    -- SaveHirelings()
    return true
end
