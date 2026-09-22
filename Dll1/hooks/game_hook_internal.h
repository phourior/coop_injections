#pragma once

#include <cstddef>
#include <cstdint>

bool VisionBytesEqual(uintptr_t address, const uint8_t* expected, size_t size);
bool WriteCodeBytes(uintptr_t address, const void* bytes, size_t size);
void UpdateFullMapVisionMapState(const char* mapPath, const char* mapDesc);

bool ApplyEnhancedVisionMode(bool observerSelected);
bool RestoreEnhancedVisionMode();
bool IsEnhancedVisionModeApplied(bool observerSelected);
bool RefreshEnhancedVisionRuntime(bool enhancedSelected, bool fullMapVisionEnabled, bool mapAllowed);
bool IsObserverVisionRuntimeReady();

bool CleanupLeaderPanelFeature();