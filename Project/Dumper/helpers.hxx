#pragma once

#include "includes.h"

namespace game {
	inline std::uint32_t rol4( std::uint32_t v, int n ) {
		return _rotl( v, n );
	}

	inline std::uint64_t rol8( std::uint64_t v, int n ) {
		return _rotl64( v, n );
	}

	inline std::uint16_t rol16( std::uint16_t x, int n ) {
		return static_cast< std::uint16_t >( ( x << n ) | ( x >> ( 16 - n ) ) );
	}

	inline std::uint64_t clmul64_low( std::uint64_t a, std::uint64_t b ) {
		std::uint64_t result = 0;
		while ( b ) {
			if ( b & 1 )
				result ^= a;
			b >>= 1;
			a <<= 1;
		}
		return result;
	}

	inline std::uint64_t decrypt_block( std::uint64_t address ) {
		if ( !Sigma::Driver::isvalid( address ) )
			return 0;

		const std::uint64_t raw0 = Sigma::Driver::Read< std::uint64_t >( address );
		const std::uint64_t raw1 = Sigma::Driver::Read< std::uint64_t >( address + 8 );

		const std::uint64_t x = _rotl64( raw0 + offsets::block_add_lo, 53 ) ^ raw1;
		return _rotl64( x + offsets::block_add_hi, 11 ) ^ raw0;
	}
}