#pragma once

// Hook sub_1420BFBE0 = CBattleNet::OnGameLobbyUpdate
// Must be called after MH_Initialize and before MH_EnableHook(MH_ALL_HOOKS)
bool SetupGameHooks();
