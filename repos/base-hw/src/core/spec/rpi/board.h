/*
 * \brief  Board driver for core
 * \author Martin Stein
 * \author Stefan Kalkowski
 * \date   2012-04-23
 */

/*
 * Copyright (C) 2012-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _CORE__SPEC__RPI__BOARD_H_
#define _CORE__SPEC__RPI__BOARD_H_

#include <hw/spec/arm/rpi_board.h>

namespace Board {
	using namespace Hw::Rpi_board;

	static constexpr bool SMP = false;
};

#endif /* _CORE__SPEC__RPI__BOARD_H_ */
