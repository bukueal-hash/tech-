#pragma once

#include "includes.h"

namespace game {
	struct gobject_array_t {
		std::uint64_t m_objects{};
		std::uint32_t m_num_elements{};
	};

	struct fname_state_t {
		std::uint16_t m_keytable[ offsets::keystream_count ]{};
		bool m_loaded = false;
	};
}