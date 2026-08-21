from pathlib import Path

path = Path(r"F:\Test\ARCs\Project\Functions\ContainerList.cpp")
text = path.read_text(encoding="utf-8")

def must_replace(old: str, new: str, label: str) -> None:
    global text
    if old not in text:
        raise SystemExit(f"FAILED: {label}")
    text = text.replace(old, new, 1)
    print("ok", label)

must_replace(
    """    std::vector<uintptr_t> retainRoots;
    retainIters.reserve(localCache.size());
    retainRoots.reserve(localCache.size());""",
    """    std::vector<uintptr_t> retainRoots;
    std::vector<std::string> retainClassFnames;
    retainIters.reserve(localCache.size());
    retainRoots.reserve(localCache.size());
    retainClassFnames.reserve(localCache.size());""",
    "declare retainClassFnames",
)

must_replace(
    """        retainIters.push_back(it);
        retainRoots.push_back(0);
        ++it;
    }""",
    """        retainIters.push_back(it);
        retainRoots.push_back(0);
        retainClassFnames.push_back(classFname);
        ++it;
    }""",
    "push classFname",
)

must_replace(
    "    for (size_t i = 0; i < retainIters.size(); ++i) {",
    """    struct OpenProbeCand {
        uintptr_t key = 0;
        size_t retainIdx = 0;
        float openDistM = 0.f;
    };
    std::vector<OpenProbeCand> openProbeCands;
    openProbeCands.reserve(4);

    for (size_t i = 0; i < retainIters.size(); ++i) {""",
    "openProbeCands decl",
)

must_replace(
    """            const std::string& retainFname = retainIters[i]->second.ActorName;
            const std::string retainClassFname = GetActorClassFName(key);""",
    """            const std::string& retainFname = retainIters[i]->second.ActorName;
            const std::string& retainClassFname = retainClassFnames[i];""",
    "reuse class fname",
)

path.write_text(text, encoding="utf-8")
print("wrote ContainerList decls OK")
