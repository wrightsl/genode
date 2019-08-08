/*
 * \brief  VMM example for ARM Virtualization
 * \author Stefan Kalkowski
 * \date   2014-07-08
 */

/*
 * Copyright (C) 2014-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <base/attached_ram_dataspace.h>
#include <base/attached_rom_dataspace.h>
#include <base/component.h>
#include <base/exception.h>
#include <base/heap.h>
#include <base/log.h>
#include <cpu/cpu_state.h>
#include <drivers/defs/arm_v7.h>
#include <os/ring_buffer.h>
#include <terminal_session/connection.h>
#include <timer_session/connection.h>
#include <util/avl_tree.h>
#include <util/mmio.h>
#include <vm_session/connection.h>

#include <cpu/vm_state_virtualization.h>
#include <board.h>


struct State : Genode::Vm_state
{
	Genode::uint32_t midr;
	Genode::uint32_t mpidr;
	Genode::uint32_t ctr;
	Genode::uint32_t ccsidr;
	Genode::uint32_t clidr;
	Genode::uint32_t pfr0;
	Genode::uint32_t mmfr0;
	Genode::uint32_t isar0;
	Genode::uint32_t isar3;
	Genode::uint32_t isar4;
	Genode::uint32_t csselr;
	Genode::uint32_t actrl;

	class Invalid_register : Genode::Exception {};

	struct Gp_register { Genode::addr_t r[16]; };

	struct Psr : Genode::Register<32>
	{
		struct Mode : Bitfield<0,5>
		{
			enum {
				USR   = 16,
				FIQ   = 17,
				IRQ   = 18,
				SVC   = 19,
				ABORT = 23,
				UND   = 27
			};
		};

		static int mode_offset(access_t v)
		{
			switch(Mode::get(v)) {
			case Mode::FIQ:   return Mode_state::Mode::FIQ;
			case Mode::IRQ:   return Mode_state::Mode::IRQ;
			case Mode::SVC:   return Mode_state::Mode::SVC;
			case Mode::ABORT: return Mode_state::Mode::ABORT;
			case Mode::UND:   return Mode_state::Mode::UND;
			default: return -1;
			};
		}
	};

	Genode::addr_t * r(unsigned i)
	{
		unsigned mo = Psr::mode_offset(cpsr);
		switch (i) {
		case 13: return (mo < 0) ? &sp : &(mode[mo].sp);
		case 14: return (mo < 0) ? &lr : &(mode[mo].lr);
		default: return &(reinterpret_cast<Gp_register*>(this)->r[i]);
		};
	}
};


class Ram {

	private:

		Genode::addr_t const _base;
		Genode::size_t const _size;
		Genode::addr_t const _local;

	public:

		Ram(Genode::addr_t const addr, Genode::size_t const sz,
		    Genode::addr_t const local)
		: _base(addr), _size(sz), _local(local) { }

		Genode::addr_t base()  const { return _base;  }
		Genode::size_t size()  const { return _size;  }
		Genode::addr_t local() const { return _local; }
};


class Vm {

	private:

		enum {
			RAM_ADDRESS   = 0x80000000,
			MACH_TYPE     = 2272, /* ARNDALE = 4274; VEXPRESS = 2272 */
			KERNEL_OFFSET = 0x8000,
			DTB_OFFSET    = 64 * 1024 * 1024,
		};

		Genode::Vm_connection          _vm;
		Genode::Attached_rom_dataspace _kernel_rom;
		Genode::Attached_rom_dataspace _dtb_rom;
		Genode::Attached_ram_dataspace _vm_ram;
		Ram                            _ram;
		Genode::Heap                   _heap;
		Genode::Vm_session::Vcpu_id    _vcpu_id;
		State &                        _state;
		bool                           _active = true;

		void _load_kernel()
		{
			Genode::memcpy((void*)(_ram.local() + KERNEL_OFFSET),
			               _kernel_rom.local_addr<void>(),
			               _kernel_rom.size());
			_state.ip = _ram.base() + KERNEL_OFFSET;
		}

		void _load_dtb()
		{
			Genode::memcpy((void*)(_ram.local() + DTB_OFFSET),
			               _dtb_rom.local_addr<void>(),
			               _dtb_rom.size());
			_state.r2 = _ram.base() + DTB_OFFSET;
		}

	public:

		class Exception : Genode::Exception
		{
			private:

				enum { BUF_SIZE = 128 };
				char _buf[BUF_SIZE];

			public:

				Exception(const char *fmt, ...)
				{
					va_list args;
					va_start(args, fmt);
					Genode::String_console sc(_buf, BUF_SIZE);
					sc.vprintf(fmt, args);
					va_end(args);
				}

				Exception() : Exception("undefined") {}

				void print() { Genode::error(Genode::Cstring(_buf)); }
		};


		Vm(const char *kernel, const char *dtb, Genode::size_t const ram_size,
		   Genode::Vm_handler_base &handler, Genode::Env & env)
		: _vm(env),
		  _kernel_rom(env, kernel),
		  _dtb_rom(env, dtb),
		  _vm_ram(env.ram(), env.rm(), ram_size, Genode::UNCACHED),
		  _ram(RAM_ADDRESS, ram_size, (Genode::addr_t)_vm_ram.local_addr<void>()),
		  _heap(env.ram(), env.rm()),
		  _vcpu_id(_vm.create_vcpu(_heap, env, handler)),
		  _state(*((State*)env.rm().attach(_vm.cpu_state(_vcpu_id))))
		{
			Genode::log("ram is at ",
			            Genode::Hex(Genode::Dataspace_client(_vm_ram.cap()).phys_addr()));

			_vm.attach(_vm_ram.cap(), RAM_ADDRESS);
			_vm.attach_pic(0x2C002000);
		}

		void start()
		{
			Genode::memset((void*)&_state, 0, sizeof(Genode::Cpu_state_modes));
			_load_kernel();
			_load_dtb();
			_state.r1    = MACH_TYPE;
			_state.cpsr  = 0x93; /* SVC mode and IRQs disabled */

			_state.timer_ctrl = 0;
			_state.timer_val  = 0;
			_state.timer_irq  = false;

			_state.gic_hcr    = 0b101;
			_state.gic_vmcr   = 0x4c0000;
			_state.gic_apr    = 0;
			_state.gic_lr[0]  = 0;
			_state.gic_lr[1]  = 0;
			_state.gic_lr[2]  = 0;
			_state.gic_lr[3]  = 0;

			Genode::log("ready to run");
		}

		void run()                { if (_active) _vm.run(_vcpu_id); }
		void pause()              { _vm.pause(_vcpu_id); }
		void wait_for_interrupt() { _active = false;              }
		void interrupt()          { _active = true;               }
		bool active()             { return _active;               }

		void dump()
		{
			using namespace Genode;

			const char * const modes[] =
				{ "und", "svc", "abt", "irq", "fiq" };
			const char * const exc[] =
				{ "nope", "reset", "undefined", "svc", "pf_abort",
			      "data_abort", "irq", "fiq", "trap" };

			log("Cpu state:");
			log("  r0         = ", Hex(_state.r0,   Hex::PREFIX, Hex::PAD));
			log("  r1         = ", Hex(_state.r1,   Hex::PREFIX, Hex::PAD));
			log("  r2         = ", Hex(_state.r2,   Hex::PREFIX, Hex::PAD));
			log("  r3         = ", Hex(_state.r3,   Hex::PREFIX, Hex::PAD));
			log("  r4         = ", Hex(_state.r4,   Hex::PREFIX, Hex::PAD));
			log("  r5         = ", Hex(_state.r5,   Hex::PREFIX, Hex::PAD));
			log("  r6         = ", Hex(_state.r6,   Hex::PREFIX, Hex::PAD));
			log("  r7         = ", Hex(_state.r7,   Hex::PREFIX, Hex::PAD));
			log("  r8         = ", Hex(_state.r8,   Hex::PREFIX, Hex::PAD));
			log("  r9         = ", Hex(_state.r9,   Hex::PREFIX, Hex::PAD));
			log("  r10        = ", Hex(_state.r10,  Hex::PREFIX, Hex::PAD));
			log("  r11        = ", Hex(_state.r11,  Hex::PREFIX, Hex::PAD));
			log("  r12        = ", Hex(_state.r12,  Hex::PREFIX, Hex::PAD));
			log("  sp         = ", Hex(_state.sp,   Hex::PREFIX, Hex::PAD));
			log("  lr         = ", Hex(_state.lr,   Hex::PREFIX, Hex::PAD));
			log("  ip         = ", Hex(_state.ip,   Hex::PREFIX, Hex::PAD));
			log("  cpsr       = ", Hex(_state.cpsr, Hex::PREFIX, Hex::PAD));
			for (unsigned i = 0;
			     i < State::Mode_state::MAX; i++) {
				log("  sp_", modes[i], "     = ",
				    Hex(_state.mode[i].sp, Hex::PREFIX, Hex::PAD));
				log("  lr_", modes[i], "     = ",
				    Hex(_state.mode[i].lr, Hex::PREFIX, Hex::PAD));
				log("  spsr_", modes[i], "   = ",
				    Hex(_state.mode[i].spsr, Hex::PREFIX, Hex::PAD));
			}
			log("  exception  = ", exc[_state.cpu_exception]);
		}

		State & state() const { return _state; }
};



