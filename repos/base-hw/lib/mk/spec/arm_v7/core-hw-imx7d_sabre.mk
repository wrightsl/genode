#
# \brief  Build config for Genodes core process
# \author Stefan Kalkowski
# \date   2015-02-09
#

# add include paths
INC_DIR += $(REP_DIR)/src/core/spec/imx7d_sabre
INC_DIR += $(REP_DIR)/src/core/spec/arm_v7/virtualization

# add C++ sources
SRC_CC += spec/arm_gic/pic.cc
SRC_CC += spec/arndale/platform_services.cc
SRC_CC += spec/imx7d_sabre/timer.cc
SRC_CC += kernel/vm_thread_on.cc
SRC_CC += spec/arm_v7/virtualization/kernel/vm.cc
SRC_CC += spec/arm_v7/vm_session_component.cc
SRC_CC += spec/arm_v7/virtualization/vm_session_component.cc
SRC_CC += vm_session_common.cc

# add assembly sources
SRC_S += spec/arm_v7/virtualization/exception_vector.s

NR_OF_CPUS = 2

#
# we need more specific compiler hints for some 'special' assembly code
# override -march=armv7-a because it conflicts with -mcpu=cortex-a7
#
CC_MARCH = -mcpu=cortex-a7 -mfpu=vfpv3 -mfloat-abi=softfp

# include less specific configuration
include $(BASE_DIR)/../base-hw/lib/mk/spec/cortex_a15/core-hw.inc
