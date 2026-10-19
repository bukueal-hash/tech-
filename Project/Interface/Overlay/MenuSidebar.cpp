#define WIN32_LEAN_AND_MEAN
#include "Menu.h"
#include "MenuLayout.h"
#include "MenuTheme.h"

#include <cfloat>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>
#include <chrono>

#include "../../ThirdParty/ImGui/imgui.h"
#include "../../DMA/Memory.h"
#include "../../Core/Memory.h"
#include "../../Core/Engine.h"
#include "../../Core/PoiParser.hpp"
#include "../../Core/PoiStore.hpp"
#include "../../Functions/WorldScanCommon.h"
#include "../../Functions/AimMonitor.h"
#include "../Render.h"
#include "../OverlayHost.h"
#include "../Utils/Variables/index.h"
#include "../Utils/AutoConfig.h"

// Defined in Functions/Esp.cpp: latest aim status line for the Debug tab.
const char* AimStatusLine();
#include "../Utils/UpgradeOptions.hpp"
#include "../Utils/Visuals/Crosshair.hpp"
#include "../../Hardware/KmBox.h"
#include "../../Input/Controller.h"
#include "../../Input/DmaGamepad.h"
#include "../../Input/KeyBind.h"

#include "ImGuiKeybind.h"
#include "../../Core/WorldItemCategory.h"
#include "../../Core/EntityFilters.hpp"
#include "../../Core/EmbreeVisCheck.hpp"

