/*
 * \brief  Board driver for core on Zynq
 * \author Johannes Schlatow
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

#ifndef _CORE__SPEC__ZYNQ_QEMU__BOARD_H_
#define _CORE__SPEC__ZYNQ_QEMU__BOARD_H_

#include <hw/spec/arm/zynq_qemu_board.h>

namespace Board {
	using namespace Hw::Zynq_qemu_board;

	static constexpr bool SMP = true;

	L2_cache & l2_cache();
}

#endif /* _CORE__SPEC__ZYNQ_QEMU__BOARD_H_ */