class Vmm
{
	private:

		template <typename T>
		struct Signal_handler : Genode::Vm_handler<Signal_handler<T>>
		{
			using Base = Genode::Vm_handler<Signal_handler<T>>;

			Vmm & vmm;
			T  & obj;
			void (T::*member)();

			void handle()
			{
				try {
					vmm.handle_vm([this] () { (obj.*member)(); });
				} catch(Vm::Exception &e) {
					e.print();
					vmm.vm().dump();
				}
			}

			Signal_handler(Vmm & vmm, Genode::Entrypoint &ep, T & o,
			               void (T::*f)())
			: Base(ep, *this, &Signal_handler::handle),
			  vmm(vmm), obj(o), member(f) {}
		};

		struct Hsr : Genode::Register<32>
		{
			struct Ec : Bitfield<26, 6>
			{
				enum {
					WFI  = 0x1,
					CP15 = 0x3,
					HVC  = 0x12,
					DA   = 0x24
				};
			};
		};

		class Coprocessor
		{
			protected:

				struct Iss : Hsr
				{
					struct Direction : Bitfield<0,  1> {};
					struct Crm       : Bitfield<1,  4> {};
					struct Register  : Bitfield<5,  4> {};
					struct Crn       : Bitfield<10, 4> {};
					struct Opcode1   : Bitfield<14, 3> {};
					struct Opcode2   : Bitfield<17, 3> {};

					static access_t value(unsigned crn, unsigned op1,
					                      unsigned crm, unsigned op2)
					{
						access_t v = 0;
						Crn::set(v, crn);
						Crm::set(v, crm);
						Opcode1::set(v, op1);
						Opcode2::set(v, op2);
						return v;
					};

					static access_t mask_encoding(access_t v)
					{
						return Crm::masked(v) |
						       Crn::masked(v) |
						       Opcode1::masked(v) |
						       Opcode2::masked(v);
					}
				};


