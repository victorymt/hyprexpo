#pragma once

#include "globals.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <string>
#include <vector>

namespace HyprexpoRuntime {
Config::INTEGER configInt(const std::string& name, Config::INTEGER fallback = 0);
Config::FLOAT   configFloat(const std::string& name, Config::FLOAT fallback = 0.F);
Config::STRING  configString(const std::string& name, Config::STRING fallback = "");

PHLMONITOR getMonitorFromCursor();
const std::vector<PHLWINDOW>& windows();
const std::vector<PHLWORKSPACEREF>& workspaceRefs();
PHLWORKSPACE getWorkspaceByID(WORKSPACEID id);
PHLWORKSPACE createNewWorkspace(WORKSPACEID id, MONITORID monitorID, const std::string& name, bool isEmpty);
void moveWindowToWorkspaceSafe(const PHLWINDOW& window, const PHLWORKSPACE& workspace);
void warpCursorTo(const Vector2D& point);
void scheduleFrameForMonitor(const PHLMONITOR& monitor);
std::string windowAddressSelector(const PHLWINDOW& window);
PHLWINDOW getWindowByAddressSelector(const std::string& selector);
}