namespace {

// Row tooltip on whichever sub-widget of a composite row is hovered.
static void RowTip(const char* txt)
{
    if (txt && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", txt);
}

template<size_t N>
static void DrawSubtypeFilters(const char* title, uint64_t& mask, const EntityFilters::Type (&types)[N], bool elite = false)
{
    ImGui::PushID(title);
    if (ImGui::CollapsingHeader(title)) {
        static char searches[6][96]{};
        const size_t group = &mask == &EntityFilters::arcMask ? 0u
            : &mask == &EntityFilters::resourceMask ? 1u
            : &mask == &EntityFilters::grenadeMask ? 2u
            : &mask == &EntityFilters::mineMask ? 3u
            : &mask == &EntityFilters::trapMask ? 4u : 5u;
        char* search = searches[group];
        ImGui::InputText("Search types", search, sizeof(searches[group]));
        ArcMenuHoverTooltip("Filter the type controls by name; searching does not change which entities are enabled.");
        if (ImGui::Button("Select all")) { mask = UINT64_MAX; AutoConfig_MarkDirty(); }
        ArcMenuHoverTooltip("Enable every subtype, including Other / Unknown. The category master still applies.");
        ImGui::SameLine();
        if (ImGui::Button("Deselect all")) { mask = 0; AutoConfig_MarkDirty(); }
        ArcMenuHoverTooltip("Hide all subtypes in this group from ESP and radar; does not change the category master.");
        if (elite) {
            if (ImGui::Button("Only elite")) {
                mask = (uint64_t{1} << 14) | (uint64_t{1} << 15) | (uint64_t{1} << 16);
                AutoConfig_MarkDirty();
            }
            ArcMenuHoverTooltip("Select Queen, Sentinel and Matriarch only. Other ARC types, including unknown types, are disabled.");
        }
        const std::string query = EntityFilters::Lower(search);
        for (size_t i = 0; i < N; ++i) {
            if (ArcMenuLayout::SearchHides(types[i].name)) continue;
            if (!query.empty() && EntityFilters::Lower(types[i].name).find(query) == std::string::npos) continue;
            bool enabled = (mask & (uint64_t{1} << i)) != 0;
            if (ImGui::Checkbox(types[i].name, &enabled)) {
                if (enabled) mask |= uint64_t{1} << i; else mask &= ~(uint64_t{1} << i);
                AutoConfig_MarkDirty();
            }
            ArcMenuHoverTooltip(i + 1 == N
                ? "Show entities whose cached names do not match a known subtype. Keep enabled to avoid losing newly introduced types."
                : "Show this cached-name subtype in ESP and radar when its category master is enabled. Does not alter aim targeting.");
        }
    }
    ArcMenuHoverTooltip("Expand subtype filters. All types default on; classification uses cached actor/display names without render-thread memory reads.");
    ImGui::PopID();
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
    if (ArcMenuLayout::SearchHides(label))
        return;
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
    ArcMenuHoverTooltip(rowTooltip);

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
    if (ArcMenuLayout::SearchHides(label))
        return false;
    const float startX = ImGui::GetCursorStartPos().x;
    ImGui::PushID(sliderId);
    bool changed = false;

    ArcMenuLayout::Label(label);
    ImGui::SetNextItemWidth(kContainerSpColumnX - startX - 4.f);
    if (ImGui::SliderFloat(sliderId, value, vMin, vMax, fmt)) {
        changed = true;
        AutoConfig_MarkDirty();
    }
    ArcMenuHoverTooltip(rowTooltip);

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
    if (ArcMenuLayout::SearchHides(label))
        return false;
    const float startX = ImGui::GetCursorStartPos().x;
    ImGui::PushID(comboId);
    bool changed = false;

    ArcMenuLayout::Label(label);
    ImGui::SetNextItemWidth(kContainerSpColumnX - startX - 4.f);
    if (ImGui::Combo(comboId, current, items, count)) {
        changed = true;
        AutoConfig_MarkDirty();
    }
    ArcMenuHoverTooltip(rowTooltip);

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
        "Clean-shell placeholders, ESP visibility colours, and crosshair settings.",
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
            RowTip("Select the cached player whose resolved/unresolved fields are shown below. Does not change ESP or aim targeting.");
        }
        ImGui::EndCombo();
    }
    ArcMenuHoverTooltip("Inspect a cached player; this selector changes diagnostics only, not targeting.");
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
    const char* steamState = !(var::show_steam_ids || UpgradeOptions::platformBadges)
        ? "not read (identity toggles off)"
        : (row.steamIdResolved
            ? (row.steamId64 ? "resolved" : "component resolved; ID unavailable")
            : "unresolved");
    const char* squadState = !var::show_squad_idx
        ? "not read (Squad tags off)"
        : (row.squadResolved ? "resolved" : "unresolved");
    const bool inventoryRequested = var::show_weapon || var::show_armor_line
        || var::show_player_kit;
    const char* inventoryState = !inventoryRequested
        ? "not read (features off)"
        : (row.inventoryResolved ? "resolved" : "unresolved");
    const char* dbnoState = !(var::show_dbno_badge || var::show_activity_feed || UpgradeOptions::playerStates)
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
            ArcMenuHoverTooltip("Player/bot ESP settings. Sub-tabs: Player, Bot.");
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
                    ArcMenuHoverTooltip("Draw the selected box style around tracked players; independent of bot boxes.");
                    if (var::box) {
                    static const char* kPlayerBoxStyles[] = { "Full", "Corners", "Dashed", "Rounded" };
                    ArcMenuLayout::Combo("Box style", "##player_box_style", &var::player_box_style,
                        kPlayerBoxStyles, IM_ARRAYSIZE(kPlayerBoxStyles));
                    ArcMenuHoverTooltip("Full rectangle, corner segments, dashed edges, or rounded rectangle. Replaces the old fixed rounded box.");
                    ArcMenuLayout::SliderFloat("Box thickness", "##player_box_thickness",
                        &var::player_box_thickness, 0.5f, 5.f, "%.1f px");
                    ArcMenuHoverTooltip("Player box stroke width in screen pixels; does not change bot or skeleton thickness.");
                    ArcMenuLayout::Checkbox("Box outline", &var::player_box_outline);
                    ArcMenuHoverTooltip("Black border behind the selected box style for contrast on bright backgrounds.");
                    ArcMenuLayout::CheckboxWithColorRow("Box fill", &var::player_box_fill,
                        var::player_box_fill_color, "##player_box_fill_color",
                        "Fill the player rectangle; color alpha controls opacity. Independent of body silhouette.");
                    ArcMenuHoverTooltip("Fill the player rectangle; color alpha controls opacity. Independent of body silhouette.");
                    }
                    ArcMenuLayout::CheckboxWithColorRow("Head dot", &var::show_head_dot,
                        var::head_dot_color, "##head_dot_color",
                        "Draw a dot on the cached head bone, independent of box and skeleton toggles. Omitted when no valid head bone is available.");
                    ArcMenuHoverTooltip("Draw a dot on the cached head bone, independent of box and skeleton toggles. Omitted when no valid head bone is available.");
                    ImGui::BeginDisabled(!var::show_head_dot);
                    ArcMenuLayout::SliderFloat("Head dot radius", "##head_dot_radius", &var::head_dot_radius_px, 1.f, 10.f, "%.1f px");
                    ArcMenuHoverTooltip("Head dot radius in screen pixels; a one-pixel black border is added for contrast.");
                    ImGui::EndDisabled();
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
                    ArcMenuLayout::Checkbox("Platform badge", &UpgradeOptions::platformBadges);
                    ArcMenuHoverTooltip("Badge beside the player name: S for a validated Steam identity, Xbox when the identity is unresolved.");
                    ArcMenuLayout::SliderFloat("Badge size", "##platform_badge_size", &UpgradeOptions::badgeSize, 10.f, 28.f, "%.0f px");
                    ArcMenuHoverTooltip("Platform badge text size in screen pixels; background scales with it.");
                    ArcMenuLayout::Checkbox("Player state labels", &UpgradeOptions::playerStates);
                    ArcMenuHoverTooltip("Cached downed state and fresh horizontal velocity: Idle or Walking / moving. ADS, firing, crouch, sprint and other activities are unavailable and never guessed.");
                    ArcMenuLayout::Checkbox("State: idle", &UpgradeOptions::stateIdle);
                    ArcMenuHoverTooltip("Show Idle when fresh horizontal velocity is at most 0.2m/s; not proof of inactivity.");
                    ArcMenuLayout::Checkbox("State: walking", &UpgradeOptions::stateWalking);
                    ArcMenuHoverTooltip("Show Walking / moving above 0.2m/s; velocity alone cannot distinguish walking from sprinting.");
                    ArcMenuLayout::Checkbox("State: downed", &UpgradeOptions::stateDowned);
                    ArcMenuHoverTooltip("Show the cached Downed state; separate from the existing revive badge and timer.");
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
                    ArcMenuLayout::Checkbox("Hide allies", &var::hide_allies);
                    ArcMenuHoverTooltip("No ESP or radar on teammates (box, skeleton, silhouette, names, etc.).");
                    ArcMenuLayout::Checkbox("Squad tags", &var::show_squad_idx);
                    ArcMenuHoverTooltip(
                        "Show [S1] / [S2] ... on enemies: real squad membership when it "
                        "resolves, TeamID grouping as the fallback.");
                    ImGui::EndDisabled();
                    ArcMenuLayout::CheckboxWithColorRow("Off-screen arrows", &UpgradeOptions::offscreen, UpgradeOptions::arrowColor, "##offscreen_arrow_color",
                        "Directional markers for fresh living enemies outside the screen. Does not enable player ESP.");
                    ArcMenuHoverTooltip("Directional markers for fresh living enemies outside the screen, placed near the screen edge.");
                    ArcMenuLayout::SliderFloat("Arrow size", "##offscreen_size", &UpgradeOptions::arrowSize, 5.f, 30.f, "%.0f px");
                    ArcMenuHoverTooltip("Off-screen triangle length in screen pixels.");
                    ArcMenuLayout::SliderFloat("Arrow gap", "##offscreen_inset", &UpgradeOptions::arrowInset, 20.f, 200.f, "%.0f px");
                    ArcMenuHoverTooltip("Extra pixels to inset or extend the off-screen marker radius.");
                    ArcMenuLayout::Checkbox("Threat warnings", &UpgradeOptions::warnings);
                    ArcMenuHoverTooltip("Fresh cached enemy positions only: nearby/multiple enemies, behind and sustained approach. Not proof an enemy sees or targets you; warning range participates in player collection and position refresh.");
                    ArcMenuLayout::SliderFloat("Warning range", "##warning_range", &UpgradeOptions::warningRange, 5.f, 500.f, "%.0f m");
                    ArcMenuHoverTooltip("Maximum 3D range for nearby, behind and approach warnings. Also widens player collection range; never enables normal player ESP.");
                    ArcMenuLayout::Checkbox("Warn behind", &UpgradeOptions::warningBehind);
                    ArcMenuHoverTooltip("Warn when an enemy is in the rear half-plane relative to camera yaw.");
                    ArcMenuLayout::Checkbox("Warn approach", &UpgradeOptions::warningApproach);
                    ArcMenuHoverTooltip("Use the enemy's fresh radial velocity toward your current camera position; does not compensate your own movement.");
                    ArcMenuLayout::SliderFloat("Closing speed", "##warning_speed", &UpgradeOptions::warningClosing, 0.5f, 20.f, "%.1f m/s");
                    ArcMenuHoverTooltip("Minimum enemy radial speed toward your position before an approach warning can activate.");
                    ArcMenuLayout::SliderFloat("Warning debounce", "##warning_debounce", &UpgradeOptions::warningDebounce, 0.f, 3.f, "%.1f s");
                    ArcMenuHoverTooltip("Condition must remain present this long. Missing/stale enemies clear warnings immediately; never inferred hostile intent.");
                    if (ImGui::Button("Warnings: close combat")) { UpgradeOptions::warningRange = 30.f; UpgradeOptions::warningDebounce = 0.2f; UpgradeOptions::warningClosing = 2.f; AutoConfig_MarkDirty(); }
                    ArcMenuHoverTooltip("Set warning range 30m, debounce 0.2s and closing threshold 2m/s. Does not enable warnings.");
                    if (ImGui::Button("Warnings: cautious")) { UpgradeOptions::warningRange = 100.f; UpgradeOptions::warningDebounce = 0.6f; UpgradeOptions::warningClosing = 1.f; AutoConfig_MarkDirty(); }
                    ArcMenuHoverTooltip("Set warning range 100m, debounce 0.6s and closing threshold 1m/s. Does not enable warnings.");
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
                    ArcMenuLayout::Checkbox("Entity ESP log##entity", &var::esp_entity_debug);
                    ArcMenuHoverTooltip("Log every bot and player each second: distance and whether ESP painted it (help/entity_diagnostics.ndjson).");
                    DrawSubtypeFilters("ARC type filters", EntityFilters::arcMask, EntityFilters::kArcs, true);
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
                    ArcMenuLayout::CheckboxWithColorRow(
                            "Dead bot bodies",
                            &var::show_dead_bots,
                            var::color_dead_bots,
                            "##col_dead_bots",
                            "Show destroyed/broken robot wrecks; color applies to dead bot ESP and radar.");
                    ArcMenuHoverTooltip("Show destroyed/broken robot wrecks; color applies to dead bot ESP and radar.");
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
        if (ImGui::BeginTabItem("Quest"))
        {
            ArcMenuHoverTooltip(
                "Quest objective items (ST_ItemNames_QuestItems / DA_Item_Salvage_Quest_*). "
                "They draw as a gold star on the ESP and radar, from any distance.");
            DrawArcQuestContent();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// Quest objective items: previously unconditional (no UI). Its own tab so the
// master toggle and the pulse can be changed without hunting through Loot.
void DrawArcQuestContent()
{
    ArcMenuLayout::HoverableText("Quest items");
    ArcMenuHoverTooltip(
        "Objective items the game tags as quest loot: gold star on the ESP and radar, "
        "always drawn from far away and never filtered by loot settings. These controls "
        "are the only filter they have.");
    ArcMenuLayout::Checkbox("Show quest items", &var::show_world_quest);
    ArcMenuHoverTooltip(
        "Master switch for quest item markers on both the ESP and the radar. On by "
        "default, which is the previous always-on behaviour. Off hides them entirely.");
    ImGui::BeginDisabled(!var::show_world_quest);
    ArcMenuLayout::Checkbox("Pulse", &var::quest_pulse);
    ArcMenuHoverTooltip(
        "Slowly breathe the gold star so it stands out in a pile of loot. Off draws a "
        "steady star, which reads calmer and is easier to read while moving.");
    ImGui::EndDisabled();
}

// Collision geometry (visibility groundwork): its own tab, because it is a
// reader with its own state rather than a loot setting. Moved here unchanged
// from under Loot.
void DrawArcLootContent()
{
    ArcMenuLayout::Checkbox("Enable world ESP", &var::enable_world);
    ArcMenuHoverTooltip(
        "Master switch for item/container scanners and world radar blips. Off = no world cache updates.");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Loot");
    ArcMenuHoverTooltip("Loot container ESP settings.");
    ArcMenuLayout::Checkbox("Show loot", &var::showLoot);
    ArcMenuHoverTooltip(
        "Master switch for loot/container ESP draw. Off = hide all world loot labels. "
        "On = each Container types row below filters what is shown.");
    ImGui::BeginDisabled(!var::showLoot);
    DrawSubtypeFilters("Resource types", EntityFilters::resourceMask, EntityFilters::kResources);
    DrawSubtypeFilters("Grenade types", EntityFilters::grenadeMask, EntityFilters::kGrenades);
    DrawSubtypeFilters("Mine types", EntityFilters::mineMask, EntityFilters::kMines);
    DrawSubtypeFilters("Trap types", EntityFilters::trapMask, EntityFilters::kTraps);
    DrawSubtypeFilters("Extraction types", EntityFilters::extractionMask, EntityFilters::kExtractions);
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
    ArcMenuLayout::Checkbox("Grey looted crates", &var::grey_looted_containers);
    ArcMenuHoverTooltip(
        "Crates someone already emptied draw grey with a (Looted) tag so you never "
        "waste a trip. Off = old behavior (opened containers hide unless "
        "'Open container' is on).");
    {
        static const char* kStateModes[] = { "Normal", "Dim", "Grey", "Hide" };
        // The mode only does something when opened containers are drawn at all
        // (the "Open container" row or "Grey looted crates"); grey it out otherwise
        // so it cannot silently do nothing and look broken.
        ImGui::BeginDisabled(!var::show_world_open_container && !var::grey_looted_containers);
        int om = std::clamp(var::state_opened_mode, 0, 3);
        if (ArcMenuLayout::Combo("Opened container", "##state_opened_mode", &om,
                kStateModes, IM_ARRAYSIZE(kStateModes)))
            var::state_opened_mode = om;
        ImGui::EndDisabled();
        ArcMenuHoverTooltip(
            "How a looted/opened container draws in BOTH the ESP and the radar, "
            "so the two surfaces can never disagree.");

        // Same: inert while the master "Dead bot bodies" toggle is off.
        ImGui::BeginDisabled(!var::show_dead_bots);
        int dm = std::clamp(var::state_dead_mode, 0, 3);
        if (ArcMenuLayout::Combo("Dead bot / Arc", "##state_dead_mode", &dm,
                kStateModes, IM_ARRAYSIZE(kStateModes)))
            var::state_dead_mode = dm;
        ImGui::EndDisabled();
        ArcMenuHoverTooltip("How a destroyed bot/Arc draws in both the ESP and the radar.");

        ImGui::BeginDisabled(
            var::state_opened_mode != 1 && var::state_dead_mode != 1);
        ArcMenuLayout::SliderFloat("Dim alpha", "##state_dim_alpha",
            &var::state_dim_alpha, 0.f, 1.f, "%.2f");
        ArcMenuHoverTooltip("Alpha left when a state is set to Dim (1 = undimmed).");
        ImGui::EndDisabled();
    }
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + kContainerSpColumnX);
    ImGui::TextUnformatted("SP");
    ArcMenuHoverTooltip(
        "Nothing is hidden. Per-filter SP: checked = far (SP distance) for matching pickups; "
        "unchecked = close (loot / category distance). Non-matches stay at close distance.");
    static const char* kMinRarityLabels[] = {
        "Any", "Uncommon+", "Rare+", "Epic+", "Legendary only"
    };
    LootFilterSliderWithSp(
            "Min loot value",
            "##loot_min_value",
            &var::loot_min_value,
            0.f,
            5000.f,
            "%.0f c",
            &var::loot_min_val_sp,
            "Never hides. SP checked = far distance for pickups at/above min value.",
            "Does not hide loot. At/above threshold: SP distance when checked, "
            "loot/category distance when unchecked. Below threshold: loot/category distance. 0 = off.");
    ArcMenuHoverTooltip(
        "Does not hide loot. At/above threshold: SP distance when checked, "
        "loot/category distance when unchecked. Below threshold: loot/category distance. 0 = off.");
    LootFilterComboWithSp(
            "Min rarity",
            "##loot_min_rarity",
            &var::loot_min_rarity,
            kMinRarityLabels,
            IM_ARRAYSIZE(kMinRarityLabels),
            &var::loot_min_rar_sp,
            "Never hides. SP checked = far distance for pickups at/above min rarity.",
            "Does not hide loot. At/above threshold: SP distance when checked, "
            "loot/category distance when unchecked. Below threshold: loot/category distance. Any = off.");
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
    ContainerTypeRow("Weapon rack", &var::show_world_weapon_rack, var::color_world_weapon_rack, "##col_w_weapon_rack",
        WorldItemCategory::WeaponRack,
        "Show weapon racks. SP unchecked = Loot distance, checked = SP distance.");
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
    ArcMenuHoverTooltip("Top-down blips for players and AI units. Works without Enemy ESP enabled.");
    ImGui::BeginDisabled(!var::show_radar);
    ArcMenuLayout::SliderFloat("Map size", "##radar_scale", &var::radar_scale, 30.f, 120.f, "%.0f px");
    ArcMenuHoverTooltip("Radar diameter on screen in pixels.");
    static const char* kRadarShapeLabels[] = { "Circle", "Square" };
    int radarShape = var::radar_shape_circle ? 0 : 1;
    if (ArcMenuLayout::Combo("Shape", "##radar_shape", &radarShape, kRadarShapeLabels, IM_ARRAYSIZE(kRadarShapeLabels)))
        var::radar_shape_circle = (radarShape == 0);
    ArcMenuHoverTooltip("Circle clips blips to range; square uses a box outline.");
    ArcMenuLayout::SliderFloat("World range", "##radar_range", &var::radar_range, 20.f, var::kMaxDistanceSliderM, "%.0f m");
    ArcMenuHoverTooltip(
        "Radar radius for players, bots, and rare loot blips. "
        "Live zoom: Alt+- and Alt+= change this while a raid is running.");
    ArcMenuLayout::Checkbox("Map mode (north-up)", &var::radar_map_mode);
    ArcMenuHoverTooltip(
        "Geographically correct radar: north-up whole-map fit from the minimap bounds "
        "(north-up range view until the bounds resolve).");

    ArcMenuLayout::Checkbox("Dim underground", &var::radar_underground_dim);
    ArcMenuHoverTooltip(
        "Blips on a lower floor draw hollow (detected by Z below you). ");
    ArcMenuLayout::SliderFloat("Blip size", "##radar_dot_scale", &var::radar_dot_scale, 0.5f, 2.5f, "%.2fx");
    var::radar_dot_scale = (std::max)(0.5f, (std::min)(var::radar_dot_scale, 2.5f));
    ArcMenuHoverTooltip("Scales every radar blip radius. Independent of Map size.");
    ArcMenuLayout::Checkbox("Altitude on blips", &var::radar_show_altitude);
    ArcMenuHoverTooltip(
        "Show metres above/below you beside blips (e.g. +12m / -3m). ");
    ArcMenuLayout::Checkbox("Compass ticks", &var::radar_compass);
    ArcMenuHoverTooltip(
        "N/E/S/W marks on the radar edge. In Map mode they stay fixed; otherwise they rotate with your view.");

    static const char* kFocusLabels[] = { "All", "Players", "Bots", "Loot" };
    ArcMenuLayout::Combo(
        "Focus", "##radar_focus", &var::radar_focus,
        kFocusLabels, IM_ARRAYSIZE(kFocusLabels));
    ArcMenuHoverTooltip(
        "Show only one class of blip: everything, just players, just bots, or "
        "just containers/pickups. Cuts the clutter when a screen is busy.");

    ArcMenuLayout::Checkbox("Ghost blips", &var::radar_ghost);
    ArcMenuHoverTooltip(
        "When a tracked player or bot leaves the pipeline (DMA stall or cache "
        "eviction), keep its last known position as a fading ring with an age "
        "label instead of blinking out with no explanation.");
    ArcMenuLayout::SliderFloat("Ghost seconds", "##radar_ghost_secs", &var::radar_ghost_secs, 1.f, 30.f, "%.0f s");
    var::radar_ghost_secs = (std::max)(1.f, (std::min)(var::radar_ghost_secs, 30.f));
    ArcMenuHoverTooltip("How long a ghost stays on the radar before it expires.");
    
    ArcMenuLayout::Checkbox("Suppress ghosts in POI zones", &var::radar_ghost_suppress_poi);
    ArcMenuHoverTooltip(
        "Hide ghost blips when they fall inside any POI boundary polygon. "
        "Useful to reduce clutter around exits and hot zones.");

    // D17 points of interest.
    ImGui::Separator();
    ArcMenuLayout::Checkbox("POIs", &var::show_pois);
    ArcMenuHoverTooltip(
        "Named markers read from Data\\pois.json: exits, hot loot, vendors, "
        "quest starts. Drawn as diamonds so they can never be confused with an "
        "entity blip. Use the buttons below to add one at your current spot, or "
        "edit the file directly.");
    ArcMenuLayout::Checkbox("POI names", &var::poi_show_labels);
    ArcMenuHoverTooltip("Draw the POI's name in text beside its diamond.");

    static const char* const kPoiLabels[PoiParser::kCategoryCount] = {
        "Exfil", "Spawn", "Hot", "Vendor", "Quest", "Custom"
    };
    for (int ci = 0; ci < PoiParser::kCategoryCount; ++ci) {
        bool on = (var::poi_categories & (1 << ci)) != 0;
        ImGui::PushID(ci);
        if (ImGui::Checkbox(kPoiLabels[ci], &on)) {
            if (on)
                var::poi_categories |= (1 << ci);
            else
                var::poi_categories &= ~(1 << ci);
            AutoConfig_MarkDirty();
        }
        ArcMenuHoverTooltip("Show or hide this POI category on the radar.");
        ImGui::PopID();
        if ((ci % 3) != 2)
            ImGui::SameLine();
    }

    PoiStore::EnsureLoaded();
    ArcMenuLayout::HoverableTextF(
        "%d POI%s loaded from Data\\pois.json",
        static_cast<int>(PoiStore::Items().size()),
        PoiStore::Items().size() == 1 ? "" : "s");
    ArcMenuHoverTooltip(
        "Load once on first use. Missing, unreadable, invalid and valid-empty "
        "files have separate status; unreadable files are never overwritten.");
    const auto poiStatus = PoiStore::Status();
    const char* poiStatusText = poiStatus == PoiStore::LoadStatus::Missing ? "POI file missing"
        : poiStatus == PoiStore::LoadStatus::ReadFailed ? "POI file unreadable or too large"
        : poiStatus == PoiStore::LoadStatus::Invalid ? "POI file invalid; original preserved"
        : poiStatus == PoiStore::LoadStatus::Empty ? "POI file valid but empty" : nullptr;
    if (poiStatusText)
        ArcMenuLayout::HoverableTextF("%s", poiStatusText);
    if (poiStatus != PoiStore::LoadStatus::Loaded && ImGui::Button("Retry POI load"))
        PoiStore::RetryLoad();

    static char s_poiName[48] = {};
    static int s_poiCat = 0;
    static int s_poiSaveState = 0;  // 0 idle, 1 saved, 2 write failed
    ArcMenuLayout::InputText("New POI name", "##poi_new_name", s_poiName, sizeof(s_poiName));
    ArcMenuHoverTooltip("What the marker will be labelled with on the radar.");
    ArcMenuLayout::Combo(
        "New POI category", "##poi_new_cat", &s_poiCat,
        kPoiLabels, PoiParser::kCategoryCount);
    ArcMenuHoverTooltip(
        "Decides the marker colour: exfil green, spawn blue, hot red, vendor "
        "yellow, quest purple, custom grey.");

    std::shared_ptr<const Engine::EspRenderFrame> captureFrame;
    {
        std::shared_lock<std::shared_mutex> lock(engine.m_espFrameMutex, std::try_to_lock);
        if (lock.owns_lock())
            captureFrame = engine.m_espFrameShared;
    }
    const uint64_t captureNowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool captureReady = engine.IsInRaidRaw() && captureFrame && captureFrame->valid
        && captureFrame->worldGeneration == engine.m_worldGeneration.load(std::memory_order_acquire)
        && captureNowMs >= captureFrame->collectStampMs
        && captureNowMs - captureFrame->collectStampMs <= 500;
    ImGui::BeginDisabled(!captureReady);
    if (ImGui::Button("Add POI at my position")) {
        const Engine::CameraCache& cam = captureFrame->camera;
        PoiParser::Poi p;
        p.name = s_poiName[0] ? s_poiName : "POI";
        p.category = PoiParser::kCategoryNames[s_poiCat];
        p.x = cam.Location.x;
        p.y = cam.Location.y;
        p.z = cam.Location.z;
        const bool ok = PoiStore::Add(p);
        s_poiSaveState = ok ? 1 : 2;
        if (ok)
            s_poiName[0] = '\0';
    }
    ImGui::EndDisabled();
    ArcMenuHoverTooltip(
        "Requires a valid same-world camera frame no older than 500 ms. "
        "Captures your current world position (the camera read) and appends it to "
        "Data\\pois.json. Needs a live camera read — outside a raid nothing is "
        "captured yet, so add POIs while in one.");

    if (s_poiSaveState == 1)
        ArcMenuLayout::HoverableTextColoredF(
            ImVec4(0.45f, 1.f, 0.55f, 1.f), "POI saved to Data\\pois.json");
    else if (s_poiSaveState == 2)
        ArcMenuLayout::HoverableTextColoredF(
            ImVec4(1.f, 0.45f, 0.45f, 1.f), "Could not write Data\\pois.json");
    if (s_poiSaveState != 0)
        ArcMenuHoverTooltip(
            "The file sits beside ArcRaiders.exe. Saving rewrites it whole and "
            "keeps every existing entry.");

    ImGui::EndDisabled();

    // HUD and feed are independent dashboard features; they must remain
    // configurable when the radar itself is off.
    ImGui::Separator();
    ArcMenuLayout::Checkbox("Raid HUD", &var::show_raid_hud);
    ArcMenuHoverTooltip(
        "Top-center panel: cached raid clock, grace timer, named phase, enemy/loot counts, "
        "raw map-condition ID and replicated client server-health status. Unknown reads stay unknown; "
        "the condition name is unresolved and server health is not measured CPU/GPU telemetry.");
    ArcMenuLayout::Checkbox("Objectives", &var::show_objectives);
    ArcMenuHoverTooltip(
        "Cached mission objective from MainObjectiveComponent: objective id, state, "
        "progress and objective timer. Task count only - objective titles are FText and "
        "cannot be decoded without a resolved text table, so no title is shown. "
        "Unknown reads display nothing rather than a fabricated objective.");
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

void DrawArcAimbotTab()
{
    if (ImGui::BeginTabBar("##aimbot_tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Aimbot"))
        {
            ArcMenuHoverTooltip("Phase 1 aim assist. Read-only: nothing is written to the game.");
            ArcMenuLayout::Checkbox("Enable Aimbot", &var::enable_aimbot);
            ArcMenuHoverTooltip("Master switch. Off stops the aim tick immediately.");

            // Was the whole content of the removed ESP "Viz" tab.
            ArcMenuLayout::Checkbox("Per-bone vischeck", &UpgradeOptions::visCheckPerBone);
            ArcMenuHoverTooltip("Per-bone raycast visibility (Embree): each bone coloured visible/behind-cover; the box shows when any bone ray is clear.");
            ImGui::BeginDisabled(!UpgradeOptions::visCheckPerBone);
            ArcMenuLayout::SliderFloat("Ray range", "##ray_range_m", &UpgradeOptions::rayRangeM, 0.f, 500.f, "%.0f m");
            ArcMenuHoverTooltip("How far the vischeck rays reach, in metres. Targets farther than this are not traced (no ray drawn, treated as visible). 0 = unlimited.");
            ImGui::EndDisabled();

            // Phase 1 movement settings. The layout helper takes floats, so each
            // int setting goes through a temp and is rounded back.
            float aimRadius = static_cast<float>(var::aim_radius);
            if (ArcMenuLayout::SliderFloat(
                    "Aim FOV", "##aim_radius", &aimRadius, 10.f, 120.f, "%.0f px"))
                var::aim_radius = static_cast<int>(aimRadius + 0.5f);
            ArcMenuHoverTooltip("FOV circle around the crosshair. Targets outside it are ignored.");

            ArcMenuLayout::Checkbox("Show FOV circle", &var::aim_show_circle);
            ArcMenuHoverTooltip("Draws the FOV circle while the menu is open so you can size it live, and in raid. Off = invisible everywhere.");
            if (var::aim_show_circle) {
                const ImVec2 scrCenter(ImGui::GetIO().DisplaySize.x * 0.5f,
                                       ImGui::GetIO().DisplaySize.y * 0.5f);
                ImGui::GetForegroundDrawList()->AddCircle(
                    scrCenter, aimRadius, IM_COL32(90, 180, 255, 160), 64, 1.5f);
            }

            ArcMenuLayout::Checkbox("Deadzone", &var::aim_deadzone);
            ArcMenuHoverTooltip("Stop moving once the crosshair is inside the deadzone. Off = keep correcting even at tiny error (may buzz).");
            ArcMenuLayout::SliderFloat(
                "Deadzone size", "##aim_stop_zone", &var::aim_stop_zone, 0.5f, 6.f, "%.1f px");
            ArcMenuHoverTooltip("How many pixels around the crosshair count as on target. No move is sent inside it.");

            float aimSpeed = static_cast<float>(var::aim_speed);
            if (ArcMenuLayout::SliderFloat(
                    "Max speed", "##aim_speed", &aimSpeed, 10.f, 300.f, "%.0f u/s"))
                var::aim_speed = static_cast<int>(aimSpeed + 0.5f);
            ArcMenuHoverTooltip("Cap on relative mouse units per second. Lower is smoother and slower.");

            float aimResponse = static_cast<float>(var::aim_response);
            if (ArcMenuLayout::SliderFloat(
                    "Response", "##aim_response", &aimResponse, 1.f, 100.f, "%.0f"))
                var::aim_response = static_cast<int>(aimResponse + 0.5f);
            ArcMenuHoverTooltip("How much of the remaining error is corrected each tick. Retimed to a 20 ms tick.");

            ArcMenuLayout::Checkbox("Aim jitter", &var::aim_jitter);
            ArcMenuHoverTooltip("Wiggles response +-5% and FOV +-1 px every tick so your aim never follows a perfectly repeatable curve.");

            ArcMenuLayout::Checkbox("Close-range boost", &var::aim_close_range);
            ArcMenuHoverTooltip("Under the close distance: double aim speed and faster autofire (one confirm, bigger ring, box points allowed).");
            float aimCloseDist = var::aim_close_distance;
            if (ArcMenuLayout::SliderFloat(
                    "Close distance", "##aim_close_distance", &aimCloseDist, 2.f, 10.f, "%.0f m"))
                var::aim_close_distance = aimCloseDist;
            ArcMenuHoverTooltip("Targets closer than this get the close-range boost.");

            ArcMenuLayout::SliderFloat(
                "Recoil comp", "##aim_recoil_comp", &var::aim_recoil_comp, 1.f, 2.f, "%.2f");
            ArcMenuHoverTooltip("Multiplies the vertical move while you shoot (pad RT or autofire) to fight weapon climb. 1.00 = off.");

            // Phase 2: which target, where on the body, and how far away.
            static const char* const kPriorityNames[] = { "Screen", "Distance", "Balanced" };
            int aimPriority = var::aim_priority;
            if (ArcMenuLayout::Combo(
                    "Priority", "##aim_priority", &aimPriority,
                    kPriorityNames, IM_ARRAYSIZE(kPriorityNames)))
                var::aim_priority = aimPriority;
            ArcMenuHoverTooltip("Screen = closest to the crosshair. Distance = nearest actor. Balanced = both.");

            static const char* const kBoneNames[] = {
                "Closest to crosshair",
                "Head", "Neck", "Chest",
                "Spine 3", "Spine 2", "Spine 1", "Pelvis",
                "Upper arm L", "Lower arm L", "Hand L",
                "Upper arm R", "Lower arm R", "Hand R",
                "Thigh L", "Calf L", "Foot L",
                "Thigh R", "Calf R", "Foot R",
            };
            int aimBone = var::aim_bone;
            if (ArcMenuLayout::Combo(
                    "Aim point", "##aim_bone", &aimBone,
                    kBoneNames, IM_ARRAYSIZE(kBoneNames)))
                var::aim_bone = aimBone;
            ArcMenuHoverTooltip("Closest = the bone nearest the crosshair; it sticks until another is clearly closer. A fixed bone is used when valid, else the estimated box point. Bots always aim at the heart (box centre).");

            float aimPoint = static_cast<float>(var::aim_point_percent);
            if (ArcMenuLayout::SliderFloat(
                    "Estimated head", "##aim_point_percent", &aimPoint, 2.f, 45.f, "%.0f %%"))
                var::aim_point_percent = static_cast<int>(aimPoint + 0.5f);
            ArcMenuHoverTooltip("Fallback head point, measured down from the top of the box. Chest is always 45%.");

            float aimDistance = static_cast<float>(var::aim_max_distance);
            if (ArcMenuLayout::SliderFloat(
                    "Max aim distance", "##aim_max_distance", &aimDistance,
                    5.f, static_cast<float>(var::kMaxDistanceSliderM), "%.0f m"))
                var::aim_max_distance = static_cast<int>(aimDistance + 0.5f);
            ArcMenuHoverTooltip("Actors further away than this are ignored.");

            ArcMenuLayout::Checkbox("Sticky lock", &var::aim_sticky);
            ArcMenuHoverTooltip("Keep the same actor while it stays near the crosshair, so the pick cannot flicker.");
            ImGui::Separator();

            // Phase 3: trust gates. With all three off the aim point is a guess.
            ArcMenuLayout::Checkbox("Require confirmed enemy", &var::aim_require_enemy);
            ArcMenuHoverTooltip("Reject actors whose team could not be read. Off = unknown team is treated as enemy.");
            ArcMenuLayout::Checkbox("Require visible", &var::aim_require_visible);
            ArcMenuHoverTooltip("Needs the game's own on-screen stamp, sampled within 0.5 s. Needs ESP visibility colours on.");
            ArcMenuLayout::Checkbox("Require validated bones", &var::aim_require_bones);
            ArcMenuHoverTooltip("Reject targets whose head/chest bone did not validate, instead of using the box estimate.");

            if (!var::aim_require_enemy && !var::aim_require_visible && !var::aim_require_bones) {
                ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.72f, 0.3f, 1.f),
                    "Approximate mode: enemy, line of sight and bone are unconfirmed.");
                ArcMenuHoverTooltip("Shown when all three Require boxes are off. Aim still works; the target is just not double-checked.");
            }
            ImGui::Separator();

            // Phase 4: autofire. Saved with the config; clicks only from a
            // converged bone point.
            ArcMenuLayout::Checkbox("Autofire", &var::aim_autofire);
            ArcMenuHoverTooltip("Clicks from a converged aim point on a real bone. Saved in auto config.");

            float shootRadius = static_cast<float>(var::aim_autofire_radius);
            if (ArcMenuLayout::SliderFloat(
                    "Shoot radius", "##aim_autofire_radius", &shootRadius, 3.f, 12.f, "%.0f px"))
                var::aim_autofire_radius = static_cast<int>(shootRadius + 0.5f);
            ArcMenuHoverTooltip("Widest error that still counts as aligned. Smaller is stricter.");

            float shootInterval = static_cast<float>(var::aim_autofire_interval);
            if (ArcMenuLayout::SliderFloat(
                    "Shoot interval", "##aim_autofire_interval", &shootInterval, 80.f, 1000.f, "%.0f ms"))
                var::aim_autofire_interval = static_cast<int>(shootInterval + 0.5f);
            ArcMenuHoverTooltip("Minimum time between clicks.");

            float clickHold = static_cast<float>(var::aim_click_hold);
            if (ArcMenuLayout::SliderFloat(
                    "Click hold", "##aim_click_hold", &clickHold, 10.f, 100.f, "%.0f ms"))
                var::aim_click_hold = static_cast<int>(clickHold + 0.5f);
            ArcMenuHoverTooltip("How long the device button stays down. The paint thread waits for this, so keep it short.");

            ArcMenuLayout::Checkbox("Aim telemetry", &var::aim_telemetry);
            ArcMenuHoverTooltip("One 1 Hz line under the ring: actors seen, in the ring, drop reasons, pick type and autofire state.");
            ImGui::Separator();

            ArcMenuLayout::SliderFloat(
                "Calibration X", "##aim_calib_x", &var::aim_calib_x, 0.1f, 20.f, "%.2f px/unit");
            ArcMenuHoverTooltip("Pixels the view moves per mouse unit horizontally. Raise it if you overshoot.");

            ArcMenuLayout::SliderFloat(
                "Calibration Y", "##aim_calib_y", &var::aim_calib_y, 0.1f, 20.f, "%.2f px/unit");
            ArcMenuHoverTooltip("Pixels the view moves per mouse unit vertically. Raise it if you overshoot.");
            ImGui::Separator();
            ImGui::Keybind("Aim hotkey", &var::aim_hold_key);
            ArcMenuHoverTooltip("Hold this key to aim. Release stops the tick instantly.");
            ImGui::Separator();
            ArcMenuLayout::HoverableText("KMBox / MAKCU device status");
            ArcMenuHoverTooltip("Configure the KMBox or MAKCU device under Settings -> General.");
            if (!g_kmbox.kmboxConfig.initialized)
                ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.2f, 1.f),
                    "Not connected — configure under Settings -> General");
            else
                ImGui::TextDisabled("Connected — ready to send aim moves.");
            ArcMenuHoverTooltip("Device state right now. Moves are sent only while Connected.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Crosshair"))
        {
            ArcMenuHoverTooltip("Crosshair overlay: style, color, size, and animation.");
            
            ArcMenuLayout::Checkbox("Show crosshair", &var::show_crosshair);
            ArcMenuHoverTooltip("Paint-only screen-center crosshair overlay (independent of aimbot).");

            int crosshairStyle = var::crosshair_style;
            if (ArcMenuLayout::Combo(
                    "Style",
                    "##crosshair_style",
                    &crosshairStyle,
                    ArcCrosshair::kStyles,
                    IM_ARRAYSIZE(ArcCrosshair::kStyles)))
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
            ArcMenuHoverTooltip("Spin rate for your original T spin, Cross spin, and Tri spin styles. Saved style numbers are preserved.");
            
            ArcMenuLayout::Checkbox("Crosshair outline", &var::crosshair_outline);
            ArcMenuHoverTooltip("Draw a border behind all reticle primitives for contrast; paint only, no game writes.");
            
            ArcMenuLayout::Label("Outline color");
            ArcMenuLayout::ColorEditAtColumn("##crosshair_outline_color", var::crosshair_outline_color);
            ArcMenuHoverTooltip("Reticle border color and opacity.");
            
            ArcMenuLayout::CheckboxWithColorRow("Accent color", &var::crosshair_use_accent,
                var::crosshair_accent_color, "##crosshair_accent_color", "Use a second color on rings, secondary dots and decorative accents.");
            ArcMenuHoverTooltip("Use a second color on rings, secondary dots and decorative accents.");
            
            ArcMenuLayout::Checkbox("Center dot", &var::crosshair_center_dot);
            ArcMenuHoverTooltip("Add a center dot unless the chosen style already contains one.");
            
            ArcMenuLayout::Checkbox("Rounded endpoints", &var::crosshair_rounded);
            ArcMenuHoverTooltip("Add round caps to reticle line ends.");
            
            ArcMenuLayout::Checkbox("Pulse reticle", &var::crosshair_pulse);
            ArcMenuHoverTooltip("Animate reticle size using overlay time only; does not react to firing or game state.");
            
            struct CrosshairFloatRow { const char* label; const char* id; float* value; float lo; float hi; const char* tip; };
            const CrosshairFloatRow crosshairRows[] = {
                { "Rotation", "##xh_rotation", &var::crosshair_rotation, -180.f, 180.f, "Static rotation in degrees; spin styles add their RPM animation on top." },
                { "Horizontal scale", "##xh_horizontal", &var::crosshair_horizontal_scale, 0.5f, 2.f, "Horizontal reticle stretch multiplier." },
                { "Vertical scale", "##xh_vertical", &var::crosshair_vertical_scale, 0.5f, 2.f, "Vertical reticle stretch multiplier." },
                { "Dot radius", "##xh_dot", &var::crosshair_dot_size, 1.f, 8.f, "Radius in pixels of added/composite dots; the original Dot style retains its size-based radius." },
                { "Ring scale", "##xh_ring", &var::crosshair_ring_scale, 0.5f, 2.f, "Ring radius multiplier for new ring/composite styles." },
                { "Bracket length", "##xh_bracket", &var::crosshair_bracket_length, 2.f, 30.f, "Length in pixels of bracket segments." },
                { "Chevron angle", "##xh_chevron", &var::crosshair_chevron_angle, 20.f, 80.f, "Slope angle in degrees for up/down chevron arms." },
                { "Segment gap", "##xh_segment_gap", &var::crosshair_segment_gap, 2.f, 35.f, "Angular gap in degrees between segmented-circle arcs." },
                { "Pulse speed", "##xh_pulse_speed", &var::crosshair_pulse_speed, 0.25f, 5.f, "Size-animation cycles per second; used only when Pulse reticle is enabled." },
                { "Pulse amount", "##xh_pulse_amount", &var::crosshair_pulse_amount, 0.f, 10.f, "Maximum animated size change in pixels; used only when Pulse reticle is enabled." }
            };
            for (const auto& row : crosshairRows) {
                ArcMenuLayout::SliderFloat(row.label, row.id, row.value, row.lo, row.hi, "%.2f");
                ArcMenuHoverTooltip(row.tip);
            }
            
            if (ImGui::SliderInt("Segment count", &var::crosshair_segment_count, 3, 12)) AutoConfig_MarkDirty();
            ArcMenuHoverTooltip("Number of arcs in Segmented circle, or radial ticks in Shotgun ring.");
            
            const ImVec2 previewPos = ImGui::GetCursorScreenPos();
            const float previewWidth = (std::max)(120.f, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton("##crosshair_preview", ImVec2(previewWidth, 120.f));
            ArcMenuHoverTooltip("Live preview using the same renderer as the overlay. Large styles are fitted to this panel; disabling Show crosshair also clears this preview.");
            ImDrawList* previewDraw = ImGui::GetWindowDrawList();
            previewDraw->AddRectFilled(previewPos, ImVec2(previewPos.x + previewWidth, previewPos.y + 120.f), IM_COL32(15, 20, 28, 255), 6.f);
            ArcCrosshair::Draw(previewDraw, ImVec2(previewPos.x + previewWidth * 0.5f, previewPos.y + 60.f), 48.f);
            
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
            ArcMenuLayout::Checkbox("Combat mode", &UpgradeOptions::combatEnabled);
            ArcMenuHoverTooltip("Temporarily suppress world loot, near-loot HUD and POIs. Players retain their normal master; no aim settings or saved display masters are changed.");
            ImGui::Keybind("Combat hotkey", &UpgradeOptions::combatKey);
            ArcMenuHoverTooltip("Combat activation key. Unbound by default. Polled while the raid is active and the menu is closed; the game may also receive this key.");
            static const char* combatModes[] = { "Hold", "Toggle" };
            ArcMenuLayout::Combo("Combat activation", "##combat_activation", &UpgradeOptions::combatMode, combatModes, IM_ARRAYSIZE(combatModes));
            ArcMenuHoverTooltip("Hold activates only while pressed; Toggle switches on each press. Opening the menu or leaving the raid clears active combat mode.");
            ArcMenuLayout::Checkbox("Show ARCs in combat", &UpgradeOptions::combatShowArcs);
            ArcMenuHoverTooltip("Keep normally enabled ARC ESP and radar visible during combat mode. Off suppresses ARC visuals only; does not disable ARC aim.");
            ArcMenuLayout::Checkbox("System HUD", &UpgradeOptions::systemHud);
            ArcMenuHoverTooltip("Local overlay metrics, not game or target-PC metrics. CPU sampled once a second; GPU usage is explicitly unavailable in this implementation.");
            ArcMenuLayout::Checkbox("HUD: overlay FPS", &UpgradeOptions::hudFps);
            ArcMenuHoverTooltip("ImGui averaged overlay frame rate; not game FPS.");
            ArcMenuLayout::Checkbox("HUD: system CPU", &UpgradeOptions::hudCpu);
            ArcMenuHoverTooltip("Host Windows total CPU utilization from GetSystemTimes, sampled once per second. First sample is unavailable.");
            ArcMenuLayout::Checkbox("HUD: GPU status", &UpgradeOptions::hudGpu);
            ArcMenuHoverTooltip("Displays GPU usage unavailable; no reliable GPU utilization collector is installed.");
            ArcMenuLayout::Checkbox("HUD: resolution", &UpgradeOptions::hudResolution);
            ArcMenuHoverTooltip("Overlay viewport dimensions in pixels, not target game resolution.");
            ArcMenuLayout::Checkbox("HUD: clock", &UpgradeOptions::hudClock);
            ArcMenuHoverTooltip("Local host wall clock in 24-hour format.");
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
                    RowTip("Move the overlay to this host monitor; saves the monitor selection.");
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
        if (ImGui::BeginTabItem("Dumper"))
        {
            ArcMenuHoverTooltip("SDK Dumper: generate offsets and SDK headers.");
            arc_ui::DrawArcDumperTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void DrawArcDebugContent()
{
    ArcMenuLayout::HoverableText("Aim assist - live monitor");
    ArcMenuHoverTooltip("Live aim state, refreshed every frame while the menu is open. Hold the aim key and watch the counters and log here.");
    {
        AimMonitor::Live live{};
        AimMonitor::Snapshot(live);
        const ImVec4 okCol(0.45f, 0.90f, 0.45f, 1.f);
        const ImVec4 badCol(0.95f, 0.40f, 0.40f, 1.f);
        const ImVec4 warnCol(0.95f, 0.80f, 0.35f, 1.f);
        auto row = [](const char* label, const char* value, const ImVec4& col) {
            ImGui::TextUnformatted(label);
            ImGui::SameLine(170.f);
            ImGui::TextColored(col, "%s", value);
        };
        row("Aim key held", live.keyheld ? "yes" : "no",
            live.keyheld ? okCol : badCol);
        row("Enabled", live.enable ? "yes" : "no", live.enable ? okCol : badCol);
        row("KmBox ready", live.kmbox ? "yes" : "no", live.kmbox ? okCol : badCol);
        row("MAKCU", live.makcuConnected ? "connected" : "not connected",
            live.makcuConnected ? okCol : badCol);
        row("Raid active", live.raid ? "yes" : "no", live.raid ? okCol : badCol);
        row("Aim tick", live.tickRan ? "ran" : "NEVER RAN",
            live.tickRan ? okCol : badCol);
        if (live.tickRan)
            row("Tick age", (std::to_string(live.tickAgeMs) + " ms").c_str(),
                live.tickAgeMs < 50 ? okCol : warnCol);

        ImGui::Spacing();
        row("Moves sent", (std::to_string(live.moveCount)).c_str(),
            live.moveCount > 0 ? okCol : warnCol);
        row("Moves accepted",
            (std::to_string(live.moveOk) + " / " + std::to_string(live.moveCount)).c_str(),
            live.moveOk > 0 && live.moveOk == live.moveCount ? okCol : warnCol);
        row("Last move", (std::to_string(live.lastDx) + ", " + std::to_string(live.lastDy)).c_str(),
            live.hasLastMove && live.lastMoveOk ? okCol : warnCol);
        if (live.hasLastMove)
            row("Last move age", (std::to_string(live.lastMoveAgeMs) + " ms").c_str(), warnCol);
        char errBuf[64];
        std::snprintf(errBuf, sizeof(errBuf), "%.1f px", live.errPx);
        row("Error at pick", live.errPx < 0.0 ? "no target" : errBuf,
            live.errPx < 0.0 ? badCol : (live.errPx <= 6.0 ? okCol : warnCol));

        ImGui::Spacing();
        ArcMenuLayout::HoverableTextF("Status: %s", live.status);
        ArcMenuHoverTooltip("Last reason from the aim tick: idle reason, or the 1 Hz held line with seen/ring/dropped counts.");

        ArcMenuLayout::Checkbox("Draw aim monitor on screen", &UpgradeOptions::aimMonitorPanel);
        ArcMenuHoverTooltip("Small live panel in the top-left corner during a raid (menu closed): key, moves, error and the status line.");
    }

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Aim log (live)");
    ArcMenuHoverTooltip("Same lines Build/aim_debug.log receives, oldest at the top. New lines are added when the aim state changes.");
    {
        static bool autoScroll = true;
        ImGui::Checkbox("Auto-scroll", &autoScroll);
        char lines[32][200]{};
        uint64_t times[32]{};
        const int n = AimMonitor::CopyLog(lines, times, 32);
        const float logH = ImGui::GetTextLineHeightWithSpacing() * 10.f;
        if (ImGui::BeginChild("##aimlog", ImVec2(0.f, logH), true,
                ImGuiWindowFlags_HorizontalScrollbar)) {
            if (n == 0) {
                ImGui::TextDisabled("(no aim events yet - hold the aim key in a raid)");
            }
            for (int i = 0; i < n; ++i) {
                const ImVec4 col = std::string(lines[i]).find("aim idle") != std::string::npos
                    ? ImVec4(0.65f, 0.65f, 0.65f, 1.f)
                    : ImVec4(0.80f, 0.90f, 1.f, 1.f);
                ImGui::TextColored(col, "%8.1fs  %s", times[i] * 0.001, lines[i]);
            }
            if (autoScroll)
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::Separator();
    ArcMenuLayout::HoverableTextF("Enable Aimbot: %s", var::enable_aimbot ? "yes" : "no");
    ArcMenuHoverTooltip("Copy of the Aimbot tab master switch.");
    ArcMenuLayout::HoverableTextF("KmBox ready: %s", g_kmbox.kmboxConfig.initialized ? "yes" : "no");
    ArcMenuHoverTooltip("MAKCU/KMBox connection. Aim moves are blocked while no.");
    ArcMenuLayout::HoverableTextF("Raid active: %s", engine.IsEspRaidActive() ? "yes" : "no");
    ArcMenuHoverTooltip("The aim tick only runs during a raid.");
    ArcMenuLayout::HoverableTextF("Menu open: %s", showmenu ? "yes" : "no");
    ArcMenuHoverTooltip("Aim is paused while this overlay menu is open.");
    ImGui::Separator();
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
    ArcMenuHoverTooltip("Players that would draw right now (passes the distance and admission gates).");
    ImGui::Separator();
    ArcMenuLayout::HoverableText("TimeSeconds probe");
    ArcMenuHoverTooltip("Finds UWorld's clock offset by sampling UWorld+0x000..0x2000 twice a second apart. This build reorders UWorld, so the stock offset does not apply.");
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
    ArcMenuHoverTooltip("Camera transform is sane and usable by ESP.");
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
    ImGui::Keybind("Debug overlay hotkey", &var::debug_overlay_key);
    ArcMenuHoverTooltip("Click, then press any key: toggles the offset debug panel (green timers). Default F8. Saved to auto_config.ini.");
    
    ArcMenuLayout::Checkbox("Skeleton lag probe", &var::debug_skeleton_lag);
    ArcMenuHoverTooltip(
        "Draws box anchor (red) vs bone pelvis (green) with age/delta text, "
        "and prints [debugSkel] lines. Shows why the skeleton trails moving targets.");
    ArcMenuLayout::Checkbox("Log ghost bot decisions", &var::debug_ghost_bots);
    ArcMenuHoverTooltip(
        "Write every bot draw rejection and ghost-expiry decision to help/entity_diagnostics.ndjson. "
        "Includes actor address, name, reason, position age, distance, identity, and broken state.");
    // Debug-flag-gated baseline: writes an esp_paint_profile line (frame time +
    // per-lock try/miss/wait) to help/entity_diagnostics.ndjson once a second.
    ArcMenuLayout::Checkbox("ESP paint profile (frame + lock waits)",
        &var::debug_esp_paint);
    ArcMenuHoverTooltip(
        "Write a once-a-second esp_paint_profile line (paint frame time + per-lock "
        "try/miss/wait) to help/entity_diagnostics.ndjson.");
    ArcMenuLayout::Checkbox("Hatch detect probe", &var::debug_hatch_detect);
    ArcMenuHoverTooltip(
        "Prints [debugHatch] per hatch-candidate actor: fname, class fname, "
        "extract state and why it was/wasn't admitted. Shows why hatches don't draw.");
    ArcMenuLayout::Checkbox("Near loot HUD", &var::show_near_loot_hud);
    ArcMenuHoverTooltip(
        "List nearby ground pickups on-screen. Press F7 to mark the nearest as picked "
        "(hides it via sticky so ESP does not blink it back).");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Embree VisCheck");
    ArcMenuHoverTooltip("Raycast visibility diagnostics: obstruction behaviour, wireframe overlays, and the world BVH scene.");
    ArcMenuLayout::Checkbox("Dynamic Obstructions (Smoke/Shields)", &UpgradeOptions::visibilityObstructionSpheres);
    ArcMenuHoverTooltip(
        "Block rays through smoke, gas, fog, barriers, and barricades.");

    ImGui::Separator();
    ArcMenuLayout::HoverableText("Debug Wireframe Overlay");
    ArcMenuHoverTooltip("Draw the raycast scene and rays to screen. Read-only; only drawn when you turn these on.");
    ArcMenuLayout::Checkbox("Draw World Mesh Triangles", &UpgradeOptions::collisionDebugShapes);
    ArcMenuHoverTooltip(
        "Draw white wireframe triangles for the Embree collision scene.");
    ArcMenuLayout::Checkbox("Draw Obstruction Shapes", &UpgradeOptions::visDebugObstructions);
    ArcMenuHoverTooltip(
        "Draw orange wireframe boxes/spheres for active smokes and barriers.");
    ArcMenuLayout::Checkbox("Draw Raycast Lines", &UpgradeOptions::collisionDebugRays);
    ArcMenuHoverTooltip(
        "Draw each camera-to-bone ray. Green = clear, red = blocked.");
    ArcMenuLayout::Checkbox("Draw Hit Markers", &UpgradeOptions::collisionDebugHitMarkers);
    ArcMenuHoverTooltip(
        "Show a red dot where blocked rays stop.");
    ArcMenuLayout::SliderFloat("Debug Draw Radius", "##vis_dbg_rad",
        &UpgradeOptions::collisionDebugRadiusM, 10.f, 200.f, "%.0f m");
    ArcMenuHoverTooltip("Only draw debug shapes/rays within this many metres of the camera.");
    ImGui::SliderInt("Max Raycast Lines", &UpgradeOptions::collisionDebugMaxRays, 1, 256);
    ArcMenuHoverTooltip("Cap on how many ray lines are drawn per frame. Lower = less clutter.");
    ImGui::SliderInt("Debug Triangle Cap", &UpgradeOptions::collisionDebugSceneTriCap, 500, 30000);
    ArcMenuHoverTooltip("Cap on how many scene triangles the wireframe overlay draws per frame.");

    ImGui::Separator();
    const bool visReady = EmbreeVis::g_visibility_meshes_ready.load(std::memory_order_acquire);
    const int visMeshCount = EmbreeVis::g_last_mesh_actors.load(std::memory_order_relaxed);
    const int visTriCount = EmbreeVis::g_last_triangles.load(std::memory_order_relaxed);
    ArcMenuLayout::HoverableTextF("Scene status: %s  |  Meshes: %d  |  Triangles: %d",
        visReady ? "Ready" : "Waiting / Building", visMeshCount, visTriCount);
    ArcMenuHoverTooltip("Whether the wall scene finished building, and how many meshes/triangles it holds.");
    if (ImGui::Button("Rebuild World Mesh Now")) {
        EmbreeVis::RequestRebuildWorldMeshes();
    }
    ArcMenuHoverTooltip("Clear and rebuild the Embree world BVH on the next visibility tick.");
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
    WrappedBulletText("Aimbot: hold the aim key to move the crosshair inside the selection circle; stop zone, speed, response and X/Y calibration are live.");
    WrappedBulletText("ESP visibility colours can be toggled from the Aimbot tab.");
    WrappedBulletText("Crosshair sub-tab: screen-center reticle style, colour, size, gap, and animation.");

    ImGui::Spacing();
    ImGui::TextWrapped("Settings tab:");
    WrappedBulletText("General sub-tab: DMA controller status; re-init controller / KmBox; overlay monitor; MAKCU (COM) or Net (IP/UUID) device settings.");
    WrappedBulletText("Debug sub-tab: offset validation, near-loot HUD, KmBox/config/gamepad status, engine cache and camera readouts.");

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
    const bool itemHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
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

    // Search filters settings rows only — the main menu is pure navigation.
    if (g_currentPage > 0) {
        ArcMenuLayout::DrawSearchBox();
        ImGui::Spacing();
    }

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

    // Version stamp, very bottom of the sidebar, bottom-right and small.
    ImGui::SetWindowFontScale(0.7f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.64f, 0.9f));
    {
        const char* ver = "v0.001";
        const float tw = ImGui::CalcTextSize(ver).x;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > tw)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - tw);
        ImGui::TextUnformatted(ver);
    }
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);

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
