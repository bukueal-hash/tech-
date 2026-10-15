#pragma once

#include "includes.h"

namespace game::sdk {
	// FField name: the packed FName is the app's DecodeFFieldName over the
	// encrypted window at FField::NamePrivate (NameCrypto.hpp kFfieldFnameEnc),
	// which is exactly what Reflection::PropertyNameIndex uses.
	inline std::uint64_t decode_ffield_name( std::uint64_t field_address ) {
		if ( !Sigma::Driver::isvalid( field_address ) )
			return 0;

		const auto reader          = Reflection::Dma( );
		const std::uint64_t source = namecrypto::ReadVal< std::uint64_t >(
			reader, field_address + namecrypto::kFfieldFnameEnc );
		const std::uint64_t second = namecrypto::ReadVal< std::uint64_t >(
			reader, field_address + namecrypto::kFfieldFnameEnc + 8 );
		return namecrypto::DecodeFFieldName( source, second );
	}

	// FFieldClass name: the app's test-pinned gasm decode, which reads the
	// encrypted u16[4] window at +0x20 (Core/SDK.hpp decode_ffieldclass_name).
	inline std::uint64_t decode_ffieldclass_name( std::uint64_t class_address ) {
		if ( !Sigma::Driver::isvalid( class_address ) )
			return 0;

		std::uint8_t buf[ 0x60 ]{};
		if ( !Sigma::Driver::ReadBytes( class_address, sizeof( buf ), buf ) )
			return 0;

		return game::gasm::decode_ffieldclass_name(
			game::gasm::decrypt_ffieldclass_name_context{}, buf );
	}

	// UStruct walkers - all layout comes from the generated Offsets.h through
	// Reflection, which also tries the extra children/next candidates so a
	// layout drift does not truncate the chain.
	inline std::uint64_t get_super_struct( std::uint64_t ustruct ) {
		return Reflection::SuperStruct( ustruct );
	}

	inline std::uint64_t get_property_link( std::uint64_t ustruct ) {
		return Reflection::PropertyLink( ustruct );
	}

	// True when obj's metaclass chain reaches "Class" (UClass and friends).
	inline bool is_uclass_instance( std::uint64_t obj ) {
		if ( !Sigma::Driver::isvalid( obj ) )
			return false;

		std::uint64_t current = gnames::get_class_ptr( obj );
		for ( int depth = 0; depth < 16 && Sigma::Driver::isvalid( current ); ++depth ) {
			const std::uint64_t super = Reflection::SuperStruct( current );
			if ( !Sigma::Driver::isvalid( super ) )
				return false;
			if ( gnames::decrypt_actor_name( super ) == "Class" )
				return true;
			current = super;
		}

		return false;
	}

	// property iteration
	struct property_info_t {
		std::uint64_t m_address{};
		std::uint64_t m_name_com_index{};
		std::uint64_t m_class_address{};
		std::uint32_t m_offset{};
	};

	inline void for_each_property( std::uint64_t ustruct, const std::function< void( property_info_t& ) >& callback ) {
		if ( !namecrypto::IsPlausibleNamePtr( ustruct ) )
			return;

		const auto reader = Reflection::Dma( );
		std::uint64_t prop = Reflection::PropertyLink( ustruct );
		for ( int guard = 0; namecrypto::IsPlausibleNamePtr( prop ) && guard < 8192; ++guard ) {
			property_info_t info{};
			info.m_address        = prop;
			info.m_name_com_index = decode_ffield_name( prop );
			info.m_class_address  = namecrypto::ReadVal< std::uint64_t >(
				reader, prop + namecrypto::kFfieldClassPrivate );
			info.m_offset         = Reflection::PropertyOffset( prop );

			callback( info );

			prop = Reflection::NextProperty( prop );
		}
	}

	// ffieldclass hierarchy
	inline std::string get_ffieldclass_name( std::uint64_t class_address ) {
		const std::uint64_t packed = decode_ffieldclass_name( class_address );
		if ( !packed )
			return {};

		return gnames::cached_name_string( static_cast< std::int32_t >( packed & 0xFFFFFFFFu ) );
	}

	inline bool is_class_of( std::uint64_t class_address, const std::string& target_name ) {
		std::uint64_t current = class_address;
		while ( Sigma::Driver::isvalid( current ) ) {
			if ( get_ffieldclass_name( current ) == target_name )
				return true;

			current = Sigma::Driver::Read< std::uint64_t >(
				current + offsets::ffieldclass_super_class );
		}

		return false;
	}

	// object property class (FObjectPropertyBase::PropertyClass)
	inline std::uint64_t get_property_class( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return 0;

		return Sigma::Driver::Read< std::uint64_t >(
			property_address + offsets::fobject_property_base_property_class );
	}

	// struct property struct
	inline std::uint64_t get_struct_property_struct( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return 0;

		return Sigma::Driver::Read< std::uint64_t >( property_address + offsets::fstruct_property_struct );
	}

	// enum property enum
	inline std::uint64_t get_enum_property_enum( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return 0;

		return Sigma::Driver::Read< std::uint64_t >( property_address + offsets::fenum_property_enum );
	}

	// byte property enum (FByteProperty::Enum)
	inline std::uint64_t get_byte_property_enum( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return 0;

		return Sigma::Driver::Read< std::uint64_t >( property_address + offsets::fbyte_property_enum );
	}

	// array property inner
	inline std::uint64_t get_array_property_inner( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return 0;

		return Sigma::Driver::Read< std::uint64_t >( property_address + offsets::farray_property_inner );
	}

	// set property inner
	inline std::uint64_t get_set_property_inner( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return 0;

		return Sigma::Driver::Read< std::uint64_t >( property_address + offsets::fset_property_inner );
	}

	// map property key
	inline std::uint64_t get_map_property_key( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return 0;

		return Sigma::Driver::Read< std::uint64_t >( property_address + offsets::fmap_property_key );
	}

	// map property value
	inline std::uint64_t get_map_property_value( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return 0;

		return Sigma::Driver::Read< std::uint64_t >( property_address + offsets::fmap_property_value );
	}
}
