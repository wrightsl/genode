/*
 * \brief  Board driver
 * \author Stefan Kalkowski
 * \date   2017-10-18
 */

/*
 * Copyright (C) 2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _CORE__SPEC__NIT6_SOLOX__BOARD_H_
#define _CORE__SPEC__NIT6_SOLOX__BOARD_H_

#include <hw/spec/arm/nit6_solox_board.h>

namespace Board {
	using namespace Hw::Nit6_solox_board;

	using L2_cache = Hw::Pl310;

	static constexpr bool SMP = true;

	L2_cache & l2_cache();
}

#endif /* _CORE__SPEC__NIT6_SOLOX__BOARD_H_ */
