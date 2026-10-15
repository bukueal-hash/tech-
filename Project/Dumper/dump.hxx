#pragma once

#include "includes.h"

namespace game::dump {
	inline std::string known_struct_type( const std::string& struct_name ) {
		if ( struct_name == "Vector" || struct_name == "FVector" ) return "Vector";
		if ( struct_name == "Vector2D" || struct_name == "FVector2D" ) return "Vector2D";
		if ( struct_name == "Vector3f" || struct_name == "FVector3f" ) return "Vector3f";
		if ( struct_name == "Vector4" || struct_name == "FVector4" ) return "Vector4";
		if ( struct_name == "Rotator" || struct_name == "FRotator" ) return "Rotator";
		if ( struct_name == "Quat" || struct_name == "FQuat" ) return "Quat";
		if ( struct_name == "Transform" || struct_name == "FTransform" ) return "Transform";
		if ( struct_name == "LinearColor" || struct_name == "FLinearColor" ) return "LinearColor";
		if ( struct_name == "Color" || struct_name == "FColor" ) return "Color";
		if ( struct_name == "Guid" || struct_name == "FGuid" ) return "Guid";
		if ( struct_name == "Box" || struct_name == "FBox" ) return "Box";
		if ( struct_name == "BoxSphereBounds" || struct_name == "FBoxSphereBounds" ) return "BoxSphereBounds";
		return "F" + struct_name;
	}

	inline const char* property_type_name( const std::string& ffield_class );

	inline const char* class_prefix_for_name( const std::string& class_name, bool is_struct, const std::vector< std::string >& inheritance ) {
		if ( is_struct )
			return "F";
		if ( class_name == "Actor" )
			return "A";
		for ( const auto& a : inheritance )
			if ( a == "Actor" )
				return "A";
		return "U";
	}

	inline std::string prefixed_name( const std::string& bare_name, bool is_struct, const std::vector< std::string >& inheritance ) {
		return std::string( class_prefix_for_name( bare_name, is_struct, inheritance ) ) + bare_name;
	}

	inline std::string prefixed_name_inherit( const std::string& bare_name ) {
		if ( bare_name == "Actor" ) return "AActor";
		if ( bare_name == "Object" ) return "UObject";
		if ( bare_name == "Class" ) return "UClass";
		if ( bare_name == "Struct" ) return "UStruct";
		return "U" + bare_name;
	}

	// determine the F/A/U prefix for a parent UStruct based on its actual runtime type.
	// UScriptStruct instance  -> F prefix
	// UClass whose super chain contains "Actor" -> A prefix
	// AActor itself -> A prefix (special-case, no "Actor" in its own super chain)
	// everything else -> U prefix
	inline std::string prefixed_parent_name( std::uint64_t parent_obj ) {
		if ( !Sigma::Driver::isvalid( parent_obj ) )
			return {};

		const std::string bare = gnames::decrypt_actor_name( parent_obj );
		if ( bare.empty( ) )
			return {};

		if ( bare == "Actor" )
			return "AActor";

		const std::string cname = gnames::get_object_class_name( parent_obj );
		if ( cname == "ScriptStruct" )
			return "F" + bare;

		std::uint64_t current = parent_obj;
		for ( int depth = 0; depth < 16 && Sigma::Driver::isvalid( current ); ++depth ) {
			const std::uint64_t super = sdk::get_super_struct( current );
			if ( !Sigma::Driver::isvalid( super ) )
				break;
			const std::string super_name = gnames::decrypt_actor_name( super );
			if ( super_name == "Actor" )
				return "A" + bare;
			current = super;
		}

		return "U" + bare;
	}

	inline std::string resolve_enum_type( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return {};

		const std::uint64_t inner_class = Sigma::Driver::Read< std::uint64_t >( property_address + offsets::ffield_class_private );
		const std::string inner_class_name = sdk::get_ffieldclass_name( inner_class );

		std::uint64_t enum_cls = 0;
		if ( inner_class_name == "EnumProperty" )
			enum_cls = sdk::get_enum_property_enum( property_address );
		else if ( inner_class_name == "ByteProperty" )
			enum_cls = sdk::get_byte_property_enum( property_address );

		if ( !Sigma::Driver::isvalid( enum_cls ) )
			return {};

		std::string enum_name = gnames::decrypt_actor_name( enum_cls );
		if ( enum_name.empty( ) )
			return {};

		if ( enum_name[ 0 ] != 'E' )
			enum_name = "E" + enum_name;

		return enum_name;
	}

