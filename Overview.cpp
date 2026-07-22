#include "Overview.hpp"
#include "HyprexpoRuntime.hpp"
#include "HyprexpoConfig.hpp"
#include "OverviewInternal.hpp"
#include "HyprexpoLogic.hpp"
#include <hyprland/src/event/EventBus.hpp>
#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/helpers/varlist/VarList.hpp>
#include <hyprland/src/helpers/Format.hpp>
#include <drm_fourcc.h>
#undef private
#undef protected
#include "OverviewPassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <pango/pangocairo.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
namespace Runtime = HyprexpoRuntime;

namespace HyprexpoRuntime {
template <typename T>
static T configValue(const std::string& name, T fallback) {
    const auto VALUE = Config::mgr()->getConfigValue(name);
    if (!VALUE.dataptr || !VALUE.type || *VALUE.type != typeid(T))
        return fallback;

    const auto PTR = reinterpret_cast<T* const*>(VALUE.dataptr);
    return PTR && *PTR ? **PTR : fallback;
}

Config::INTEGER configInt(const std::string& name, Config::INTEGER fallback) {
    return configValue<Config::INTEGER>(name, fallback);
}

Config::FLOAT configFloat(const std::string& name, Config::FLOAT fallback) {
    return configValue<Config::FLOAT>(name, fallback);
}

Config::STRING configString(const std::string& name, Config::STRING fallback) {
    return configValue<Config::STRING>(name, std::move(fallback));
}

PHLMONITOR getMonitorFromCursor() {
    if (!g_pInputManager)
        return nullptr;

    return State::monitorState()->query().vec(g_pInputManager->getMouseCoordsInternal()).run();
}

const std::vector<PHLWINDOW>& windows() {
    return Desktop::windowState()->windows();
}

const std::vector<PHLWORKSPACEREF>& workspaceRefs() {
    return State::workspaceState()->workspaceRefs();
}

PHLWORKSPACE getWorkspaceByID(WORKSPACEID id) {
    return State::workspaceState()->query().id(id).run();
}

PHLWORKSPACE createNewWorkspace(WORKSPACEID id, MONITORID monitorID, const std::string& name, bool isEmpty) {
    return State::workspaceState()->create(id, monitorID, name, isEmpty);
}

void moveWindowToWorkspaceSafe(const PHLWINDOW& window, const PHLWORKSPACE& workspace) {
    if (window && workspace)
        Desktop::globalWindowController()->moveWindowToWorkspace(window, workspace);
}

void warpCursorTo(const Vector2D& point) {
    Pointer::pointerController()->warpTo(point, true);
}

void scheduleFrameForMonitor(const PHLMONITOR& monitor) {
    if (monitor)
        monitor->scheduleFrame();
}

std::string windowAddressSelector(const PHLWINDOW& window) {
    return window ? std::format("address:0x{:x}", reinterpret_cast<uintptr_t>(window.get())) : "";
}

PHLWINDOW getWindowByAddressSelector(const std::string& selector) {
    std::string_view value = selector;
    constexpr std::string_view PREFIX = "address:";
    if (value.starts_with(PREFIX))
        value.remove_prefix(PREFIX.size());
    if (value.starts_with("0x") || value.starts_with("0X"))
        value.remove_prefix(2);

    uintptr_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        return nullptr;

    for (const auto& window : windows()) {
        if (window && window->m_isMapped && reinterpret_cast<uintptr_t>(window.get()) == parsed)
            return window;
    }

    return nullptr;
}
}

static SP<Hyprutils::Animation::SAnimationPropertyConfig> overviewAnimationConfig() {
    static auto config = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();

    config->internalEnabled = 1;
    config->internalSpeed   = std::max<Config::FLOAT>(0.0F, Runtime::configFloat("plugin:hyprexpo:animation_speed", HyprexpoConfig::ANIMATION_SPEED_DEFAULT));
    config->internalBezier  = Runtime::configString("plugin:hyprexpo:animation_bezier", HyprexpoConfig::ANIMATION_BEZIER_DEFAULT);
    config->internalStyle   = "";
    config->overridden      = true;
    config->pValues         = config;

    return config;
}

