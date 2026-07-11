#include "index.h"

namespace var {
    /* Esp */
    bool enableesp = true;
    bool box = true;
    bool health = true;
    bool names = true;
    bool snaplines = false;
    bool skeleton = false;
    float esp_distance = 250.f;

    /* Aimbot */
    bool enable_aimbot = false;
    bool humanizer = false;
    bool visiblecheck = true;
    bool predict = true;
    bool randombone = true;
    float aimbot_fov = 50.f;
    bool show_fov = true;
    float aimbot_distance = 1000.f;
    float smoothness = 5.f;

    /* World */
    bool enable_world = true;
    bool droppedItems = false;
    bool raiderStock = false;
    bool showRobots = false;
    bool showArc = false;
    bool showDeadPlayers = true;
    float world_distance = 250.f;
}