	inline std::string resolve_inner_type( std::uint64_t property_address ) {
		if ( !Sigma::Driver::isvalid( property_address ) )
			return "Unknown";

		const std::uint64_t inner_class = Sigma::Driver::Read< std::uint64_t >( property_address + offsets::ffield_class_private );
		const std::string inner_class_name = sdk::get_ffieldclass_name( inner_class );

		if ( inner_class_name == "ObjectProperty" || inner_class_name == "ClassProperty" || inner_class_name == "WeakObjectProperty" ) {
			const std::uint64_t prop_cls = sdk::get_property_class( property_address );
			if ( Sigma::Driver::isvalid( prop_cls ) ) {
				std::string obj_name = gnames::decrypt_actor_name( prop_cls );
				if ( !obj_name.empty( ) )
					return obj_name + "*";
			}
		}
		else if ( inner_class_name == "StructProperty" ) {
			const std::uint64_t struct_cls = sdk::get_struct_property_struct( property_address );
			if ( Sigma::Driver::isvalid( struct_cls ) ) {
				std::string struct_name = gnames::decrypt_actor_name( struct_cls );
				if ( !struct_name.empty( ) )
					return known_struct_type( struct_name );
			}
		}
		else if ( inner_class_name == "EnumProperty" || inner_class_name == "ByteProperty" ) {
			std::string enum_name = resolve_enum_type( property_address );
			if ( !enum_name.empty( ) )
				return enum_name;
		}

		const char* base = property_type_name( inner_class_name );
		return base ? base : "Unknown";
	}

	inline const char* property_type_name( const std::string& ffield_class ) {
		if ( ffield_class == "BoolProperty" ) return "bool";
		if ( ffield_class == "ByteProperty" ) return "uint8_t";
		if ( ffield_class == "Int8Property" ) return "int8_t";
		if ( ffield_class == "Int16Property" ) return "int16_t";
		if ( ffield_class == "UInt16Property" ) return "uint16_t";
		if ( ffield_class == "IntProperty" ) return "int32_t";
		if ( ffield_class == "UInt32Property" ) return "uint32_t";
		if ( ffield_class == "Int64Property" ) return "int64_t";
		if ( ffield_class == "UInt64Property" ) return "uint64_t";
		if ( ffield_class == "FloatProperty" ) return "float";
		if ( ffield_class == "DoubleProperty" ) return "double";
		if ( ffield_class == "NameProperty" ) return "FName";
		if ( ffield_class == "StrProperty" ) return "FString";
		if ( ffield_class == "TextProperty" ) return "FText";
		if ( ffield_class == "ClassProperty" ) return "UClass*";
		if ( ffield_class == "ObjectProperty" ) return "UObject*";
		if ( ffield_class == "WeakObjectProperty" ) return "TWeakObjectPtr<UObject>";
		if ( ffield_class == "LazyObjectProperty" ) return "TLazyObjectPtr<UObject>";
		if ( ffield_class == "SoftObjectProperty" ) return "TSoftObjectPtr<UObject>";
		if ( ffield_class == "SoftClassProperty" ) return "TSoftClassPtr<UObject>";
		if ( ffield_class == "InterfaceProperty" ) return "Interface";
		if ( ffield_class == "StructProperty" ) return "FStruct";
		if ( ffield_class == "ArrayProperty" ) return "TArray<Unknown>";
		if ( ffield_class == "MapProperty" ) return "TMap<Unknown, Unknown>";
		if ( ffield_class == "SetProperty" ) return "TSet<Unknown>";
		if ( ffield_class == "EnumProperty" ) return "uint8_t";
		if ( ffield_class == "DelegateProperty" ) return "FDelegate";
		if ( ffield_class == "MulticastDelegateProperty" ) return "MulticastDelegate";
		if ( ffield_class == "MulticastInlineDelegateProperty" ) return "MulticastInlineDelegate";
		if ( ffield_class == "MulticastSparseDelegateProperty" ) return "MulticastSparseDelegate";
		return "Unknown";
	}

	inline std::uint32_t property_type_size( const std::string& ffield_class ) {
		if ( ffield_class == "BoolProperty" ) return 1;
		if ( ffield_class == "ByteProperty" ) return 1;
		if ( ffield_class == "Int8Property" ) return 1;
		if ( ffield_class == "Int16Property" ) return 2;
		if ( ffield_class == "UInt16Property" ) return 2;
		if ( ffield_class == "IntProperty" ) return 4;
		if ( ffield_class == "UInt32Property" ) return 4;
		if ( ffield_class == "Int64Property" ) return 8;
		if ( ffield_class == "UInt64Property" ) return 8;
		if ( ffield_class == "FloatProperty" ) return 4;
		if ( ffield_class == "DoubleProperty" ) return 8;
		if ( ffield_class == "NameProperty" ) return 12;
		if ( ffield_class == "ObjectProperty" ) return 8;
		if ( ffield_class == "ClassProperty" ) return 8;
		if ( ffield_class == "WeakObjectProperty" ) return 8;
		if ( ffield_class == "InterfaceProperty" ) return 16;
		if ( ffield_class == "StrProperty" ) return 16;
		if ( ffield_class == "TextProperty" ) return 24;
		if ( ffield_class == "DelegateProperty" ) return 16;
		if ( ffield_class == "MulticastDelegateProperty" ) return 16;
		if ( ffield_class == "MulticastInlineDelegateProperty" ) return 16;
		return 0;
	}