void clearWithColor(const CHyprColor& color) {
    glClearColor(color.r, color.g, color.b, color.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

uint32_t framebufferFormatWithAlpha(uint32_t drmFormat) {
    const auto alphaFormat = NFormatUtils::alphaFormat(drmFormat);
    return alphaFormat == 0 ? DRM_FORMAT_ABGR8888 : alphaFormat;
}

bool isTransformRotated(wl_output_transform t) {
    return t == WL_OUTPUT_TRANSFORM_90 || t == WL_OUTPUT_TRANSFORM_270 ||
           t == WL_OUTPUT_TRANSFORM_FLIPPED_90 || t == WL_OUTPUT_TRANSFORM_FLIPPED_270;
}

std::string trimString(std::string value) {
    return Hyprexpo::trimString(std::move(value));
}

std::vector<std::string> splitCommaList(const std::string& value) {
    return Hyprexpo::splitCommaList(value);
}

std::string lowerString(std::string value) {
    return Hyprexpo::lowerString(std::move(value));
}

std::string fallbackTokenForVisibleIndex(int visibleIndex) {
    return Hyprexpo::fallbackTokenForVisibleIndex(visibleIndex);
}

int fallbackTokenToVisibleIndex(const std::string& token) {
    return Hyprexpo::fallbackTokenToVisibleIndex(token);
}

SHyprGradientSpec parseGradientSpec(const std::string& inRaw) {
    SHyprGradientSpec spec;
    const auto        parsed = Hyprexpo::parseGradientSpec(inRaw);
    if (!parsed.valid)
        return spec;

    spec.c1       = CHyprColor{parsed.c1.r, parsed.c1.g, parsed.c1.b, parsed.c1.a};
    spec.c2       = CHyprColor{parsed.c2.r, parsed.c2.g, parsed.c2.b, parsed.c2.a};
    spec.angleDeg = parsed.angleDeg;
    spec.valid    = true;
    return spec;
}

// Helper to detect if a border config string is a gradient or solid color
bool isGradientBorderSpec(const std::string& borderSpec) {
    return Hyprexpo::isGradientBorderSpec(borderSpec);
}

SP<Render::ITexture> renderNumberTexture(const std::string& text, const CHyprColor& color, const Vector2D& bufferSize, const float scale, const int fontSize) {
    const auto CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bufferSize.x, bufferSize.y);
    const auto CAIRO        = cairo_create(CAIROSURFACE);

    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    PangoLayout* layout = pango_cairo_create_layout(CAIRO);
    pango_layout_set_text(layout, text.c_str(), -1);

    // font options from config
    const auto fontFamily = Runtime::configString("plugin:hyprexpo:label_font_family", HyprexpoConfig::LABEL_FONT_FAMILY_DEFAULT);
    const bool fontBold   = Runtime::configInt("plugin:hyprexpo:label_font_bold", HyprexpoConfig::LABEL_FONT_BOLD_DEFAULT) != 0;
    const bool fontItalic = Runtime::configInt("plugin:hyprexpo:label_font_italic", HyprexpoConfig::LABEL_FONT_ITALIC_DEFAULT) != 0;
    const bool underline  = Runtime::configInt("plugin:hyprexpo:label_text_underline", HyprexpoConfig::LABEL_TEXT_UNDERLINE_DEFAULT) != 0;
    const bool strike     = Runtime::configInt("plugin:hyprexpo:label_text_strikethrough", HyprexpoConfig::LABEL_TEXT_STRIKETHROUGH_DEFAULT) != 0;

    PangoFontDescription* fontDesc = pango_font_description_from_string(fontFamily.c_str());
    pango_font_description_set_size(fontDesc, fontSize * scale * PANGO_SCALE);
    pango_font_description_set_weight(fontDesc, fontBold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(fontDesc, fontItalic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_layout_set_font_description(layout, fontDesc);
    pango_font_description_free(fontDesc);

    if (underline || strike) {
        PangoAttrList* attrs = pango_attr_list_new();
        if (underline) {
            pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        }
        if (strike) {
            pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));
        }
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
    }

    pango_layout_set_width(layout, bufferSize.x * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

    cairo_set_source_rgba(CAIRO, color.r, color.g, color.b, color.a);

    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_extents(layout, &ink_rect, &logical_rect);

    // center inside the provided buffer using ink rect (accounts for glyph bearings)
    const int    inkW   = std::max(0, ink_rect.width / PANGO_SCALE);
    const int    inkH   = std::max(0, ink_rect.height / PANGO_SCALE);
    const int    inkX   = ink_rect.x / PANGO_SCALE; // can be negative
    const int    inkY   = ink_rect.y / PANGO_SCALE; // can be negative
    const auto centerAdjustX = Runtime::configInt("plugin:hyprexpo:label_center_adjust_x", HyprexpoConfig::LABEL_CENTER_ADJUST_X_DEFAULT);
    const auto centerAdjustY = Runtime::configInt("plugin:hyprexpo:label_center_adjust_y", HyprexpoConfig::LABEL_CENTER_ADJUST_Y_DEFAULT);
    const double xOffset = (bufferSize.x - inkW) / 2.0 - inkX + centerAdjustX;
    const double yOffset = (bufferSize.y - inkH) / 2.0 - inkY + centerAdjustY;

    cairo_move_to(CAIRO, xOffset, yOffset);
    pango_cairo_show_layout(CAIRO, layout);
    g_object_unref(layout);

    cairo_surface_flush(CAIROSURFACE);

    auto tex = g_pHyprRenderer->createTexture(CAIROSURFACE);
    if (tex) {
        tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }

    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);

    return tex;
}

static void damageMonitor(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    g_pOverview->damage();
}

SWorkspacePreviewState applyWorkspacePreviewState(const PHLWORKSPACE& workspace) {
    SWorkspacePreviewState state;
    if (!workspace)
        return state;

    state.visible        = workspace->m_visible;
    state.forceRendering = workspace->m_forceRendering;
    state.alphaValue     = workspace->m_alpha->value();
    state.alphaGoal      = workspace->m_alpha->goal();
    state.offsetValue    = workspace->m_renderOffset->value();
    state.offsetGoal     = workspace->m_renderOffset->goal();

    workspace->m_visible        = true;
    workspace->m_forceRendering = true;
    workspace->m_alpha->setValueAndWarp(1.F);
    *workspace->m_alpha = 1.F;
    workspace->m_renderOffset->setValueAndWarp(Vector2D{});
    *workspace->m_renderOffset = Vector2D{};

    return state;
}

void restoreWorkspacePreviewState(const PHLWORKSPACE& workspace, const SWorkspacePreviewState& state) {
    if (!workspace)
        return;

    workspace->m_visible        = state.visible;
    workspace->m_forceRendering = state.forceRendering;
    workspace->m_alpha->setValueAndWarp(state.alphaValue);
    *workspace->m_alpha = state.alphaGoal;
    workspace->m_renderOffset->setValueAndWarp(state.offsetValue);
    *workspace->m_renderOffset = state.offsetGoal;
}

std::vector<std::pair<PHLWORKSPACE, SWorkspacePreviewState>> applyExclusiveWorkspacePreviewState(const PHLWORKSPACE& targetWorkspace) {
    std::vector<std::pair<PHLWORKSPACE, SWorkspacePreviewState>> states;

    for (const auto& workspaceRef : Runtime::workspaceRefs()) {
        const auto workspace = workspaceRef.lock();
        if (!workspace)
            continue;

        states.push_back({
            workspace,
            {
                .visible        = workspace->m_visible,
                .forceRendering = workspace->m_forceRendering,
                .alphaValue     = workspace->m_alpha->value(),
                .alphaGoal      = workspace->m_alpha->goal(),
                .offsetValue    = workspace->m_renderOffset->value(),
                .offsetGoal     = workspace->m_renderOffset->goal(),
            },
        });

        if (workspace == targetWorkspace) {
            workspace->m_visible        = true;
            workspace->m_forceRendering = true;
            workspace->m_alpha->setValueAndWarp(1.F);
            *workspace->m_alpha = 1.F;
        } else {
            workspace->m_visible        = false;
            workspace->m_forceRendering = false;
            workspace->m_alpha->setValueAndWarp(0.F);
            *workspace->m_alpha = 0.F;
        }

        workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        *workspace->m_renderOffset = Vector2D{};
    }

    return states;
}

void restoreWorkspacePreviewStates(const std::vector<std::pair<PHLWORKSPACE, SWorkspacePreviewState>>& states) {
    for (const auto& [workspace, state] : states)
        restoreWorkspacePreviewState(workspace, state);
}

void normalizeMonitorWorkspaceRenderState(PHLMONITOR monitor) {
    if (!monitor || !monitor->m_activeWorkspace)
        return;

    for (const auto& workspaceRef : Runtime::workspaceRefs()) {
        const auto workspace = workspaceRef.lock();
        if (!workspace || workspace->m_monitor != monitor || workspace->m_isSpecialWorkspace)
            continue;

        const bool active = workspace == monitor->m_activeWorkspace;
        workspace->m_forceRendering = false;

        if (active) {
            workspace->m_visible = true;
            Animation::Workspace::startAnimation(workspace, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
        } else if (!workspace->m_alpha->isBeingAnimated() && !workspace->m_renderOffset->isBeingAnimated()) {
            workspace->m_visible = false;
        }
    }

    for (const auto& window : Runtime::windows()) {
        if (!window || !window->m_isMapped || window->isHidden() || window->m_pinned || !window->m_workspace || window->m_workspace->m_monitor != monitor)
            continue;

        window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->setValueAndWarp(1.F);
        *window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE) = 1.F;
    }
}

bool showPinnedWindowsInPreview() {
    return Runtime::configInt("plugin:hyprexpo:show_pinned_windows", HyprexpoConfig::SHOW_PINNED_WINDOWS_DEFAULT) != 0;
}

std::vector<SPinnedWindowPreviewState> applyPinnedWindowPreviewState(bool showPinnedWindows) {
    std::vector<SPinnedWindowPreviewState> states;
    if (showPinnedWindows)
        return states;

    for (const auto& window : Runtime::windows()) {
        if (!window || !window->m_isMapped || !window->m_pinned)
            continue;

        states.push_back({
            .window = window,
            .workspace = window->m_workspace,
            .pinned = window->m_pinned,
        });

        window->m_pinned = false;
        window->m_workspace.reset();
    }

    return states;
}

void restorePinnedWindowPreviewState(const std::vector<SPinnedWindowPreviewState>& states) {
    for (const auto& state : states) {
        if (!state.window)
            continue;

        state.window->m_workspace = state.workspace;
        state.window->m_pinned    = state.pinned;
    }
}

CPinnedWindowPreviewGuard::CPinnedWindowPreviewGuard(bool showPinnedWindows) : m_states(applyPinnedWindowPreviewState(showPinnedWindows)) {}

CPinnedWindowPreviewGuard::~CPinnedWindowPreviewGuard() {
    restorePinnedWindowPreviewState(m_states);
}

CMonitorPreviewRenderGuard::CMonitorPreviewRenderGuard(PHLMONITOR monitor, CBox& monbox) : m_monitor(monitor) {
    if (!m_monitor)
        return;

    m_transform       = m_monitor->m_transform;
    m_pixelSize       = m_monitor->m_pixelSize;
    m_transformedSize = m_monitor->m_transformedSize;
    m_rotated         = isTransformRotated(m_transform);

    if (!m_rotated)
        return;

    monbox = {{0, 0}, {monbox.h, monbox.w}};
    m_monitor->m_transform       = WL_OUTPUT_TRANSFORM_NORMAL;
    m_monitor->m_pixelSize       = {monbox.w, monbox.h};
    m_monitor->m_transformedSize = {monbox.w, monbox.h};
}

CMonitorPreviewRenderGuard::~CMonitorPreviewRenderGuard() {
    if (!m_monitor)
        return;

    m_monitor->m_transform       = m_transform;
    m_monitor->m_pixelSize       = m_pixelSize;
    m_monitor->m_transformedSize = m_transformedSize;
}

CWorkspacePreviewRenderGuard::CWorkspacePreviewRenderGuard(PHLMONITOR monitor, const PHLWORKSPACE& workspace, const PHLWORKSPACE& startedOn, const PHLWORKSPACE& openSpecial) :
    m_monitor(monitor), m_workspace(workspace), m_startedOn(startedOn), m_openSpecial(openSpecial) {
    if (!m_monitor || !m_workspace)
        return;

    m_previousWorkspace = activateWorkspaceForPreview(m_monitor, m_workspace);
    m_workspaceStates   = applyExclusiveWorkspacePreviewState(m_workspace);
    if (m_workspace != m_startedOn)
        m_windowStates = applyWorkspaceWindowGoalState(m_workspace);

    if (m_workspace == m_startedOn)
        m_monitor->m_activeSpecialWorkspace = m_openSpecial;
}

CWorkspacePreviewRenderGuard::~CWorkspacePreviewRenderGuard() {
    restoreWorkspaceWindowGoalState(m_windowStates);
    restoreWorkspacePreviewStates(m_workspaceStates);
    restoreActiveWorkspaceAfterPreview(m_monitor, m_previousWorkspace);

    if (m_monitor && m_workspace == m_startedOn)
        m_monitor->m_activeSpecialWorkspace.reset();
}

void renderWorkspacePreview(PHLMONITOR monitor, const PHLWORKSPACE& workspace, const PHLWORKSPACE& startedOn, const PHLWORKSPACE& openSpecial, const CBox& monbox) {
    if (workspace) {
        CWorkspacePreviewRenderGuard workspaceGuard{monitor, workspace, startedOn, openSpecial};
        CPinnedWindowPreviewGuard    pinnedWindowGuard{showPinnedWindowsInPreview()};
        g_pHyprRenderer->renderWorkspace(monitor, workspace, Time::steadyNow(), monbox);
        return;
    }

    CPinnedWindowPreviewGuard pinnedWindowGuard{showPinnedWindowsInPreview()};
    g_pHyprRenderer->renderWorkspace(monitor, workspace, Time::steadyNow(), monbox);
}

bool windowVisibleOnWorkspace(const PHLWINDOW& window, const PHLWORKSPACE& workspace) {
    return window && workspace && window->m_workspace == workspace && window->m_isMapped && !window->isHidden() && !window->m_pinned;
}

void settleWorkspaceMoveAnimation(const PHLWINDOW& window) {
    if (!window)
        return;

    window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->resetAllCallbacks();
    window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->setValueAndWarp(1.F);
    *window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE) = 1.F;
    window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->setValueAndWarp(1.F);
    *window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE) = 1.F;
    window->m_monitorMovedFrom                                      = -1;
}

