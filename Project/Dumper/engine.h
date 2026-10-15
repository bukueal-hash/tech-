#pragma once

// Layout and crypto constants are the app's, not the dumper's. Offsets.h is
// generated from the SDK drop and NameCrypto.hpp holds the tested decode
// primitives, so every value here is an alias and a patch only has to touch the
// two headers the rest of the app already depends on. The stale private fork
// that used to live here (hardcoded keys/offsets that silently rotted) is gone.

namespace game {
	namespace offsets {
		// -- GObjects / GNames (Core/Offsets.h) --
		inline constexpr std::uint64_t gobjects_enc_rva  = Offsets::GObjectsRva;
		inline constexpr std::uint64_t gobjects_xor      = Offsets::GObjectsXor;
		inline constexpr std::uint64_t name_rva          = Offsets::GNamePoolRva;
		inline constexpr std::uint64_t keytable_rva      = Offsets::FNameKeyTableRva;
		inline constexpr std::uint64_t gobjects_num_xor_off    = static_cast<std::uint64_t>(Offsets::GObjects_NumXorOff);
		inline constexpr std::uint32_t gobjects_num_xor        = Offsets::GObjects_NumXor;
		inline constexpr std::uint64_t gobjects_chunks_xor_off = static_cast<std::uint64_t>(Offsets::GObjects_ChunksXorOff);
		inline constexpr std::uint64_t gobjects_chunks_xor     = Offsets::GObjects_ChunksXor;

		// -- UStruct / FField / FProperty layout (Reflection uses exactly these) --
		inline constexpr std::uint32_t ustruct_super_struct         = static_cast<std::uint32_t>(Offsets::UStruct_SuperStruct);
		inline constexpr std::uint32_t ustruct_property_link        = static_cast<std::uint32_t>(Offsets::UStruct_PropertyLink);
		inline constexpr std::uint32_t fproperty_property_link_next = static_cast<std::uint32_t>(Offsets::FProperty_PropertyLinkNext);
		inline constexpr std::uint32_t ffield_class_private         = static_cast<std::uint32_t>(namecrypto::kFfieldClassPrivate);
		inline constexpr std::uint32_t ffield_name_private          = static_cast<std::uint32_t>(namecrypto::kFfieldFnameEnc);
		inline constexpr std::uint32_t ffield_offset_off            = static_cast<std::uint32_t>(namecrypto::kFfieldOffsetEnc);
		inline constexpr std::uint32_t fobject_property_base_property_class =
			static_cast<std::uint32_t>(Offsets::FObjectPropertyBase_PropertyClass);
		inline constexpr std::uint32_t ffieldclass_super_class      = static_cast<std::uint32_t>(Offsets::FFieldClass_SuperClass);

		// -- Dumper-only (no generated source; probed values kept) --
		inline constexpr std::uint32_t ffieldclass_name        = 0x20;  // game::gasm::decode_ffieldclass_name window
		inline constexpr std::uint32_t fstruct_property_struct = 0x148;
		inline constexpr std::uint32_t fenum_property_enum     = 0x130;
		inline constexpr std::uint32_t fbyte_property_enum     = 0x148;
		inline constexpr std::uint32_t farray_property_inner   = 0x150;
		inline constexpr std::uint32_t fset_property_inner     = 0x150;
		inline constexpr std::uint32_t fmap_property_key       = 0x148;
		inline constexpr std::uint32_t fmap_property_value     = 0x150;
		inline constexpr std::uint32_t uenum_names_data        = 0xB0;
		inline constexpr std::uint32_t uenum_values_data       = 0xB8;
		inline constexpr std::uint32_t uenum_count             = 0xC0;

		// -- legacy helpers still referenced by util code --
		inline constexpr int keystream_count = static_cast<int>(namecrypto::kKeystreamCount);
		inline constexpr std::uint64_t block_add_lo = 0x1F53CB8D1AA510D5ULL;
		inline constexpr std::uint64_t block_add_hi = 0xE0ACCDCF9338EF2AULL;
	}
}
