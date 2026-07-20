from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# --- MenuSidebar.cpp ---
ms = ROOT / "Project/Interface/Overlay/MenuSidebar.cpp"
text = ms.read_text(encoding="utf-8")

if '#include "../Utils/PlayerTrack.h"' not in text:
    text = text.replace(
        '#include "../Utils/AutoConfig.h"',
        '#include "../Utils/AutoConfig.h"\n#include "../Utils/PlayerTrack.h"\n#include <unordered_map>',
    )

text = text.replace(
    'const char* tabs[] = { "ESP", "Radar", "Aimbot", "Settings", "Help", "Close" };',
    'const char* tabs[] = { "ESP", "Players", "Radar", "Aimbot", "Settings", "Help", "Close" };',
)

text = text.replace(
    '        "ESP sub-tab: player/bot ESP. VisCheck sub-tab: raycast LOS. Loot sub-tab: container/loot ESP.",\n'
    '        "Mini-map: enable, size, and world range.",',
    '        "ESP sub-tab: player/bot ESP. VisCheck sub-tab: raycast LOS. Loot sub-tab: container/loot ESP.",\n'
    '        "Live player list: name, squad/team, weapon. Check Track to draw a bullseye above their head.",\n'
    '        "Mini-map: enable, size, and world range.",',
)

text = text.replace(
    "        case 0: arc_ui::DrawArcEspTab(); break;\n"
    "        case 1: arc_ui::DrawArcRadarTab(); break;\n"
    "        case 2: arc_ui::DrawArcAimbotTab(); break;\n"
    "        case 3: arc_ui::DrawArcSettingsTab(); break;\n"
    "        case 4: arc_ui::DrawArcHelpTab(); break;",
    "        case 0: arc_ui::DrawArcEspTab(); break;\n"
    "        case 1: arc_ui::DrawArcPlayersTab(); break;\n"
    "        case 2: arc_ui::DrawArcRadarTab(); break;\n"
    "        case 3: arc_ui::DrawArcAimbotTab(); break;\n"
    "        case 4: arc_ui::DrawArcSettingsTab(); break;\n"
    "        case 5: arc_ui::DrawArcHelpTab(); break;",
)

if "void DrawArcPlayersTab()" not in text:
    players_fn = r'''
void DrawArcPlayersTab()
{
    ImGui::TextUnformatted("Players");
    ImGui::TextWrapped(
        "All players in the raid. Sorted by squad/team. Check Track to mark a bullseye above their head.");

    std::vector<Engine::PlayerMenuRow> rows;
    engine.SnapshotPlayerMenuRows(rows);

    std::vector<uintptr_t> livePawns;
    livePawns.reserve(rows.size());
    for (const Engine::PlayerMenuRow& row : rows) {
        if (row.pawn)
            livePawns.push_back(row.pawn);
    }
    PlayerTrack::PruneNotIn(livePawns);

    std::sort(rows.begin(), rows.end(),
        [](const Engine::PlayerMenuRow& a, const Engine::PlayerMenuRow& b) {
            if (a.isAlly != b.isAlly)
                return !a.isAlly;
            if (a.squadIdx != b.squadIdx)
                return a.squadIdx < b.squadIdx;
            return a.name < b.name;
        });

    static std::unordered_map<uintptr_t, std::string> s_weaponCache;
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    static uint64_t s_lastWeaponRefreshMs = 0;
    constexpr uint64_t kWeaponRefreshMs = 400;

    if (nowMs - s_lastWeaponRefreshMs >= kWeaponRefreshMs) {
        s_lastWeaponRefreshMs = nowMs;
        for (Engine::PlayerMenuRow& row : rows) {
            if (!row.pawn)
                continue;
            std::string liveWeapon, stowed0, stowed1;
            int wq = -1, sq0 = -1, sq1 = -1;
            float ap = 0.f, app = 0.f;
            engine.ReadPlayerInventory(
                row.pawn, liveWeapon, wq, stowed0, sq0, stowed1, sq1, ap, app);
            Engine::PlayerCacheEntry probe{};
            probe.weaponName = liveWeapon;
            probe.stowedWeapon0 = stowed0;
            probe.stowedWeapon1 = stowed1;
            row.weapon = engine.ResolvePlayerActiveWeaponLabel(probe);
            if (!row.weapon.empty())
                s_weaponCache[row.pawn] = row.weapon;
        }
        for (auto it = s_weaponCache.begin(); it != s_weaponCache.end(); ) {
            const bool live = std::any_of(rows.begin(), rows.end(),
                [&](const Engine::PlayerMenuRow& r) { return r.pawn == it->first; });
            if (!live)
                it = s_weaponCache.erase(it);
            else
                ++it;
        }
    } else {
        for (Engine::PlayerMenuRow& row : rows) {
            const auto fit = s_weaponCache.find(row.pawn);
            if (fit != s_weaponCache.end() && !fit->second.empty())
                row.weapon = fit->second;
        }
    }

    if (ImGui::Button("Clear tracks", ImVec2(100.f, 0.f)))
        PlayerTrack::ClearAll();
    ImGui::SameLine();
    ImGui::TextDisabled("Tracked: %zu", PlayerTrack::TrackedCount());

    const float tableHeight = ImGui::GetContentRegionAvail().y - 4.f;
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##players_table", 4, tableFlags, ImVec2(0.f, tableHeight))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 18.f);
        ImGui::TableSetupColumn("Weapon", ImGuiTableColumnFlags_WidthStretch, 0.36f);
        ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (Engine::PlayerMenuRow& row : rows) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(row.pawn & 0x7FFFFFFF));

            ImGui::TableSetColumnIndex(0);
            if (row.isDead)
                ImGui::TextDisabled("%s", row.name.c_str());
            else
                ImGui::TextUnformatted(row.name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::SetWindowFontScale(0.78f);
            if (row.isAlly)
                ImGui::TextUnformatted("A");
            else if (row.squadIdx > 0)
                ImGui::Text("%u", static_cast<unsigned>(row.squadIdx));
            else
                ImGui::TextUnformatted("-");
            ImGui::SetWindowFontScale(1.f);

            ImGui::TableSetColumnIndex(2);
            if (row.weapon.empty())
                ImGui::TextDisabled("-");
            else
                ImGui::TextUnformatted(row.weapon.c_str());

            ImGui::TableSetColumnIndex(3);
            bool tracked = PlayerTrack::IsTracked(row.pawn);
            if (ImGui::Checkbox("##track", &tracked))
                PlayerTrack::SetTracked(row.pawn, tracked);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

'''
    text = text.replace("\nvoid DrawArcRadarTab()", players_fn + "\nvoid DrawArcRadarTab()")

