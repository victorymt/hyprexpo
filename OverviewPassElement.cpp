#include "OverviewPassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include "Overview.hpp"

COverviewPassElement::COverviewPassElement() {
    ;
}

std::vector<UP<IPassElement>> COverviewPassElement::draw(Render::CRenderContext& ctx) {
    if (!g_pOverview)
        return {};

    g_pOverview->fullRender(ctx);
    return {};
}

bool COverviewPassElement::needsLiveBlur(Render::CRenderContext&) {
    return false;
}

bool COverviewPassElement::needsPrecomputeBlur(Render::CRenderContext&) {
    return false;
}

std::optional<CBox> COverviewPassElement::boundingBox(Render::CRenderContext&) {
    if (!g_pOverview)
        return std::nullopt;

    const auto MON = g_pOverview->pMonitor.lock();
    if (!MON)
        return std::nullopt;

    return CBox{{}, MON->m_size};
}

CRegion COverviewPassElement::opaqueRegion(Render::CRenderContext&) {
    if (!g_pOverview)
        return CRegion{};

    const auto MON = g_pOverview->pMonitor.lock();
    if (!MON)
        return CRegion{};

    return CBox{{}, MON->m_size};
}

ePassElementType COverviewPassElement::type() {
    return EK_CUSTOM;
}
