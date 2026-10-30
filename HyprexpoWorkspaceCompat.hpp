#pragma once

// Hyprland 0.57 compatibility shims for hyprexpo.
//
// 0.57 replaced the flat integer workspace identity with a variant:
//
//   using WorkspaceID = std::variant<SWorkspaceNumberedID, SWorkspaceSpecialID>;
//
// and removed, among others, the globals WORKSPACEID, WORKSPACE_INVALID and
// getWorkspaceIDNameFromString() plus Monitor::CMonitor::activeWorkspaceID().
//
// hyprexpo's overview layout only ever indexes *numbered* workspaces, so this
// header restores the exact vocabulary the layout code was written against.
// Special workspaces map to WORKSPACE_INVALID, which is what the old code
// assumed anyway (it never tiles special workspaces).
//
// Keeping the old names is deliberate: it confines the 0.57 port to this file
// plus the render-pipeline changes, instead of rewriting the layout algorithm.

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/state/workspace/Target.hpp>
#include <hyprland/src/workspace/AbstractWorkspace.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/workspace/RegularWorkspace.hpp>
#include <hyprland/src/workspace/query/Query.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

// ─── Restored workspace vocabulary ──────────────────────────────────────────

using WORKSPACEID = int64_t;

inline constexpr WORKSPACEID WORKSPACE_INVALID = -1;

// Mirrors the struct the removed getWorkspaceIDNameFromString() returned, so
// call sites written as `getWorkspaceIDNameFromString(sel).id` keep working.
struct SWorkspaceIDName {
    WORKSPACEID id = WORKSPACE_INVALID;
};

namespace Hyprexpo::Compat {

inline constexpr auto MAX_NUMBERED_ID = std::numeric_limits<::Workspace::WorkspaceIDContainer>::max();

// A numbered id is representable when it fits WorkspaceIDContainer and is > 0.
inline bool validNumberedID(WORKSPACEID id) {
    return id > 0 && static_cast<uint64_t>(id) <= static_cast<uint64_t>(MAX_NUMBERED_ID);
}

inline WORKSPACEID workspaceID(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return WORKSPACE_INVALID;

    const auto NUMBERED = workspace->numberedID();
    return NUMBERED ? static_cast<WORKSPACEID>(*NUMBERED) : WORKSPACE_INVALID;
}

inline WORKSPACEID activeWorkspaceID(const PHLMONITOR& monitor) {
    return monitor ? workspaceID(monitor->m_activeWorkspace) : WORKSPACE_INVALID;
}

// Resolves to a numbered id, or WORKSPACE_INVALID for special/unknown results.
inline WORKSPACEID workspaceIDFromTarget(const State::Workspace::STarget& target) {
    if (!target.id)
        return WORKSPACE_INVALID;

    const auto NUMBERED = std::get_if<::Workspace::SWorkspaceNumberedID>(&*target.id);
    return NUMBERED ? static_cast<WORKSPACEID>(NUMBERED->value) : WORKSPACE_INVALID;
}

inline WORKSPACEID workspaceIDForSelector(const std::string& selector) {
    return workspaceIDFromTarget(State::Workspace::resolver()->getWorkspaceTargetFromString(selector));
}

inline WORKSPACEID workspaceIDForSelector(const PHLMONITOR& monitor, const std::string& selector) {
    return workspaceIDFromTarget(State::Workspace::resolver()->getWorkspaceTargetFromString(selector, monitor));
}

inline PHLWORKSPACE findWorkspaceByID(WORKSPACEID id) {
    if (!validNumberedID(id))
        return nullptr;

    const auto NUMBERED = ::Workspace::SWorkspaceNumberedID{static_cast<::Workspace::WorkspaceIDContainer>(id)};
    return State::workspaceState()->query().numbered(NUMBERED).run();
}

// 0.57 workspace creation takes a monitor handle, but the overview layout only
// carries a MONITORID, so resolve it through the monitor state tracker.
inline PHLMONITOR monitorByID(MONITORID monitorID) {
    for (const auto& candidate : State::monitorState()->monitors()) {
        if (candidate && candidate->m_id == monitorID)
            return candidate;
    }
    return nullptr;
}

inline PHLWORKSPACE createWorkspace(WORKSPACEID id, MONITORID monitorID, const std::string& name, bool isEmpty) {
    if (!validNumberedID(id))
        return nullptr;

    const auto MONITOR = monitorByID(monitorID);
    if (!MONITOR)
        return nullptr;

    return ::Workspace::CRegularWorkspace::create(::Workspace::SWorkspaceNumberedID{static_cast<::Workspace::WorkspaceIDContainer>(id)}, MONITOR, name, isEmpty);
}

// ─── Window property shims ──────────────────────────────────────────────────
//
// 0.56 exposed CWindow::m_pinned and CWindow::m_isMapped directly; 0.57 moved
// the first into the WINDOW_STATE_* bitfield and the second behind the virtual
// mapped() accessor.

// WindowState is a bitmask wrapper (EXPOSE_ENUM_AS_MASK), not an integer, so
// membership is tested through its explicit operator bool.
inline bool isPinned(const PHLWINDOW& window) {
    return window && static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED);
}

inline void setPinned(const PHLWINDOW& window, bool pinned) {
    if (!window)
        return;

    if (pinned)
        window->m_state |= Desktop::View::WINDOW_STATE_PINNED;
    else
        window->m_state &= ~Desktop::View::WINDOW_STATE_PINNED;
}

inline bool isMapped(const PHLWINDOW& window) {
    return window && window->mapped();
}

} // namespace Hyprexpo::Compat

// ─── Drop-in replacements for removed Hyprland globals ──────────────────────

inline WORKSPACEID workspaceID(const PHLWORKSPACE& workspace) {
    return Hyprexpo::Compat::workspaceID(workspace);
}

inline WORKSPACEID activeWorkspaceID(const PHLMONITOR& monitor) {
    return Hyprexpo::Compat::activeWorkspaceID(monitor);
}

// The overview builds relative selectors ("m+2", "r-1", "emptynm"); 0.57's
// resolver understands the same relative forms, so they are forwarded as-is.
inline SWorkspaceIDName getWorkspaceIDNameFromString(const std::string& selector) {
    return SWorkspaceIDName{.id = Hyprexpo::Compat::workspaceIDForSelector(selector)};
}

inline SWorkspaceIDName getWorkspaceIDNameFromString(const std::string& selector, const PHLMONITOR& monitor) {
    return SWorkspaceIDName{.id = Hyprexpo::Compat::workspaceIDForSelector(monitor, selector)};
}