	struct sdk_property_t {
		std::string m_type;
		std::string m_name;
		std::uint32_t m_offset{};
		std::string m_class_name;
	};

	struct sdk_inherit_t {
		std::string m_bare;       // unprefixed name — used for parent_size lookup
		std::string m_prefixed;   // full F/A/U-prefixed name — used for output
	};

	struct sdk_class_t {
		std::string m_name;
		std::string m_path;
		std::vector< sdk_inherit_t > m_inheritance;
		std::vector< sdk_property_t > m_properties;
		std::uint32_t m_size{};
		std::uint64_t m_address{};
		bool m_is_struct = false;
	};

	struct sdk_param_t {
		std::string m_type;
		std::string m_name;
	};

	inline std::string sanitize_name( const std::string& name ) {
		std::string out;
		for ( char c : name ) {
			if ( std::isalnum( c ) || c == '_' )
				out += c;
			else
				out += '_';
		}
		return out;
	}

	inline std::uint64_t get_outer_ptr( std::uint64_t obj ) {
		if ( !Sigma::Driver::isvalid( obj ) )
			return 0;

		// The app's outer resolver tries every rung (encrypted slot / drop
		// cipher / plain) and returns the first plausible pointer.
		return OuterLink::FromObject( obj ).ptr;
	}

	inline std::string get_class_full_name( std::uint64_t obj ) {
		std::string name = gnames::decrypt_actor_name( obj );
		if ( name.empty( ) )
			return {};

		std::string path = name;
		std::uint64_t outer = get_outer_ptr( obj );
		int depth = 0;

		while ( Sigma::Driver::isvalid( outer ) && depth < 16 ) {
			std::string outer_name = gnames::decrypt_actor_name( outer );
			if ( outer_name.empty( ) )
				break;
			path = outer_name + "." + path;
			outer = get_outer_ptr( outer );
			++depth;
		}

		return path;
	}

