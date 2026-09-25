#define WIN32_LEAN_AND_MEAN
#include "Menu.h"
#include "MenuLayout.h"
#include "MenuTheme.h"

#include <cfloat>
#include <algorithm>
#include <string>
#include <vector>
#include <chrono>

#include "../../ThirdParty/ImGui/imgui.h"
#include "../../DMA/Memory.h"
#include "../../Core/Memory.h"
#include "../../Core/Engine.h"
#include "../../Functions/LrtsVisibility.h"
#include "../../Functions/CollisionMirror.h"
#include "../../Functions/WorldScanCommon.h"
#include "../Render.h"
#include "../OverlayHost.h"
#include "../Utils/Variables/index.h"
#include "../Utils/AutoConfig.h"
#include "../../Hardware/KmBox.h"
#include "../../Input/Controller.h"
#include "../../Input/DmaGamepad.h"
#include "../../Input/KeyBind.h"
#include "ImGuiKeybind.h"
#include "../../Core/WorldItemCategory.h"

namespace {

// Row tooltip on whichever sub-widget of a composite row is hovered.
static void RowTip(const char* txt)
{
    if (txt && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", txt);
}

static bool ArcIsSaneLocation(const Vector3& loc)
{
    const double mag =
        static_cast<double>(loc.x) * loc.x +
        static_cast<double>(loc.y) * loc.y +
        static_cast<double>(loc.z) * loc.z;
    return mag > 100.0 && mag < 1.0e18;
}

constexpr float kSidebarWidth = 300.0f;
constexpr ImVec4 kTabRed(0.95f, 0.2f, 0.2f, 1.0f);
constexpr ImVec4 kTabDarkRed(0.4f, 0.08f, 0.08f, 1.0f);
constexpr ImU32 kTabDarkRedU32 = IM_COL32(102, 20, 20, 255);
static int g_currentPage = 0;
static int g_selectedTab = 0;
static bool* g_requestExitPtr = nullptr;

static bool DrawStatusChip(const char* label, bool active, bool clickable, const ImVec4& ok, const ImVec4& bad)
{
    ImGui::PushStyleColor(ImGuiCol_Text, active ? ok : bad);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.35f, 0.35f, 0.35f, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.5f, 0.5f, 0.5f, 0.55f));

    bool clicked = false;
    if (clickable)
        clicked = ImGui::Selectable(label, false, ImGuiSelectableFlags_None,
            ImVec2(ImGui::CalcTextSize(label).x + 4.0f, ImGui::GetTextLineHeight()));
    else
        ImGui::TextUnformatted(label);

    ImGui::PopStyleColor(4);
    return clicked;
}

static void PushMenuContentWrap()
{
    ImGui::PushTextWrapPos(ArcMenuLayout::ContentWrapX());
}

static void WrappedBulletText(const char* text)
{
    ImGui::Bullet();
    ImGui::SameLine();
    PushMenuContentWrap();
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
}

static void CheckboxWithColor(const char* label, bool* value, float color[4], const char* colorId, bool requestSlowCache = false)
{
    if (ArcMenuLayout::CheckboxWithColorRow(label, value, color, colorId)) {
        (void)requestSlowCache;
        AutoConfig_MarkDirty();
    }
}

using ArcMenuLayout::kColorColumnX;
using ArcMenuLayout::kContainerSpColumnX;

static void DrawContainerTypeHeaderRow()
{
    ImGui::TextUnformatted("Type");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + kColorColumnX);
    ImGui::TextUnformatted("Color");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + kContainerSpColumnX);
    ImGui::TextUnformatted("SP");
}

static void ContainerTypeRow(
    const char* label,
    bool* enabled,
    float color[4],
    const char* colorId,
    WorldItemCategory cat,
    const char* rowTooltip)
{
    const float startX = ImGui::GetCursorStartPos().x;
    ImGui::PushID(colorId);
    bool changed = false;

    if (ImGui::Checkbox("##cb", enabled))
        changed = true;
    RowTip(rowTooltip);
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetCursorPosX(startX + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushTextWrapPos(startX + kColorColumnX - 2.f);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 255, 255, 22));
        ImGui::SetTooltip("%s", rowTooltip);
    }

    if (ArcMenuLayout::ColorEditAtColumn(colorId, color))
        changed = true;
    RowTip(rowTooltip);

    ImGui::SameLine();
    ImGui::SetCursorPosX(startX + kContainerSpColumnX);
    const char* suffix = WorldItemCategoryConfigSuffix(cat);
    const std::string spId = suffix ? std::string("##csp_") + suffix : "##csp_unknown";
    bool useSp = WorldCategoryUsesSpContainerRange(cat);
    if (ImGui::Checkbox(spId.c_str(), &useSp)) {
        SetContainerRangeSp(cat, useSp);
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Checked = SP distance for this container type. Unchecked = loot distance.");

    ImGui::PopID();
    if (changed)
        AutoConfig_MarkDirty();
}

static bool LootFilterSliderWithSp(
    const char* label,
    const char* sliderId,
    float* value,
    float vMin,
    float vMax,
    const char* fmt,
    bool* spFlag,
    const char* spTooltip,
    const char* rowTooltip)
{
    const float startX = ImGui::GetCursorStartPos().x;
    ImGui::PushID(sliderId);
    bool changed = false;

    ArcMenuLayout::Label(label);
    ImGui::SetNextItemWidth(kContainerSpColumnX - startX - 4.f);
    if (ImGui::SliderFloat(sliderId, value, vMin, vMax, fmt)) {
        changed = true;
        AutoConfig_MarkDirty();
    }
    RowTip(rowTooltip);

    ImGui::SameLine();
    ImGui::SetCursorPosX(startX + kContainerSpColumnX);
    if (ImGui::Checkbox("##sp", spFlag)) {
        changed = true;
        AutoConfig_MarkDirty();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", spTooltip);

    ImGui::PopID();
    return changed;
}

static bool LootFilterComboWithSp(
    const char* label,
    const char* comboId,
    int* current,
    const char* const* items,
    int count,
    bool* spFlag,
    const char* spTooltip,
    const char* rowTooltip)
{
    const float startX = ImGui::GetCursorStartPos().x;
    ImGui::PushID(comboId);
    bool changed = false;

    ArcMenuLayout::Label(label);
    ImGui::SetNextItemWidth(kContainerSpColumnX - startX - 4.f);
    if (ImGui::Combo(comboId, current, items, count)) {
        changed = true;
        AutoConfig_MarkDirty();
    }
    RowTip(rowTooltip);

    ImGui::SameLine();
    ImGui::SetCursorPosX(startX + kContainerSpColumnX);
    if (ImGui::Checkbox("##sp", spFlag)) {
        changed = true;
        AutoConfig_MarkDirty();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", spTooltip);

    ImGui::PopID();
    return changed;
}

} // namespace

namespace arc_ui {

void DrawMainMenu()
{
    const char* tabs[] = { "ESP", "Radar", "Aimbot", "Settings", "Help", "Close" };
    const char* tabTips[] = {
        "ESP sub-tab: player/bot ESP. Loot sub-tab: container/loot ESP.",
        "Mini-map: enable, size, and world range.",
        "Aimbot controls: FOV, distance, humanizer, and hotkey.",
        "General settings: controller, KmBox, overlay. Debug: diagnostics, caches, camera.",
        "Project disclaimer and high-level feature list.",
        "Exit ARC completely (same as END key)."
    };
    const int tabCount = IM_ARRAYSIZE(tabs);

    const float buttonHeight = 40.0f;
    const float totalButtonHeight = tabCount * buttonHeight;
    const float availableForButtons = ImGui::GetContentRegionAvail().y - 100.0f;
    float spacingBetweenButtons = (availableForButtons - totalButtonHeight) / (tabCount + 1);
    spacingBetweenButtons = (spacingBetweenButtons > 8.0f) ? spacingBetweenButtons : 8.0f;

    ArcMenuUi& ui = ArcMenuTheme();
    ImGui::Dummy(ImVec2(0, spacingBetweenButtons * 0.6f));

    if (ui.headerFont)
        ImGui::PushFont(ui.headerFont);

    const ImVec4 tabBase(ui.headerColor.x, ui.headerColor.y, ui.headerColor.z, 0.92f);
    const ImVec4 tabHover(
        ui.menuAccentColor.x * 0.75f,
        ui.menuAccentColor.y * 0.75f,
        ui.menuAccentColor.z * 0.75f,
        1.0f);
    const ImVec4 tabActive = kTabRed;

    for (int i = 0; i < tabCount; ++i) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, tabBase);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tabHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, tabActive);

        if (ImGui::Button(tabs[i], ImVec2(270, buttonHeight))) {
            if (i == tabCount - 1) {
                if (g_requestExitPtr)
                    *g_requestExitPtr = true;
            } else {
                g_selectedTab = i;
                g_currentPage = 1;
            }
        }
        ArcMenuHoverTooltip(tabTips[i]);

        ImGui::PopStyleColor(4);
        ImGui::Dummy(ImVec2(0, spacingBetweenButtons * 0.7f));
    }

    if (ui.headerFont)
        ImGui::PopFont();
}

} // namespace