				class Register : public Genode::Avl_node<Register>
				{
					private:

						const Iss::access_t       _encoding;
						const char               *_name;
						const bool                _writeable;
						Genode::uint32_t  State::*_r;
						const Genode::addr_t      _init_value;

					public:

						Register(unsigned crn, unsigned op1,
						         unsigned crm, unsigned op2,
						         const char * name,
						         bool writeable,
						         Genode::uint32_t State::*r,
						         Genode::addr_t v)
						: _encoding(Iss::value(crn, op1, crm, op2)),
						  _name(name),
						  _writeable(writeable), _r(r), _init_value(v) {}

						const char * name()       const { return _name;      }
						const bool   writeable()  const { return _writeable; }
						const Genode::addr_t init_value() const {
							return _init_value; }

						Register * find_by_encoding(Iss::access_t e)
						{
							if (e == _encoding) return this;

							Register * r =
								Avl_node<Register>::child(e > _encoding);
							return r ? r->find_by_encoding(e) : nullptr;
						}

						void write(State & state, Genode::addr_t v) {
							state.*_r = (Genode::uint32_t)v; }

						Genode::addr_t read(State & state) const {
							return (Genode::addr_t)(state.*_r); }


						/************************
						 ** Avl node interface **
						 ************************/

						bool higher(Register *r) {
							return (r->_encoding > _encoding); }
				};

				Genode::Avl_tree<Register> _reg_tree;

			public:

				bool handle_trap(State & state)
				{
					Iss::access_t v = state.hsr;
					Register * reg = _reg_tree.first();
					if (reg) reg = reg->find_by_encoding(Iss::mask_encoding(v));

					if (!reg) {
						Genode::error("unknown cp15 access @ ip=", state.ip, ":");
						Genode::error(Iss::Direction::get(v) ? "read" : "write",
						              ": "
						              "c15 ", Iss::Opcode1::get(v), " "
						              "r",    Iss::Register::get(v),   " "
						              "c",    Iss::Crn::get(v),        " "
						              "c",    Iss::Crm::get(v), " ",
						                      Iss::Opcode2::get(v));
						return false;
					}

					if (Iss::Direction::get(v)) { /* read access  */
						*(state.r(Iss::Register::get(v))) = reg->read(state);
					} else {                      /* write access */
						if (!reg->writeable()) {
							Genode::error("writing to cp15 register ",
							              reg->name(), " not allowed!");
							return false;
						}
						reg->write(state, *(state.r(Iss::Register::get(v))));
					}
					state.ip += sizeof(Genode::addr_t);
					return true;
				}
		};


		class Cp15 : public Coprocessor
		{
			private:

				Register _regs_0  {  0, 0, 0, 0, "MIDR",       false, &State::midr,   0x412fc0f1 };
				Register _regs_1  {  0, 0, 0, 5, "MPIDR",      false, &State::mpidr,  0x40000000 };
				Register _regs_2  {  0, 0, 0, 1, "CTR",        false, &State::ctr,    0x8444c004 };
				Register _regs_3  {  0, 1, 0, 0, "CCSIDR",     false, &State::ccsidr, 0x701fe00a };
				Register _regs_4  {  0, 1, 0, 1, "CLIDR",      false, &State::clidr,  0x0a200023 };
				Register _regs_5  {  0, 0, 1, 0, "PFR0",       false, &State::pfr0,   0x00001031 };
				Register _regs_6  {  0, 0, 1, 4, "MMFR0",      false, &State::mmfr0,  0x10201105 };
				Register _regs_7  {  0, 0, 2, 0, "ISAR0",      false, &State::isar0,  0x02101110 };
				Register _regs_8  {  0, 0, 2, 3, "ISAR3",      false, &State::isar3,  0x11112131 };
				Register _regs_9  {  0, 0, 2, 4, "ISAR4",      false, &State::isar4,  0x10011142 };
				Register _regs_10 {  0, 2, 0, 0, "CSSELR",     true,  &State::csselr, 0x00000000 };
				Register _regs_11 {  1, 0, 0, 0, "SCTRL",      true,  &State::sctrl,  0 /* 0xc5007a 0x00c5187a*/ };
				Register _regs_12 {  1, 0, 0, 1, "ACTRL",      true,  &State::actrl,  0x00000040 };
				Register _regs_13 {  1, 0, 0, 2, "CPACR",      true,  &State::cpacr,  0x00000000 };
				Register _regs_14 {  2, 0, 0, 0, "TTBR0",      true,  &State::ttbr0,  0x00000000 };
				Register _regs_15 {  2, 0, 0, 1, "TTBR1",      true,  &State::ttbr1,  0x00000000 };
				Register _regs_16 {  2, 0, 0, 2, "TTBCR",      true,  &State::ttbcr,  0x00000000 };
				Register _regs_17 {  3, 0, 0, 0, "DACR",       true,  &State::dacr,   0x55555555 };
				Register _regs_18 {  5, 0, 0, 0, "DFSR",       true,  &State::dfsr,   0x00000000 };
				Register _regs_19 {  5, 0, 0, 1, "IFSR",       true,  &State::ifsr,   0x00000000 };
				Register _regs_20 {  5, 0, 1, 0, "ADFSR",      true,  &State::adfsr,  0x00000000 };
				Register _regs_21 {  5, 0, 1, 1, "AIFSR",      true,  &State::aifsr,  0x00000000 };
				Register _regs_22 {  6, 0, 0, 0, "DFAR",       true,  &State::dfar,   0x00000000 };
				Register _regs_23 {  6, 0, 0, 2, "IFAR",       true,  &State::ifar,   0x00000000 };
				Register _regs_24 { 10, 0, 2, 0, "PRRR",       true,  &State::prrr,   0x00098aa4 };
				Register _regs_25 { 10, 0, 2, 1, "NMRR",       true,  &State::nmrr,   0x44e048e0 };
				Register _regs_26 { 13, 0, 0, 1, "CONTEXTIDR", true,  &State::cidr,   0x00000000 };

