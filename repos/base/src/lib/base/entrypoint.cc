/*
 * \brief  Entrypoint for serving RPC requests and dispatching signals
 * \author Norman Feske
 * \author Christian Helmuth
 * \date   2015-12-17
 */

/*
 * Copyright (C) 2015-2019 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

/* Genode includes */
#include <base/entrypoint.h>
#include <base/component.h>
#include <cpu/atomic.h>
#include <util/retry.h>
#include <base/rpc_client.h>

/* base-internal includes */
#include <base/internal/globals.h>

using namespace Genode;

/*
 * XXX move declarations to base-internal headers
 */
namespace Genode {

	extern bool inhibit_tracing;
	void call_global_static_constructors();
	void destroy_signal_thread();
}


/**
 * Return thread name used for the component's initial entrypoint
 */
static char const *initial_ep_name() { return "ep"; }


void Entrypoint::Signal_proxy_component::signal()
{
	ep._process_deferred_signals();

	bool io_progress = false;

	/*
	 * Try to dispatch one pending signal picked-up by the signal-proxy thread.
	 * Note, we handle only one signal here to ensure fairness between RPCs and
	 * signals.
	 */
	try {
		Signal sig = ep._sig_rec->pending_signal();
		ep._dispatch_signal(sig);

		if (sig.context()->level() == Signal_context::Level::Io) {
			/* trigger the progress handler */
			io_progress = true;
		}
	} catch (Signal_receiver::Signal_not_pending) { }

	if (io_progress)
		ep._handle_io_progress();
}


void Entrypoint::_dispatch_signal(Signal &sig)
{
	Signal_dispatcher_base *dispatcher = 0;
	dispatcher = dynamic_cast<Signal_dispatcher_base *>(sig.context());

	if (!dispatcher)
		return;

	dispatcher->dispatch(sig.num());
}


void Entrypoint::_defer_signal(Signal &sig)
{
	Signal_context *context = sig.context();

	Lock::Guard guard(_deferred_signals_mutex);
	_deferred_signals.remove(context->deferred_le());
	_deferred_signals.insert(context->deferred_le());
}


void Entrypoint::_process_deferred_signals()
{
	for (;;) {
		Signal_context *context = nullptr;
		{
			Lock::Guard guard(_deferred_signals_mutex);
			if (!_deferred_signals.first()) return;

			context = _deferred_signals.first()->object();
			_deferred_signals.remove(_deferred_signals.first());
		}

		Signal_dispatcher_base *dispatcher =
			dynamic_cast<Signal_dispatcher_base *>(context);
		if (dispatcher) dispatcher->dispatch(1);
	}
}


void Entrypoint::_process_incoming_signals()
{
	for (;;) {

		do {
			_sig_rec->block_for_signal();

			int success;
			{
				Lock::Guard guard(_signal_pending_lock);
				success = cmpxchg(&_signal_recipient, NONE, SIGNAL_PROXY);
			}

			/* common case, entrypoint is not in 'wait_and_dispatch_one_io_signal' */
			if (success) {
				/*
				 * It might happen that we try to forward a signal to the
				 * entrypoint, while the context of that signal is already
				 * destroyed. In that case we will get an ipc error exception
				 * as result, which has to be caught.
				 */
				try {
					retry<Blocking_canceled>(
						[&] () { _signal_proxy_cap.call<Signal_proxy::Rpc_signal>(); },
						[]  () { warning("blocking canceled during signal processing"); });
				} catch (Genode::Ipc_error) { /* ignore - context got destroyed in meantime */ }

				cmpxchg(&_signal_recipient, SIGNAL_PROXY, NONE);
			} else {
				/*
				 * Entrypoint is in 'wait_and_dispatch_one_io_signal', wakup it up and
				 * block for next signal
				 */
				_sig_rec->unblock_signal_waiter(*_rpc_ep);

				/*
				 * wait for the acknowledgment by the entrypoint
				 */
				_signal_pending_ack_lock.lock();
			}

			/* entrypoint destructor requested to stop signal handling */
			if (_stop_signal_proxy) {
				 return;
			}
		} while (!_suspended);

		_deferred_signal_handler.destruct();
		_suspend_dispatcher.destruct();
		_sig_rec.destruct();
		dissolve(_signal_proxy);
		deinit_heartbeat_monitoring();
		_signal_proxy_cap = Capability<Signal_proxy>();
		_rpc_ep.destruct();
		destroy_signal_thread();

		/* execute fork magic in noux plugin */
		_suspended_callback();

		init_signal_thread(_env);

		_rpc_ep.construct(&_env.pd(), Component::stack_size(), initial_ep_name());
		init_heartbeat_monitoring(_env);
		_signal_proxy_cap = manage(_signal_proxy);
		_sig_rec.construct();

		/*
		 * Before calling the resumed callback, we reset the callback pointer
		 * as these may be set again in the resumed code to initiate the next
		 * suspend-resume cycle (e.g., exit()).
		 */
		void (*resumed_callback)() = _resumed_callback;
		_suspended_callback        = nullptr;
		_resumed_callback          = nullptr;
		_suspended                 = false;

		resumed_callback();
	}
}


