#pragma once

#include "includes.h"

namespace game::gnames {
	// The app's tested pipeline, not a private fork: Reflection owns the FName
	// keystream + pool (EnsureNames / ResolveNameString) and steam_decrypt owns
	// the encrypted UObject slots (GetActorFNameString / GetActorClassPtr). The
	// dumper's old shard/fnv/keystream-base constants had already drifted from
	// this build and resolved names to garbage.

	inline bool init( ) {
		if ( !Sigma::Driver::Base )
			return false;
		const bool names = Reflection::EnsureNames( Sigma::Driver::Base );
		// Also load steam_decrypt's reference key table so GetActorClassPtr can
		// use the reference tier from the very first object.
		steam_decrypt::InitFNameState( Sigma::Driver::Base );
		return names;
	}

	// CI -> text (memoized, "" when it does not resolve - never garbage).
	inline std::string cached_name_string( std::int32_t comp_index ) {
		if ( comp_index < 0 )
			return {};
		return Reflection::ResolveNameString( comp_index, Sigma::Driver::Base );
	}

	// An object's own name through the encrypted name slot.
	inline std::string decrypt_actor_name( std::uint64_t actor ) {
		if ( !Sigma::Driver::isvalid( actor ) )
			return {};
		return steam_decrypt::GetActorFNameString( static_cast< std::uintptr_t >( actor ) );
	}

	// An object's UClass pointer (reference tier -> plain -> slot fallbacks).
	inline std::uint64_t get_class_ptr( std::uint64_t obj ) {
		if ( !Sigma::Driver::isvalid( obj ) )
			return 0;
		const std::uint64_t cls =
			steam_decrypt::GetActorClassPtr( static_cast< std::uintptr_t >( obj ) );
		return Sigma::Driver::isvalid( cls ) ? cls : 0;
	}

	inline std::string get_object_class_name( std::uint64_t obj ) {
		const std::uint64_t cls = get_class_ptr( obj );
		if ( !cls )
			return {};
		return decrypt_actor_name( cls );
	}
}
