#pragma once

#include "includes.h"

namespace game::gobjects {
	// Filled by get() with the first failure reason so a failed run names the
	// read that failed instead of a generic "failed to resolve gobjects".
	inline std::string& last_error( ) {
		static std::string s;
		return s;
	}

	// The live GUObjectArray decode (sdk/sdk.txt, CL-1426848). The global at
	// GObjectsRva is a pure-XOR blob (^ GObjectsXor) that yields the FUObjectArray
	// registry; NumElements and Chunks are byte-swap + XOR decoded off it. Every
	// value comes from Offsets.h, so a patch only has to regenerate that header.
	inline gobject_array_t get( ) {
		gobject_array_t r{};
		last_error( ).clear( );

		const std::uintptr_t base = Sigma::Driver::Base;
		if ( !base ) {
			last_error( ) = "no module base";
			return r;
		}

		std::uint64_t enc = 0;
		if ( !Sigma::Driver::ReadBytes( base + Offsets::GObjectsRva, sizeof( enc ),
				reinterpret_cast< std::uint8_t* >( &enc ) ) ) {
			last_error( ) = "gobjects blob read failed";
			return r;
		}

		// CL-1426848: the FUObjectArray global is a pure-XOR blob now. The old
		// three-RVA ROL32 key table is retired, so reading it just fails (see
		// sdk/sdk.txt: FUObjectArray = enc ^ UNIVERSAL_PTR_XOR).
		const std::uint64_t registry = enc ^ Offsets::GObjectsXor;
		if ( !Sigma::Driver::isvalid( registry ) ) {
			last_error( ) = "registry pointer invalid";
			return r;
		}

		const std::uint32_t count = _byteswap_ulong(
			Sigma::Driver::Read< std::uint32_t >(
				registry + static_cast< std::uint64_t >( Offsets::GObjects_NumXorOff ) )
			^ Offsets::GObjects_NumXor );
		const std::uint64_t chunks = _byteswap_uint64(
			Sigma::Driver::Read< std::uint64_t >(
				registry + static_cast< std::uint64_t >( Offsets::GObjects_ChunksXorOff ) )
			^ Offsets::GObjects_ChunksXor );

		if ( count < 1 || count > 2000000 ) {
			last_error( ) = "element count out of range";
			return r;
		}
		if ( !Sigma::Driver::isvalid( chunks ) ) {
			last_error( ) = "chunk table pointer invalid";
			return r;
		}

		r.m_num_elements = count;
		r.m_objects      = chunks;
		return r;
	}

	// FUObjectItem stride 24, chunk stride 8, 0x10000 items per chunk. The
	// item's object pointer is +0 in the standard layout and +8 in the SDK1
	// variant, so accept whichever slot holds a live pointer (Utils.cpp).
	inline std::uint64_t get_object_from_index( std::uint64_t chunk_table, std::uint32_t idx ) {
		if ( !Sigma::Driver::isvalid( chunk_table ) )
			return 0;

		const std::uint64_t chunk =
			Sigma::Driver::Read< std::uint64_t >( chunk_table + 8ULL * ( idx >> 16 ) );
		if ( !Sigma::Driver::isvalid( chunk ) )
			return 0;

		const std::uint64_t item = chunk + 24ULL * ( idx & 0xFFFFu );
		const std::uint64_t at0  = Sigma::Driver::Read< std::uint64_t >( item );
		if ( Sigma::Driver::isvalid( at0 ) )
			return at0;
		const std::uint64_t at8 = Sigma::Driver::Read< std::uint64_t >( item + 8 );
		if ( Sigma::Driver::isvalid( at8 ) )
			return at8;
		return 0;
	}
}
