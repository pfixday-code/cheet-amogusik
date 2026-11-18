#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Which features resolved successfully against the running build.
struct FeatureStatus {
    bool speed = false;
    bool noclip = false;
    bool light = false;
    bool kill = false;
    bool zoom = false;
    bool reveal = false;
    bool ghosts = false;
    bool wallVision = false;
    bool invisible = false;
    bool players = false;
    bool teleport = false;
    bool bring = false;
    bool spectate = false;
    bool report = false;
    bool completeTasks = false;
    bool localPlayerSeen = false;
};
extern FeatureStatus g_feat;

// Player actions are queued by the Android UI and executed only on Unity's game thread.
enum PlayerAction : int {
    ACTION_TELEPORT_TO = 1,
    ACTION_BRING = 2,
    ACTION_SPECTATE = 3,
    ACTION_RETURN_CAMERA = 4,
    ACTION_REPORT_BODY = 5,
    ACTION_COMPLETE_TASKS = 6,
};

struct PlayerEntry {
    int id = -1;
    std::string name;
    bool dead = false;
    bool disconnected = false;
    bool local = false;
};

struct PlayerActionState {
    uint32_t completed = 0;
    bool pending = false;
    bool spectating = false;
    std::string message;
};

// Getting a snapshot schedules a cheap game-thread refresh. The returned copy is safe on JNI/UI
// threads and never contains live Unity pointers.
std::vector<PlayerEntry> GetPlayersSnapshot();
bool QueuePlayerAction(int action, int targetId);
PlayerActionState GetPlayerActionState();

void InitFeatures();
