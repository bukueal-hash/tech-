from pathlib import Path

path = Path(r"F:\Test\ARCs\Project\Functions\ItemList.cpp")
text = path.read_text(encoding="utf-8")

start = text.find("    // H7: ground-pickup shells")
end = text.find("\n\n\n\n    if (m_worldGeneration.load", start)
if start < 0 or end < 0:
    raise SystemExit(f"markers not found start={start} end={end}")

new_block = r'''    // H7: ground-pickup shells that were liveNear <6m (hid=0/noCol=0 proven on
    // live BP_PickupBase) — clear on 0→1 hid/noCol / HiddenOrDestroyed without
    // waiting for ~15m actorGone (Canister L2258 / Battery L2259: strong=1).
    // Absolute noCol alone false-positives live shells; require prior live sample.
    // Bukupex P3: batch the fixed-offset shell probes (one scatter for all
    // pickup retain entries) instead of serial ProbeGroundLootPickupSignals.
    {
        static std::unordered_map<uintptr_t, uint8_t> s_prevShellBits; // 1=hid 2=noCol
        static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> s_recentNearLive;
        constexpr auto kRecentNearTtl = std::chrono::seconds(45);
        const auto nowShell = std::chrono::steady_clock::now();

        struct ShellBatchRow {
            uintptr_t key = 0;
            decltype(localCache.begin()) it{};
            uintptr_t root = 0;
            uintptr_t collider = 0;
            uint8_t actorHidden = 0;
            uint8_t ddFlags = 0;
            uint8_t rootHid = 0;
            uint8_t colliderHid = 0;
            int hid = 0;
            int noCol = 0;
        };
        std::vector<ShellBatchRow> shellRows;
        shellRows.reserve(localCache.size());
        for (auto it = localCache.begin(); it != localCache.end(); ++it) {
            const auto cat = static_cast<WorldItemCategory>(it->second.worldCategory);
            if (!IsGroundPickupCategory(cat)
                && !FnameLooksLikeDroppedPickup(it->second.ActorName))
                continue;
            ShellBatchRow row{};
            row.key = it->first;
            row.it = it;
            shellRows.push_back(row);
        }

        int scatterExecs = 0;
        if (!shellRows.empty()) {
            {
                ScatterSession s1;
                if (s1.isValid()) {
                    bool ok = true;
                    for (auto& row : shellRows) {
                        ok = s1.prepare(row.key + Offsets::RootComponent, row.root) && ok;
                        ok = s1.prepare(
                                 row.key + Offsets::Pickup_RootCollider, row.collider)
                            && ok;
                        ok = s1.prepare(
                                 row.key + Offsets::Actor_bHiddenByte, row.actorHidden)
                            && ok;
                        ok = s1.prepare(row.key + Offsets::Actor_FlagsDd, row.ddFlags)
                            && ok;
                    }
                    if (ok && s1.execute())
                        ++scatterExecs;
                }
            }
            {
                ScatterSession s2;
                if (s2.isValid()) {
                    bool ok = true;
                    int prepared = 0;
                    for (auto& row : shellRows) {
                        if (row.root && Memory::IsValidPtrFast2(row.root)) {
                            ok = s2.prepare(
                                     row.root + Offsets::Scene_bHiddenInGameByte,
                                     row.rootHid)
                                && ok;
                            ++prepared;
                        }
                        if (row.collider && Memory::IsValidPtrFast2(row.collider)) {
                            ok = s2.prepare(
                                     row.collider + Offsets::Scene_bHiddenInGameByte,
                                     row.colliderHid)
                                && ok;
                            ++prepared;
                        }
                    }
                    if (prepared > 0 && ok && s2.execute())
                        ++scatterExecs;
                }
            }
            for (auto& row : shellRows) {
                const bool actorHid =
                    (row.actorHidden & Offsets::Actor_bHiddenMask) != 0
                    || (row.ddFlags & Offsets::Actor_bActorIsBeingDestroyedMask) != 0;
                const bool sceneHid =
                    (row.root && Memory::IsValidPtrFast2(row.root)
                        && (row.rootHid & Offsets::Scene_bHiddenInGameMask) != 0)
                    || (row.collider && Memory::IsValidPtrFast2(row.collider)
                        && (row.colliderHid & Offsets::Scene_bHiddenInGameMask) != 0);
                row.hid = (actorHid || sceneHid) ? 1 : 0;
                row.noCol =
                    ((row.ddFlags & Offsets::Actor_bActorEnableCollisionMask) == 0)
                    ? 1
                    : 0;
            }
        }

        // #region agent log
        {
            static auto s_lastShellLog = std::chrono::steady_clock::time_point{};
            if (s_lastShellLog.time_since_epoch().count() == 0
                || nowShell - s_lastShellLog >= std::chrono::seconds(2)) {
                s_lastShellLog = nowShell;
                int pickedHidden = 0;
                for (const auto& row : shellRows)
                    pickedHidden += row.hid;
                std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                if (f) {
                    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    f << "{\"sessionId\":\"c190fb\",\"runId\":\"batch\",\"hypothesisId\":\"P3\","
                      << "\"location\":\"ItemList.cpp:ItemList\",\"message\":\"item_shell_batch\","
                      << "\"data\":{\"n\":" << shellRows.size()
                      << ",\"scatterExecs\":" << scatterExecs
                      << ",\"pickedHidden\":" << pickedHidden << "}"
                      << ",\"timestamp\":" << ts << "}\n";
                }
            }
        }
        // #endregion

        std::unordered_set<uintptr_t> eraseKeys;
        for (const auto& row : shellRows) {
            auto& entry = row.it->second;
            const int hid = row.hid;
            const int noCol = row.noCol;

            if (entry.Drawing && entry.Distance >= 0.f && entry.Distance < 6.f
                && hid == 0 && noCol == 0) {
                s_recentNearLive[row.key] = nowShell;
            }

            const auto nearIt = s_recentNearLive.find(row.key);
            const bool recentNear = nearIt != s_recentNearLive.end()
                && (nowShell - nearIt->second) <= kRecentNearTtl;

            if (!s_prevShellBits.contains(row.key)) {
                s_prevShellBits[row.key] = static_cast<uint8_t>(
                    (hid ? 1u : 0u) | (noCol ? 2u : 0u));
                continue;
            }
            const uint8_t prevBits = s_prevShellBits[row.key];
            const int prevHid = (prevBits & 1u) ? 1 : 0;
            const int prevNoCol = (prevBits & 2u) ? 1 : 0;
            const bool hidRise = (hid == 1 && prevHid == 0);
            const bool noColRise = (noCol == 1 && prevNoCol == 0);
            const bool gate = recentNear && (hidRise || noColRise);
            s_prevShellBits[row.key] = static_cast<uint8_t>(
                (hid ? 1u : 0u) | (noCol ? 2u : 0u));

            if (!gate)
                continue;

            MarkGroundPickupGoneSticky(row.key);
            ClearItemPosMiss(row.key);
            // #region agent log
            {
                char buf[192];
                snprintf(buf, sizeof(buf),
                    "{\"label\":\"%.48s\",\"reason\":\"shell_gate\",\"key\":%llu}",
                    entry.ItemDisplayName.c_str(),
                    static_cast<unsigned long long>(row.key));
                AgentItemLog("item_evict", "G1", buf);
            }
            // #endregion
            s_recentNearLive.erase(row.key);
            s_prevShellBits.erase(row.key);
            eraseKeys.insert(row.key);
        }
        for (uintptr_t key : eraseKeys)
            localCache.erase(key);
    }


'''

path.write_text(text[:start] + new_block + text[end:], encoding="utf-8")
print("patched ItemList.cpp OK")
