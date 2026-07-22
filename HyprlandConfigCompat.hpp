#pragma once

#include "globals.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <string>
#include <vector>

namespace CompatHyprlandAPI {
struct SConfigValueCompat {
    Config::INTEGER integer = 0;
    Config::INTEGER* integerPtr = &integer;
    Config::FLOAT floating = 0;
    Config::FLOAT* floatingPtr = &floating;
    Config::STRING string;
    void* ptr = nullptr;

    void* getDataStaticPtr() {
        return ptr;
    }
};

SConfigValueCompat* getConfigValue(HANDLE handle, const std::string& name);
PHLMONITOR getMonitorFromCursor();
const std::vector<PHLWINDOW>& windows();
const std::vector<PHLWORKSPACEREF>& workspaceRefs();
PHLWORKSPACE getWorkspaceByID(WORKSPACEID id);
PHLWORKSPACE createNewWorkspace(WORKSPACEID id, MONITORID monitorID, const std::string& name, bool isEmpty);
void moveWindowToWorkspaceSafe(const PHLWINDOW& window, const PHLWORKSPACE& workspace);
void warpCursorTo(const Vector2D& point);
void scheduleFrameForMonitor(const PHLMONITOR& monitor);
PHLWINDOW getWindowByAddress(const std::string& address);
}