ms.write_text(text, encoding="utf-8", newline="\n")
print("MenuSidebar.cpp OK")

# --- Esp.cpp ---
esp_path = ROOT / "Project/Functions/Esp.cpp"
et = esp_path.read_text(encoding="utf-8")

if "PlayerTrack.h" not in et:
    et = et.replace(
        '#include "../Interface/Utils/Variables/index.h"',
        '#include "../Interface/Utils/Variables/index.h"\n#include "../Interface/Utils/PlayerTrack.h"',
    )

if "DrawTrackBullseye" not in et:
    bullseye = r'''
static void DrawTrackBullseye(ImDrawList* drawList, float centerX, float centerY, float distanceM)
{
    if (!drawList)
        return;
    const float px = Visuals::LabelTextPx(distanceM);
    const float outerR = (std::max)(4.f, px * 0.55f);
    const float innerR = outerR * 0.28f;
    const ImU32 ring = IM_COL32(255, 40, 40, 230);
    const ImU32 dot = IM_COL32(255, 255, 255, 255);
    drawList->AddCircle(ImVec2(centerX, centerY), outerR, ring, 24, 2.f);
    drawList->AddLine(
        ImVec2(centerX - outerR, centerY), ImVec2(centerX + outerR, centerY), ring, 1.5f);
    drawList->AddLine(
        ImVec2(centerX, centerY - outerR), ImVec2(centerX, centerY + outerR), ring, 1.5f);
    drawList->AddCircleFilled(ImVec2(centerX, centerY), innerR, dot, 12);
}

static float EstimatePlayerLabelStackAbove(const Engine::PlayerCacheEntry& actor)
{
    float h = 0.f;
    if (var::show_squad_idx && !actor.isAlly && actor.squadIdx > 0) {
        char tagBuf[16]{};
        snprintf(tagBuf, sizeof(tagBuf), "@%u", static_cast<unsigned>(actor.squadIdx));
        h += LabelTextHeight(tagBuf, actor.Distance) + 2.f;
    }
    if (var::names) {
        const char* nameLabel = actor.ActorName.empty() ? "Raider" : actor.ActorName.c_str();
        h += LabelTextHeight(nameLabel, actor.Distance) + 6.f;
    }
    if (var::show_weapon) {
        const std::string active = engine.ResolvePlayerActiveWeaponLabel(actor);
        if (!active.empty())
            h += LabelTextHeight(active.c_str(), actor.Distance) + 2.f;
    }
    if (var::show_distance)
        h += LabelTextHeight("999m", actor.Distance) + 4.f;
    return h;
}

static float ComputeTrackBullseyeCenterY(
    const Engine::PlayerCacheEntry& actor,
    float headY,
    float boxH,
    const Visuals::EspDrawScale& scale)
{
    float y = headY;
    if (var::health)
        y -= 18.f * scale.labelScale;
    y -= EstimatePlayerLabelStackAbove(actor);
    y -= 10.f * scale.labelScale;
    return y;
}

'''
    et = et.replace("static ImU32 SquadColor(uint8_t idx)", bullseye + "static ImU32 SquadColor(uint8_t idx)")

if "DrawTrackBullseye(drawList, head.x, bullseyeY" not in et:
    et = et.replace(
        "        float labelStackY = head.y;\n"
        "        if (var::health) {",
        "        if (PlayerTrack::IsTracked(live.APawn)) {\n"
        "            const float bullseyeY = ComputeTrackBullseyeCenterY(\n"
        "                live, head.y, boxH, scale);\n"
        "            DrawTrackBullseye(drawList, head.x, bullseyeY, live.Distance);\n"
        "        }\n\n"
        "        float labelStackY = head.y;\n"
        "        if (var::health) {",
    )

esp_path.write_text(et, encoding="utf-8", newline="\n")
print("Esp.cpp OK")

# --- Menu.h ---
menu_h = ROOT / "Project/Interface/Overlay/Menu.h"
mh = menu_h.read_text(encoding="utf-8")
if "DrawArcPlayersTab" not in mh:
    mh = mh.replace(
        "void DrawArcEspTab();\nvoid DrawArcRadarTab();",
        "void DrawArcEspTab();\nvoid DrawArcPlayersTab();\nvoid DrawArcRadarTab();",
    )
    menu_h.write_text(mh, encoding="utf-8", newline="\n")
    print("Menu.h OK")
else:
    print("Menu.h already OK")
