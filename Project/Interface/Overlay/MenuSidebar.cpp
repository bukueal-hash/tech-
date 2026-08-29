#define WIN32_LEAN_AND_MEAN
#include "Menu.h"
#include "MenuLayout.h"
#include "MenuTheme.h"

#include <cfloat>
#include <string>
#include <vector>
#include <chrono>

#include "../../ThirdParty/ImGui/imgui.h"
#include "../../DMA/Memory.h"
#include "../../Core/Memory.h"
#include "../../Core/Engine.h"
#include "../../Functions/LrtsVisibility.h"
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
#include "../../Dumper/Dumper.h"

namespace {

static void RequestArcSlowCache() {}

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
    if (ArcMenuLayout::CheckboxWithColorRow(label, value, color, colorId) && requestSlowCache)
        RequestArcSlowCache();
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
    WorldItemCategory cat)
{
    const float startX = ImGui::GetCursorStartPos().x;
    ImGui::PushID(colorId);
    bool changed = false;

    if (ImGui::Checkbox("##cb", enabled))
        changed = true;
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetCursorPosX(startX + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushTextWrapPos(startX + kColorColumnX - 2.f);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered())
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(255, 255, 255, 22));

    if (ArcMenuLayout::ColorEditAtColumn(colorId, color))
        changed = true;

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
        RequestArcSlowCache();
}

