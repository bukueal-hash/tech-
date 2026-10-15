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
#include "../../Core/PoiParser.hpp"
#include "../../Core/PoiStore.hpp"
#include "../../Core/ColorProfiles.hpp"
#include "../Utils/ColorProfile.h"
#include "../../Functions/WorldScanCommon.h"
#include "../../Functions/CollisionRead.h"
#include "../../Functions/CollisionBvh.h"
#include "../../Functions/CollisionDebug.h"
#include "../Render.h"
#include "../OverlayHost.h"
#include "../Utils/Variables/index.h"
#include "../Utils/AutoConfig.h"
#include "../Utils/UpgradeOptions.hpp"
#include "../Utils/Visuals/Crosshair.hpp"
#include "../../Hardware/KmBox.h"
#include "../../Input/Controller.h"
#include "../../Input/DmaGamepad.h"
#include "../../Input/KeyBind.h"

#include "ImGuiKeybind.h"
#include "../../Core/WorldItemCategory.h"
#include "../../Core/EntityFilters.hpp"
#include "AimbotDebugPanel.hpp"

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
        || var::show_player_kit || var::enable_aimbot;
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
                    ImGui::BeginDisabled(!var::box);
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
                    ImGui::EndDisabled();
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
                        "Directional screen-edge markers for fresh living enemies outside the screen, within normal player ESP range. Does not enable player ESP.");
                    ArcMenuHoverTooltip("Directional screen-edge markers for fresh living enemies outside the screen, within normal player ESP range. Does not enable player ESP.");
                    ArcMenuLayout::SliderFloat("Arrow size", "##offscreen_size", &UpgradeOptions::arrowSize, 5.f, 30.f, "%.0f px");
                    ArcMenuHoverTooltip("Off-screen triangle length in screen pixels.");
                    ArcMenuLayout::SliderFloat("Arrow inset", "##offscreen_inset", &UpgradeOptions::arrowInset, 20.f, 200.f, "%.0f px");
                    ArcMenuHoverTooltip("Distance from the screen edge; automatically bounded on small viewports.");
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
                    ArcMenuLayout::Label("Behind cover");
                    ArcMenuLayout::ColorEditAtColumn("##bot_occluded", var::bot_color_occluded);
                    ArcMenuHoverTooltip("Color for robots that are visible on the radar but blocked from the camera.");
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
        if (ImGui::BeginTabItem("Collision"))
        {
            ArcMenuHoverTooltip(
                "Collision geometry readout: what the AggGeom reader pulled out of the target, "
                "per asset, plus the AABB agreement check. Read-only - it changes nothing that "
                "is drawn.");
            DrawArcCollisionContent();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// Collision geometry (visibility groundwork): its own tab, because it is a
// reader with its own state rather than a loot setting. Moved here unchanged
// from under Loot.
void DrawArcCollisionContent()
{
    ArcMenuLayout::HoverableText("Collision geometry (AggGeom)");
    ArcMenuHoverTooltip(
        "Reads UStaticMeshComponent -> UStaticMesh -> UBodySetup -> FKAggregateGeom on the target "
        "and caches the decoded shapes per asset. This is the visibility groundwork: it collects "
        "evidence, it does not change what is drawn or aimed at. Best run in raid, and again after "
        "a map load so the cache covers the new level.");
    ArcMenuLayout::Checkbox("Enable collision probe", &UpgradeOptions::collisionProbe);
    ArcMenuHoverTooltip(
        "Arm the probe. Off = nothing below is read. On = the button below reads collision "
        "geometry for up to 256 cached actors.");
    {
        const CollisionRead::ProbeResult cr = CollisionRead::GetProbeResult();
        ImGui::BeginDisabled(cr.running || !UpgradeOptions::collisionProbe);
        if (ImGui::Button("Run Collision Probe"))
            CollisionRead::StartProbe();
        ImGui::SetItemTooltip(
            "Runs on a background job and writes collision_asset / collision_probe lines to the "
            "verify log. Takes under a second for a warm actor list.");
        ImGui::SameLine();
        if (ImGui::Button("Clear collision cache"))
            CollisionRead::ClearCache();
        ImGui::SetItemTooltip(
            "Drop the per-asset cache. Asset pointers only mean something inside one level, so "
            "clear this after a map change before the next run.");
        ImGui::EndDisabled();

        if (cr.running) {
            ArcMenuLayout::HoverableText("reading geometry...");
            ArcMenuHoverTooltip("The probe job is running; results appear when it finishes.");
        } else if (!cr.ran) {
            ArcMenuLayout::HoverableText("not run yet");
            ArcMenuHoverTooltip("Nothing collected yet - run the probe above (in raid).");
        } else if (!cr.note.empty()) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", cr.note.c_str());
            ArcMenuHoverTooltip("The probe stopped early: the count below is what it did read.");
            ArcMenuLayout::HoverableTextF("actors %d walked  %d static-mesh roots",
                cr.actorsWalked, cr.staticMeshComponentRoots);
        } else {
            ArcMenuLayout::HoverableTextF("%d/%d assets parsed  (%d shared, %d null BodySetup)",
                cr.assetsParsed, cr.assetsUnique, cr.assetsFromCache, cr.assetsBodySetupNull);
            ArcMenuHoverTooltip(
                "Assets whose AggGeom decoded / assets read / actors served from the cache / "
                "assets whose UStaticMesh::BodySetup read null (the slot is Transient).");
            ArcMenuLayout::HoverableTextF("%d primitives  (%d declared, %d capped, %d unsupported)",
                cr.primitivesStored, cr.declaredByShape[0] + cr.declaredByShape[1]
                    + cr.declaredByShape[2] + cr.declaredByShape[3],
                cr.truncatedByShape[0] + cr.truncatedByShape[1]
                    + cr.truncatedByShape[2] + cr.truncatedByShape[3],
                cr.unsupportedByShape[4] + cr.unsupportedByShape[5] + cr.unsupportedByShape[6]
                    + cr.unsupportedByShape[7] + cr.unsupportedByShape[8]);
            ArcMenuHoverTooltip(
                "Stored shapes, shapes the assets declared, declared shapes that hit the per-array "
                "cap, and non-empty arrays this build's dump gives no layout for.");
            ArcMenuLayout::HoverableTextF("boxes %d  spheres %d  capsules %d  convex %d",
                cr.parsedByShape[1], cr.parsedByShape[0], cr.parsedByShape[2], cr.parsedByShape[3]);
            ArcMenuHoverTooltip("Decoded shapes by type.");
            ArcMenuLayout::HoverableTextColoredF(
                (cr.boundsChecked > 0 && cr.boundsAgree25cm * 2 >= cr.boundsChecked)
                    ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "AABB vs asset bounds: %d/%d within 25cm (median %.1f cm)",
                cr.boundsAgree25cm, cr.boundsChecked, cr.medianBoundsDeltaCm);
            ArcMenuHoverTooltip(
                "The decoded shapes' world AABB against the asset's own ExtendedBounds, under the "
                "same component transform. Both come from the target, so agreement is evidence the "
                "decode is right; a large delta means complex-only collision or a decode problem.");
            ArcMenuLayout::HoverableTextF("roots skipped: %d  (missing %d)",
                cr.otherComponentRoots, cr.rootsMissing);
            ArcMenuHoverTooltip(
                "Actors whose root component is not a StaticMeshComponent (the static-mesh path "
                "does not apply to them) and actors whose root would not resolve.");
            if (cr.skippedRootClasses > 0) {
                ArcMenuLayout::HoverableTextF("skipped root classes: %s %s %s",
                    cr.skippedRootClassSample[0].c_str(),
                    cr.skippedRootClassSample[1].c_str(),
                    cr.skippedRootClassSample[2].c_str());
                ArcMenuHoverTooltip(
                    "Bounded sample of the root component classes the static-mesh path skipped, "
                    "so the coverage gap has names instead of only a count.");
            }
            ArcMenuLayout::HoverableTextF("cache: %s", CollisionRead::CacheSummary().c_str());
            ArcMenuHoverTooltip("Per-asset collision cache held by the reader.");
            ArcMenuLayout::HoverableTextF("%llu reads  %.1f KB  %.0f ms",
                static_cast<unsigned long long>(cr.reads),
                static_cast<double>(cr.bytesRead) / 1024.0, cr.elapsedMs);
            ArcMenuHoverTooltip("DMA cost of the run, for the read ledger and the frame budget.");
        }
    }

    // ── Stage 3/4: the triangle BVH and its queries ────────────────────────
    ImGui::Separator();
    ArcMenuLayout::HoverableText("Collision BVH (Embree)");
    ArcMenuHoverTooltip(
        "Tessellates every cached asset instance into world space and builds one triangle BVH over "
        "it. Build it after a probe run, and again after a map load. The self-test proves the ray "
        "pipeline on synthetic geometry, so it works with no target attached.");
    static CollisionBvh::BuildStats lastBuild{};
    static CollisionBvh::SelfTestResult lastTest{};
    static std::string lastBuildNote;
    {
        ImGui::BeginDisabled(!UpgradeOptions::collisionProbe);
        if (ImGui::Button("Build collision BVH")) {
            lastBuild = CollisionBvh::RebuildFromCache();
            lastBuildNote = lastBuild.note;
        }
        ImGui::SetItemTooltip(
            "Tessellate the cached shapes and commit the BVH. Seconds on a full level; runs on the "
            "UI thread because it is an explicit request, never on the aim tick.");
        ImGui::SameLine();
        if (ImGui::Button("Run BVH self-test")) {
            lastTest = CollisionBvh::RunSelfTest();
            lastBuildNote.clear();
            lastBuild = CollisionBvh::BuildStats{};  // the self-test owns the scene now
        }
        ImGui::SetItemTooltip(
            "Six synthetic checks: a box tessellates to 12 triangles, a ray through a wall is "
            "blocked, a ray around it is clear, the closest hit lands on the near face at 200 cm, a "
            "span shorter than the clearance is not judged, and a cleared scene answers clear.");
        ImGui::EndDisabled();

        // Opt-in, takes effect on the next build. Adds two BVH queries per ray.
        static bool compareOcclusion = false;
        if (ImGui::Checkbox("Compare box vs mesh occlusion", &compareOcclusion))
            CollisionBvh::SetOcclusionComparison(compareOcclusion);
        ArcMenuHoverTooltip(
            "Counts, per query, whether the coarse occluder boxes and the decoded mesh/hull "
            "triangles block the same ray. Takes effect on the next Build collision BVH.");
        if (compareOcclusion) {
            const CollisionBvh::OcclusionCompare cmp = CollisionBvh::CompareStats();
            ImGui::Text("both %llu  box-only %llu  mesh-only %llu  neither %llu  mismatch %llu",
                static_cast<unsigned long long>(cmp.bothBlock),
                static_cast<unsigned long long>(cmp.boxOnly),
                static_cast<unsigned long long>(cmp.meshOnly),
                static_cast<unsigned long long>(cmp.neither),
                static_cast<unsigned long long>(cmp.mismatch));
        }

        if (!lastBuildNote.empty()) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(1.f, 0.75f, 0.35f, 1.f), "%s", lastBuildNote.c_str());
            ArcMenuHoverTooltip("No scene was published: fix this before trusting a query.");
        } else if (lastBuild.built) {
            ArcMenuLayout::HoverableTextColoredF(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                "gen %llu: %d tris from %d assets / %d instances",
                static_cast<unsigned long long>(lastBuild.generation), lastBuild.triangles,
                lastBuild.assets, lastBuild.instances);
            ArcMenuHoverTooltip(
                "The published scene: generation counter, triangles, contributing assets and world "
                "placements actually tessellated.");
            ArcMenuLayout::HoverableTextF("boxes %d  spheres %d  capsules %d  hulls %d  hull-as-box %d  skipped %d",
                lastBuild.boxes, lastBuild.spheres, lastBuild.capsules,
                lastBuild.convexHulls, lastBuild.convexAsBox, lastBuild.skipped);
            ArcMenuHoverTooltip(
                "Shapes by type. Hulls are tessellated from their real point cloud; hull-as-box counts "
                "hulls whose cloud did not decode and fell back to the ElemBox; skipped means no usable size.");
            ArcMenuLayout::HoverableTextF("tess %.0f ms   commit %.0f ms   total %.0f ms",
                lastBuild.tessMs, lastBuild.commitMs, lastBuild.totalMs);
            ArcMenuHoverTooltip("Where the build time went: shape tessellation vs BVH commit.");
        }

        if (lastTest.ran) {
            ArcMenuLayout::HoverableTextColoredF(
                lastTest.passed ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.f, 0.4f, 0.4f, 1.f),
                "self-test %s: %d/%d cases", lastTest.passed ? "PASS" : "FAIL",
                lastTest.cases - lastTest.failed, lastTest.cases);
            ArcMenuHoverTooltip("Synthetic ray checks through the real tessellation and BVH code.");
            if (!lastTest.passed)
                ArcMenuLayout::HoverableTextF("%s", lastTest.detail.c_str());
            ArcMenuHoverTooltip("Per-case result with the observed value.");
        }
        ArcMenuLayout::HoverableTextF("queries: %s", CollisionBvh::Summary().c_str());
        ArcMenuHoverTooltip("Published scene and query counters since launch.");
    }

    // ── Vischeck as a feature (not a debug draw) ───────────────────────────
    ImGui::Separator();
    ArcMenuLayout::HoverableText("Visibility (vischeck)");
    ArcMenuHoverTooltip(
        "What the collision BVH's occlusion answer DOES, beyond drawing rays. Both switches fail "
        "open: with no scene published every segment reads clear, so neither can change behaviour "
        "until the probe has built one. The scene's occluders include conservative bounds boxes for "
        "meshes whose real collision is complex, so cover can read where a thin gap exists.");
    ArcMenuLayout::Checkbox("Refuse occluded aim target", &UpgradeOptions::visCheckAim);
    ArcMenuHoverTooltip(
        "The tick's chosen target is refused (TargetRejection::Occluded) when the BVH finds cover "
        "between the camera and the aim point, so the aim is never pulled at something the shot "
        "cannot reach. The lock itself is kept -- clearing cover resumes the same target.");

    // ── Debug draw: the shapes and the rays ────────────────────────────────
    ImGui::Separator();
    ArcMenuLayout::HoverableText("Collision debug draw");
    ArcMenuHoverTooltip(
        "Draws what the BVH was built from and what the rays answered, so a wrong decode and a "
        "wrong query can be told apart by eye. Overlay only: it reads nothing and changes nothing "
        "that is drawn or aimed at. Per-frame caps live in auto_config.ini "
        "(collision_debug_max_prims, collision_debug_max_rays).");
    {
        const CollisionDebug::Stats ds = CollisionDebug::LastFrame();
        ArcMenuLayout::Checkbox("Draw collision shapes", &UpgradeOptions::collisionDebugShapes);
        ArcMenuHoverTooltip(
            "Wireframe of every decoded primitive near the camera: boxes are their 12 edges, "
            "spheres three great circles, capsules two rings plus four sides, and convex hulls "
            "their box (magenta) until the hull point cloud is read. Blue = box, green = sphere, "
            "amber = capsule. If a shape does not line up with the object it belongs to, the "
            "decode or the element transform is wrong.");
        ArcMenuLayout::Checkbox("Draw visibility rays", &UpgradeOptions::collisionDebugRays);
        ArcMenuHoverTooltip(
            "One ray from the camera to each drawn world object and each drawn actor, coloured by "
            "the query: green clear, red blocked, grey not judgeable (the span is shorter than the "
            "biases). Aimed at the head anchor where one exists, so the line and the box disagree "
            "only if the query does. Capped per frame; if the budget is hit the panel says so.");
        ArcMenuLayout::Checkbox("Mark hit points", &UpgradeOptions::collisionDebugHitMarkers);
        ArcMenuHoverTooltip("Draw a marker at the closest blocker along each ray.");
        ArcMenuLayout::Checkbox("Draw scene triangles", &UpgradeOptions::collisionDebugSceneDecoded);
        ArcMenuHoverTooltip(
            "Wireframe of the triangles the BVH was actually built from - the shapes the decode "
            "read, placed in the world. Icy blue, thin lines, culled by the debug radius below "
            "(centroid distance). This is the scene's own buffers, not a re-tessellation, so what "
            "you see is exactly what a ray is tested against.");
        ArcMenuLayout::Checkbox("Draw occluder boxes", &UpgradeOptions::collisionDebugSceneBoxes);
        ArcMenuHoverTooltip(
            "Amber: the coarse bounds boxes standing in for placed meshes whose collision is "
            "complex (tri-mesh) and therefore does not decode from FKAggregateGeom. A level's "
            "walls are usually these, and a box is never smaller than the real mesh, so this is "
            "the layer that makes over-blocking visible. Each lane is capped separately "
            "(collision_debug_scene_tri_cap).");
        ArcMenuLayout::SliderFloat("Debug radius", "##collision_dbg_radius",
            &UpgradeOptions::collisionDebugRadiusM, 1.f, 200.f, "%.0f m");
        ArcMenuHoverTooltip(
            "Only geometry and rays within this distance of the camera are drawn; larger means "
            "more lines per frame.");
        ArcMenuLayout::HoverableTextF("last frame: %d prims / %d components, %d segs",
            ds.primitives, ds.components, ds.segments);
        ArcMenuLayout::HoverableTextF("shapes: %d decoded, %d stand-in",
            ds.decoded, ds.standIns);
        ArcMenuHoverTooltip(
            "Decoded = from FKAggregateGeom (blue box, green sphere, amber capsule, magenta "
            "hull). Stand-in (pink) = geometry no AggGeom exists for: a player or bot capsule "
            "read from its CapsuleComponent, or a primitive component's own LocalBounds box. "
            "Players and bots can only ever appear as stand-ins, because their root is a "
            "capsule or a skeletal mesh, not a static mesh.");
        ArcMenuLayout::HoverableTextF("rays: %d world, %d entity",
            ds.worldRays, ds.entityRays);
        ArcMenuHoverTooltip("What the debug draw actually emitted in the last painted frame.");
        ArcMenuLayout::HoverableTextF("rays: %d clear, %d blocked", ds.clear, ds.occluded);
        ArcMenuHoverTooltip("Query outcomes for the rays traced in that frame.");
        ArcMenuLayout::HoverableTextF("rays unanswered: %d span too short, %d no BVH",
            ds.tooClose, ds.noScene);
        ArcMenuHoverTooltip(
            "Two different reasons a ray had no answer, and they need opposite fixes. \"Span too "
            "short\" is about the two points: the segment is shorter than the 5 cm tnear plus the "
            "50 cm clearance, so neither a hit nor a miss would mean anything. \"No BVH\" is about "
            "the scene: nothing is published, so every query answers CLEAR by contract and the "
            "whole vischeck is inert -- run the collision probe. Those rays are drawn steel blue, "
            "an unjudgeable span grey, and a no-BVH ray no longer counts as a clear one.");
        if (ds.noScene > 0)
            ArcMenuLayout::HoverableText("no BVH published - run the collision probe");
        if (ds.culled > 0 || ds.projectFailed > 0) {
            ArcMenuLayout::HoverableTextF("culled %d, off-view %d", ds.culled, ds.projectFailed);
            ArcMenuHoverTooltip(
                "Primitives skipped by the radius or the frame cap, and loops dropped because a "
                "point left the view.");
        }
        if (ds.raysBudgetExhausted || ds.entityRaysBudgetExhausted)
            ArcMenuLayout::HoverableTextF("ray budget reached (%s) - raise collision_debug_max_rays",
                ds.entityRaysBudgetExhausted ? "entity" : "world");
        ArcMenuLayout::HoverableTextF("scene: %d tris | drew %d (decoded %d / occluder %d)",
            ds.sceneTriangles, ds.sceneDecoded + ds.sceneBoxes, ds.sceneDecoded, ds.sceneBoxes);
        ArcMenuHoverTooltip(
            "The published scene: all committed triangles, then how many were drawn in the last "
            "painted frame from each lane. Zero triangles drawn against a non-zero scene means the "
            "radius cull or a lane cap stopped them, which the lines below report.");
        if (ds.sceneNoScene)
            ArcMenuLayout::HoverableText("no BVH published - nothing to wireframe");
        if (ds.sceneCulled > 0 || ds.sceneTruncated)
            ArcMenuLayout::HoverableTextF("scene culled %d%s", ds.sceneCulled,
                ds.sceneTruncated ? ", lane cap reached" : "");
    }
}

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
    ArcMenuLayout::Checkbox("World bounds", &UpgradeOptions::worldBounds);
    ArcMenuHoverTooltip("Wireframe asset bounds for supported root StaticMeshComponents only. Worker reads validated asset extents and transform; invalid, unsupported or older-than-2s bounds are omitted. Adds scanner reads; labels remain unchanged.");
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
        int om = std::clamp(var::state_opened_mode, 0, 3);
        if (ArcMenuLayout::Combo("Opened container", "##state_opened_mode", &om,
                kStateModes, IM_ARRAYSIZE(kStateModes)))
            var::state_opened_mode = om;
        ArcMenuHoverTooltip(
            "How a looted/opened container draws in BOTH the ESP and the radar, "
            "so the two surfaces can never disagree.");

        int dm = std::clamp(var::state_dead_mode, 0, 3);
        if (ArcMenuLayout::Combo("Dead bot / Arc", "##state_dead_mode", &dm,
                kStateModes, IM_ARRAYSIZE(kStateModes)))
            var::state_dead_mode = dm;
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
            ArcMenuHoverTooltip("Aim assist settings: target selection, priority, motion, and hardware.");
            
            ArcMenuLayout::Checkbox("Enable Aimbot", &var::enable_aimbot);
            ArcMenuHoverTooltip("KmBox hardware aim — requires MAKCU or Net device connected. One profile: players and ARC are targeted alike; whichever candidate is nearest the crosshair wins.");

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
    static const char* motionPresets[] = { "Sticky", "Balanced", "Natural", "Custom" };
    if (ArcMenuLayout::Combo("Motion preset", "##motion_preset", &UpgradeOptions::playerPreset, motionPresets, IM_ARRAYSIZE(motionPresets)))
        UpgradeOptions::ApplyPreset(UpgradeOptions::playerPreset);
    ArcMenuHoverTooltip("Deterministic motion presets: Sticky 65ms/50-count cap, Balanced 120ms/35, Natural 220ms/20. Applies to players and ARC alike. No injected shake; calibration stays unchanged.");
    struct MotionRow { const char* label; const char* id; float* value; float lo, hi; const char* tip; };
    const MotionRow motionRows[] = {
        {"Response", "##aim_response", &UpgradeOptions::playerResponseMs,20.f,500.f,"Response time in milliseconds; lower pulls faster. Time-normalized exponential smoothing, not a reaction-delay timer. Shared by players and ARC."},
        {"Output rate", "##aim_rate", &UpgradeOptions::playerRate,30.f,250.f,"Maximum hardware commands per second; actual worker/device cadence can be lower."},
        {"Calibration X", "##aim_counts", &UpgradeOptions::playerCountsPerPixel,0.05f,10.f,"Horizontal mouse counts per overlay pixel; hardware/game sensitivity dependent. Requires real hardware calibration."},
        {"Calibration Y", "##aim_counts_y", &UpgradeOptions::playerCountsPerPixelY,0.05f,10.f,"Vertical counts per overlay pixel, independent of X. Defaults to the X value; lower it to stop vertical overshoot."},
        {"Response curve", "##aim_curve", &UpgradeOptions::playerResponseCurve,0.f,1.f,"0 is exactly linear (the previous behaviour). Above 0, error nearer than 30 px is pulled slower and error further away faster."},
        {"Per-tick cap", "##aim_tickcap", &UpgradeOptions::playerMaxCountsPerTick,0.f,100.f,"Absolute mouse-count cap per tick; always tightens, never loosens, the distance burst cap. 0 defers entirely."},
        {"Count cap", "##aim_cap", &UpgradeOptions::playerMaxCounts,1.f,100.f,"Maximum mouse-count vector magnitude per command, including fractional accumulation and integer output."},
    };
    for (const auto& row : motionRows) {
        if (ArcMenuLayout::SliderFloat(row.label,row.id,row.value,row.lo,row.hi,"%.2f"))
            UpgradeOptions::playerPreset=3;
        ArcMenuHoverTooltip(row.tip);
    }
    // One profile: FOV and range are the Aimbot tab's own (aimbot_fov /
    // aimbot_distance) for players and ARC alike. The separate ARC cone and ARC
    // distance sliders are gone, and so is the ARC aim hotkey — it was never read
    // by the aim path, so it only ever looked like a gate. Saved arc_aim_fov /
    // arc_aim_distance / arc_aim_key keys stay readable; they no longer drive
    // anything.
    ImGui::Keybind("ARC part hotkey", &UpgradeOptions::arcWeakspotKey);
    ArcMenuHoverTooltip("While ARC aim is active, select the cached component point nearest the crosshair. Component order is not proven to match HP indices, so no damage/weakspot claim is made. Missing points fall back to the normal heart point.");
    ArcMenuLayout::Checkbox("ARC prediction", &UpgradeOptions::arcPrediction);
    ArcMenuHoverTooltip("Lead ARC cached part/heart position using fresh cached velocity and the shared configured bullet speed; off by default.");
    ArcMenuLayout::Checkbox("ARC sticky lock", &UpgradeOptions::arcSticky);
    ArcMenuHoverTooltip("Retain the ARC lock within its FOV plus the shared sticky pixel bias; never retain an unobserved stale target for hardware movement.");
    ArcMenuLayout::Checkbox("Strict target gate (player)", &UpgradeOptions::playerStrictGate);
    ArcMenuHoverTooltip("On by default. Strict drops a candidate whose aim data is not this tick's own sample (an interpolated hold-drift point) or is older than the pipeline's freshness bound, and names the reason. Off restores the pre-4.2 candidate rules exactly.");
    ArcMenuLayout::Checkbox("Strict target gate (ARC)", &UpgradeOptions::arcStrictGate);
    ArcMenuHoverTooltip("Same gate for ARC targets, where the age of the resolved bot entry is what is bounded. A bot's aim point is inferred from its box, so it is reported as estimated either way -- estimated is a label, not a refusal.");
    ArcMenuLayout::Checkbox("FOV axis is horizontal", &UpgradeOptions::fovAxisHorizontal);
    ArcMenuHoverTooltip("Off (default) keeps the projection's vertical FOV reading it has always used. On makes the projection agree with the aim FOV ring, which has always measured its radius from half the width. Only the projection's reading changes; no other value moves.");
    ArcMenuLayout::Checkbox("Practice / calibration reference", &var::practice_mode);
    ArcMenuHoverTooltip("Draws a screen-centre cross, the FOV rings, a 5-degree axis ruler and a counts probe for reading counts-per-pixel off real input. Nothing else is drawn and normal ESP output does not depend on it.");
    ArcMenuLayout::Checkbox("Random bone", &var::randombone);
    ArcMenuHoverTooltip("Optional weighted high-value bone drift with interpolated transitions. Overrides Aim bone; off by default in a fresh config. Existing saved choice is preserved.");
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
    ImGui::BeginDisabled(!var::sticky_target_lock && !UpgradeOptions::arcSticky);
    ArcMenuLayout::SliderFloat(
        "Sticky FOV bias",
        "##aim_sticky_fov_bias_px",
        &var::aim_sticky_fov_bias_px,
        0.f,
        120.f,
        "%.0f px");
    ArcMenuHoverTooltip("Extra FOV pixels granted to a sticky Player or ARC lock. Retention is bounded by its own FOV plus this bias, never unlimited.");
    ImGui::EndDisabled();

    ArcMenuLayout::HoverableText("Hardware motion uses deterministic response smoothing. Legacy jitter and unobserved-target movement are disabled.");
    ArcMenuHoverTooltip("Old humanizer, randomized pull profiles and hardware loss-of-sight replay are no longer used. Saved legacy keys remain readable for compatibility.");

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
            ImGui::Separator();

            // Raid hotkeys (B3/B4) are polled globally and cannot be rebound, so
            // this is the one place a user can discover them.
            ArcMenuLayout::HoverableText("Raid hotkeys (hold Alt, menu closed):");
            ArcMenuLayout::HoverableTextF(
                "Alt+N  Near loot HUD        Alt+C  Crosshair\n"
                "Alt+M  Radar map mode       Alt+G  World loot ESP\n"
                "Alt+T  Triggerbot           Alt+H  Raid HUD\n"
                "Alt+R  Force rescan (3s cooldown)\n"
                "Alt+- / Alt+=  Radar zoom in / out");
            ArcMenuHoverTooltip(
                "Alt must be pressed before the letter — pressing the letter first "
                "and then Alt does nothing. The overlay is click-through during a "
                "raid, so these are polled globally and the keystroke also reaches "
                "the game; Alt+letter keeps that from colliding with normal binds. "
                "Rescan flushes the read cache and rebuilds the ESP caches.");

            // D19 color profiles: 8 switchable palettes over every color var.
            ImGui::Separator();
            ArcMenuLayout::HoverableText("Color profiles");
            ArcMenuHoverTooltip(
                "Swap the whole palette in one step. Capture stores the colors "
                "you have set in the menu into a slot; picking a slot writes it "
                "back. Slots live in Data\\color_profiles.json, separate from "
                "auto_config.ini, so a look can be shared without sharing an "
                "entire config.");

            static const char* const kProfileLabels[ColorProfiles::kSlotCount + 1] = {
                "Off", "Slot 1", "Slot 2", "Slot 3", "Slot 4",
                "Slot 5", "Slot 6", "Slot 7", "Slot 8"
            };
            int profileSel = var::color_profile_slot + 1;  // -1 (off) -> 0
            if (profileSel < 0 || profileSel > ColorProfiles::kSlotCount)
                profileSel = 0;
            if (ArcMenuLayout::Combo(
                    "Apply profile", "##color_profile_slot",
                    &profileSel, kProfileLabels, ColorProfiles::kSlotCount + 1)) {
                var::color_profile_slot = profileSel - 1;
                if (var::color_profile_slot >= 0)
                    ColorProfile::ApplySlot(var::color_profile_slot);
                AutoConfig_MarkDirty();
            }
            ArcMenuHoverTooltip(
                "Off leaves your colors untouched. Picking a slot writes that "
                "saved palette over the current colors; keys the slot does not "
                "contain are left alone, so an older profile cannot wipe a "
                "newly added color.");

            ColorProfile::EnsureLoaded();
            const int profileSlot = var::color_profile_slot;
            static int s_profileSave = 0;  // 0 idle, 1 ok, 2 failed
            ImGui::BeginDisabled(profileSlot < 0);
            if (ImGui::Button("Save colors into this slot"))
                s_profileSave = ColorProfile::CaptureSlot(profileSlot) ? 1 : 2;
            ArcMenuHoverTooltip(
                "Copies every color currently set in the menu into the selected "
                "slot and writes Data\\color_profiles.json.");
            if (ImGui::Button("Clear this slot"))
                s_profileSave = ColorProfile::ClearSlot(profileSlot) ? 1 : 2;
            ArcMenuHoverTooltip("Forgets the selected slot's saved palette.");
            ImGui::EndDisabled();

            if (s_profileSave == 1)
                ArcMenuLayout::HoverableTextColoredF(
                    ImVec4(0.45f, 1.f, 0.55f, 1.f), "Profile file saved");
            else if (s_profileSave == 2)
                ArcMenuLayout::HoverableTextColoredF(
                    ImVec4(1.f, 0.45f, 0.45f, 1.f),
                    "Could not write Data\\color_profiles.json");
            if (s_profileSave != 0)
                ArcMenuHoverTooltip(
                    "The file sits beside ArcRaiders.exe, in Data\\. Saving "
                    "rewrites it whole and keeps every other slot.");

            if (profileSlot >= 0) {
                const std::string profileName = ColorProfile::SlotName(profileSlot);
                if (!profileName.empty())
                    ArcMenuLayout::HoverableTextF(
                        "Slot %d \"%s\" covers %d colors",
                        profileSlot + 1, profileName.c_str(),
                        ColorProfile::ColorCount());
                else
                    ArcMenuLayout::HoverableTextF(
                        "Slot %d is empty - save a palette into it.",
                        profileSlot + 1);
                ArcMenuHoverTooltip(
                    "Color count is how many keys a complete profile carries. "
                    "A lower count means the file was saved by an older build.");
            }
            ImGui::Separator();

            // D20 baseline / diff / export / apply.
            ArcMenuLayout::HoverableText("Config baseline");
            ArcMenuHoverTooltip(
                "Freeze the current settings, see exactly what changed since, "
                "and move settings in or out as a file. Everything lives in "
                "Data\\ beside ArcRaiders.exe and is plain key=value, so it can "
                "be edited by hand and loaded straight back.");

            static std::vector<std::string> s_cfgDiff;
            static bool s_cfgDiffShown = false;
            static int s_cfgOp = 0;  // 0 none, 1 baseline, 2 export, 3 import
            static bool s_cfgOpOk = false;

            if (ImGui::Button("Set baseline here")) {
                s_cfgOp = 1;
                s_cfgOpOk = AutoConfig_SnapshotBaseline();
            }
            ArcMenuHoverTooltip(
                "Flushes and copies the live settings to "
                "Data\\config_baseline.ini, replacing any previous baseline.");

            if (ImGui::Button("Diff against baseline")) {
                // Flush first: the diff must see the settings on screen, not the
                // debounced copy still waiting out its 0.5s save delay.
                AutoConfig_SaveNow();
                s_cfgDiff = AutoConfig_DiffBaseline();
                s_cfgDiffShown = true;
                s_cfgOp = 0;
            }
            ArcMenuHoverTooltip(
                "Lists every setting that differs from the baseline, one per "
                "line: \"key: old -> new\", \"+ key\" for keys the baseline "
                "does not have, \"- key\" for ones only the baseline has. An "
                "empty list means the two match.");

            if (ImGui::Button("Export config")) {
                s_cfgOp = 2;
                s_cfgOpOk = AutoConfig_ExportCurrent();
            }
            ArcMenuHoverTooltip("Writes the live settings to Data\\config_export.ini.");
            if (ImGui::Button("Apply exported config")) {
                s_cfgOp = 3;
                s_cfgOpOk = AutoConfig_ImportExported();
            }
            ArcMenuHoverTooltip(
                "Loads Data\\config_export.ini over the current settings and "
                "schedules a save. Use it after hand-editing an export.");

            if (s_cfgOp != 0) {
                const ImVec4 opCol = s_cfgOpOk
                    ? ImVec4(0.45f, 1.f, 0.55f, 1.f)
                    : ImVec4(1.f, 0.45f, 0.45f, 1.f);
                const char* opMsg = nullptr;
                if (s_cfgOp == 1)
                    opMsg = s_cfgOpOk ? "Baseline saved to Data\\config_baseline.ini"
                                      : "Could not write the baseline file";
                else if (s_cfgOp == 2)
                    opMsg = s_cfgOpOk ? "Exported to Data\\config_export.ini"
                                      : "Could not write the export file";
                else
                    opMsg = s_cfgOpOk ? "Applied Data\\config_export.ini"
                                      : "Export file missing or held no known keys";
                ArcMenuLayout::HoverableTextColoredF(opCol, "%s", opMsg);
                ArcMenuHoverTooltip(
                    "Files sit beside ArcRaiders.exe in Data\\. A failed write "
                    "usually means that folder is missing or read-only.");
            }

            if (s_cfgDiffShown) {
                ImGui::Separator();
                if (s_cfgDiff.empty()) {
                    ArcMenuLayout::HoverableTextColoredF(
                        ImVec4(0.45f, 1.f, 0.55f, 1.f),
                        "No differences from baseline");
                } else {
                    constexpr size_t kMaxShown = 12;
                    const size_t shown =
                        (std::min)(s_cfgDiff.size(), kMaxShown);
                    ArcMenuLayout::HoverableTextF(
                        "%d difference(s) vs baseline:",
                        static_cast<int>(s_cfgDiff.size()));
                    for (size_t di = 0; di < shown; ++di)
                        ArcMenuLayout::HoverableText(s_cfgDiff[di].c_str());
                    if (s_cfgDiff.size() > shown)
                        ArcMenuLayout::HoverableTextF(
                            "... and %d more",
                            static_cast<int>(s_cfgDiff.size() - shown));
                }
                if (ImGui::Button("Hide diff")) {
                    s_cfgDiffShown = false;
                    s_cfgDiff.clear();
                }
                ArcMenuHoverTooltip("Collapse the difference list.");
            }

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
    ArcMenuLayout::HoverableText("Aimbot runtime");
    ArcMenuHoverTooltip("Live aim state for diagnostics.");
    
    // Aimbot debug monitor toggle
    ImGui::Spacing();
    bool aimbotMonitorVisible = OverlayUI::AimbotDebugPanel::IsVisible();
    if (ArcMenuLayout::Checkbox("Show Aimbot Debug Monitor", &aimbotMonitorVisible)) {
        OverlayUI::AimbotDebugPanel::ToggleVisibility();
    }
    ArcMenuHoverTooltip("Toggle live aimbot telemetry monitor. Shows target state, aim deltas, jitter, lag, prediction, and performance metrics in real-time.");
    
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
    ArcMenuLayout::HoverableTextF("Aimbot enabled: %s (players + ARC share this profile)", var::enable_aimbot ? "yes" : "no");
    ArcMenuHoverTooltip("Master switch for hardware aim.");
    ArcMenuLayout::HoverableTextF("ARC targets: %s",
        FeaturePolicy::ShouldCollectRobotTargets(
            FeaturePolicy::AimFeatures{ var::enable_aimbot, var::robotAimEnabled,
                var::enable_triggerbot }) ? "active with the aim pass" : "no aim pass running");
    ArcMenuHoverTooltip("Bots are aim targets whenever the aim pass runs (aimbot or trigger). Target kind is not a separate switch.");
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
    ImGui::Keybind("Debug overlay hotkey", &var::debug_overlay_key);
    ArcMenuHoverTooltip("Click, then press any key: toggles the offset debug panel (green timers). Default F8. Saved to auto_config.ini.");
    
    ArcMenuLayout::Checkbox("Skeleton lag probe", &var::debug_skeleton_lag);
    ArcMenuHoverTooltip(
        "Draws box anchor (red) vs bone pelvis (green) with age/delta text, "
        "and prints [debugSkel] lines. Shows why the skeleton trails moving targets.");
    ArcMenuLayout::Checkbox("Aim shake probe", &var::debug_aim_shake);
    ArcMenuHoverTooltip(
        "Prints [debugShake] per error sign-flip (overshoot oscillation) plus "
        "extended [debugAim] fields: command, px/mouse, tick time, switches.");
    ArcMenuLayout::Checkbox("Log ghost bot decisions", &var::debug_ghost_bots);
    // Debug-flag-gated baseline: writes an esp_paint_profile line (frame time +
    // per-lock try/miss/wait) to help/entity_diagnostics.ndjson once a second.
    ArcMenuLayout::Checkbox("ESP paint profile (frame + lock waits)",
        &var::debug_esp_paint);
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
    WrappedBulletText("Robot aim: cached heart point; separate ARC hold key and optional nearest cached component-point selection. No injected shake.");
    WrappedBulletText("FOV, max distance, deadzone, aim hotkey (hold).");
    WrappedBulletText(
        "One shared motion preset and calibration for players and ARC. Hardware motion stops on target loss; legacy humanizer/grace motion is disabled. Optional random bone remains, off by default for new profiles.");
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