	inline void dump_sdk( ) {
		logging::print( "resolving gobjects..." );

		const auto objs = gobjects::get( );
		if ( !objs.m_objects || !objs.m_num_elements ) {
			logging::print( "failed to resolve gobjects (%s)", gobjects::last_error( ).c_str( ) );
			return;
		}

		logging::print( "gobjects: 0x%llX (%u elements)" , objs.m_objects, objs.m_num_elements );
		logging::print( "initializing gnames..." );

		if ( !gnames::init( ) ) {
			logging::print( "failed to initialize gnames" );
			return;
		}

		logging::print( "dumping..." );

		std::map< std::string, std::uint32_t > class_size_map;
		std::vector< sdk_class_t > classes;
		std::vector< sdk_class_t > enums;
		std::uint32_t class_count = 0;
		std::uint32_t enum_count = 0;

		for ( std::uint32_t i = 0; i < objs.m_num_elements; ++i ) {
			if ( ( i % 200000u ) == 0 )
				logging::print( "scanned %u/%u objects (%u classes, %u enums)...",
					i, objs.m_num_elements, class_count, enum_count );

			const std::uint64_t obj = gobjects::get_object_from_index( objs.m_objects, i );
			if ( !obj )
				continue;

			const std::string cname = gnames::get_object_class_name( obj );
			const bool known_type = ( cname == "ScriptStruct" || cname == "Enum" || cname == "UserDefinedEnum" );
			if ( !known_type ) {
				if ( cname != "Class" && !sdk::is_uclass_instance( obj ) )
					continue;
			}

			const std::string name = gnames::decrypt_actor_name( obj );
			if ( name.empty( ) )
				continue;

			const bool is_enum = ( cname == "Enum" || cname == "UserDefinedEnum" );

			if ( is_enum ) {
				sdk_class_t enm{};
				enm.m_name    = name;
				enm.m_path    = get_class_full_name( obj );
				enm.m_address = obj;

				const std::uint64_t names_data = Sigma::Driver::Read< std::uint64_t >( obj + offsets::uenum_names_data ) & ~1ULL;
				const std::int32_t  count      = Sigma::Driver::Read< std::int32_t >( obj + offsets::uenum_count );

				for ( std::int32_t j = 0; j < count; ++j ) {
					const std::uint32_t comp_index = Sigma::Driver::Read< std::uint32_t >( names_data + j * 8 );
					std::string entry_name = gnames::cached_name_string( static_cast< std::int32_t >( comp_index ) );
					if ( entry_name.empty( ) )
						entry_name = "__Unknown_" + std::to_string( j );

					sdk_property_t p{};
					p.m_name = entry_name;
					p.m_offset = static_cast< std::uint32_t >( j );
					p.m_type = "int64_t";
					p.m_class_name = "EnumValue";
					enm.m_properties.push_back( p );
				}

				enm.m_size = static_cast< std::uint32_t >( count );
				enums.push_back( enm );
				++enum_count;
				continue;
			}

			sdk_class_t cls{};
			cls.m_name = name;
			cls.m_path = get_class_full_name( obj );
			cls.m_address = obj;
			cls.m_is_struct = ( cname == "ScriptStruct" );

			std::uint64_t super = sdk::get_super_struct( obj );
			while ( Sigma::Driver::isvalid( super ) ) {
				const std::string super_bare = gnames::decrypt_actor_name( super );
				if ( super_bare.empty( ) )
					break;
				const std::string super_prefixed = prefixed_parent_name( super );
				cls.m_inheritance.push_back( { super_bare, super_prefixed } );
				super = sdk::get_super_struct( super );
			}

			std::uint32_t max_end = 0;
			sdk::for_each_property( obj, [ & ]( sdk::property_info_t& prop ) {
				sdk_property_t p{};
				p.m_name = gnames::cached_name_string( static_cast< std::int32_t >( prop.m_name_com_index & 0xFFFFFFFFu ) );
				if ( p.m_name.empty( ) )
					return;
				p.m_offset = prop.m_offset;
				p.m_class_name = sdk::get_ffieldclass_name( prop.m_class_address );
				p.m_type = property_type_name( p.m_class_name );

				if ( p.m_class_name == "ObjectProperty" || p.m_class_name == "ClassProperty" || p.m_class_name == "WeakObjectProperty" ) {
					const std::uint64_t prop_cls = sdk::get_property_class( prop.m_address );
					if ( Sigma::Driver::isvalid( prop_cls ) ) {
						std::string obj_name = gnames::decrypt_actor_name( prop_cls );
						if ( !obj_name.empty( ) )
							p.m_type = obj_name + "*";
					}
				}
				else if ( p.m_class_name == "StructProperty" ) {
					const std::uint64_t struct_cls = sdk::get_struct_property_struct( prop.m_address );
					if ( Sigma::Driver::isvalid( struct_cls ) ) {
						std::string struct_name = gnames::decrypt_actor_name( struct_cls );
						if ( !struct_name.empty( ) )
							p.m_type = known_struct_type( struct_name );
					}
				}
				else if ( p.m_class_name == "EnumProperty" || p.m_class_name == "ByteProperty" ) {
					std::string enum_name = resolve_enum_type( prop.m_address );
					if ( !enum_name.empty( ) )
						p.m_type = enum_name;
				}
				else if ( p.m_class_name == "ArrayProperty" ) {
					const std::uint64_t inner_prop = sdk::get_array_property_inner( prop.m_address );
					if ( Sigma::Driver::isvalid( inner_prop ) )
						p.m_type = "TArray<" + resolve_inner_type( inner_prop ) + ">";
				}
				else if ( p.m_class_name == "SetProperty" ) {
					const std::uint64_t inner_prop = sdk::get_set_property_inner( prop.m_address );
					if ( Sigma::Driver::isvalid( inner_prop ) )
						p.m_type = "TSet<" + resolve_inner_type( inner_prop ) + ">";
				}
				else if ( p.m_class_name == "MapProperty" ) {
					const std::uint64_t key_prop = sdk::get_map_property_key( prop.m_address );
					const std::uint64_t value_prop = sdk::get_map_property_value( prop.m_address );
					if ( Sigma::Driver::isvalid( key_prop ) && Sigma::Driver::isvalid( value_prop ) )
						p.m_type = "TMap<" + resolve_inner_type( key_prop ) + ", " + resolve_inner_type( value_prop ) + ">";
				}

				cls.m_properties.push_back( p );

				const std::uint32_t prop_size = property_type_size( p.m_class_name );
				if ( prop_size && p.m_offset + prop_size > max_end )
					max_end = p.m_offset + prop_size;
			} );

			cls.m_size = max_end;
			class_size_map[ cls.m_name ] = cls.m_size;
			classes.push_back( cls );
			++class_count;
		}

		logging::print( "found %u classes, %u enums, writing sdk..." , class_count, enum_count );

		FILE* file = nullptr;
		fopen_s( &file, "sdk.h", "w" );
		if ( !file ) {
			logging::print( "failed to open sdk.h" );
			return;
		}

		fprintf( file, "// SDK generated by flakydumper\n" );
		fprintf( file, "// Dumper made by flakydolphin\n" );
		fprintf( file, "// Classes: %u | Enums: %u\n\n", class_count, enum_count );
		fprintf( file, "#pragma once\n\n" );
		fprintf( file, "#include <cstdint>\n\n" );

		for ( const auto& enm : enums ) {
			if ( !enm.m_path.empty( ) )
				fprintf( file, "// %s\n", enm.m_path.c_str( ) );

			std::string enum_short_name = enm.m_name;
			if ( !enum_short_name.empty() && enum_short_name[ 0 ] == 'E' )
				enum_short_name = enum_short_name.substr( 1 );
			fprintf( file, "enum class E%s : int64_t\n{\n", sanitize_name( enum_short_name ).c_str( ) );

			for ( const auto& prop : enm.m_properties ) {
				fprintf( file, "    %s = %d,\n", sanitize_name( prop.m_name ).c_str( ), prop.m_offset );
			}

			fprintf( file, "};\n\n" );
		}

		for ( const auto& cls : classes ) {
			if ( !cls.m_path.empty( ) )
				fprintf( file, "// %s\n", cls.m_path.c_str( ) );

			std::vector< std::string > inherit_bare;
			inherit_bare.reserve( cls.m_inheritance.size( ) );
			for ( const auto& ih : cls.m_inheritance )
				inherit_bare.push_back( ih.m_bare );

			const std::string self_name = prefixed_name( cls.m_name, cls.m_is_struct, inherit_bare );
			fprintf( file, "struct %s", self_name.c_str( ) );
			if ( !cls.m_inheritance.empty( ) ) {
				fprintf( file, " : public %s", cls.m_inheritance[ 0 ].m_prefixed.c_str( ) );
				for ( std::size_t j = 1; j < cls.m_inheritance.size( ); ++j )
					fprintf( file, ", public %s", cls.m_inheritance[ j ].m_prefixed.c_str( ) );
			}
			fprintf( file, "\n{\npublic:\n" );

			const std::uint32_t parent_size = cls.m_inheritance.empty( ) ? 0 :
				class_size_map.count( cls.m_inheritance.back( ).m_bare ) ? class_size_map[ cls.m_inheritance.back( ).m_bare ] : 0;

			for ( const auto& prop : cls.m_properties ) {
				if ( prop.m_offset < parent_size )
					continue;
				fprintf( file, "    %s %s; // 0x%X\n", prop.m_type.c_str( ), sanitize_name( prop.m_name ).c_str( ), prop.m_offset );
			}

			fprintf( file, "}; // Size: 0x%x\n\n", cls.m_size );
		}

		fclose( file );
		logging::print( "sdk written (%u classes, %u enums)" , class_count, enum_count );
	}

