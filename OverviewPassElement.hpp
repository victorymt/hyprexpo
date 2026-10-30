#pragma once
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

class COverview;

class COverviewPassElement : public IPassElement {
  public:
    COverviewPassElement();
    virtual ~COverviewPassElement() = default;

    // Hyprland 0.57 threaded Render::CRenderContext through every IPassElement
    // virtual, which is what turned this class abstract after the upgrade.
    virtual std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx);
    virtual bool                          needsLiveBlur(Render::CRenderContext& ctx);
    virtual bool                          needsPrecomputeBlur(Render::CRenderContext& ctx);
    virtual std::optional<CBox>           boundingBox(Render::CRenderContext& ctx);
    virtual CRegion                       opaqueRegion(Render::CRenderContext& ctx);
    virtual ePassElementType              type();

    virtual const char*                   passName() {
        return "COverviewPassElement";
    }
};