				void _init_reg(Register &reg, State &state)
				{
					_reg_tree.insert(&reg);
					reg.write(state, reg.init_value());
				}

			public:

				Cp15(State & state)
				{
					_init_reg(_regs_0, state);
					_init_reg(_regs_1, state);
					_init_reg(_regs_2, state);
					_init_reg(_regs_3, state);
					_init_reg(_regs_4, state);
					_init_reg(_regs_5, state);
					_init_reg(_regs_6, state);
					_init_reg(_regs_7, state);
					_init_reg(_regs_8, state);
					_init_reg(_regs_9, state);
					_init_reg(_regs_10, state);
					_init_reg(_regs_11, state);
					_init_reg(_regs_12, state);
					_init_reg(_regs_13, state);
					_init_reg(_regs_14, state);
					_init_reg(_regs_15, state);
					_init_reg(_regs_16, state);
					_init_reg(_regs_17, state);
					_init_reg(_regs_18, state);
					_init_reg(_regs_19, state);
					_init_reg(_regs_20, state);
					_init_reg(_regs_21, state);
					_init_reg(_regs_22, state);
					_init_reg(_regs_23, state);
					_init_reg(_regs_24, state);
					_init_reg(_regs_25, state);
					_init_reg(_regs_26, state);
				}
		};


		class Device : public Genode::Avl_node<Device>
		{
			protected:

				struct Iss : Hsr
				{
					struct Write        : Bitfield<6,  1> {};
					struct Register     : Bitfield<16, 4> {};
					struct Sign_extend  : Bitfield<21, 1> {};
					struct Access_size  : Bitfield<22, 2> {
						enum { BYTE, HALFWORD, WORD }; };
					struct Valid        : Bitfield<24, 1> {};

					static bool valid(access_t v) {
						return Valid::get(v) && !Sign_extend::get(v); }

					static bool write(access_t v) { return Write::get(v); }
					static unsigned r(access_t v) { return Register::get(v); }
				};

				const char * const     _name;
				const Genode::uint64_t _addr;
				const Genode::uint64_t _size;
				Vm &                   _vm;

				using Error = Vm::Exception;

			public:

				Device(const char * const       name,
				       const Genode::uint64_t   addr,
				       const Genode::uint64_t   size,
				       Vm &                     vm)
				: _name(name), _addr(addr), _size(size), _vm(vm) { }

				Genode::uint64_t addr() { return _addr; }
				Genode::uint64_t size() { return _size; }
				const char *     name() { return _name; }

				virtual void read  (Genode::uint32_t * reg,
				                    Genode::uint64_t   off) {
					throw Error("Device %s: word-wise read of %llx not allowed",
					            name(), off); }

				virtual void write (Genode::uint32_t * reg,
				                    Genode::uint64_t   off) {
					throw Error("Device %s: word-wise write of %llx not allowed",
					            name(), off); }

				virtual void read  (Genode::uint16_t * reg,
				                    Genode::uint64_t   off) {
					throw Error("Device %s: halfword read of %llx not allowed",
					            name(), off); }

				virtual void write (Genode::uint16_t * reg,
				                    Genode::uint64_t   off) {
					throw Error("Device %s: halfword write of %llx not allowed",
					            name(), off); }

				virtual void read  (Genode::uint8_t  * reg,
				                    Genode::uint64_t   off) {
					throw Error("Device %s: byte-wise read of %llx not allowed",
					            name(), off); }

				virtual void write (Genode::uint8_t  * reg,
				                    Genode::uint64_t   off) {
					throw Error("Device %s: byte-wise write of %llx not allowed",
					            name(), off); }

				virtual void irq_enabled (unsigned irq) { }
				virtual void irq_disabled(unsigned irq) { }
				virtual void irq_handled (unsigned irq) { }

				void handle_memory_access(State & state)
				{
					using namespace Genode;

					if (!Iss::valid(state.hsr))
						throw Error("Device %s: unknown HSR=%lx",
						            name(), state.hsr);

					bool     wr  = Iss::Write::get(state.hsr);
					unsigned idx = Iss::Register::get(state.hsr);
					uint64_t ipa = (uint64_t)state.hpfar << 8;
					uint64_t off = ipa - addr() + (state.hdfar & ((1 << 13) - 1));

					switch (Iss::Access_size::get(state.hsr)) {
					case Iss::Access_size::BYTE:
						{
							uint8_t * p = (uint8_t*)state.r(idx) + (off & 0b11);
							wr ? write(p, off) : read(p, off);
							break;
						}
					case Iss::Access_size::HALFWORD:
						{
							uint16_t * p = (uint16_t*) state.r(idx) + (off & 0b1);
							wr ? write(p, off) : read(p, off);
							break;
						}
					case Iss::Access_size::WORD:
						{
							uint32_t * p = (uint32_t*) state.r(idx);
							wr ? write(p, off) : read(p, off);
							break;
						}
					default:
						throw Error("Device %s: invalid alignment", name());
					};
				}

				/************************
				 ** Avl node interface **
				 ************************/

				bool higher(Device *d) { return d->addr() > addr(); }