	inline void dump_objects( ) {
		logging::print( "resolving gobjects..." );
		const auto objs = gobjects::get( );
		if ( !objs.m_objects || !objs.m_num_elements ) {
			logging::print( "failed to resolve gobjects" );
			return;
		}

		logging::print( "gobjects: 0x%llX (%u elements)" , objs.m_objects, objs.m_num_elements );
		logging::print( "resolving gnames..." );

		if ( !gnames::init( ) ) {
			logging::print( "failed to initialize gnames" );
			return;
		}

		logging::print( "dumping to objects.txt..." );
		FILE* file = nullptr;
		fopen_s( &file, "objects.txt", "w" );
		if ( !file ) {
			logging::print( "failed to open objects.txt" );
			return;
		}

		std::uint32_t dumped = 0;

		for ( std::uint32_t i = 0; i < objs.m_num_elements; ++i ) {
			const std::uint64_t obj = gobjects::get_object_from_index( objs.m_objects, i );
			if ( !obj )
				continue;

			const std::string name  = gnames::decrypt_actor_name( obj );
			if ( name.empty( ) )
				continue;

			const std::string cname = gnames::get_object_class_name( obj );
			fprintf( file, "[%u] 0x%llX %s %s\n", i, obj, cname.c_str( ), name.c_str( ) );
			++dumped;
		}

		fclose( file );
		logging::print( "dumped %u objects to objects.txt" , dumped );
	}
}