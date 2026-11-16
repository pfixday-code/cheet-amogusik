#pragma once

// Which features resolved successfully against the running build.
struct FeatureStatus {
    bool speed = false;
    bool noclip = false;
    bool light = false;
    bool kill = false;
    bool localPlayerSeen = false;
};
extern FeatureStatus g_feat;

void InitFeatures();