void settleWorkspaceMoveAnimations() {
    for (const auto& window : Runtime::windows()) {
        if (!window)
            continue;

        const bool movingWorkspace = window->m_monitorMovedFrom != -1 || window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->isBeingAnimated() ||
            window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->isBeingAnimated();
        if (!movingWorkspace)
            continue;

        settleWorkspaceMoveAnimation(window);
    }
}

void ensureFramebuffer(COverview::SWorkspaceImage& image, const CBox& monbox, uint32_t drmFormat) {
    if (!image.fb)
        image.fb = g_pHyprRenderer->createFB("hyprexpo");

    if (image.fb->m_size != monbox.size()) {
        image.fb->release();
        image.fb->alloc(monbox.w, monbox.h, drmFormat);
    }
}

std::vector<SWindowPreviewState> applyWorkspaceWindowGoalState(const PHLWORKSPACE& workspace) {
    std::vector<SWindowPreviewState> states;
    if (!workspace)
        return states;

    for (const auto& window : Runtime::windows()) {
        if (!windowVisibleOnWorkspace(window, workspace))
            continue;

        states.push_back({
            .window        = window,
            .positionValue = window->m_realPosition->value(),
            .positionGoal  = window->m_realPosition->goal(),
            .sizeValue     = window->m_realSize->value(),
            .sizeGoal      = window->m_realSize->goal(),
        });

        window->m_realPosition->setValueAndWarp(window->m_realPosition->goal());
        window->m_realSize->setValueAndWarp(window->m_realSize->goal());
    }

    return states;
}