static bool LootFilterSliderWithSp(
    const char* label,
    const char* sliderId,
    float* value,
    float vMin,
    float vMax,
    const char* fmt,
    bool* spFlag,
    const char* spTooltip)
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
    const char* spTooltip)
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
                        "##esp_invis");
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
                        RequestArcSlowCache();
                    ArcMenuHoverTooltip("No ESP or radar on teammates (box, skeleton, silhouette, names, etc.).");
                    ArcMenuLayout::Checkbox("Squad tags", &var::show_squad_idx);
                    ArcMenuHoverTooltip("Show @1 / @2 / @3 on enemies to identify which squad they belong to.");
                    ImGui::EndDisabled();
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
                        "##bot_invis");
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
                    if (ArcMenuLayout::CheckboxWithColorRow(
                            "Dead bot bodies",
                            &var::show_dead_bots,
                            var::color_dead_bots,
                            "##col_dead_bots"))
                        RequestArcSlowCache();
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
                    ImGui::BeginDisabled(!var::vis_enabled);
                    ImGui::Separator();
                    ArcMenuLayout::HoverableText("Status");
                    {
                        std::lock_guard<std::mutex> lk(LrtsVis::g_session.mu);
                        const char* state = LrtsVis::g_session.verified ? "LOCKED" : "scanning...";
                        ImGui::Text("Key: %s", state);
                        ImGui::Text("WorldTime: %.1f", LrtsVis::g_session.lastWorldTime);
                        ImGui::Text("Raw Submit: %.3f  OnScreen: %.3f",
                            LrtsVis::g_session.lastRawSubmit,
                            LrtsVis::g_session.lastRawOnScreen);
                        ImGui::Text("Scan passes: %d  candidates: %d",
                            LrtsVis::g_session.scanAttempts,
                            LrtsVis::g_session.pendingCollected);
                        ImGui::Text("Visible: %d  Occluded: %d  Unknown: %d  ReadFail: %d",
                            LrtsVis::g_session.visibleCount,
                            LrtsVis::g_session.occludedCount,
                            LrtsVis::g_session.unknownCount,
                            LrtsVis::g_session.readFailures);
                        ImGui::Text("noMesh: %d  noKey: %d",
                            LrtsVis::g_session.unkNoMesh,
                            LrtsVis::g_session.unkNoKey);
                        ImGui::Text("readZero: %d  keyMiss: %d",
                            LrtsVis::g_session.unkReadZero,
                            LrtsVis::g_session.unkKeyMiss);
                        ImGui::Text("brr0: %d  brrNoBit: %d",
                            LrtsVis::g_session.scanBrrZero,
                            LrtsVis::g_session.scanBrrNoBit);
                        ImGui::Text("brrPass: %d  bulkFail: %d",
                            LrtsVis::g_session.scanBrrPass,
                            LrtsVis::g_session.scanBulkFail);
                        ImGui::Text("directZero: %d  directInsane: %d",
                            LrtsVis::g_session.directReadZero,
                            LrtsVis::g_session.directInsane);
                        ImGui::Text("brrByte: 0x%02X", LrtsVis::g_session.lastBrrByte);
                        ImGui::Text("decrypted: %.2f", LrtsVis::g_session.lastDirectValue);
                        ImGui::Text("TimeSec: %.2f  RealTimeSec: %.2f",
                            LrtsVis::g_session.lastWorldTime,
                            LrtsVis::g_session.lastRealTime);
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
        RequestArcSlowCache();
    ArcMenuHoverTooltip(
        "Master switch for item/container scanners and world radar blips. Off = no world cache updates.");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Loot");
    ArcMenuHoverTooltip("Loot container ESP settings.");
    if (ArcMenuLayout::Checkbox("Show loot", &var::showLoot))
        RequestArcSlowCache();
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
            "Never hides. SP checked = far distance for pickups at/above min value."))
        RequestArcSlowCache();
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
            "Never hides. SP checked = far distance for pickups at/above min rarity."))
        RequestArcSlowCache();
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
        WorldItemCategory::DroppedPickup);
    ArcMenuHoverTooltip("Show dropped loot items.");
    ContainerTypeRow("Raider stock", &var::raiderStock, var::color_raider_stock, "##col_raider",
        WorldItemCategory::RaiderCache);
    ArcMenuHoverTooltip("Show raider stock/world pickups list.");
    ContainerTypeRow("ARC entities", &var::showArc, var::color_arc_entities, "##col_arc",
        WorldItemCategory::ArcCargoship);
    ArcMenuHoverTooltip("Show ARC-specific world entities.");
    ContainerTypeRow("Corpses", &var::showDeadPlayers, var::color_world_corpses, "##col_corpse",
        WorldItemCategory::Corpse);
    ArcMenuHoverTooltip("World ESP corpse markers (not the same as downed players in player ESP).");
    ContainerTypeRow("Items", &var::show_world_items, var::color_world_items, "##col_w_items", WorldItemCategory::Items);
    ContainerTypeRow("Ammo", &var::show_world_ammo, var::color_world_ammo, "##col_w_ammo", WorldItemCategory::Ammo);
    ContainerTypeRow("Arc loot", &var::show_world_arc_loot, var::color_world_arc_loot, "##col_w_arc_loot", WorldItemCategory::ArcLoot);
    ContainerTypeRow("Backpack", &var::show_world_backpack, var::color_world_backpack, "##col_w_backpack", WorldItemCategory::Backpack);
    ContainerTypeRow("Crate", &var::show_world_crate, var::color_world_crate, "##col_w_crate", WorldItemCategory::Crate);
    ContainerTypeRow("Furniture", &var::show_world_furniture, var::color_world_furniture, "##col_w_furniture", WorldItemCategory::Furniture);
    ContainerTypeRow("Grenade", &var::show_world_grenade, var::color_world_grenade, "##col_w_grenade", WorldItemCategory::Grenade);
    ContainerTypeRow("Harvestable", &var::show_world_harvestable, var::color_world_harvestable, "##col_w_harvestable", WorldItemCategory::Harvestable);
    ContainerTypeRow("Industrial", &var::show_world_industrial, var::color_world_industrial, "##col_w_industrial", WorldItemCategory::Industrial);
    ContainerTypeRow("Medical", &var::show_world_medical, var::color_world_medical, "##col_w_medical", WorldItemCategory::Medical);
    ContainerTypeRow("Other", &var::show_world_other, var::color_world_other, "##col_w_other", WorldItemCategory::Other);
    ContainerTypeRow("Probe", &var::show_world_probe, var::color_world_probe, "##col_w_probe", WorldItemCategory::Probe);
    ContainerTypeRow("Vehicles", &var::show_world_vehicles, var::color_world_vehicles, "##col_w_vehicles", WorldItemCategory::Vehicles);
    ContainerTypeRow("Weapon case", &var::show_world_weapon_case, var::color_world_weapon_case, "##col_w_weapon_case", WorldItemCategory::WeaponCase);
    ContainerTypeRow("Field crate", &var::show_world_field_crate, var::color_world_field_crate, "##col_w_field_crate", WorldItemCategory::FieldCrate);
    ContainerTypeRow("Supply station", &var::show_world_supply_station, var::color_world_supply_station, "##col_w_supply", WorldItemCategory::SupplyCallStation);
    ContainerTypeRow("Keys", &var::show_world_keys, var::color_world_keys, "##col_w_keys", WorldItemCategory::Keys);
    ContainerTypeRow("Locker", &var::show_world_locker, var::color_world_locker, "##col_w_locker", WorldItemCategory::Locker);
    ContainerTypeRow("Trash", &var::show_world_trash, var::color_world_trash, "##col_w_trash", WorldItemCategory::Trash);
    ContainerTypeRow("Open container", &var::show_world_open_container, var::color_world_open_container, "##col_w_open", WorldItemCategory::OpenedContainer);
    ContainerTypeRow("Hatches", &var::showHatches, var::color_hatches, "##col_hatches", WorldItemCategory::Hatch);
    ArcMenuHoverTooltip("Already searched/opened containers. Uses its own color so you can tell at a glance.");
    ContainerTypeRow("Safe", &var::show_world_safe, var::color_world_safe, "##col_w_safe", WorldItemCategory::Safe);
    ContainerTypeRow("Buried", &var::show_world_buried, var::color_world_buried, "##col_w_buried", WorldItemCategory::Buried);
    ContainerTypeRow("Dead drop", &var::show_world_deaddrop, var::color_world_deaddrop, "##col_w_deaddrop", WorldItemCategory::DeadDrop);
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
    ImGui::Separator();
    ArcMenuLayout::HoverableTextF("While this menu is open, click and drag the radar to move it. Position saves automatically when you close the menu.");
    ArcMenuLayout::HoverableTextF("Players/bots use Visuals ESP colors; rare loot uses rarity colors; SP containers use their type colors.");
    ImGui::EndDisabled();
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
        RequestArcSlowCache();
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
    ArcMenuLayout::HoverableTextF("worldCache: %zu", engine.WorldCacheCount());
    ArcMenuLayout::HoverableTextF("robotCache: %zu", engine.RobotCacheCount());
    ArcMenuLayout::HoverableTextF("esp drawable players: %zu", engine.CountEspDrawablePlayers());
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
        } else if (!probe.ran) {
            ArcMenuLayout::HoverableText("not run yet");
        } else if (!probe.note.empty()) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", probe.note.c_str());
        } else {
            ArcMenuLayout::HoverableTextF("actors %d  roots %d  mesh %d/%d(legacy)",
                probe.actorsWalked, probe.rootsValid,
                probe.meshFromPrimary, probe.meshFromLegacy);
            ArcMenuLayout::HoverableTextF("bodySetups %d  unique %d  nonEmpty %d",
                probe.bodySetupsValid, probe.bodySetupsUnique, probe.bodySetupsNonEmpty);
            ArcMenuLayout::HoverableTextColoredF(
                probe.headersRejected ? ImVec4(1.f, 0.6f, 0.2f, 1.f) : ImVec4(0.6f, 0.6f, 0.6f, 1.f),
                "bodySetups rejected: %d", probe.headersRejected);
            ArcMenuHoverTooltip("Whole BodySetups thrown out because a header in the 0x70 block was garbage. Non-zero means the struct was misread, not that collision is absent.");
            ArcMenuLayout::HoverableTextF("sph %d  box %d  sphyl %d  convex %d",
                probe.sphereElems, probe.boxElems, probe.sphylElems, probe.convexElems);
            ArcMenuLayout::HoverableTextF("tapered %d  levelSet %d  skinnedLevelSet %d",
                probe.taperedCapsuleElems, probe.levelSetElems, probe.skinnedLevelSetElems);
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
        } else if (!clk.ran) {
            ArcMenuLayout::HoverableText("not run yet");
        } else if (!clk.note.empty()) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", clk.note.c_str());
        } else {
            ArcMenuLayout::HoverableTextF("candidates %d  over %.2fs  bytesChanged %d",
                clk.hits, clk.elapsed, clk.bytesChanged);
            ArcMenuLayout::HoverableTextColoredF(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                "first: UWorld+0x%X (%s) = %.2f",
                clk.firstOffset, clk.firstIsFloat ? "float" : "double", clk.firstValue);
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
        } else if (!tk.ran) {
            ArcMenuLayout::HoverableText("not run yet");
        } else if (!tk.note.empty()) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", tk.note.c_str());
        } else {
            ArcMenuLayout::HoverableTextF("samples %d  slots moved %d", tk.samples, tk.slotsChanged);
            for (int k = 0; k < WorldScan::TickProbeResult::kTop; ++k) {
                if (tk.topCount[k] <= 0)
                    continue;
                ArcMenuLayout::HoverableTextColoredF(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                    "+0x%X changed %d/%d", tk.topOffset[k], tk.topCount[k], tk.samples);
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
    ArcMenuHoverTooltip("ESP scanning state. Scanning only runs while in a raid.");
    }
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
            ArcMenuLayout::HoverableTextF("Rotation: %.2f, %.2f, %.2f", cam.Rotation.x, cam.Rotation.y, cam.Rotation.z);
            ArcMenuLayout::HoverableTextF("FOV: %.2f", cam.FOV);
        }
    }

    if (const char* attached = Memory::GetAttachedGameExe())
        ArcMenuLayout::HoverableTextF("Attached: %s", attached);
    ArcMenuHoverTooltip("Game executable the DMA reader is attached to.");
    ArcMenuLayout::HoverableTextF("GWorld: 0x%llX", static_cast<unsigned long long>(engine.GWorld));
    ArcMenuHoverTooltip("Current GWorld pointer read from the game.");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Debug overlay");
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
        if (ImGui::BeginTabItem("Dumper"))
        {
            ArcMenuHoverTooltip("SDK / globals dumper tool and its output logs.");
            Dumper::DrawHelpDumperTab();
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