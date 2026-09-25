// SDK reflection suite — covers the decoders the walker (Core/Reflection.hpp)
// depends on, so every game::gasm routine used by the project is pinned:
//   decode_ffield_nameprivate, decode_ffproperty_offset, decode_ffieldclass_name
// plus the UStruct/FField layout offsets (SuperStruct / PropertyLink /
// PropertyLinkNext / FField_ClassPrivate / FObjectPropertyBase_PropertyClass /
// FFieldClass_SuperClass) and the FString verification offset.

#include "tests_main.hpp"

#include "fake_mem.hpp"

#pragma warning(push)
#pragma warning(disable : 4201)
#include "Core/Reflection.hpp"
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <cstring>
#include <vector>

namespace {

uint64_t rol64(uint64_t v, int n) { return (v << n) | (v >> (64 - n)); }
uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
uint32_t bswap32(uint32_t v) { return _byteswap_ulong(v); }

// ── Reference implementations (mirror Core/SDK.hpp) ─────────────────────────
uint64_t refFieldNameprivate(const uint8_t* field_buf)
{
    uint64_t source = 0, hi = 0;
    std::memcpy(&source, field_buf + 0x90, 8);
    std::memcpy(&hi, field_buf + 0x98, 8);

    uint64_t value = rol64(source + 0x44CB31F912F95FFFull, 17);
    value ^= hi;
    value = rol64(value + 0xBB34B0C01E5AA000ull, 47);
    return rol64(source ^ value, 32);
}

uint64_t refPropertyOffset(const uint8_t* property_buf)
{
    uint32_t encrypted = 0;
    std::memcpy(&encrypted, property_buf + 0xBC, 4);
    return bswap32(encrypted ^ 0xDE28E18Du);
}

uint64_t refFieldClassName(const uint8_t* rax_buf)
{
    uint16_t rotated[4];
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t value = 0;
        std::memcpy(&value, rax_buf + 0x20 + i * 2, 2);
        rotated[i] = static_cast<uint16_t>((value << 13) | (value >> 3));
    }
    const unsigned shuffle[4] = { 1, 3, 0, 2 };
    uint64_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<uint64_t>(rotated[shuffle[i]]) << (i * 16);
    const uint32_t low = rol32(static_cast<uint32_t>(value), 10);
    const uint32_t high = rol32(static_cast<uint32_t>(value >> 32), 10);
    return rol64(static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32), 32);
}

// Inverse of decode_ffproperty_offset, for building synthetic FPropertys.
uint32_t encodePropertyOffset(uint32_t offset) { return bswap32(offset) ^ 0xDE28E18Du; }

constexpr uint64_t kUClass = 0x500000;
constexpr uint64_t kBaseClass = 0x510000;
constexpr uint64_t kProp1 = 0x400000;
constexpr uint64_t kProp2 = 0x401000;

// UClass with a two-property PropertyLink chain and a SuperStruct back to
// kBaseClass (which has no properties and no parent).
void installClassGraph(FakeMem& fm)
{
    fm.writeU64(kUClass + Offsets::UStruct_PropertyLink, kProp1);
    fm.writeU64(kUClass + Offsets::UStruct_SuperStruct, kBaseClass);

    fm.writeU32(kProp1 + 0xBC, encodePropertyOffset(0x100));
    fm.writeU64(kProp1 + Offsets::FProperty_PropertyLinkNext, kProp2);

    fm.writeU32(kProp2 + 0xBC, encodePropertyOffset(0x110));
    fm.writeU64(kProp2 + Offsets::FProperty_PropertyLinkNext, 0);
    fm.writeU64(kProp2 + Offsets::FObjectPropertyBase_PropertyClass, 0);
}

} // namespace

TEST_CASE("SDK FProperty offset decode matches reference")
{
    ScopedFakeMem fm;

    const uint32_t offsets[] = { 0x100, 0x2A8, 0xDD8, 0x1220 };
    for (uint32_t expected : offsets) {
        fm.mem.writeU32(kProp1 + 0xBC, encodePropertyOffset(expected));
        CHECK(Reflection::PropertyOffset(kProp1) == expected);
    }

    // Unreadable property (offset decodes past the sanity gate) -> 0.
    fm.mem.writeU32(kProp2 + 0xBC, encodePropertyOffset(0x50000));
    CHECK(Reflection::PropertyOffset(kProp2) == 0);
}

