#pragma once

#include <cstdint>
#include <vector>

namespace Dumper {

class ModuleImageCache {
public:
    bool Load(uint64_t moduleBase, uint32_t imageSize, class DumperState& state);
    bool IsLoaded() const { return !data_.empty(); }

    uint64_t BaseVa() const { return baseVa_; }
    const uint8_t* Data() const { return data_.data(); }
    size_t Size() const { return data_.size(); }

private:
    uint64_t baseVa_ = 0;
    std::vector<uint8_t> data_;
};

} // namespace Dumper