bool Entrypoint::_wait_and_dispatch_one_io_signal(bool const dont_block)
{
	if (!_rpc_ep->is_myself())
		warning(__func__, " called from non-entrypoint thread \"",
		       Thread::myself()->name(), "\"");

	for (;;) {

		try {
			_signal_pending_lock.lock();

			cmpxchg(&_signal_recipient, NONE, ENTRYPOINT);
			Signal sig =_sig_rec->pending_signal();
			cmpxchg(&_signal_recipient, ENTRYPOINT, NONE);

			_signal_pending_lock.unlock();

			_signal_pending_ack_lock.unlock();

			/* defer application-level signals */
			if (sig.context()->level() == Signal_context::Level::App) {
				_defer_signal(sig);
				continue;
			}

			_dispatch_signal(sig);
			break;

		} catch (Signal_receiver::Signal_not_pending) {
			_signal_pending_lock.unlock();
			if (dont_block) {
				/* indicate that we leave wait_and_dispatch_one_io_signal */
				cmpxchg(&_signal_recipient, ENTRYPOINT, NONE);
				return false;
			}
			_sig_rec->block_for_signal();
		}
	}

	_handle_io_progress();

	/* initiate potential deferred-signal handling in entrypoint */
	if (_deferred_signals.first()) {
		/* construct the handler on demand (otherwise we break core) */
		if (!_deferred_signal_handler.constructed())
			_deferred_signal_handler.construct(*this, *this,
			                                   &Entrypoint::_handle_deferred_signals);
		Signal_transmitter(*_deferred_signal_handler).submit();
	}

	return true;
}


void Entrypoint::schedule_suspend(void (*suspended)(), void (*resumed)())
{
	_suspended_callback = suspended;
	_resumed_callback   = resumed;

	/*
	 * We always construct the dispatcher when the suspend is scheduled and
	 * destruct it when the suspend is executed.
	 */
	_suspend_dispatcher.construct(*this, *this, &Entrypoint::_handle_suspend);

	/* trigger wakeup of the signal-dispatch loop for suspend */
	Signal_transmitter(*_suspend_dispatcher).submit();
}


Signal_context_capability Entrypoint::manage(Signal_dispatcher_base &dispatcher)
{
	/* _sig_rec is invalid for a small window in _process_incoming_signals */
	return _sig_rec.constructed() ? _sig_rec->manage(&dispatcher)
	                              : Signal_context_capability();
}


void Genode::Entrypoint::dissolve(Signal_dispatcher_base &dispatcher)
{
	/* _sig_rec is invalid for a small window in _process_incoming_signals */
	if (_sig_rec.constructed())
		_sig_rec->dissolve(&dispatcher);

	/* also remove context from deferred signal list */
	{
		Lock::Guard guard(_deferred_signals_mutex);
		_deferred_signals.remove(dispatcher.deferred_le());
	}
}


namespace {

	struct Constructor : Interface
	{
		GENODE_RPC(Rpc_construct, void, construct);
		GENODE_RPC_INTERFACE(Rpc_construct);
	};

	struct Constructor_component : Rpc_object<Constructor, Constructor_component>
	{
		Env &env;
		Constructor_component(Env &env) : env(env) { }

		void construct()
		{
			/* enable tracing support */
			Genode::inhibit_tracing = false;

			Genode::call_global_static_constructors();
			Genode::init_signal_transmitter(env);
			Genode::init_tracing(env);

			/*
			 * Now, as signaling is available, initialize the asynchronous
			 * parent resource mechanism
			 */
			init_parent_resource_requests(env);

			init_heartbeat_monitoring(env);

			Component::construct(env);
		}
	};

	void invoke_constructor_at_entrypoint(Capability<Constructor> cap)
	{
		cap.call<Constructor::Rpc_construct>();
	}
}


Entrypoint::Entrypoint(Env &env)
:
	_env(env),
	_rpc_ep(&env.pd(), Component::stack_size(), initial_ep_name()),

	/* initialize signalling before creating the first signal receiver */
	_signalling_initialized((init_signal_thread(env), true))
{
	/* initialize emulation of the original synchronous root interface */
	init_root_proxy(_env);

	/*
	 * Invoke Component::construct function in the context of the entrypoint.
	 */
	Constructor_component constructor(env);

	_env.ep().manage(constructor);

	try {
		invoke_constructor_at_entrypoint(constructor.cap());
	} catch (Blocking_canceled) {
		warning("blocking canceled in entrypoint constructor");
	}

	_env.ep().dissolve(constructor);

	/*
	 * The calling initial thread becomes the signal proxy thread for this
	 * entrypoint
	 */
	_process_incoming_signals();
}


Entrypoint::Entrypoint(Env &env, size_t stack_size, char const *name,
                       Affinity::Location location)
:
	_env(env),
	_rpc_ep(&env.pd(), stack_size, name, true, location),
	_signalling_initialized(true)
{
	_signal_proxy_thread.construct(env, *this, location,
	                               Thread::Weight(), env.cpu());
}

Entrypoint::~Entrypoint()
{
	/* stop the signal proxy before destruction */
	_stop_signal_proxy_handler.construct(
		*this, *this, &Entrypoint::_handle_stop_signal_proxy);
	Signal_transmitter(*_stop_signal_proxy_handler).submit();
	_signal_proxy_thread->join();
	_stop_signal_proxy_handler.destruct();

	_rpc_ep->dissolve(&_signal_proxy);
}
