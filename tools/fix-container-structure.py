from pathlib import Path

path = Path(r"F:\Test\ARCs\Project\Functions\ContainerList.cpp")
text = path.read_text(encoding="utf-8")

broken = """    if (!retainIters.empty()) {
        std::vector<WorldScan::CacheRootScatterRow> scatterRows;
        scatterRows.reserve(retainIters.size());
        struct OpenProbeCand {
        uintptr_t key = 0;
        size_t retainIdx = 0;
        float openDistM = 0.f;
    };
    std::vector<OpenProbeCand> openProbeCands;
    openProbeCands.reserve(4);

    for (size_t i = 0; i < retainIters.size(); ++i) {
            WorldScan::CacheRootScatterRow row{};
            row.actorKey = retainIters[i]->first;
            scatterRows.push_back(row);
        }
        if (WorldScan::ScatterReadActorRootPositions(scatterRows)) {
            for (size_t i = 0; i < retainIters.size(); ++i) {
                const WorldScan::CacheRootScatterRow& row = scatterRows[i];
                if (!row.rootValid)
                    continue;
                retainRoots[i] = row.root;
                auto& entry = retainIters[i]->second;
                entry.rootComponent = row.root;
                if (row.posValid)
                    entry.WorldPos = row.worldPos;
            }
        }
    }

    for (size_t i = 0; i < retainIters.size(); ++i) {
        const uintptr_t key = retainIters[i]->first;"""

fixed = """    if (!retainIters.empty()) {
        std::vector<WorldScan::CacheRootScatterRow> scatterRows;
        scatterRows.reserve(retainIters.size());
        for (size_t i = 0; i < retainIters.size(); ++i) {
            WorldScan::CacheRootScatterRow row{};
            row.actorKey = retainIters[i]->first;
            scatterRows.push_back(row);
        }
        if (WorldScan::ScatterReadActorRootPositions(scatterRows)) {
            for (size_t i = 0; i < retainIters.size(); ++i) {
                const WorldScan::CacheRootScatterRow& row = scatterRows[i];
                if (!row.rootValid)
                    continue;
                retainRoots[i] = row.root;
                auto& entry = retainIters[i]->second;
                entry.rootComponent = row.root;
                if (row.posValid)
                    entry.WorldPos = row.worldPos;
            }
        }
    }

    struct OpenProbeCand {
        uintptr_t key = 0;
        size_t retainIdx = 0;
        float openDistM = 0.f;
    };
    std::vector<OpenProbeCand> openProbeCands;
    openProbeCands.reserve(4);

    for (size_t i = 0; i < retainIters.size(); ++i) {
        const uintptr_t key = retainIters[i]->first;"""

if broken not in text:
    raise SystemExit("broken block not found")
path.write_text(text.replace(broken, fixed, 1), encoding="utf-8")
print("structure fixed")
