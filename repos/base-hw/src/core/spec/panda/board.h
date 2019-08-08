/*
 * \brief  Board driver for core on pandaboard
 * \author Stefan Kalkowski
 * \author Martin Stein
 * \date   2014-06-02
 */

/*
 * Copyright (C) 2014-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _CORE__SPEC__PANDA__BOARD_H_
#define _CORE__SPEC__PANDA__BOARD_H_

#include <hw/spec/arm/panda_board.h>

namespace Board {
	using namespace Hw::Panda_board;
	
	static constexpr bool SMP = true;

	class L2_cache : public Hw::Pl310
	{
		private:

			unsigned long _debug_value()
			{
				Debug::access_t v = 0;
				Debug::Dwb::set(v, 1);
				Debug::Dcl::set(v, 1);
				return v;
			}

		public:

			L2_cache(Genode::addr_t mmio) : Hw::Pl310(mmio) { }

			void clean_invalidate()
			{
				using namespace Hw;
				call_panda_firmware(L2_CACHE_SET_DEBUG_REG, _debug_value());
				Pl310::clean_invalidate();
				call_panda_firmware(L2_CACHE_SET_DEBUG_REG, 0);
			}
	};

	L2_cache & l2_cache();
}

#endif /* _CORE__SPEC__PANDA__BOARD_H_ */
