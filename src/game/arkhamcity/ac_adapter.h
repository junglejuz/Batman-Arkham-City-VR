#pragma once
// Batman: Arkham City GOTY (Rocksteady UE3, 32-bit, DX11) adapter.
// Stage 1: build gate + read-only probe of the viewport draw root and CalcSceneView.

#include "game/igame_adapter.h"

namespace bvr::ac {

class ArkhamCityAdapter final : public bvr::game::IGameAdapter {
public:
    uint32_t capabilities() const override;
    bool init(const bvr::pattern_scan::ProcessImage& image) override;
    void setFov(float hfovDeg) override;
    void drawDebugUi() override;
    bool handleCommand(const char* cmd, const char* args) override;
};

bvr::game::IGameAdapter* create_adapter();

} // namespace bvr::ac
