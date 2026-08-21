from pathlib import Path

path = Path(r"F:\Test\ARCs\Project\Functions\ContainerList.cpp")
text = path.read_text(encoding="utf-8")

start = text.find("                if (mayProbe\n                    && ContainerLootLooksOpened(")
if start < 0:
    raise SystemExit("start not found")
end = text.find("\n    if (doMetadata) {", start)
if end < 0:
    raise SystemExit("end not found")

replacement = r'''                if (mayProbe) {
                    OpenProbeCand cand{};
                    cand.key = key;
                    cand.retainIdx = i;
                    cand.openDistM = openDistM;
                    openProbeCands.push_back(cand);
                }
            }
        }
    }

    // P4: scatter LI pointers + searched bytes for the bounded probe set.
    if (!openProbeCands.empty()) {
        struct OpenProbeRow {
            uintptr_t key = 0;
            size_t retainIdx = 0;
            float openDistM = 0.f;
            uintptr_t liComp = 0;
            uintptr_t liCont = 0;
            uintptr_t liSimple = 0;
            uint8_t searched0 = 0;
            uint8_t searched1 = 0;
            uint8_t searched2 = 0;
        };
        std::vector<OpenProbeRow> rows;
        rows.reserve(openProbeCands.size());
        for (const auto& c : openProbeCands) {
            OpenProbeRow row{};
            row.key = c.key;
            row.retainIdx = c.retainIdx;
            row.openDistM = c.openDistM;
            rows.push_back(row);
        }
        int scatterExecs = 0;
        {
            ScatterSession s1;
            if (s1.isValid()) {
                bool ok = true;
                for (auto& row : rows) {
                    ok = s1.prepare(
                             row.key + Offsets::LootInteractionComponent, row.liComp)
                        && ok;
                    ok = s1.prepare(
                             row.key + Offsets::LootInteraction_Container, row.liCont)
                        && ok;
                    ok = s1.prepare(
                             row.key + Offsets::SimpleLootActivity_LootInteraction,
                             row.liSimple)
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
                for (auto& row : rows) {
                    if (row.liComp && Memory::IsValidPtrFast2(row.liComp)) {
                        ok = s2.prepare(
                                 row.liComp + Offsets::LootInteraction_Searched,
                                 row.searched0)
                            && ok;
                        ++prepared;
                    }
                    if (row.liCont && Memory::IsValidPtrFast2(row.liCont)) {
                        ok = s2.prepare(
                                 row.liCont + Offsets::LootInteraction_Searched,
                                 row.searched1)
                            && ok;
                        ++prepared;
                    }
                    if (row.liSimple && Memory::IsValidPtrFast2(row.liSimple)) {
                        ok = s2.prepare(
                                 row.liSimple + Offsets::LootInteraction_Searched,
                                 row.searched2)
                            && ok;
                        ++prepared;
                    }
                }
                if (prepared > 0 && ok && s2.execute())
                    ++scatterExecs;
            }
        }
        std::unordered_set<uintptr_t> eraseOpened;
        for (auto& row : rows) {
            const bool opened =
                ((row.liComp && Memory::IsValidPtrFast2(row.liComp)
                     && (row.searched0 & 0x1) != 0)
                    || (row.liCont && Memory::IsValidPtrFast2(row.liCont)
                        && (row.searched1 & 0x1) != 0)
                    || (row.liSimple && Memory::IsValidPtrFast2(row.liSimple)
                        && (row.searched2 & 0x1) != 0));
            if (!opened)
                continue;
            // #region agent log
            {
                static std::unordered_set<uintptr_t> s_openFlipSeen;
                if (s_openFlipSeen.insert(row.key).second) {
                    char buf[224];
                    const auto& entry = retainIters[row.retainIdx]->second;
                    snprintf(buf, sizeof(buf),
                        "{\"label\":\"%.48s\",\"distM\":%d,\"hideOpened\":%d,\"key\":%llu}",
                        entry.ItemDisplayName.c_str(),
                        static_cast<int>(row.openDistM),
                        var::show_world_open_container ? 0 : 1,
                        static_cast<unsigned long long>(row.key));
                    AgentCrateLog("O1", "container_open_flip", buf);
                }
            }
            // #endregion
            if (!var::show_world_open_container)
                eraseOpened.insert(row.key);
        }
        for (uintptr_t key : eraseOpened)
            localCache.erase(key);
        // #region agent log
        {
            static auto s_lastOpenBatch = std::chrono::steady_clock::time_point{};
            const auto nowB = std::chrono::steady_clock::now();
            if (s_lastOpenBatch.time_since_epoch().count() == 0
                || nowB - s_lastOpenBatch >= std::chrono::seconds(2)) {
                s_lastOpenBatch = nowB;
                std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                if (f) {
                    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    f << "{\"sessionId\":\"c190fb\",\"runId\":\"batch\",\"hypothesisId\":\"P4\","
                      << "\"location\":\"ContainerList.cpp:ContainerList\",\"message\":\"container_open_batch\","
                      << "\"data\":{\"probed\":" << rows.size()
                      << ",\"scatterExecs\":" << scatterExecs << "}"
                      << ",\"timestamp\":" << ts << "}\n";
                }
            }
        }
        // #endregion
    }
'''

path.write_text(text[:start] + replacement + text[end:], encoding="utf-8")
print("patched open probe OK")