void restoreWorkspaceWindowGoalState(const std::vector<SWindowPreviewState>& states) {
    for (const auto& state : states) {
        if (!state.window)
            continue;

        state.window->m_realPosition->setValueAndWarp(state.positionValue);
        *state.window->m_realPosition = state.positionGoal;
        state.window->m_realSize->setValueAndWarp(state.sizeValue);
        *state.window->m_realSize = state.sizeGoal;
    }
}

static void recalculateWorkspaceLayout(const PHLWORKSPACE& workspace) {
    if (workspace && workspace->m_space)
        workspace->m_space->recalculate(Layout::RECALCULATE_REASON_WORKSPACE_CHANGE);
}

PHLWORKSPACE activateWorkspaceForPreview(PHLMONITOR monitor, const PHLWORKSPACE& workspace) {
    if (!monitor)
        return nullptr;

    const auto previousWorkspace = monitor->m_activeWorkspace;
    if (!workspace)
        return previousWorkspace;

    monitor->m_activeWorkspace = workspace;
    if (g_layoutManager)
        g_layoutManager->recalculateMonitor(monitor);
    recalculateWorkspaceLayout(workspace);

    return previousWorkspace;
}

void restoreActiveWorkspaceAfterPreview(PHLMONITOR monitor, const PHLWORKSPACE& workspace) {
    if (!monitor || !workspace)
        return;

    monitor->m_activeWorkspace = workspace;
    if (g_layoutManager)
        g_layoutManager->recalculateMonitor(monitor);
    recalculateWorkspaceLayout(workspace);
}