				Device *find_by_addr(Genode::uint64_t a)
				{
					if ((a >= addr()) && (a < (addr()+size())))
						return this;

					Device *d = Avl_node<Device>::child(a > addr());
					return d ? d->find_by_addr(a) : nullptr;
				}
		};


		class Gic : public Device
		{
				enum {
					GICD_CTLR        = 0,
					GICD_TYPER       = 0x4,
					GICD_ISENABLER0  = 0x100,
					GICD_ISENABLERL  = 0x17c,
					GICD_ICENABLER0  = 0x180,
					GICD_ICENABLERL  = 0x1fc,
					GICD_IPRIORITYR0 = 0x400,
					GICD_IPRIORITYRL = 0x7f8,
					GICD_ITARGETSR0  = 0x800,
					GICD_ITARGETSRL  = 0xbf8,
					GICD_ICFGR2      = 0xc08,
					GICD_ICFGRL      = 0xcfc,
				};

				enum Irqs {
					SGI_MAX = 15,
					TIMER   = Arm_v7::VT_TIMER_IRQ,
					MAX_IRQ = 256,
				};

				struct Irq {

					enum Cpu_state   { INACTIVE, PENDING };
					enum Distr_state { ENABLED, DISABLED };

					Cpu_state   cpu_state   = INACTIVE;
					Distr_state distr_state = DISABLED;
					Device    * device      = nullptr;
					bool        eoi         = false;
				};

				Irq  _irqs[MAX_IRQ+1];
				bool _distr_enabled = false;

				using Error = Vm::Exception;


				/**********************
				 **  GICH interface  **
				 **********************/

				struct Gich_lr : Genode::Register<32>
				{
					struct Virt_id : Bitfield<0,  10> { };
					struct Phys_id : Bitfield<10, 10> { };
					struct Prio    : Bitfield<23,  5> { };
					struct State   : Bitfield<28,  2> { };
					struct Hw      : Bitfield<31,  1> { };
				};

				void _handle_eoi()
				{
					if (!(_vm.state().gic_misr & 1)) return;

					for (unsigned i = 0; i < State::NR_IRQ; i++) {
						if (_vm.state().gic_eisr & (1 << i)) {
							unsigned irq = Gich_lr::Virt_id::get(_vm.state().gic_lr[i]);
							if (irq > MAX_IRQ)
								throw Error("IRQ out of bounds");
							_vm.state().gic_lr[i] = 0;
							_vm.state().gic_elrsr0 |= 1 << i;
							if (irq == TIMER &&
								_irqs[irq].distr_state == Irq::ENABLED)
								_vm.state().timer_irq = true;
							_irqs[irq].cpu_state = Irq::INACTIVE;
						}
					}

					_vm.state().gic_misr = 0;
				}

				void _inject_irq(unsigned irq, bool eoi)
				{
					if (irq == TIMER)
						_vm.state().timer_irq = false;

					for (unsigned i = 0; i < State::NR_IRQ; i++) {
						if (!(_vm.state().gic_elrsr0 & (1 << i))) {
							Gich_lr::access_t v = _vm.state().gic_lr[i];
							if (Gich_lr::Virt_id::get(v) == irq)
								return;
						}
					}

					for (unsigned i = 0; i < State::NR_IRQ; i++) {
						if (!(_vm.state().gic_elrsr0 & (1 << i)))
							continue;

						_vm.state().gic_elrsr0 &= ~(1 << i);
						Gich_lr::access_t v = 0;
						Gich_lr::Virt_id::set(v, irq);
						Gich_lr::Phys_id::set(v, eoi ? 1 << 9 : 0);
						Gich_lr::Prio::set(v, 0);
						Gich_lr::State::set(v, 0b1);
						_vm.state().gic_lr[i] = v;
						return;
					}

					throw Error("IRQ queue full, can't inject irq %u", irq);
				}

				void _enable_irq(unsigned irq)
				{
					if (irq > MAX_IRQ || !_irqs[irq].device)
						throw Error("GIC: can't enable unknown IRQ %d", irq);

					if (_irqs[irq].distr_state == Irq::ENABLED)
						return;

					_irqs[irq].distr_state = Irq::ENABLED;
					_irqs[irq].device->irq_enabled(irq);

					if (irq == TIMER)
						_vm.state().timer_irq = true;
				}

				void _disable_irq(unsigned irq)
				{
					if (irq > MAX_IRQ)
						throw Error("IRQ out of bounds");

					if (_irqs[irq].distr_state == Irq::DISABLED)
						return;

					_irqs[irq].distr_state = Irq::DISABLED;
					_irqs[irq].device->irq_disabled(irq);

					if (irq == TIMER)
						_vm.state().timer_irq = false;
				}

			public:

				Gic(const char * const       name,
				    const Genode::uint64_t   addr,
				    const Genode::uint64_t   size,
				    Vm &                     vm)
				: Device(name, addr, size, vm)
				{
					for (unsigned i = 0; i <= MAX_IRQ; i++) {
						_irqs[i] = Irq();
						if (i <= SGI_MAX)
							_irqs[i].device = this;
					}
				}

				void read (Genode::uint32_t * reg, Genode::uint64_t off)
				{
					if (off >= GICD_ICFGR2 && off <= GICD_ICFGRL) {
					    *reg = 0;
						return;
					}

					/* read enable registers */
					if (off >= GICD_ISENABLER0 && off <= GICD_ISENABLERL) {
						*reg = 0;
						Genode::addr_t idx = ((Genode::addr_t)off - GICD_ISENABLER0) * 8;
						for (unsigned i = 0; i < 32; i++) {
							if (_irqs[idx + i].distr_state == Irq::ENABLED)
								*reg |= 1 << i;
						}
						return;
					}

					if (off >= GICD_ITARGETSR0 && off <= GICD_ITARGETSRL) {
						*reg = 0x01010101;
						return;
					}

					switch (off) {
					case GICD_CTLR:
						*reg = _distr_enabled ? 1 : 0;
						return;
					case GICD_TYPER:
						*reg = 0b101;
						return;
					default:
						throw Error("GIC: unsupported read offset %llx", off);
					};
				}