void ArcMenuRestartApplication()
{
    WCHAR exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exePath, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void ArcMenuResetController()
{
    if (!g_mem.IsInitialized())
        return;
    g_controller.InitController();
}

void ArcMenuResetKmBox()
{
    g_kmbox.Initialize();
}

int ArcGetMonitorCount()
{
    return OverlayDisplay_GetMonitorCount();
}

static void DrawPlayerEspDiagnostics(Engine& eng)
{
    const std::vector<Engine::PlayerEspDiagnostic> rows =
        eng.GetPlayerEspDiagnostics();
    ImGui::Separator();
    ArcMenuLayout::HoverableText("Player ESP diagnostics");
    ArcMenuHoverTooltip(
        "Read-only snapshot of the player worker's latest resolved values. "
        "Opening this panel does not perform DMA reads.");
    if (rows.empty()) {
        ImGui::TextDisabled("No player cache entries yet.");
        return;
    }

    static uintptr_t selectedActor = 0;
    uintptr_t selectedKey = selectedActor;
    const auto selected = std::find_if(rows.begin(), rows.end(),
        [selectedKey](const Engine::PlayerEspDiagnostic& row) {
            return row.actorKey == selectedKey;
        });
    if (selected == rows.end()) {
        selectedActor = rows.front().actorKey;
        selectedKey = selectedActor;
    }

    const auto initialSelectedIt = std::find_if(rows.begin(), rows.end(),
        [selectedKey](const Engine::PlayerEspDiagnostic& row) {
            return row.actorKey == selectedKey;
        });
    const std::string selectedName =
        (initialSelectedIt->name.empty() ? std::string("Raider") : initialSelectedIt->name)
        + "  [" + std::to_string(static_cast<int>(initialSelectedIt->distance)) + "m]";
    if (ImGui::BeginCombo("Target", selectedName.c_str())) {
        for (const auto& row : rows) {
            const std::string label =
                (row.name.empty() ? std::string("Raider") : row.name)
                + "  [" + std::to_string(static_cast<int>(row.distance)) + "m]";
            if (ImGui::Selectable(label.c_str(), row.actorKey == selectedActor))
                selectedActor = row.actorKey;
        }
        ImGui::EndCombo();
    }
    selectedKey = selectedActor;
    const auto selectedIt = std::find_if(rows.begin(), rows.end(),
        [selectedKey](const Engine::PlayerEspDiagnostic& row) {
            return row.actorKey == selectedKey;
        });
    ImGui::TextDisabled("Nearest cached player is selected automatically.");
    ImGui::Separator();

    const Engine::PlayerEspDiagnostic& row = *selectedIt;
    const char* healthState = row.healthResolved ? "resolved" : "unresolved";
    const char* armorState = (var::health || var::show_armor_line)
        ? (row.armorResolved ? "resolved" : "unresolved")
        : "not read (toggle off)";
    const char* steamState = !var::show_steam_ids
        ? "not read (Steam IDs off)"
        : (row.steamIdResolved
            ? (row.steamId64 ? "resolved" : "component resolved; ID unavailable")
            : "unresolved");
    const char* squadState = !var::show_squad_idx
        ? "not read (Squad tags off)"
        : (row.squadResolved ? "resolved" : "unresolved");
    const bool inventoryRequested = var::show_weapon || var::show_armor_line
        || var::show_player_kit || var::enable_aimbot;
    const char* inventoryState = !inventoryRequested
        ? "not read (features off)"
        : (row.inventoryResolved ? "resolved" : "unresolved");
    const char* dbnoState = !(var::show_dbno_badge || var::show_activity_feed)
        ? "not read (DBNO toggle off)"
        : (row.dbnoResolved ? "resolved" : "unresolved");

    ImGui::BeginChild("##player_esp_diagnostics", ImVec2(0.0f, 300.0f), true);
    ArcMenuLayout::HoverableTextF("Actor: 0x%llx  PS: 0x%llx  %.1fm  %s",
        static_cast<unsigned long long>(row.actorKey),
        static_cast<unsigned long long>(row.actorState),
        row.distance, row.drawing ? "drawing" : "not drawing");
    ArcMenuLayout::HoverableTextF("Health raw: %.2f / %.2f  [%s]",
        row.health, row.maxHealth, healthState);
    ArcMenuLayout::HoverableTextF("Armor raw: %.2f / %.2f  [%s]",
        row.armor, row.maxArmor, armorState);
    if (row.steamId64) {
        ArcMenuLayout::HoverableTextF("SteamID64: %llu  [%s]",
            static_cast<unsigned long long>(row.steamId64), steamState);
    } else {
        ArcMenuLayout::HoverableTextF("SteamID64: unavailable  [%s]", steamState);
    }
    ArcMenuLayout::HoverableTextF("Squad: ptr 0x%llx  index %u  [%s]",
        static_cast<unsigned long long>(row.squadPtr),
        static_cast<unsigned>(row.squadIdx), squadState);
    ArcMenuLayout::HoverableTextF("Inventory: %s  weapon: %s  tier %d  clip %d",
        inventoryState,
        row.weaponName.empty() ? "<none>" : row.weaponName.c_str(),
        row.weaponQuality, row.weaponClip);
    ArcMenuLayout::HoverableTextF("Armor item: %s  plates %.2f  stowed: %s / %s",
        row.armorName.empty() ? "<none>" : row.armorName.c_str(),
        row.armorPlates,
        row.stowedWeapon0.empty() ? "<none>" : row.stowedWeapon0.c_str(),
        row.stowedWeapon1.empty() ? "<none>" : row.stowedWeapon1.c_str());
    ArcMenuLayout::HoverableTextF("DBNO: %s  broken armor: %s  [%s]",
        row.isDbno ? "yes" : "no",
        row.hasBrokenArmor ? "yes" : "no", dbnoState);
    ArcMenuLayout::HoverableTextF("Revive timer: %.1f / %.1f seconds",
        row.reviveRemainS, row.reviveTotalS);
    ArcMenuLayout::HoverableTextF("Kit: tool %s  pouch %s  belt %d  pack %d",
        row.kitTool.empty() ? "<none>" : row.kitTool.c_str(),
        row.kitPouch.empty() ? "<none>" : row.kitPouch.c_str(),
        row.kitBeltSlots, row.kitPackSlots);
    ImGui::EndChild();
}

namespace arc_ui {

void DrawArcEspTab()
{
    if (ImGui::BeginTabBar("##visuals_tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("ESP"))
        {
            ArcMenuHoverTooltip("Player/bot ESP settings. Sub-tabs: Player, Bot, LRTS.");
            if (ImGui::BeginTabBar("##esp_sub_tabs", ImGuiTabBarFlags_None))
            {
                if (ImGui::BeginTabItem("Player"))
                {
                    ArcMenuLayout::CheckboxWithDualColorRow(
                        "Enable ESP",
                        &var::enableesp,
                        var::esp_color_visible,
                        "##esp_vis",
                        var::esp_color_invisible,
                        "##esp_invis",
                        "Master switch for player ESP. Colors: visible (left), invisible (right).");
                    ArcMenuHoverTooltip("Master switch for player ESP. Colors: visible (left), invisible (right).");
                    ArcMenuLayout::SliderFloat("ESP distance", "##esp_distance", &var::esp_distance, 50.f, var::kMaxDistanceSliderM, "%.0f m");
                    ArcMenuHoverTooltip("Maximum distance for player ESP rendering.");
                    ImGui::BeginDisabled(!var::enableesp);
                    ArcMenuLayout::Checkbox("Box", &var::box);
                    ArcMenuHoverTooltip("Draw box around tracked players.");
                    ArcMenuLayout::Checkbox("Health", &var::health);
                    ArcMenuHoverTooltip("HP and shield bar above the player's head.");
                    ArcMenuLayout::Checkbox("Names", &var::names);
                    ArcMenuHoverTooltip("Display player name labels.");
                    ArcMenuLayout::Checkbox("Weapon", &var::show_weapon);
                    ArcMenuHoverTooltip(
                        "Held item in hand — gun, bandage, shield recharger, grenade, defibrillator, etc. "
                        "Guns tint by tier; other items use a neutral color.");
                    ArcMenuLayout::Checkbox("Armor + plates", &var::show_armor_line);
                    ArcMenuHoverTooltip(
                        "Loadout readout: armor tier (I-IV) and plate count under the weapon line.");
                    ArcMenuLayout::Checkbox("Full kit line", &var::show_player_kit);
                    ArcMenuHoverTooltip(
                        "Full kit readout under the loadout: stowed guns (when the weapon line is off), "
                        "stowed tool, safe pouch item with rarity, and belt/backpack slot capacity.");
                    ArcMenuLayout::Checkbox("Downed + revive", &var::show_dbno_badge);
                    ArcMenuHoverTooltip(
                        "DOWNED badge plus a revive/defib countdown ring on downed players.");
                    ArcMenuLayout::Checkbox("Steam IDs", &var::show_steam_ids);
                    ArcMenuHoverTooltip(
                        "Permanent SteamID64 under each player label (stable across raids), with "
                        "[Bot]/[Spec]/[Done] status tags when the player state resolves them.");
                    ArcMenuLayout::CheckboxWithColorRow(
                        "Look arrows",
                        &var::show_look_arrows,
                        var::color_look_arrow,
                        "##col_look_arrow",
                        "Arrow above each player pointing where they are actually aiming "
                        "(ControlRotation, clamped-validated).");
                    ArcMenuHoverTooltip(
                        "Arrow above each player pointing where they are actually aiming "
                        "(ControlRotation, clamped-validated).");
                    ArcMenuLayout::Checkbox("Snaplines", &var::snaplines);
                    ArcMenuHoverTooltip("Draw lines from screen bottom to players.");
                    ArcMenuLayout::Checkbox("Skeleton", &var::skeleton);
                    ArcMenuHoverTooltip("Draw bone skeleton lines on players.");
                    ArcMenuLayout::Checkbox("Silhouette", &var::silhouette);
                    ArcMenuHoverTooltip(
                        "Filled body within max distance. Beyond that, skeleton only (when both are on).");
                    ImGui::Indent(16.f);
                    ImGui::BeginDisabled(!var::silhouette);
                    ArcMenuLayout::Checkbox("Soft fill", &var::silhouette_soft_fill);
                    ArcMenuHoverTooltip("Translucent body fill instead of solid chalk.");
                    ArcMenuLayout::SliderFloat(
                        "Silhouette max (m, 0=25)",
                        "##silhouette_max_distance_m",
                        &var::silhouette_max_distance_m,
                        0.f,
                        var::kMaxDistanceSliderM,
                        "%.0f");
                    ArcMenuHoverTooltip(
                        "Close: silhouette. Past this range: skeleton lines only. 0 defaults to 25 m.");
                    ImGui::EndDisabled();
                    ImGui::Unindent(16.f);
                    ArcMenuLayout::Checkbox("Distance", &var::show_distance);
                    ArcMenuHoverTooltip("Show distance in meters below each player.");
                    if (ArcMenuLayout::Checkbox("Hide allies", &var::hide_allies))
                    ArcMenuHoverTooltip("No ESP or radar on teammates (box, skeleton, silhouette, names, etc.).");
                    ArcMenuLayout::Checkbox("Squad tags", &var::show_squad_idx);
                    ArcMenuHoverTooltip(
                        "Show [S1] / [S2] ... on enemies: real squad membership when it "
                        "resolves, TeamID grouping as the fallback.");
                    ImGui::EndDisabled();
                    DrawPlayerEspDiagnostics(engine);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Bot"))
                {
                    ArcMenuLayout::CheckboxWithDualColorRow(
                        "Show robots",
                        &var::showRobots,
                        var::bot_color_visible,
                        "##bot_vis",
                        var::bot_color_invisible,
                        "##bot_invis",
                        "Draw ARC robots. Colors: visible (left), invisible (right).");
                    ArcMenuHoverTooltip("Draw ARC robots. Colors: visible (left), invisible (right).");
                    ArcMenuLayout::SliderFloat(
                        "Bot ESP distance", "##bot_esp_distance", &var::bot_esp_distance, 50.f, var::kMaxDistanceSliderM, "%.0f m");
                    ArcMenuHoverTooltip("Maximum distance for robot ESP rendering.");
                    ImGui::BeginDisabled(!var::showRobots);
                    ArcMenuLayout::Checkbox("Box##bot", &var::bot_box);
                    ArcMenuHoverTooltip("Draw box around tracked robots.");
                    ArcMenuLayout::Checkbox("Names##bot", &var::bot_names);
                    ArcMenuHoverTooltip("Display robot name labels.");
                    ArcMenuLayout::Checkbox("Snaplines##bot", &var::bot_snaplines);
                    ArcMenuHoverTooltip("Draw lines from screen bottom to robots.");
                    ArcMenuLayout::Checkbox("Distance##bot", &var::bot_show_distance);
                    ArcMenuHoverTooltip("Show distance in meters below each robot.");
                    ArcMenuLayout::Checkbox("Heart", &var::bot_heart);
                    ArcMenuHoverTooltip("Pulsating heart at box center; robot aim targets the same point.");
                    ArcMenuLayout::Checkbox("Loadout line", &var::show_bot_loadout);
                    ArcMenuHoverTooltip(
                        "Best-effort weapon/armor readout for bots that carry an inventory.");
                    ArcMenuLayout::Checkbox("Vision cones", &var::show_bot_vision);
                    ArcMenuHoverTooltip(
                        "Draw each bot's sight cone on the ground (red = Combat). ");
                    ImGui::Indent(16.f);
                    ImGui::BeginDisabled(!var::show_bot_vision);
                    ArcMenuLayout::Checkbox("Alertness tag", &var::show_bot_alertness);
                    ArcMenuHoverTooltip(
                        "Idle / Alert / Searching / Combat under the bot - Combat means it is hunting.");
                    ImGui::EndDisabled();
                    ImGui::Unindent(16.f);
                    ArcMenuLayout::Checkbox("Part damage pips", &var::show_bot_parts);
                    ArcMenuHoverTooltip(
                        "Per-part hp pips over each bot limb (red = blown off) plus a summary line.");
                    if (ArcMenuLayout::CheckboxWithColorRow(
                            "Dead bot bodies",
                            &var::show_dead_bots,
                            var::color_dead_bots,
                            "##col_dead_bots",
                            "Show destroyed/broken robot wrecks; color applies to dead bot ESP and radar."))
                    ArcMenuHoverTooltip("Show destroyed/broken robot wrecks; color applies to dead bot ESP and radar.");
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("LRTS"))
                {
                    ArcMenuHoverTooltip("Encrypted render-time occlusion — auto-discovers XOR key at runtime.");
                    ArcMenuLayout::Checkbox("Enable LRTS", &var::vis_enabled);
                    ArcMenuHoverTooltip("Toggles occlusion-based visibility. Visible = white, behind wall = ESP color.");
                    ArcMenuLayout::Checkbox("Trace to log", &var::lrts_debug_trace);
                    ArcMenuHoverTooltip("Append per-second vis_trace/vis_gate NDJSON rows to debug-c190fb.log.");
                    ArcMenuLayout::Checkbox("Trace tree", &var::lrts_debug_tree);
                    ArcMenuHoverTooltip("Include per-component flag bytes in each vis_trace row (slot + children).");
                    ArcMenuLayout::Checkbox("Burst capture", &var::lrts_debug_burst);
                    ArcMenuHoverTooltip("Log every frame for the first bot it sees (~15s) to measure true LOS-to-verdict latency; auto-disables.");
                    ArcMenuLayout::Checkbox("Draw collision blocks", &var::collision_debug_draw);
                    ArcMenuHoverTooltip("Wireframe of the collision KD-tree triangles near the camera. Red = triangle a LOS ray actually hits.");
                    ArcMenuLayout::Checkbox("Draw LOS rays", &var::collision_debug_rays);
                    ArcMenuHoverTooltip("Green = ray clear, red = blocked by collision geometry. Camera to each bot.");
                    ArcMenuLayout::Checkbox("Enable collision voting", &var::collision_vis_enabled);
                    ArcMenuHoverTooltip("LRTS+collision combined visibility: Occluded wins, collision breaks stale-stamp ties and fills Unknown gaps. Verify rays look right before enabling.");
                    {
                        std::lock_guard<std::mutex> lk(LrtsVis::g_session.mu);
                        ImGui::Separator();
                        ArcMenuLayout::HoverableText("Collision tree");
                        ArcMenuHoverTooltip("Mirrored collision-geometry KD-tree behind the collision visibility vote.");
                        ImGui::Text("Ready: %s  tris: %zu  meshes: %d",
                            CollisionMirror::IsReady() ? "yes" : "no",
                            CollisionMirror::TriangleCount(),
                            CollisionMirror::MeshCount());
                        ArcMenuHoverTooltip("Whether the collision mirror is built, plus its triangle and mesh counts.");
                        ImGui::Text("rebuilds: %d  lastMs: %d",
                            CollisionMirror::RebuildCount(),
                            CollisionMirror::LastRebuildMs());
                        ArcMenuHoverTooltip("How many mirror rebuilds have run and how long the last one took.");
                    }
                    ImGui::BeginDisabled(!var::vis_enabled);
                    ImGui::Separator();
                    ArcMenuLayout::HoverableText("Status");
                    ArcMenuHoverTooltip("Live LRTS session diagnostics: key state, read rates, and verdict counters.");
                    {
                        std::lock_guard<std::mutex> lk(LrtsVis::g_session.mu);
                        const char* state = LrtsVis::g_session.verified ? "LOCKED" : "scanning...";
                        ImGui::Text("Key: %s", state);
                        ArcMenuHoverTooltip("XOR key state for the encrypted render stamps. LOCKED = discovered and verified.");
                        ImGui::Separator();
                        ImGui::Text("[Tuned] Hide: 2 checks | Reveal: instant");
                        ArcMenuHoverTooltip("Verdict hysteresis: two occluded checks to hide, instant reveal.");
                        ImGui::Text("[Tuned] Freshness: 0.15s | BRR fast-hide: on");
                        ArcMenuHoverTooltip("Render-stamp freshness window and the bulk-read fast-hide path.");
                        ImGui::Text("[Tuned] BRR+Occluded=fast | BRR+Vis=counts toward hide");
                        ArcMenuHoverTooltip("How bulk-read verdicts feed the hysteresis counters.");
                        ImGui::Separator();
                        ImGui::Text("WorldTime: %.1f", LrtsVis::g_session.lastWorldTime);
                        ArcMenuHoverTooltip("Game world clock as last observed.");
                        ImGui::Text("Raw Submit: %.3f  OnScreen: %.3f",
                            LrtsVis::g_session.lastRawSubmit,
                            LrtsVis::g_session.lastRawOnScreen);
                        ArcMenuHoverTooltip("Raw Submit/OnScreen render-stamp values as last read (the vis check's 0x480/0x484 floats).");
                        ImGui::Text("Scan passes: %d  candidates: %d",
                            LrtsVis::g_session.scanAttempts,
                            LrtsVis::g_session.pendingCollected);
                        ArcMenuHoverTooltip("Key-scan passes run and candidates still queued.");
                        ImGui::Text("Visible: %d  Occluded: %d  Unknown: %d  ReadFail: %d",
                            LrtsVis::g_session.visibleCount,
                            LrtsVis::g_session.occludedCount,
                            LrtsVis::g_session.unknownCount,
                            LrtsVis::g_session.readFailures);
                        ArcMenuHoverTooltip("Verdict totals across tracked meshes.");
                        ImGui::Text("noMesh: %d  noKey: %d",
                            LrtsVis::g_session.unkNoMesh,
                            LrtsVis::g_session.unkNoKey);
                        ArcMenuHoverTooltip("Unknowns split: missing mesh pointer vs missing XOR key.");
                        ImGui::Text("readZero: %d  keyMiss: %d",
                            LrtsVis::g_session.unkReadZero,
                            LrtsVis::g_session.unkKeyMiss);
                        ArcMenuHoverTooltip("Unknowns split: stamp read as zero vs failed key check.");
                        ImGui::Text("brr0: %d  brrNoBit: %d",
                            LrtsVis::g_session.scanBrrZero,
                            LrtsVis::g_session.scanBrrNoBit);
                        ArcMenuHoverTooltip("Bulk-read scan counters: zero block vs missing flag bit.");
                        ImGui::Text("brrPass: %d  bulkFail: %d  dropFull: %d",
                            LrtsVis::g_session.scanBrrPass,
                            LrtsVis::g_session.scanBulkFail,
                            LrtsVis::g_session.scanDropFull);
                        ArcMenuHoverTooltip("Bulk-read passes, bulk-read failures, and dropped batches.");
                        ImGui::Text("directZero: %d  directInsane: %d",
                            LrtsVis::g_session.directReadZero,
                            LrtsVis::g_session.directInsane);
                        ArcMenuHoverTooltip("Direct decrypt reads that returned zero or implausible values.");
                        ImGui::Text("brrByte: 0x%02X", LrtsVis::g_session.lastBrrByte);
                        ArcMenuHoverTooltip("Last raw flag byte seen by the bulk scan.");
                        ImGui::Text("decrypted: %.2f", LrtsVis::g_session.lastDirectValue);
                        ArcMenuHoverTooltip("Last decrypted render-stamp value.");
                        ImGui::Text("TimeSec: %.2f  RealTimeSec: %.2f",
                            LrtsVis::g_session.lastWorldTime,
                            LrtsVis::g_session.lastRealTime);
                        ArcMenuHoverTooltip("Game clock vs real seconds for read-rate sanity.");
                        // Hysteresis diagnostics: flips should stay near 0
                        // during steady aim; unknownHolds counts how often a
                        // read failure kept the previous verdict instead of
                        // popping the box through a wall.
                        ImGui::Text("Smooth flips: occ->vis %llu  vis->occ %llu",
                            static_cast<unsigned long long>(
                                LrtsVis::g_flipsToVisible.load(std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                LrtsVis::g_flipsToOccluded.load(std::memory_order_relaxed)));
                        ArcMenuHoverTooltip("Hysteresis flips between verdicts; should stay near zero while steadily aiming.");
                        ImGui::Text("Unknown keeps verdict: %llu",
                            static_cast<unsigned long long>(
                                LrtsVis::g_unknownHolds.load(std::memory_order_relaxed)));
                        ArcMenuHoverTooltip("Times a read failure kept the previous verdict instead of popping the box through a wall.");
                    }
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Loot"))
        {
            ArcMenuHoverTooltip("Container/loot ESP: distances, SP ranges, per-type toggles and colors.");
            DrawArcLootContent();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void DrawArcLootContent()
{
    if (ArcMenuLayout::Checkbox("Enable world ESP", &var::enable_world))
    ArcMenuHoverTooltip(
        "Master switch for item/container scanners and world radar blips. Off = no world cache updates.");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Loot");
    ArcMenuHoverTooltip("Loot container ESP settings.");
    if (ArcMenuLayout::Checkbox("Show loot", &var::showLoot))
    ArcMenuHoverTooltip(
        "Master switch for loot/container ESP draw. Off = hide all world loot labels. "
        "On = each Container types row below filters what is shown.");
    ImGui::BeginDisabled(!var::showLoot);
    ArcMenuLayout::Checkbox("Color loot by rarity", &var::loot_rarity_color);
    ArcMenuHoverTooltip("Color dropped pickup labels by item rarity tier.");
    ArcMenuLayout::Label("Loot label color");
    ArcMenuLayout::ColorEditAtColumn("##color_loot", var::color_loot);
    ArcMenuHoverTooltip("Pickup/loot label color when rarity coloring is off.");
    ArcMenuLayout::Checkbox("Show loot value on label", &var::show_loot_value);
    ArcMenuHoverTooltip("Append coin value to resolved pickup names.");
    ArcMenuLayout::Checkbox("Show crate contents", &var::show_crate_contents);
    ArcMenuHoverTooltip(
        "List what is inside each container on its label: 'Crate [Bandage x2 (Rare), Med Kit]'. "
        "Best-effort from the container's spawn list - a crate that does not expose "
        "one just shows the plain label. Resolved stacks carry a rarity tag, and "
        "socket-loot containers show their dispenser ports: ' (2 ports)'.");
    ArcMenuLayout::Checkbox("Show stack counts", &var::show_stack_counts);
    ArcMenuHoverTooltip("Append xN to ground-loot labels when the stack holds more than one item.");
    if (ArcMenuLayout::Checkbox("Grey looted crates", &var::grey_looted_containers))
    ArcMenuHoverTooltip(
        "Crates someone already emptied draw grey with a (Looted) tag so you never "
        "waste a trip. Off = old behavior (opened containers hide unless "
        "'Open container' is on).");
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + kContainerSpColumnX);
    ImGui::TextUnformatted("SP");
    ArcMenuHoverTooltip(
        "Nothing is hidden. Per-filter SP: checked = far (SP distance) for matching pickups; "
        "unchecked = close (loot / category distance). Non-matches stay at close distance.");
    static const char* kMinRarityLabels[] = {
        "Any", "Uncommon+", "Rare+", "Epic+", "Legendary only"
    };
    if (LootFilterSliderWithSp(
            "Min loot value",
            "##loot_min_value",
            &var::loot_min_value,
            0.f,
            5000.f,
            "%.0f c",
            &var::loot_min_val_sp,
            "Never hides. SP checked = far distance for pickups at/above min value.",
            "Does not hide loot. At/above threshold: SP distance when checked, "
            "loot/category distance when unchecked. Below threshold: loot/category distance. 0 = off."))
    ArcMenuHoverTooltip(
        "Does not hide loot. At/above threshold: SP distance when checked, "
        "loot/category distance when unchecked. Below threshold: loot/category distance. 0 = off.");
    if (LootFilterComboWithSp(
            "Min rarity",
            "##loot_min_rarity",
            &var::loot_min_rarity,
            kMinRarityLabels,
            IM_ARRAYSIZE(kMinRarityLabels),
            &var::loot_min_rar_sp,
            "Never hides. SP checked = far distance for pickups at/above min rarity.",
            "Does not hide loot. At/above threshold: SP distance when checked, "
            "loot/category distance when unchecked. Below threshold: loot/category distance. Any = off."))
    ArcMenuHoverTooltip(
        "Does not hide loot. At/above threshold: SP distance when checked, "
        "loot/category distance when unchecked. Below threshold: loot/category distance. Any = off.");
    ArcMenuLayout::SliderFloat(
        "Loot distance", "##loot_distance", &var::loot_distance, 20.f, var::kMaxDistanceSliderM, "%.0f m");
    ArcMenuHoverTooltip("Default loot draw distance. Used whenever SP is unchecked on that row.");
    ArcMenuLayout::SliderFloat(
        "SP", "##container_distance_sp", &var::container_distance_sp, 20.f, var::kMaxDistanceSliderM, "%.0f m");
    ArcMenuHoverTooltip("Extended draw distance. Used only when SP is checked on that row.");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Container types");
    ArcMenuHoverTooltip(
        "Each row: SP unchecked = Loot distance. SP checked = SP distance. Requires Show loot on.");
    DrawContainerTypeHeaderRow();
    ContainerTypeRow("Dropped items", &var::droppedItems, var::color_dropped_items, "##col_dropped",
        WorldItemCategory::DroppedPickup, "Show dropped loot items.");
    ContainerTypeRow("Raider stock", &var::raiderStock, var::color_raider_stock, "##col_raider",
        WorldItemCategory::RaiderCache, "Show raider stock/world pickups list.");
    ContainerTypeRow("ARC entities", &var::showArc, var::color_arc_entities, "##col_arc",
        WorldItemCategory::ArcCargoship, "Show ARC-specific world entities.");
    ContainerTypeRow("Corpses", &var::showDeadPlayers, var::color_world_corpses, "##col_corpse",
        WorldItemCategory::Corpse, "World ESP corpse markers (not the same as downed players in player ESP).");
    ContainerTypeRow("Items", &var::show_world_items, var::color_world_items, "##col_w_items",
        WorldItemCategory::Items, "Show generic world item pickups.");
    ContainerTypeRow("Ammo", &var::show_world_ammo, var::color_world_ammo, "##col_w_ammo",
        WorldItemCategory::Ammo, "Show ammo pickups.");
    ContainerTypeRow("Arc loot", &var::show_world_arc_loot, var::color_world_arc_loot, "##col_w_arc_loot",
        WorldItemCategory::ArcLoot, "Show ARC loot drops.");
    ContainerTypeRow("Backpack", &var::show_world_backpack, var::color_world_backpack, "##col_w_backpack",
        WorldItemCategory::Backpack, "Show backpacks.");
    ContainerTypeRow("Crate", &var::show_world_crate, var::color_world_crate, "##col_w_crate",
        WorldItemCategory::Crate, "Show crates. With 'Show crate contents' on, the label lists what is inside.");
    ContainerTypeRow("Furniture", &var::show_world_furniture, var::color_world_furniture, "##col_w_furniture",
        WorldItemCategory::Furniture, "Show furniture containers.");
    ContainerTypeRow("Grenade", &var::show_world_grenade, var::color_world_grenade, "##col_w_grenade",
        WorldItemCategory::Grenade, "Show grenades.");
    ContainerTypeRow("Harvestable", &var::show_world_harvestable, var::color_world_harvestable, "##col_w_harvestable",
        WorldItemCategory::Harvestable, "Show harvestable resource nodes.");
    ContainerTypeRow("Industrial", &var::show_world_industrial, var::color_world_industrial, "##col_w_industrial",
        WorldItemCategory::Industrial, "Show industrial containers.");
    ContainerTypeRow("Medical", &var::show_world_medical, var::color_world_medical, "##col_w_medical",
        WorldItemCategory::Medical, "Show medical containers and supplies.");
    ContainerTypeRow("Other", &var::show_world_other, var::color_world_other, "##col_w_other",
        WorldItemCategory::Other, "Show uncategorized world items.");
    ContainerTypeRow("Probe", &var::show_world_probe, var::color_world_probe, "##col_w_probe",
        WorldItemCategory::Probe, "Show probes.");
    ContainerTypeRow("Vehicles", &var::show_world_vehicles, var::color_world_vehicles, "##col_w_vehicles",
        WorldItemCategory::Vehicles, "Show vehicles.");
    ContainerTypeRow("Weapon case", &var::show_world_weapon_case, var::color_world_weapon_case, "##col_w_weapon_case",
        WorldItemCategory::WeaponCase, "Show weapon cases.");
    ContainerTypeRow("Field crate", &var::show_world_field_crate, var::color_world_field_crate, "##col_w_field_crate",
        WorldItemCategory::FieldCrate, "Show field crates.");
    ContainerTypeRow("Supply station", &var::show_world_supply_station, var::color_world_supply_station, "##col_w_supply",
        WorldItemCategory::SupplyCallStation, "Show supply call stations.");
    ContainerTypeRow("Keys", &var::show_world_keys, var::color_world_keys, "##col_w_keys",
        WorldItemCategory::Keys, "Show keys.");
    ContainerTypeRow("Locker", &var::show_world_locker, var::color_world_locker, "##col_w_locker",
        WorldItemCategory::Locker, "Show lockers.");
    ContainerTypeRow("Trash", &var::show_world_trash, var::color_world_trash, "##col_w_trash",
        WorldItemCategory::Trash, "Show trash containers.");
    ContainerTypeRow("Open container", &var::show_world_open_container, var::color_world_open_container, "##col_w_open",
        WorldItemCategory::OpenedContainer,
        "Already searched/opened containers. Uses its own color so you can tell at a glance.");
    ContainerTypeRow("Hatches", &var::showHatches, var::color_hatches, "##col_hatches",
        WorldItemCategory::Hatch, "Show extraction hatches (extract state tracked per hatch).");
    ContainerTypeRow("Safe", &var::show_world_safe, var::color_world_safe, "##col_w_safe",
        WorldItemCategory::Safe, "Show safes.");
    ContainerTypeRow("Buried", &var::show_world_buried, var::color_world_buried, "##col_w_buried",
        WorldItemCategory::Buried, "Show buried stashes.");
    ContainerTypeRow("Dead drop", &var::show_world_deaddrop, var::color_world_deaddrop, "##col_w_deaddrop",
        WorldItemCategory::DeadDrop, "Show dead drop containers.");
    ImGui::EndDisabled();
}

void DrawArcRadarTab()
{
    ArcMenuLayout::Checkbox("Enable radar", &var::show_radar);
    ArcMenuHoverTooltip("Top-down blips for players and bots. Works without Enemy ESP enabled.");
    ImGui::BeginDisabled(!var::show_radar);
    ArcMenuLayout::SliderFloat("Map size", "##radar_scale", &var::radar_scale, 30.f, 120.f, "%.0f px");
    ArcMenuHoverTooltip("Radar diameter on screen in pixels.");
    static const char* kRadarShapeLabels[] = { "Circle", "Square" };
    int radarShape = var::radar_shape_circle ? 0 : 1;
    if (ArcMenuLayout::Combo("Shape", "##radar_shape", &radarShape, kRadarShapeLabels, IM_ARRAYSIZE(kRadarShapeLabels)))
        var::radar_shape_circle = (radarShape == 0);
    ArcMenuHoverTooltip("Circle clips blips to range; square uses a box outline.");
    ArcMenuLayout::SliderFloat("World range", "##radar_range", &var::radar_range, 20.f, var::kMaxDistanceSliderM, "%.0f m");
    ArcMenuHoverTooltip("Radar radius for players, bots, and rare loot blips.");
    ArcMenuLayout::Checkbox("Map mode (north-up)", &var::radar_map_mode);
    ArcMenuHoverTooltip(
        "Geographically correct radar: north-up whole-map fit from the minimap bounds "
        "(north-up range view until the bounds resolve).");
    ArcMenuLayout::Checkbox("Dim underground", &var::radar_underground_dim);
    ArcMenuHoverTooltip(
        "Blips on a lower floor draw hollow (detected by Z below you). ");
    ImGui::EndDisabled();

    // HUD and feed are independent dashboard features; they must remain
    // configurable when the radar itself is off.
    ImGui::Separator();
    ArcMenuLayout::Checkbox("Raid HUD", &var::show_raid_hud);
    ArcMenuHoverTooltip(
        "Top-center panel: raid clock, grace timer, phase, live server-side enemy + loot counts.");
    ArcMenuLayout::Checkbox("Activity feed", &var::show_activity_feed);
    ArcMenuHoverTooltip(
        "Bottom-left raid timeline: downs, revives, containers being opened (45s window).");

    ImGui::BeginDisabled(!var::show_radar);
    static const char* kRadarMinRarityLabels[] = { "Rare+", "Epic+", "Legendary only" };
    ArcMenuLayout::Combo(
        "Min rarity",
        "##radar_loot_min_rarity",
        &var::radar_loot_min_rarity,
        kRadarMinRarityLabels,
        IM_ARRAYSIZE(kRadarMinRarityLabels));
    ArcMenuHoverTooltip("Show dropped loot of this rarity or higher on the radar (within world range).");
    ArcMenuLayout::Checkbox("Special", &var::show_radar_special);
    ArcMenuHoverTooltip(
        "Show container types with SP checked under Visuals on the radar (within world range).");
    ArcMenuLayout::Checkbox("Ally arrows", &var::radar_ally_arrows);
    ArcMenuHoverTooltip("Show teammates as arrows pointing their facing direction instead of dots.");
    ImGui::EndDisabled();

    ImGui::Separator();
    ArcMenuLayout::HoverableTextF("While the radar is enabled and this menu is open, click and drag it to move it. Position saves automatically when you close the menu.");
    ArcMenuLayout::HoverableTextF("Players/bots use Visuals ESP colors; rare loot uses rarity colors; SP containers use their type colors.");
}

void DrawArcTriggerbotContent()
{
    ArcMenuLayout::Checkbox("Enable Triggerbot", &var::enable_triggerbot);
    ArcMenuHoverTooltip("Auto-fire when target is within deadzone. Requires KmBox connected.");

    ImGui::Separator();
    static const char* kHoldModeLabels[] = { "Hold key", "Toggle", "Always on" };
    int holdMode = var::trigger_hold_mode;
    if (ArcMenuLayout::Combo("Activation", "##trigger_hold_mode", &holdMode, kHoldModeLabels, IM_ARRAYSIZE(kHoldModeLabels)))
        var::trigger_hold_mode = holdMode;
    ArcMenuHoverTooltip("Hold key = fire while key is held. Toggle = press once to enable, press again to disable. Always on = fires whenever target is in deadzone.");

    ImGui::BeginDisabled(var::trigger_hold_mode == 2);
    ImGui::Keybind("Trigger hotkey", &var::trigger_hold_key);
    ArcMenuHoverTooltip("Key binding for triggerbot activation. Ignored in Always on mode.");
    ImGui::EndDisabled();

    ImGui::Separator();
    ArcMenuLayout::SliderFloat("Deadzone (px)", "##trigger_deadzone", &var::trigger_deadzone_px, 1.f, 50.f, "%.0f");
    ArcMenuHoverTooltip("Max pixel distance from crosshair to fire. Lower = more precise.");
    ImGui::SliderInt("Fire delay (ms)", &var::trigger_fire_delay_ms, 0, 500, "%d ms");
    ArcMenuHoverTooltip("Min time between shots. 0 = no limit. 30-50 for semi-auto, 0 for full-auto.");
    ArcMenuLayout::Checkbox("Auto hold (full-auto)", &var::trigger_auto_hold);
    ArcMenuHoverTooltip("Hold fire button while target is in deadzone instead of clicking repeatedly.");
}

void DrawArcAimbotTab()
{
    if (ImGui::BeginTabBar("##aimbot_tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Aimbot"))
        {
            ArcMenuHoverTooltip("Crosshair and aim assist settings.");
            ArcMenuLayout::Checkbox("Show crosshair", &var::show_crosshair);
            ArcMenuHoverTooltip("Paint-only screen-center crosshair overlay (independent of aimbot).");
    static const char* kCrosshairStyleLabels[] = {
        "Classic", "Dot", "Tight", "Circle", "T static", "T spin", "Cross spin", "Tri spin"
    };
    int crosshairStyle = var::crosshair_style;
    if (ArcMenuLayout::Combo(
            "Style",
            "##crosshair_style",
            &crosshairStyle,
            kCrosshairStyleLabels,
            IM_ARRAYSIZE(kCrosshairStyleLabels)))
        var::crosshair_style = crosshairStyle;
    ArcMenuHoverTooltip("Reticle shape. The spin styles rotate at 'Crosshair spin RPM'.");
    ArcMenuLayout::Label("Crosshair color");
    ArcMenuLayout::ColorEditAtColumn("##crosshair_color", var::crosshair_color);
    ArcMenuHoverTooltip("Screen-center reticle color.");
    ArcMenuLayout::SliderFloat(
        "Crosshair size", "##crosshair_size", &var::crosshair_size, 2.f, 32.f, "%.1f");
    ArcMenuHoverTooltip("Overall reticle scale in pixels.");
    ArcMenuLayout::SliderFloat(
        "Crosshair thickness", "##crosshair_thickness", &var::crosshair_thickness, 0.5f, 6.f, "%.1f");
    ArcMenuHoverTooltip("Reticle line thickness.");
    ArcMenuLayout::SliderFloat(
        "Crosshair gap", "##crosshair_gap", &var::crosshair_gap, 0.f, 16.f, "%.1f");
    ArcMenuHoverTooltip("Gap between the reticle center and its lines.");
    ArcMenuLayout::SliderFloat(
        "Crosshair spin RPM", "##crosshair_spin_rpm", &var::crosshair_spin_rpm, 1.f, 120.f, "%.0f");
    ArcMenuHoverTooltip("Spin rate for T spin, Cross spin, and Tri spin styles.");

    ImGui::Separator();
    ArcMenuLayout::Checkbox("Enable Aimbot", &var::enable_aimbot);
    ArcMenuHoverTooltip("KmBox hardware aim — requires MAKCU or Net device connected.");
    if (ArcMenuLayout::Checkbox("Robot aim", &var::robotAimEnabled))
    ArcMenuHoverTooltip("Aim at robots (works without Show robots on Visuals).");

    static const char* kAimVisLabels[] = { "Always", "Visible only" };
    int aimVis = static_cast<int>(var::aim_vis_mode);
    if (ArcMenuLayout::Combo(
            "Aim visibility",
            "##aim_vis_mode",
            &aimVis,
            kAimVisLabels,
            IM_ARRAYSIZE(kAimVisLabels)))
        var::aim_vis_mode = static_cast<AimVisMode>(aimVis);
    ArcMenuHoverTooltip("Always: aim any target in FOV. Visible only: only aim when vis check says visible.");

    ImGui::Separator();
    ArcMenuLayout::SliderFloat("FOV", "##aimbot_fov", &var::aimbot_fov, 1.f, 360.f, "%.1f");
    ArcMenuHoverTooltip("Target cone for aim pick.");
    ArcMenuLayout::Checkbox("Show FOV", &var::show_fov);
    ArcMenuHoverTooltip("Draw the aim FOV circle on screen.");
    ArcMenuLayout::SliderFloat(
        "Max distance (m)", "##aimbot_distance", &var::aimbot_distance, 5.f, var::kMaxDistanceSliderM, "%.0f");
    ArcMenuHoverTooltip("Do not consider targets beyond this distance.");

    ImGui::Separator();
    ArcMenuLayout::SliderFloat(
        "Deadzone", "##aim_deadzone", &var::aim_deadzone_px, 0.f, 50.f, "%.0f px");
    ArcMenuHoverTooltip("Stop moving when target is within this many pixels of crosshair.");
    ArcMenuLayout::Checkbox("Humanizer", &var::humanizer);
    ArcMenuHoverTooltip("Models reaction delay, a loose settling overshoot on approach, and occasional micro re-aims so the crosshair behaves like a human hand. Off by default.");
    if (var::humanizer) {
        ArcMenuLayout::SliderFloat("Intensity", "##hmzi", &var::humanizer_intensity, 0.f, 3.f, "%.2f");
        ArcMenuHoverTooltip("Scales all injected motion. 0 = disabled feel, 1 = default, higher = visibly sloppier.");
        ArcMenuLayout::SliderFloat("Reaction (ms)", "##hmzr", &var::humanizer_react_ms, 0.f, 600.f, "%.0f");
        ArcMenuHoverTooltip("Base time to begin engaging a new lock, jittered per lock. Higher = slower, more human.");
        ArcMenuLayout::SliderFloat("Overshoot", "##hmzo", &var::humanizer_overshoot, 0.f, 0.6f, "%.2f");
        ArcMenuHoverTooltip("How far the loose flick blows past the target while settling. 0 = no overshoot.");
    }
    ArcMenuLayout::Checkbox("Bullet prediction", &var::predict);
    ArcMenuHoverTooltip("Lead moving targets by travel time (uses cached velocity).");
    ArcMenuLayout::SliderFloat(
        "Bullet speed (cm/s)",
        "##aim_bullet_speed_cm_s",
        &var::aim_bullet_speed_cm_s,
        20000.f,
        150000.f,
        "%.0f");
    ArcMenuHoverTooltip("Travel speed used for lead prediction. Default 80000 (~800 m/s).");
    ArcMenuLayout::Checkbox("Random bone", &var::randombone);
    ArcMenuHoverTooltip("Cycle through torso bones over time. Overrides Aim bone while enabled.");
    static const char* kAimBoneLabels[] = {
        "Head", "Chest", "Pelvis", "Arms", "Legs", "Closest bone"
    };
    int aimBone = static_cast<int>(var::aim_bone_mode);
    if (ArcMenuLayout::Combo(
            "Aim bone",
            "##aim_bone_mode",
            &aimBone,
            kAimBoneLabels,
            IM_ARRAYSIZE(kAimBoneLabels)))
        var::aim_bone_mode = static_cast<AimBoneMode>(aimBone);
    ArcMenuHoverTooltip("Fixed bone, or closest bone inside the FOV circle each tick. Ignored while Random bone is on.");

    static const char* kAimPriorityLabels[] = {
        "FOV", "Distance", "Threat", "Low health", "FOV + distance"
    };
    int aimPriority = static_cast<int>(var::aimbot_priority);
    if (ArcMenuLayout::Combo(
            "Target priority",
            "##aimbot_priority",
            &aimPriority,
            kAimPriorityLabels,
            IM_ARRAYSIZE(kAimPriorityLabels)))
        var::aimbot_priority = static_cast<AimbotPriority>(aimPriority);
    ArcMenuHoverTooltip("How candidates are scored inside the FOV. Threat uses weapon tier when available.");

    ArcMenuLayout::Checkbox("Sticky lock", &var::sticky_target_lock);
    ArcMenuHoverTooltip("Prefer the currently locked target; resists hopping to a marginal rival.");
    ImGui::BeginDisabled(!var::sticky_target_lock);
    ArcMenuLayout::SliderFloat(
        "Sticky FOV bias",
        "##aim_sticky_fov_bias_px",
        &var::aim_sticky_fov_bias_px,
        0.f,
        120.f,
        "%.0f px");
    ArcMenuHoverTooltip("Extra FOV pixels granted to the locked target so it is harder to lose.");
    ImGui::EndDisabled();

    ArcMenuLayout::Checkbox("Loss of sight grace", &var::aim_loss_of_sight_grace_enabled);
    ArcMenuHoverTooltip("Keep aiming at the last known point briefly after the target leaves candidates.");
    ImGui::BeginDisabled(!var::aim_loss_of_sight_grace_enabled);
    {
        float graceMs = static_cast<float>(var::aim_loss_of_sight_grace_ms);
        if (ArcMenuLayout::SliderFloat(
                "Grace (ms)",
                "##aim_loss_of_sight_grace_ms",
                &graceMs,
                0.f,
                2000.f,
                "%.0f"))
            var::aim_loss_of_sight_grace_ms = static_cast<int>(graceMs);
    }
    ArcMenuHoverTooltip("How long aim keeps the last known point after the target drops out of candidates.");
    ImGui::EndDisabled();

    static const char* kAimAlgoLabels[] = { "Linear", "Accelerated" };
    int aimAlgo = static_cast<int>(var::aim_algorithm);
    if (ArcMenuLayout::Combo(
            "Aim curve",
            "##aim_algorithm",
            &aimAlgo,
            kAimAlgoLabels,
            IM_ARRAYSIZE(kAimAlgoLabels)))
        var::aim_algorithm = static_cast<AimAlgorithm>(aimAlgo);
    ArcMenuHoverTooltip("Linear = constant pull speed. Accelerated = faster when farther from crosshair.");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Hardware aim (KMBox / MAKCU)");
    ArcMenuHoverTooltip("Hardware mouse status. Configure the device under Settings -> General.");
    if (!g_kmbox.kmboxConfig.initialized)
        ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.2f, 1.f),
            "KMBox not connected — aim will not run (Settings -> Re-init KmBox)");
    else
        ImGui::TextDisabled("KMBox connected — hardware aim active (read-only DMA, no memory writes).");
    ArcMenuHoverTooltip("Hardware aim status. Configure the device under Settings -> General.");

    ImGui::Separator();
    ImGui::Keybind("Aim hotkey", &var::aim_hold_key);
    ArcMenuHoverTooltip("Hold to run aim assist.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Triggerbot"))
        {
            ArcMenuHoverTooltip("Auto-fire on target: deadzone, delay, and activation mode.");
            DrawArcTriggerbotContent();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void DrawArcSettingsTab()
{
    if (ImGui::BeginTabBar("##settings_tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("General"))
        {
            ArcMenuHoverTooltip("Controller, KmBox, overlay monitor, and device settings.");
            const bool ctrlOk = g_controller.IsReady();
            if (ctrlOk)
                ArcMenuLayout::HoverableTextColoredF(ImVec4(0.f, 1.f, 0.f, 1.f), "Controller: connected");
            else
                ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.f, 0.f, 1.f), "Controller: not connected");
            ArcMenuHoverTooltip("xusb22 kernel read. Pad must be plugged into the TARGET (game) PC.");
            ImGui::Separator();

            if (ImGui::Button("Connect controller"))
                ArcMenuResetController();
            ArcMenuHoverTooltip("Retry xusb22 connect (~10s). Wiggle sticks on target PC.");
            if (ImGui::Button("Re-init KmBox"))
                ArcMenuResetKmBox();
            ArcMenuHoverTooltip("Reconnect hardware mouse device (KmBox/MAKCU).");
            ImGui::Separator();

            const int monitorCount = OverlayDisplay_GetMonitorCount();
            int selected = OverlayDisplay_GetSelectedMonitor();
            if (selected < 0)
                selected = 0;
            if (selected >= monitorCount)
                selected = monitorCount > 0 ? monitorCount - 1 : 0;

            const char* selectedLabel = (monitorCount <= 0)
                ? "No monitors"
                : OverlayDisplay_GetMonitorLabel(selected).c_str();
            ArcMenuLayout::Label("Overlay monitor");
            if (ImGui::BeginCombo("##overlay_monitor", selectedLabel)) {
                for (int i = 0; i < monitorCount; ++i) {
                    const bool isSelected = (i == selected);
                    if (ImGui::Selectable(OverlayDisplay_GetMonitorLabel(i).c_str(), isSelected)) {
                        OverlayDisplay_SetSelectedMonitor(i);
                        g_kmbox.kmboxConfig.monitorIndex = i;
                        OverlayDisplay_ApplySelectedMonitor();
                        AutoConfig_MarkDirty();
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ArcMenuHoverTooltip("Choose which monitor displays the overlay.");

            ArcMenuLayout::HoverableTextF("Monitors detected: %d", ArcGetMonitorCount());
            ArcMenuHoverTooltip("Number of displays currently detected.");
            ImGui::Separator();
            if (ArcMenuLayout::SliderFloat(
                    "Text Size",
                    "##esp_text_scale",
                    &var::esp_text_scale,
                    0.5f,
                    3.0f,
                    "%.2fx")) {
                var::esp_text_scale = (std::max)(0.5f, (std::min)(var::esp_text_scale, 3.0f));
                AutoConfig_MarkDirty();
            }
            ArcMenuHoverTooltip("Global multiplier for all in-game ESP text (player/bot names, weapons, distance, container & item labels). Applies on top of distance scaling. Menu text is unchanged.");
            g_kmbox.renderKmboxSettings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug"))
        {
            ArcMenuHoverTooltip("Diagnostics: offsets, caches, camera, and hardware status.");
            DrawArcDebugContent();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void DrawArcDebugContent()
{
    ArcMenuLayout::HoverableTextF("KmBox type: %s", g_kmbox.kmboxConfig.type.c_str());
    ArcMenuHoverTooltip("Configured hardware mouse type.");
    if (g_kmbox.kmboxConfig.type == "MAKCU") {
        ArcMenuLayout::HoverableTextF("COM port: %s", g_kmbox.kmboxConfig.comPort.c_str());
        ArcMenuHoverTooltip("Serial port used by the MAKCU device.");
    } else if (g_kmbox.kmboxConfig.type == "Net") {
        ArcMenuLayout::HoverableTextF("Net: %s:%s", g_kmbox.kmboxConfig.ip.c_str(), g_kmbox.kmboxConfig.port.c_str());
        ArcMenuHoverTooltip("IP:port of the KmBox Net device.");
    }
    ArcMenuLayout::HoverableTextF("KmBox initialized: %s", g_kmbox.kmboxConfig.initialized ? "yes" : "no");
    ArcMenuHoverTooltip("Whether the hardware mouse connection is active.");
    ArcMenuLayout::HoverableTextF("Auto-config: auto_config.ini");
    ArcMenuHoverTooltip("Settings auto-save to this file.");
    ImGui::Spacing();
    ArcMenuLayout::HoverableTextF("Controller ready: %s", g_controller.IsReady() ? "yes" : "no");
    ArcMenuHoverTooltip("Whether the DMA gamepad is available.");
    ArcMenuLayout::HoverableTextF("Gamepad: %s", DmaGamepad::GetLastStatusMessage());
    ArcMenuHoverTooltip("Latest gamepad status message.");
    ImGui::Spacing();
    ImGui::Separator();
    ArcMenuLayout::HoverableText("Engine caches");
    ArcMenuHoverTooltip("Entity lists populated by the DMA scanner.");
    ArcMenuLayout::HoverableTextF("playerCache: %zu", engine.PlayerCacheCount());
    ArcMenuHoverTooltip("Players currently cached by the DMA scanner.");
    ArcMenuLayout::HoverableTextF("worldCache: %zu", engine.WorldCacheCount());
    ArcMenuHoverTooltip("World loot/container entries cached.");
    ArcMenuLayout::HoverableTextF("robotCache: %zu", engine.RobotCacheCount());
    ArcMenuHoverTooltip("ARC robots currently cached.");
    ArcMenuLayout::HoverableTextF("esp drawable players: %zu", engine.CountEspDrawablePlayers());
    ArcMenuHoverTooltip("Players that would draw right now (passes distance and visibility gates).");
    ImGui::Separator();
    ArcMenuLayout::HoverableText("AggGeom probe");
    ArcMenuHoverTooltip("Read-only simple-collision probe. Reads FKAggregateGeom TArray headers inline at UBodySetup+0xB8. Nothing here feeds ESP.");
    {
        const WorldScan::AggGeomProbeResult probe = WorldScan::GetAggGeomProbeResult();
        ImGui::BeginDisabled(probe.running);
        if (ImGui::Button("Run AggGeom Probe"))
            WorldScan::StartAggGeomProbe();
        ImGui::SetItemTooltip("One-shot walk of every level actor. Run it in raid. Writes agggeom_probe to the verify log.");
        ImGui::EndDisabled();

        if (probe.running) {
            ArcMenuLayout::HoverableText("running...");
            ArcMenuHoverTooltip("Probe is running - results appear when it finishes.");
        } else if (!probe.ran) {
            ArcMenuLayout::HoverableText("not run yet");
            ArcMenuHoverTooltip("Nothing collected yet - run the probe above.");
        } else if (!probe.note.empty()) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", probe.note.c_str());
        } else {
            ArcMenuLayout::HoverableTextF("actors %d  roots %d  mesh %d/%d(legacy)",
                probe.actorsWalked, probe.rootsValid,
                probe.meshFromPrimary, probe.meshFromLegacy);
            ArcMenuHoverTooltip("Level actors walked, valid roots, and meshes resolved from primary vs legacy paths.");
            ArcMenuLayout::HoverableTextF("bodySetups %d  unique %d  nonEmpty %d",
                probe.bodySetupsValid, probe.bodySetupsUnique, probe.bodySetupsNonEmpty);
            ArcMenuHoverTooltip("Body setups found, unique among them, and ones holding actual collision geometry.");
            ArcMenuLayout::HoverableTextColoredF(
                probe.headersRejected ? ImVec4(1.f, 0.6f, 0.2f, 1.f) : ImVec4(0.6f, 0.6f, 0.6f, 1.f),
                "bodySetups rejected: %d", probe.headersRejected);
            ArcMenuHoverTooltip("Whole BodySetups thrown out because a header in the 0x70 block was garbage. Non-zero means the struct was misread, not that collision is absent.");
            ArcMenuLayout::HoverableTextF("sph %d  box %d  sphyl %d  convex %d",
                probe.sphereElems, probe.boxElems, probe.sphylElems, probe.convexElems);
            ArcMenuHoverTooltip("Aggregate geometry element counts by primitive type.");
            ArcMenuLayout::HoverableTextF("tapered %d  levelSet %d  skinnedLevelSet %d",
                probe.taperedCapsuleElems, probe.levelSetElems, probe.skinnedLevelSetElems);
            ArcMenuHoverTooltip("Element counts for the rarer geometry types.");
        }
    }

    ImGui::Separator();
    ArcMenuLayout::HoverableText("TimeSeconds probe");
    ArcMenuHoverTooltip("Finds UWorld's clock offset by sampling UWorld+0x000..0x2000 twice a second apart. LRTS needs it and this build reorders UWorld, so the stock offset does not apply.");
    {
        const WorldScan::TimeSecondsProbeResult clk = WorldScan::GetTimeSecondsProbeResult();
        ImGui::BeginDisabled(clk.running);
        if (ImGui::Button("Run TimeSeconds Probe"))
            WorldScan::StartTimeSecondsProbe();
        ImGui::SetItemTooltip("Takes about a second. Run it in raid. Writes worldclock_candidate to the verify log.");
        ImGui::EndDisabled();

        if (clk.running) {
            ArcMenuLayout::HoverableText("sampling...");
            ArcMenuHoverTooltip("Sampling now - results appear when the probe finishes.");
        } else if (!clk.ran) {
            ArcMenuLayout::HoverableText("not run yet");
            ArcMenuHoverTooltip("Nothing collected yet - run the probe above.");
        } else if (!clk.note.empty()) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", clk.note.c_str());
        } else {
            ArcMenuLayout::HoverableTextF("candidates %d  over %.2fs  bytesChanged %d",
                clk.hits, clk.elapsed, clk.bytesChanged);
            ArcMenuHoverTooltip("Samples gathered and how many bytes changed between the two passes.");
            ArcMenuLayout::HoverableTextColoredF(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                "first: UWorld+0x%X (%s) = %.2f",
                clk.firstOffset, clk.firstIsFloat ? "float" : "double", clk.firstValue);
            ArcMenuHoverTooltip("Winning clock candidate: UWorld offset, value type, and first sampled value.");
        }
    }

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Tick probe");
    ArcMenuHoverTooltip("Finds render-timestamp fields by change rate instead of decrypting them. Run once with a bot in sight, once with it behind cover.");
    {
        const WorldScan::TickProbeResult tk = WorldScan::GetTickProbeResult();
        ImGui::BeginDisabled(tk.running);
        if (ImGui::Button("Run Tick Probe"))
            WorldScan::StartTickProbe();
        ImGui::SetItemTooltip("Takes about 2 seconds. Needs LRTS enabled in a raid so a mesh has been seen.");
        ImGui::EndDisabled();

        if (tk.running) {
            ArcMenuLayout::HoverableText("sampling...");
            ArcMenuHoverTooltip("Sampling now - results appear when the probe finishes.");
        } else if (!tk.ran) {
            ArcMenuLayout::HoverableText("not run yet");
            ArcMenuHoverTooltip("Nothing collected yet - run the probe above.");
        } else if (!tk.note.empty()) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", tk.note.c_str());
        } else {
            ArcMenuLayout::HoverableTextF("samples %d  slots moved %d", tk.samples, tk.slotsChanged);
            ArcMenuHoverTooltip("Tick probe samples and how many candidate slots changed during them.");
            for (int k = 0; k < WorldScan::TickProbeResult::kTop; ++k) {
                if (tk.topCount[k] <= 0)
                    continue;
                ArcMenuLayout::HoverableTextColoredF(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                    "+0x%X changed %d/%d", tk.topOffset[k], tk.topCount[k], tk.samples);
                ArcMenuHoverTooltip("Candidate render-stamp slot and how often it changed across samples.");
            }
        }
    }

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Aimbot runtime");
    ArcMenuHoverTooltip("Live aim state for diagnostics.");
    const int aimKey = var::aim_hold_key ? var::aim_hold_key : VK_SHIFT;
    if (engine.IsInRaid()) {
        ArcMenuLayout::HoverableTextColoredF(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
            "Raid: active (ESP scanning)");
    } else {
        ArcMenuLayout::HoverableTextColoredF(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
            "Raid: lobby / menu (ESP paused)");
    }
    ArcMenuHoverTooltip("ESP scanning state. Scanning only runs while in a raid.");
    ArcMenuLayout::HoverableTextF("In raid raw: %s", engine.IsInRaidRaw() ? "yes" : "no");
    ArcMenuHoverTooltip("Raw in-raid flag read from the game.");
    ArcMenuLayout::HoverableTextF("Aimbot enabled: %s", var::enable_aimbot ? "yes" : "no");
    ArcMenuHoverTooltip("Master switch for hardware aim.");
    ArcMenuLayout::HoverableTextF("Robot aim enabled: %s", var::robotAimEnabled ? "yes" : "no");
    ArcMenuHoverTooltip("Whether bots are valid aim targets.");
    ArcMenuLayout::HoverableTextF("Aim hotkey code: %d", aimKey);
    ArcMenuHoverTooltip("Virtual-key code of the aim hotkey.");
    ArcMenuLayout::HoverableTextF("Aim hotkey held: %s", KeyBindIsHeld(aimKey) ? "yes" : "no");
    ArcMenuHoverTooltip("Whether the aim hotkey is currently pressed.");
    ArcMenuLayout::HoverableTextF("KmBox ready: %s", g_kmbox.kmboxConfig.initialized ? "yes" : "no");
    ArcMenuHoverTooltip("Hardware mouse ready for aim.");
    ImGui::Separator();

    {
        Engine::CameraCache cam{};
        {
            std::shared_lock<std::shared_mutex> lock(engine.m_cameraMutex);
            cam = engine.g_Camera;
        }
        const bool fovOk = cam.FOV > 1.0f && cam.FOV <= 179.0f && ArcIsSaneLocation(cam.Location);
        ImGui::Separator();
        ArcMenuLayout::HoverableText("Camera (engine cache)");
        ArcMenuHoverTooltip("Last camera frame read from the game.");
        if (!fovOk) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Status: bad or missing");
    ArcMenuHoverTooltip("Camera transform is bad or missing — check DMA attach.");
        } else {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "Status: OK");
    ArcMenuHoverTooltip("Camera transform is sane and usable by aim/ESP.");
            ArcMenuLayout::HoverableTextF("Location: %.1f, %.1f, %.1f", cam.Location.x, cam.Location.y, cam.Location.z);
            ArcMenuHoverTooltip("Camera world position from the engine cache.");
            ArcMenuLayout::HoverableTextF("Rotation: %.2f, %.2f, %.2f", cam.Rotation.x, cam.Rotation.y, cam.Rotation.z);
            ArcMenuHoverTooltip("Camera rotation (pitch, yaw, roll) from the engine cache.");
            ArcMenuLayout::HoverableTextF("FOV: %.2f", cam.FOV);
            ArcMenuHoverTooltip("Camera field of view in degrees.");
        }
    }

    if (const char* attached = Memory::GetAttachedGameExe()) {
        ArcMenuLayout::HoverableTextF("Attached: %s", attached);
        ArcMenuHoverTooltip("Game executable the DMA reader is attached to.");
    }
    ArcMenuLayout::HoverableTextF("GWorld: 0x%llX", static_cast<unsigned long long>(engine.GWorld));
    ArcMenuHoverTooltip("Current GWorld pointer read from the game.");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Debug overlay");
    ArcMenuHoverTooltip("On-screen and console diagnostics. Close the menu to see the overlay panel.");
    ArcMenuLayout::Checkbox("Show offset validation", &var::show_debug_overlay);
    ArcMenuHoverTooltip(
        "On-screen CORE/PLAYER/COMPONENT offset panel + console [debug*] tags. Close menu to see overlay.");
    ArcMenuLayout::Checkbox("Skeleton lag probe", &var::debug_skeleton_lag);
    ArcMenuHoverTooltip(
        "Draws box anchor (red) vs bone pelvis (green) with age/delta text, "
        "and prints [debugSkel] lines. Shows why the skeleton trails moving targets.");
    ArcMenuLayout::Checkbox("Aim shake probe", &var::debug_aim_shake);
    ArcMenuHoverTooltip(
        "Prints [debugShake] per error sign-flip (overshoot oscillation) plus "
        "extended [debugAim] fields: command, px/mouse, tick time, switches.");
    ArcMenuLayout::Checkbox("Log ghost bot decisions", &var::debug_ghost_bots);
    ArcMenuHoverTooltip(
        "Write every bot draw rejection and ghost-expiry decision to help/entity_diagnostics.ndjson. "
        "Includes actor address, name, reason, position age, distance, identity, and broken state.");
    ArcMenuLayout::Checkbox("Hatch detect probe", &var::debug_hatch_detect);
    ArcMenuHoverTooltip(
        "Prints [debugHatch] per hatch-candidate actor: fname, class fname, "
        "extract state and why it was/wasn't admitted. Shows why hatches don't draw.");
    ArcMenuLayout::Checkbox("Near loot HUD", &var::show_near_loot_hud);
    ArcMenuHoverTooltip(
        "List nearby ground pickups on-screen. Press F7 to mark the nearest as picked "
        "(hides it via sticky so ESP does not blink it back).");
}

void DrawArcHelpGuideTab()
{
    ImGui::TextWrapped("Disclaimer:");
    WrappedBulletText("Educational use only. Not for sale. Owner use only.");
    ImGui::Spacing();

    ImGui::TextWrapped("ESP tab — Player ESP:");
    WrappedBulletText("Enable ESP + visible/invisible colors, distance slider.");
    WrappedBulletText("Box, health/shield bar, names, weapon (guns tint by tier), snaplines.");
    WrappedBulletText(
        "Silhouette fill within max m (0 = 25 m default); past that, skeleton only when both are on.");
    WrappedBulletText("Hide allies removes teammate ESP.");

    ImGui::Spacing();
    ImGui::TextWrapped("ESP tab — Bot ESP:");
    WrappedBulletText("Show robots + bot colors, bot distance, box/names/snaplines/distance.");
    WrappedBulletText("Dead bot wrecks + color when enabled.");
    WrappedBulletText(
        "Heart: pulsating marker at box center; Robot aim Center uses the same point.");
    WrappedBulletText("Bot HP bars are not available yet (no working reader).");

    ImGui::Spacing();
    ImGui::TextWrapped("ESP tab — World:");
    WrappedBulletText("Enable world ESP: corpses, dropped items, raider stock, ARC entities + colors.");

    ImGui::Spacing();
    ImGui::TextWrapped("ESP tab — Loot sub-tab:");
    WrappedBulletText(
        "Show loot is the master draw gate — category rows are disabled while it is off.");
    WrappedBulletText(
        "Open-container color, rarity tint, loot label color, value on label.");
    WrappedBulletText(
        "Min value/rarity + SP only pick close vs far distance — they never hide loot.");
    WrappedBulletText(
        "Per-container toggles/colors; SP unchecked = Loot distance, checked = SP distance.");
    WrappedBulletText(
        "Player intel: loadout/armor line, DOWNED + revive ring, look arrows, Steam IDs.");
    WrappedBulletText(
        "Bot intel (Bot sub-tab): vision cones with alertness tags, per-part damage pips.");
    WrappedBulletText(
        "Radar tab: raid HUD (clock/counts), activity feed, north-up map mode, underground dim.");
    WrappedBulletText(
        "Server event objects (EmbarkServerEvents) are transient dispatches and are not "
        "readable over DMA - the activity feed reflects persistent interaction state instead.");

    ImGui::Spacing();
    ImGui::TextWrapped("Radar tab:");
    WrappedBulletText("Enable radar; hidden while menu is open. Drag to move when menu closed.");
    WrappedBulletText("Circle or square, size, world range.");
    WrappedBulletText("Blips use Visuals colors; Special = rare loot + SP containers.");
    WrappedBulletText("Works without Enemy ESP enabled.");

    ImGui::Spacing();
    ImGui::TextWrapped("Aimbot tab:");
    WrappedBulletText(
        "Overlay crosshair — screen-center paint-only reticle; style, color, size, gap, and spin RPM.");
    WrappedBulletText("Enable aimbot (MAKCU or Net mouse) + Robot aim (bots even without Show robots).");
    WrappedBulletText("Robot aim: dead center of bot + small in-box shake.");
    WrappedBulletText("FOV, max distance, deadzone, aim hotkey (hold).");
    WrappedBulletText(
        "Sticky lock, loss-of-sight grace, humanizer, prediction, random bone — each optional.");
    WrappedBulletText("Triggerbot sub-tab: auto-fire on target, deadzone, fire delay, hold/toggle modes.");

    ImGui::Spacing();
    ImGui::TextWrapped("Settings tab:");
    WrappedBulletText("General sub-tab: DMA controller status; re-init controller / KmBox; overlay monitor; MAKCU (COM) or Net (IP/UUID) device settings.");
    WrappedBulletText("Debug sub-tab: offset validation, near loot HUD, KmBox/config/gamepad status, engine cache/Aimbot runtime/Camera readouts.");

    ImGui::Spacing();
    ImGui::TextWrapped("Other:");
    WrappedBulletText("DMA attach to PioneerGame before overlay; auto_config.ini auto save/load.");
    WrappedBulletText("Close tab exits the application.");

    ImGui::Spacing();
    ImGui::TextWrapped("Keys:");
    WrappedBulletText("INSERT — toggle menu. END — exit overlay. F7 — mark nearest ground loot picked (Near loot HUD).");
}

void DrawArcHelpTab()
{
    if (ImGui::BeginTabBar("##help_tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Guide"))
        {
            ArcMenuHoverTooltip("Feature overview and key bindings.");
            DrawArcHelpGuideTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace arc_ui

void ArcMenuAddVerticalSpacing(float spacing)
{
    ImGui::Dummy(ImVec2(0.0f, spacing));
    ImGui::Spacing();
}

void ArcMenuHoverTooltip(const char* txt)
{
    const bool itemHovered = ImGui::IsItemHovered();
    ImVec2 labelMin(0.f, 0.f);
    ImVec2 labelMax(0.f, 0.f);
    const bool labelHovered =
        ArcMenuLayout::LastLabelRect(&labelMin, &labelMax) &&
        ImGui::IsMouseHoveringRect(labelMin, labelMax, false);
    if (itemHovered || labelHovered) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::SetTooltip("%s", txt);
        ImGui::PopTextWrapPos();
    }
}

void DrawArcSidebar(bool& menuOpen, bool& requestExit)
{
    ArcMenuLayout::ResetHoverState();
    g_requestExitPtr = &requestExit;
    if (!menuOpen) {
        g_currentPage = 0;
        return;
    }

    ArcMenuUi& ui = ArcMenuTheme();
    ui.menuAccentColor = kTabRed;
    ui.headerColor = kTabDarkRed;
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 darkBg(0.16f, 0.16f, 0.16f, 0.95f);
    const ImVec4 logoBg(0.22f, 0.22f, 0.22f, 1.0f);
    const ImVec4 tabRed = kTabRed;
    const ImVec4 tabBorder = kTabDarkRed;

    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    const float screenW = vp.x > 0 ? vp.x : static_cast<float>(GetSystemMetrics(SM_CXSCREEN));
    const float screenH = vp.y > 0 ? vp.y : static_cast<float>(GetSystemMetrics(SM_CYSCREEN));

    ImGui::SetNextWindowPos(ImVec2(screenW - kSidebarWidth, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSidebarWidth, screenH), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, darkBg);
    ImGui::PushStyleColor(ImGuiCol_Border, tabBorder);
    ImGui::PushStyleColor(ImGuiCol_Separator, tabBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 7.0f);

    ImGui::Begin("##Sidebar", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar);

    if (ui.logoTexture != 0) {
        float texW = static_cast<float>(ui.logoWidth);
        float texH = static_cast<float>(ui.logoHeight);
        if (texW <= 0 || texH <= 0) {
            texW = 1080.0f;
            texH = 608.0f;
        }

        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const float scale = contentWidth / texW;
        const float drawW = contentWidth;
        const float drawH = texH * scale;

        // Dark grey background behind image
        ImVec2 imgMin = ImGui::GetCursorScreenPos();
        ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);
        ImGui::GetWindowDrawList()->AddRectFilled(imgMin, imgMax, IM_COL32(55, 55, 55, 255));

        ImGui::Image(ui.logoTexture, ImVec2(drawW, drawH));
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Spacing();
    if (ui.headerFont) {
        ImGui::PushFont(ui.headerFont);
        const char* sub = "Buku's Arc Manager";
        const float subW = ImGui::CalcTextSize(sub).x;
        ImGui::SetCursorPosX((kSidebarWidth - subW) * 0.5f);
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "%s", sub);
        ImGui::PopFont();
    }

    ImGui::Spacing();

    const float metricsHeight = 100.0f;
    const float availableHeight = ImGui::GetContentRegionAvail().y;
    const float mainContentHeight = availableHeight - metricsHeight - 60.0f;

    ImGui::BeginChild("Content", ImVec2(0, mainContentHeight), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    PushMenuContentWrap();

    if (ui.regularFont)
        ImGui::PushFont(ui.regularFont);

    if (g_currentPage == 0) {
        arc_ui::DrawMainMenu();
    } else {
        switch (g_selectedTab) {
        case 0: arc_ui::DrawArcEspTab(); break;
        case 1: arc_ui::DrawArcRadarTab(); break;
        case 2: arc_ui::DrawArcAimbotTab(); break;
        case 3: arc_ui::DrawArcSettingsTab(); break;
        case 4: arc_ui::DrawArcHelpTab(); break;
        default: break;
        }
    }

    if (ui.regularFont)
        ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    if (g_currentPage > 0) {
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ui.headerColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(
            ui.headerColor.x * 1.2f, ui.headerColor.y * 1.2f, ui.headerColor.z * 1.2f, ui.headerColor.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(
            ui.headerColor.x * 0.8f, ui.headerColor.y * 0.8f, ui.headerColor.z * 0.8f, ui.headerColor.w));

        if (ui.headerFont)
            ImGui::PushFont(ui.headerFont);

        constexpr float buttonWidth = 270.0f;
        constexpr float buttonHeight = 40.0f;
        ImGui::SetCursorPosX((kSidebarWidth - buttonWidth) * 0.5f);
        if (ImGui::Button("Back", ImVec2(buttonWidth, buttonHeight)))
            g_currentPage = 0;
        ArcMenuHoverTooltip("Return to main tab selection page.");

        ImGui::SetCursorPosX((kSidebarWidth - buttonWidth) * 0.5f);
        if (ImGui::Button("Close", ImVec2(buttonWidth, buttonHeight)))
            requestExit = true;
        ArcMenuHoverTooltip("Exit ARC completely (same as END key).");

        if (ui.headerFont)
            ImGui::PopFont();
        ImGui::PopStyleColor(3);
    }

    ImGui::Separator();
    ImGui::Spacing();

    constexpr ImVec2 metricsSize(264.0f, 52.0f);
    ImGui::SetCursorPosX((kSidebarWidth - metricsSize.x) * 0.5f);
    const ImVec2 metricsStart = ImGui::GetCursorScreenPos();

    ImGui::GetWindowDrawList()->AddRectFilled(
        metricsStart,
        ImVec2(metricsStart.x + metricsSize.x, metricsStart.y + metricsSize.y),
        ImColor(ui.headerColor),
        4.0f);
    ImGui::GetWindowDrawList()->AddRect(
        metricsStart,
        ImVec2(metricsStart.x + metricsSize.x, metricsStart.y + metricsSize.y),
        ImColor(tabBorder),
        4.0f, 0, 2.0f);

    const float chipRowH = ImGui::GetTextLineHeight();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (metricsSize.y - chipRowH) * 0.5f);

    if (ui.regularFont)
        ImGui::PushFont(ui.regularFont);

    const ImVec4 ok(0.35f, 1.0f, 0.35f, 1.0f);
    const ImVec4 bad(1.0f, 0.35f, 0.35f, 1.0f);

    const bool dmaOk = g_mem.IsInitialized();
    const bool ctrlOk = g_controller.IsReady();
    const bool kmOk = g_kmbox.kmboxConfig.initialized;
    const char* kmLabel = (g_kmbox.kmboxConfig.type == "MAKCU") ? "MAKCU" : "KMBOX";

    constexpr float chipGap = 18.0f;
    const float rowW =
        ImGui::CalcTextSize("DMA").x + chipGap +
        ImGui::CalcTextSize("CTRL").x + chipGap +
        ImGui::CalcTextSize(kmLabel).x;

    ImGui::SetCursorPosX((kSidebarWidth - rowW) * 0.5f);

    if (DrawStatusChip("DMA", dmaOk, dmaOk, ok, bad)) {
        ArcMenuRestartApplication();
        requestExit = true;
    }
    ArcMenuHoverTooltip("Click to restart ARC (DMA connected).");

    ImGui::SameLine(0, chipGap);
    if (DrawStatusChip("CTRL", ctrlOk, true, ok, bad))
        ArcMenuResetController();
    ArcMenuHoverTooltip("Click to re-initialize DMA gamepad (CTRL).");

    ImGui::SameLine(0, chipGap);
    if (DrawStatusChip(kmLabel, kmOk, true, ok, bad))
        ArcMenuResetKmBox();
    ArcMenuHoverTooltip("Click to re-initialize KmBox / MAKCU device.");

    ImGui::Dummy(ImVec2(0.0f, metricsSize.y - chipRowH));

    if (ui.regularFont)
        ImGui::PopFont();

    {
        const ImVec2 wPos = ImGui::GetWindowPos();
        const ImVec2 wSize(ImGui::GetWindowWidth(), ImGui::GetWindowHeight());
        const ImVec2 wMax(wPos.x + wSize.x, wPos.y + wSize.y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(wPos, wMax, kTabDarkRedU32, 0.0f, 0, 7.0f);
        dl->AddRectFilled(wPos, ImVec2(wPos.x + 7.0f, wMax.y), kTabDarkRedU32);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);

    (void)style;
}