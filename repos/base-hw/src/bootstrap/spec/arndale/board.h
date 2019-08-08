/*
 * \brief   Arndale specific board definitions
 * \author  Stefan Kalkowski
 * \date    2017-04-03
 */

/*
 * Copyright (C) 2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _SRC__BOOTSTRAP__SPEC__ARNDALE__BOARD_H_
#define _SRC__BOOTSTRAP__SPEC__ARNDALE__BOARD_H_

#include <hw/spec/arm/arndale_board.h>
#include <hw/spec/arm/lpae.h>
#include <spec/arm/cpu.h>
#include <spec/arm/pic.h>

namespace Board { using namespace Hw::Arndale_board; }

#endif /* _SRC__BOOTSTRAP__SPEC__ARNDALE__BOARD_H_ */