void removeOverview(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    if (!g_pOverview)
        return;

    const auto MON = g_pOverview->pMonitor.lock();
    g_pOverview.reset();

    if (!MON)
        return;

    // Force one normal compositor frame after the overview pass is removed.
    // Idle/empty workspaces may not produce their own damage immediately.
    g_pHyprRenderer->damageMonitor(MON);
    Runtime::scheduleFrameForMonitor(MON);
}

static bool shouldShowCursorDuringOverview() {
    return Runtime::configInt("plugin:hyprexpo:show_cursor", HyprexpoConfig::SHOW_CURSOR_DEFAULT) != 0;
}

static void ensureOverviewCursorVisible(bool forceOverviewShape = false, bool refreshPosition = false) {
    if (!shouldShowCursorDuringOverview())
        return;

    g_pHyprRenderer->setCursorHidden(false);
    if (forceOverviewShape)
        Pointer::Cursor::overrideController->setOverride("left_ptr", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    if (refreshPosition)
        g_pInputManager->simulateMouseMovement();
}

// Get workspace method configuration for a specific monitor
// Returns pair of {isCenter, startWorkspaceID}
static std::pair<bool, int> getWorkspaceMethodForMonitor(PHLMONITOR monitor) {
    const std::string monitorName = monitor->m_name;
    const std::string configStr = Runtime::configString("plugin:hyprexpo:workspace_method", HyprexpoConfig::WORKSPACE_METHOD_DEFAULT);
    const auto        parsed = Hyprexpo::resolveWorkspaceMethodForMonitor(configStr, monitorName);

    int methodStartID = monitor->activeWorkspaceID();
    if (!parsed.valid) {
        Log::logger->log(Log::ERR, "[hyprexpo] invalid workspace_method for monitor {}: {} ({})", monitorName, configStr, parsed.error);
        return {true, methodStartID};
    }

    const bool methodCenter = parsed.mode == Hyprexpo::EWorkspaceMethodMode::Center;
    if (parsed.workspace != "current") {
        methodStartID = getWorkspaceIDNameFromString(parsed.workspace).id;
        if (methodStartID == WORKSPACE_INVALID)
            methodStartID = monitor->activeWorkspaceID();
    }

    return {methodCenter, methodStartID};
}

COverview::~COverview() {
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    images.clear(); // otherwise we get a vram leak
    Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    ensureOverviewCursorVisible(false, true);
    if (const auto MON = pMonitor.lock()) {
        normalizeMonitorWorkspaceRenderState(MON);
        MON->m_blurFBDirty = true;
    }
    resetSubmapIfNeeded();
}

COverview::COverview(PHLWORKSPACE startedOn_, bool swipe_) : startedOn(startedOn_), swipe(swipe_) {
    const auto PMONITOR = Runtime::getMonitorFromCursor();
    pMonitor            = PMONITOR;

    const auto columns = Runtime::configInt("plugin:hyprexpo:columns", HyprexpoConfig::COLUMNS_DEFAULT);
    const auto gaps    = Runtime::configInt("plugin:hyprexpo:gaps_in", HyprexpoConfig::GAPS_IN_DEFAULT);
    const auto bgColor = Runtime::configInt("plugin:hyprexpo:bg_col", HyprexpoConfig::BG_COL_DEFAULT);

    SIDE_LENGTH          = Hyprexpo::clampGridColumns(columns);
    GAP_WIDTH            = std::max<Config::INTEGER>(0, gaps);
    BG_COLOR             = bgColor;
    showWorkspaceNumbers = Runtime::configInt("plugin:hyprexpo:show_workspace_numbers", HyprexpoConfig::SHOW_WORKSPACE_NUMBERS_DEFAULT) != 0;

    // Get workspace method for this specific monitor
    auto [methodCenter, methodStartID] = getWorkspaceMethodForMonitor(pMonitor.lock());

    images.resize(SIDE_LENGTH * SIDE_LENGTH);

    // r includes empty workspaces; m skips over them
    const bool    skipEmpty    = Runtime::configInt("plugin:hyprexpo:skip_empty", HyprexpoConfig::SKIP_EMPTY_DEFAULT) != 0;
    const int64_t maxWorkspace = std::max<Config::INTEGER>(0, Runtime::configInt("plugin:hyprexpo:max_workspace", HyprexpoConfig::MAX_WORKSPACE_DEFAULT));
    std::string   selector     = skipEmpty ? "m" : "r";

    if (!skipEmpty && maxWorkspace > 0) {
        const int64_t tileCount = SIDE_LENGTH * SIDE_LENGTH;
        const int64_t maxStart  = std::max<int64_t>(1, maxWorkspace - tileCount + 1);
        const int64_t startID   = methodCenter ? std::clamp<int64_t>(methodStartID - tileCount / 2, 1, maxStart) : std::clamp<int64_t>(methodStartID, 1, maxStart);

        for (size_t i = 0; i < images.size(); ++i) {
            const int64_t workspaceID = startID + i;
            images[i].workspaceID     = workspaceID <= maxWorkspace ? workspaceID : WORKSPACE_INVALID;
        }
    } else if (methodCenter) {
        int currentID = methodStartID;
        int firstID   = currentID;

        int backtracked = 0;

        // Initialize tiles to WORKSPACE_INVALID; cliking one of these results
        // in changing to "emptynm" (next empty workspace). Tiles with this id
        // will only remain if skip_empty is on.
        for (size_t i = 0; i < images.size(); i++) {
            images[i].workspaceID = WORKSPACE_INVALID;
        }

        // Scan through workspaces lower than methodStartID until we wrap; count how many
        for (size_t i = 1; i < images.size() / 2; ++i) {
            currentID = getWorkspaceIDNameFromString(selector + "-" + std::to_string(i)).id;
            if (currentID >= firstID)
                break;

            backtracked++;
            firstID = currentID;
        }

        // Scan through workspaces higher than methodStartID. If using "m"
        // (skip_empty), stop when we wrap, leaving the rest of the workspace
        // ID's set to WORKSPACE_INVALID
        for (size_t i = 0; i < (size_t)(SIDE_LENGTH * SIDE_LENGTH); ++i) {
            auto& image = images[i];
            if ((int64_t)i - backtracked < 0) {
                currentID = getWorkspaceIDNameFromString(selector + std::to_string((int64_t)i - backtracked)).id;
            } else {
                currentID = getWorkspaceIDNameFromString(selector + "+" + std::to_string((int64_t)i - backtracked)).id;
                if (i > 0 && currentID <= firstID)
                    break;
            }
            image.workspaceID = currentID;
        }

    } else {
        int currentID         = methodStartID;
        images[0].workspaceID = currentID;

        auto PWORKSPACESTART = Runtime::getWorkspaceByID(currentID);
        if (!PWORKSPACESTART)
            PWORKSPACESTART = CWorkspace::create(currentID, pMonitor.lock(), std::to_string(currentID));

        pMonitor->m_activeWorkspace = PWORKSPACESTART;

        // Scan through workspaces higher than methodStartID. If using "m"
        // (skip_empty), stop when we wrap, leaving the rest of the workspace
        // ID's set to WORKSPACE_INVALID
        for (size_t i = 1; i < (size_t)(SIDE_LENGTH * SIDE_LENGTH); ++i) {
            auto& image = images[i];
            currentID   = getWorkspaceIDNameFromString(selector + "+" + std::to_string(i)).id;
            if (currentID <= methodStartID)
                break;
            image.workspaceID = currentID;
        }

        pMonitor->m_activeWorkspace = startedOn;
    }

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    Vector2D tileSize       = pMonitor->m_size / SIDE_LENGTH;
    Vector2D tileRenderSize = (pMonitor->m_size - Vector2D{GAP_WIDTH * pMonitor->m_scale, GAP_WIDTH * pMonitor->m_scale} * (SIDE_LENGTH - 1)) / SIDE_LENGTH;
    CBox     monbox{0, 0, tileSize.x * 2, tileSize.y * 2};

    if (!ENABLE_LOWRES)
        monbox = {{0, 0}, pMonitor->m_pixelSize};

    int          currentid = 0;

    CMonitorPreviewRenderGuard monitorGuard{pMonitor.lock(), monbox};

    PHLWORKSPACE openSpecial = PMONITOR->m_activeSpecialWorkspace;
    if (openSpecial)
        PMONITOR->m_activeSpecialWorkspace.reset();

    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    settleWorkspaceMoveAnimations();

    startedOn->m_visible = false;

    for (size_t i = 0; i < (size_t)(SIDE_LENGTH * SIDE_LENGTH); ++i) {
        COverview::SWorkspaceImage& image = images[i];
        ensureFramebuffer(image, monbox, framebufferFormatWithAlpha(PMONITOR->m_output->state->state().drmFormat));

        CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, image.fb);

        clearWithColor(CHyprColor{0, 0, 0, 1.0});

        const auto PWORKSPACE = Runtime::getWorkspaceByID(image.workspaceID);

        if (PWORKSPACE == startedOn)
            currentid = i;

        if (PWORKSPACE) {
            image.pWorkspace = PWORKSPACE;
            renderWorkspacePreview(PMONITOR, PWORKSPACE, startedOn, openSpecial, monbox);
            startedOn->m_visible = false;
        } else {
            renderWorkspacePreview(PMONITOR, PWORKSPACE, startedOn, openSpecial, monbox);
        }

        image.box = {(i % SIDE_LENGTH) * tileRenderSize.x + (i % SIDE_LENGTH) * GAP_WIDTH, (i / SIDE_LENGTH) * tileRenderSize.y + (i / SIDE_LENGTH) * GAP_WIDTH, tileRenderSize.x,
                     tileRenderSize.y};

        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();

        // Capture normalizes rotated monitor geometry; Hyprland's output path adds one more half-turn.
        if (const auto texture = image.fb->getTexture(); texture)
            texture->m_transform = monitorGuard.rotated() ? HYPRUTILS_TRANSFORM_180 : HYPRUTILS_TRANSFORM_NORMAL;
    }

    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;

    PMONITOR->m_activeSpecialWorkspace = openSpecial;
    PMONITOR->m_activeWorkspace        = startedOn;
    startedOn->m_visible               = true;
    Animation::Workspace::startAnimation(startedOn, Animation::Workspace::ANIMATION_TYPE_IN, true, true);

    // zoom on the current workspace.
    // const auto& TILE = images[std::clamp(currentid, 0, SIDE_LENGTH * SIDE_LENGTH)];

    const auto ANIMATIONCONFIG = overviewAnimationConfig();
    Animation::mgr()->createAnimation(pMonitor->m_size * pMonitor->m_size / tileSize, size, ANIMATIONCONFIG, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation((-((pMonitor->m_size / (double)SIDE_LENGTH) * Vector2D{currentid % SIDE_LENGTH, currentid / SIDE_LENGTH}) * pMonitor->m_scale) *
                                          (pMonitor->m_size / tileSize),
                                      pos, ANIMATIONCONFIG, AVARDAMAGE_NONE);

    size->setUpdateCallback(damageMonitor);
    pos->setUpdateCallback(damageMonitor);

    if (!swipe) {
        *size = pMonitor->m_size;
        *pos  = {0, 0};

        size->setCallbackOnEnd([this](auto) { redrawAll(true); });
    }

    openedID = currentid;

    ensureOverviewCursorVisible(true, true);

    lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
    updateHoveredFromMouse();
    kbFocusID = openedID;

    auto onCursorMove = [this](Event::SCallbackInfo& info) {
        if (closing)
            return;

        ensureOverviewCursorVisible();

        info.cancelled    = true;
        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
        updateHoveredFromMouse();
        updateWindowDrag();
    };

    auto onCursorSelect = [this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        if (closing)
            return;

        // If expo hasn't animated in enough to be visible, close silently without
        // consuming the event. This prevents phantom triggers (e.g. a bouncing
        // mouse side-button firing the expo keybind) from swallowing real clicks.
        if (size->getPercent() < 0.05f) {
            close();
            return;
        }

        info.cancelled = true;

        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            beginWindowDrag();
            return;
        }

        if (finishWindowDrag())
            return;

        selectHoveredWorkspace();
        close();
    };

    auto onTouchSelect = [this](Event::SCallbackInfo& info) {
        if (closing)
            return;

        info.cancelled = true;
        selectHoveredWorkspace();
        close();
    };

    mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen([onCursorMove](const Vector2D&, Event::SCallbackInfo& info) { onCursorMove(info); });
    touchMoveHook = Event::bus()->m_events.input.touch.motion.listen([onCursorMove](const ITouch::SMotionEvent&, Event::SCallbackInfo& info) { onCursorMove(info); });
    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen([onCursorSelect](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) { onCursorSelect(event, info); });
    touchDownHook = Event::bus()->m_events.input.touch.down.listen([onTouchSelect](const ITouch::SDownEvent&, Event::SCallbackInfo& info) { onTouchSelect(info); });
    workspaceMoveHook = Event::bus()->m_events.window.moveToWorkspace.listen([this](PHLWINDOW window, PHLWORKSPACE workspace) { onWindowMoveToWorkspace(window, workspace); });

    enterSubmapIfEnabled();
}
