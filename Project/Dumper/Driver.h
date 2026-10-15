#pragma once

#include <cstdint>
#include "../Core/Memory.h"

namespace Sigma::Driver {
    inline uintptr_t Base = 0;

    inline bool isvalid( std::uintptr_t address ) {
        return Memory::IsValidPtrFast2(address);
    }

    template < typename T >
    inline T Read( std::uintptr_t address ) {
        return Memory::read<T>( address );
    }

    inline bool ReadBytes( std::uintptr_t address, std::size_t length, std::uint8_t* buffer ) {
        return Memory::ReadRaw( address, buffer, length );
    }

    inline void ReadBuffer( std::uintptr_t address, void* buffer, std::size_t size ) {
        Memory::ReadRaw( address, buffer, size );
    }
}