TEST_CASE("SDK FProperty nameprivate decode matches reference")
{
    ScopedFakeMem fm;

    uint8_t raw[0xA0];
    for (size_t i = 0; i < sizeof(raw); ++i)
        raw[i] = static_cast<uint8_t>(i * 7 + 3);
    fm.mem.write(kProp1, raw, sizeof(raw));

    CHECK(Reflection::PropertyNameIndex(kProp1) ==
        static_cast<int32_t>(refFieldNameprivate(raw) & 0xFFFFFFFFu));
}

TEST_CASE("SDK FFieldClass name decode matches reference")
{
    ScopedFakeMem fm;

    uint8_t raw[0x60];
    for (size_t i = 0; i < sizeof(raw); ++i)
        raw[i] = static_cast<uint8_t>(0x40 + (i * 5));
    fm.mem.write(kProp1, raw, sizeof(raw));

    const uint64_t decoded = game::gasm::decode_ffieldclass_name(
        game::gasm::decrypt_ffieldclass_name_context{}, raw);
    CHECK(decoded == refFieldClassName(raw));

    // Zeroed FFieldClass is deterministic (no crash) and differs from the
    // populated window.
    uint8_t zero[0x60] = {};
    CHECK(game::gasm::decode_ffieldclass_name(
        game::gasm::decrypt_ffieldclass_name_context{}, zero) != decoded);
}

TEST_CASE("Reflection walk follows PropertyLink and SuperStruct")
{
    ScopedFakeMem fm;
    installClassGraph(fm.mem);

    const std::vector<Reflection::PropertyInfo> props =
        Reflection::WalkProperties(kUClass, /*base=*/0x1000000, 48, false);
    REQUIRE(props.size() == 2);
    CHECK(props[0].offset == 0x100);
    CHECK(props[1].offset == 0x110);
    // No FName pipeline in this fixture, so names stay empty (not garbage).
    CHECK(props[0].name.empty());

    // Class chain: kUClass -> kBaseClass -> end.
    CHECK(Reflection::WalkClassChain(kUClass, 0x1000000).size() == 2);
    CHECK(Reflection::SuperStruct(kUClass) == kBaseClass);
    CHECK(Reflection::SuperStruct(kBaseClass) == 0);
    CHECK(Reflection::PropertyLink(kUClass) == kProp1);
    CHECK(Reflection::NextProperty(kProp1) == kProp2);
    CHECK(Reflection::NextProperty(kProp2) == 0);

    // The base class has no property chain of its own.
    CHECK(Reflection::WalkProperties(kBaseClass, 0x1000000, 48, false).empty());

    const Reflection::ClassReport report = Reflection::BuildReport(kUClass, 0x1000000, 48, false);
    CHECK(report.uclass == kUClass);
    CHECK(report.chain.size() == 2);
    CHECK(report.properties.size() == 2);

    // Invalid class -> empty report.
    const Reflection::ClassReport invalid = Reflection::BuildReport(0x10, 0x1000000, 48, false);
    CHECK(invalid.chain.empty());
    CHECK(invalid.properties.empty());
}

TEST_CASE("Reflection FString verification reads the SDK verification offset")
{
    ScopedFakeMem fm;

    const uint64_t base = 0x1000000;
    const uint64_t address = base + Offsets::FStringVerificationRva;

    // Plaintext FString "None" at the SDK's verification offset.
    fm.mem.writeU64(address, address + 0x40);   // inline data pointer (any mapped VA)
    fm.mem.writeU32(address + 0x8, 5);          // Num() includes the terminator
    const char text[] = "None";
    for (int i = 0; i <= 4; ++i)
        fm.mem.writeU16(address + 0x40 + 2u * static_cast<uint64_t>(i),
            static_cast<uint16_t>(text[i]));

    const Reflection::FStringCheck check = Reflection::CheckFStringPipeline(base);
    CHECK(check.readOk);
    CHECK(check.candidate >= 0);          // some candidate produced printable text
    CHECK(check.text.size() > 5);         // "<label>:<text>"

    // Nonsense length -> read refused, no candidate.
    fm.mem.writeU32(address + 0x8, 0);
    const Reflection::FStringCheck bad = Reflection::CheckFStringPipeline(base);
    CHECK_FALSE(bad.readOk);
    CHECK(bad.candidate == -1);
}