				void write(Genode::uint32_t * reg, Genode::uint64_t off)
				{
					using namespace Genode;

					/* only allow cpu0 as target by now */
					if (off >= GICD_ITARGETSR0 && off <= GICD_ITARGETSRL &&
					    *reg == 0x01010101)
						return;

					/* only allow level triggered && active low */
					if (off >= GICD_ICFGR2 && off <= GICD_ICFGRL &&
					    *reg == 0)
						return;

					/* ignore priority settings */
					if (off >= GICD_IPRIORITYR0 && off <= GICD_IPRIORITYRL)
						return;

					/* set enable registers */
					if (off >= GICD_ISENABLER0 && off <= GICD_ISENABLERL) {
						addr_t idx = ((addr_t)off - GICD_ISENABLER0) * 8;
						for (unsigned i = 0; i < 32; i++)
							if (((*reg >> i) & 1))
								_enable_irq(idx+i);
						return;
					}

					/* clear enable registers */
					if (off >= GICD_ICENABLER0 && off <= GICD_ICENABLERL) {
						addr_t idx = ((addr_t)off - GICD_ICENABLER0) * 8;
						for (unsigned i = 0; i < 32; i++)
							if (((*reg >> i) & 1))
								_disable_irq(idx+i);
						return;
					}

					switch (off) {
					case GICD_CTLR:
						_distr_enabled = (*reg & 0b1);
						return;
					default:
						throw Error("GIC: unsupported write offset %llx", off);
					};
				}

				void register_irq(unsigned irq, Device * d, bool eoi)
				{
					_irqs[irq].device = d;
					_irqs[irq].eoi    = eoi;
				}

				void inject_irq(unsigned irq)
				{
					if (!_irqs[irq].device)
						throw Error("No device registered for IRQ %u", irq);

					if (_irqs[irq].cpu_state == Irq::PENDING)
						throw Error("Pending IRQ should not trigger again");;

					if (_irqs[irq].eoi)
						_irqs[irq].cpu_state = Irq::PENDING;

					if (_irqs[irq].distr_state == Irq::DISABLED) {
						Genode::warning("disabled irq ", irq, " injected");
						return;
					}

					_inject_irq(irq, _irqs[irq].eoi);
					_vm.interrupt();
				}

				void irq_occured()
				{
					switch(_vm.state().gic_irq) {
					case Arm_v7::VT_MAINTAINANCE_IRQ:
						_handle_eoi();
						return;
					case TIMER:
						inject_irq(TIMER);
						return;
					default:
						throw Error("Unknown IRQ %u occured",
						            _vm.state().gic_irq);
					};
				}
		};


		class Generic_timer : public Device
		{
			private:

				Timer::Connection             _timer;
				Signal_handler<Generic_timer> _handler;
				Gic                           &_gic;

				void _timeout()
				{
					_vm.state().timer_ctrl = 5;
					_vm.state().timer_val  = 0xffffffff;
					_gic.inject_irq(Arm_v7::VT_TIMER_IRQ);
				}

			public:

				Generic_timer(const char * const       name,
				              const Genode::uint64_t   addr,
				              const Genode::uint64_t   size,
				              Vmm                     &vmm,
				              Genode::Env             &env,
				              Gic                     &gic)
				: Device(name, addr, size, vmm.vm()),
				  _timer(env),
				  _handler(vmm, env.ep(), *this, &Generic_timer::_timeout),
				  _gic(gic)
				{
					_timer.sigh(_handler);
					_gic.register_irq(Arm_v7::VT_TIMER_IRQ, this, true);
				}

				void schedule_timeout()
				{
					if ((_vm.state().timer_ctrl & 0b101) != 0b101)
						_timer.trigger_once(_vm.state().timer_val / 24);
				}
		};


		class System_register : public Device
		{
			private:

				enum {
					SYS_LED     = 0x8,
					SYS_FLASH   = 0x4c,
					SYS_24MHZ   = 0x5c,
					SYS_MCI     = 0x48,
					SYS_MISC    = 0x60,
					SYS_PROCID0 = 0x84,
					SYS_CFGDATA = 0xa0,
					SYS_CFGCTRL = 0xa4,
					SYS_CFGSTAT = 0xa8,
				};

				struct Sys_cfgctrl : Genode::Register<32>
				{
					struct Device   : Bitfield<0,12> {};
					struct Position : Bitfield<12,4> {};
					struct Site     : Bitfield<16,2> {};
					struct Function : Bitfield<20,6> {};
					struct Write    : Bitfield<30,1> {};
					struct Start    : Bitfield<31,1> {};
				};

				Timer::Connection _timer;
				Genode::uint32_t  _spi_data = 0;
				Genode::uint32_t  _spi_stat = 1;

				using Error = Vm::Exception;

				void _mcc_control(unsigned device, unsigned func, bool write)
				{
					if (func == 1 && !write) {
						switch (device) {
						case 0: /* OSCCLK0 */
							_spi_data = 60000000;
							return;
						case 2: /* OSCCLK2 */
							_spi_data = 24000000;
							return;
						case 4: /* OSCCLK4 */
							_spi_data = 40000000;
							return;
						case 5: /* OSCCLK5 */
							_spi_data = 23750000;
							return;
						case 6: /* OSCCLK6 */
							_spi_data = 50000000;
							return;
						case 7: /* OSCCLK7 */
							_spi_data = 60000000;
							return;
						case 8: /* OSCCLK8 */
							_spi_data = 40000000;
							return;
						default:
							throw Error("Sys regs: unsupported MCC device");
						};
					}

					if (func == 2 && !write) {
						switch (device) {
						case 0: /* VOLT0 */
							_spi_data = 900000;
							return;
						default: ;
						};
					}

					throw Error("Unknown device %u func=%u write=%d",
					            device, func, write);
				};

			public:

				System_register(const char * const       name,
				                const Genode::uint64_t   addr,
				                const Genode::uint64_t   size,
				                Vm &                     vm,
				                Genode::Env &            env)
				: Device(name, addr, size, vm), _timer(env) {}

				void read(Genode::uint32_t * reg, Genode::uint64_t off)
				{
					switch (off) {
					case SYS_LED:
						*reg = 0xff;
						return;
					case SYS_FLASH:
						*reg = 0;
						return;
					case SYS_24MHZ: /* 24 MHz counter */
						*reg = (Genode::uint32_t)_timer.elapsed_ms() * 24000;
						return;
					case SYS_MISC:
						*reg = 1 << 12;
						return;
					case SYS_PROCID0:
						*reg = 0x14000237; /* daughterboard ID */
						return;
					case SYS_MCI:
						*reg = 0; /* no mmc inside */
						return;
					case SYS_CFGSTAT:
						*reg = _spi_stat;
						return;
					case SYS_CFGCTRL:
						*reg = 0;
						return;
					case SYS_CFGDATA:
						*reg = _spi_data;
						return;
					};
					throw Error("Sys regs: read of offset %llx forbidden", off);
				}

				void write(Genode::uint32_t * reg, Genode::uint64_t off)
				{
					switch (off) {
					case SYS_CFGDATA:
						_spi_data = *reg;
						return;
					case SYS_CFGSTAT:
						_spi_stat = *reg;
						return;
					case SYS_CFGCTRL:
						if (Sys_cfgctrl::Start::get(*reg)) {
							_spi_stat = 1;
							_mcc_control(Sys_cfgctrl::Device::get(*reg),
							             Sys_cfgctrl::Function::get(*reg),
							             Sys_cfgctrl::Write::get(*reg));
							return;
						}
					case SYS_24MHZ:
					case SYS_MISC:
					case SYS_PROCID0:
					case SYS_MCI:
						;
					};
					throw Error("Sys regs: write of offset %llx forbidden", off);
				}
		};


		class Pl011 : public Device
		{
			private:

				using Board = Vea9x4::Board;
				using Ring_buffer = Genode::Ring_buffer<char, 1024,
				                                        Genode::Ring_buffer_unsynchronized>;

				class Wrong_offset {};

				enum {
					UARTDR        = 0x0,
					UARTFR        = 0x18,
					UARTIBRD      = 0x24,
					UARTFBRD      = 0x28,
					UARTLCR_H     = 0x2c,
					UARTCR        = 0x30,
					UARTIFLS      = 0x34,
					UARTIMSC      = 0x38,
					UARTMIS       = 0x40,
					UARTICR       = 0x44,
					UARTPERIPHID0 = 0xfe0,
					UARTPERIPHID1 = 0xfe4,
					UARTPERIPHID2 = 0xfe8,
					UARTPERIPHID3 = 0xfec,
					UARTPCELLID0  = 0xff0,
					UARTPCELLID1  = 0xff4,
					UARTPCELLID2  = 0xff8,
					UARTPCELLID3  = 0xffc,
				};

				Terminal::Connection     _terminal;
				Signal_handler<Pl011>    _handler;
				Gic                     &_gic;
				Ring_buffer              _rx_buf;
				Genode::uint16_t         _ibrd  = 0;
				Genode::uint16_t         _fbrd  = 0;
				Genode::uint16_t         _lcr_h = 0;
				Genode::uint16_t         _imsc  = 0b1111;
				Genode::uint16_t         _ris   = 0;
				Genode::uint16_t         _cr    = 0x300;

				void _out_char(unsigned char c) {
					_terminal.write(&c, 1);
				}

				unsigned char _get_char()
				{
					if (_rx_buf.empty()) return 0;
					return _rx_buf.get();
				}

				Genode::uint16_t _get(Genode::uint64_t off)
				{
					switch (off) {
					case UARTDR:        return _get_char();
					case UARTPERIPHID0: return 0x11;
					case UARTPERIPHID1: return 0x10;
					case UARTPERIPHID2: return 0x14;
					case UARTPERIPHID3: return 0x0;
					case UARTPCELLID0:  return 0xd;
					case UARTPCELLID1:  return 0xf0;
					case UARTPCELLID2:  return 0x5;
					case UARTPCELLID3:  return 0xb1;
					case UARTFR:        return _rx_buf.empty() ? 16 : 64;
					case UARTCR:        return _cr;
					case UARTIMSC:      return _imsc;
					case UARTMIS:       return _ris & _imsc;
					case UARTFBRD:      return _fbrd;
					case UARTIBRD:      return _ibrd;
					case UARTLCR_H:     return _lcr_h;
					default:
						throw Wrong_offset();
					};
				}

				void _mask_irqs(Genode::uint32_t mask)
				{
					/* TX IRQ unmask */
					if (mask & (1 << 5) && !(_imsc & (1 << 5))) {
						_gic.inject_irq(Board::PL011_0_IRQ);
						_ris |= 1 << 5;
					}

					/* RX IRQ unmask */
					if (mask & (1 << 4) && !(_imsc & (1 << 4)) &&
						!_rx_buf.empty()) {
						_gic.inject_irq(Board::PL011_0_IRQ);
						_ris |= 1 << 4;
					}

					_imsc = mask;
				}

				void _read()
				{
					if (!_terminal.avail()) return;

					while (_terminal.avail()) {
						unsigned char c = 0;
						_terminal.read(&c, 1);
						_rx_buf.add(c);
					}

					_gic.inject_irq(Board::PL011_0_IRQ);
					_ris |= 1 << 4;
				}

				using Error = Vm::Exception;

			public:

				Pl011(const char * const       name,
				      const Genode::uint64_t   addr,
				      const Genode::uint64_t   size,
				      Vmm                     &vmm,
				      Genode::Env             &env,
				      Gic                     &gic)
				: Device(name, addr, size, vmm.vm()),
				  _terminal(env),
				  _handler(vmm, env.ep(), *this, &Pl011::_read),
				  _gic(gic) {
					_terminal.read_avail_sigh(_handler);
					_gic.register_irq(Board::PL011_0_IRQ, this, false);
				}

				void read(Genode::uint16_t * reg, Genode::uint64_t off)
				{
					try {
						*reg = _get(off);
					} catch(Wrong_offset &e) {
						throw Error("UART: halfword read of offset %llx", off);
					}
				}

				void read(Genode::uint32_t * reg, Genode::uint64_t off) {
					read((Genode::uint16_t*) reg, off); }

				void write(Genode::uint8_t * reg, Genode::uint64_t off)
				{
					if (off != UARTDR)
						throw Error("UART: byte write %x to offset %llx",
						            *reg, off);
					_terminal.write(reg, 1);
				}

				void write(Genode::uint16_t * reg, Genode::uint64_t off)
				{
					switch (off) {
					case UARTDR:
						_terminal.write(reg, 1);
						return;
					case UARTFBRD:
						_fbrd = *reg;
						return;
					case UARTIMSC:
						_mask_irqs(*reg);
						return;
					case UARTIBRD:
						_ibrd = *reg;
						return;
					case UARTLCR_H:
						_lcr_h = *reg;
						return;
					case UARTICR:
						_ris = _ris & ~*reg;
						return;
					case UARTCR:
						_cr = *reg;
						return;
					case UARTIFLS:
						return;
					default:
						throw Error("UART: halfword write %x to offset %llx",
						            *reg, off);
					};
				}
		};


		Signal_handler<Vmm>            _vm_handler;
		Vm                             _vm;
		Cp15                           _cp15;
		Genode::Avl_tree<Device>       _device_tree;
		Gic                            _gic;
		Generic_timer                  _timer;
		System_register                _sys_regs;
		Pl011                          _uart;

		void _handle_hyper_call() {
			throw Vm::Exception("Unknown hyper call!"); }

		void _handle_data_abort()
		{
			Genode::uint64_t ipa = (Genode::uint64_t)_vm.state().hpfar << 8;
			Device * device = _device_tree.first()
				? _device_tree.first()->find_by_addr(ipa) : nullptr;
			if (!device)
				throw Vm::Exception("No device at IPA=%llx", ipa);
			device->handle_memory_access(_vm.state());
			_vm.state().ip += sizeof(Genode::addr_t);
		}

		void _handle_wfi()
		{
			if (_vm.state().hsr & 1)
				throw Vm::Exception("WFE not implemented yet");

			_vm.wait_for_interrupt();
			_timer.schedule_timeout();
			_vm.state().ip += sizeof(Genode::addr_t);
		}

		void _handle_trap()
		{
			/* check device number*/
			switch (Hsr::Ec::get(_vm.state().hsr)) {
			case Hsr::Ec::HVC:
				_handle_hyper_call();
				break;
			case Hsr::Ec::CP15:
				_cp15.handle_trap(_vm.state());
				break;
			case Hsr::Ec::DA:
				_handle_data_abort();
				break;
			case Hsr::Ec::WFI:
				_handle_wfi();
				return;
			default:
				throw Vm::Exception("Unknown trap: %x",
				                    Hsr::Ec::get(_vm.state().hsr));
			};
		}

		void _handle() {} /* dummy handler */

	public:

		Vmm(Genode::Env & env)
		: _vm_handler(*this, env.ep(), *this, &Vmm::_handle),
		  _vm("linux", "dtb", 1024 * 1024 * 128, _vm_handler, env),
		  _cp15(_vm.state()),
		  _gic      ("Gic",             0x2c001000, 0x2000, _vm),
		  _timer    ("Timer",           0x2a430000, 0x1000, *this, env, _gic),
		  _sys_regs ("System Register", 0x1c010000, 0x1000, _vm, env),
		  _uart     ("Pl011",           0x1c090000, 0x1000, *this, env, _gic)
		{
			_device_tree.insert(&_gic);
			_device_tree.insert(&_sys_regs);
			_device_tree.insert(&_uart);

			Genode::log("Start virtual machine ...");

			_vm.start();
			_vm.run();
		};

		Vm & vm() { return _vm; }

		template <typename FUNC>
		void handle_vm(FUNC handler)
		{
			if (_vm.active()) {

				_vm.pause();

				enum { IRQ = 6, TRAP = 8 };

				/* check exception reason */
				switch (_vm.state().cpu_exception) {
				case IRQ:
					_gic.irq_occured();
					break;
				case TRAP:
					_handle_trap();
					break;
				default:
					throw Vm::Exception("Curious exception occured");
				}
			}

			handler();

			if (_vm.active()) _vm.run();
		}
};


void Component::construct(Genode::Env & env) { static Vmm vmm(env); }
