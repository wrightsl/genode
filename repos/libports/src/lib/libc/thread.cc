/*
 * \brief  POSIX thread implementation
 * \author Christian Prochaska
 * \date   2012-03-12
 *
 */

/*
 * Copyright (C) 2012-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#include <base/log.h>
#include <base/sleep.h>
#include <base/thread.h>
#include <util/list.h>
#include <libc/allocator.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h> /* malloc, free */
#include "thread.h"
#include "task.h"
#include "timed_semaphore.h"
#include "libc_init.h"

using namespace Genode;


static Libc::Allocator object_alloc;


static Env *_env_ptr;  /* solely needed to spawn the timeout thread for the
                          timed semaphore */


void Libc::init_pthread_support(Genode::Env &env) { _env_ptr = &env; }


static Libc::Timeout_entrypoint &_global_timeout_ep()
{
	class Missing_call_of_init_pthread_support { };
	if (!_env_ptr)
		throw Missing_call_of_init_pthread_support();

	static Libc::Timeout_entrypoint timeout_ep { *_env_ptr };
	return timeout_ep;
}


/*
 * We initialize the main-thread pointer in a constructor depending on the
 * assumption that libpthread is loaded on application startup by ldso. During
 * this stage only the main thread is executed.
 */
static __attribute__((constructor)) Thread * main_thread()
{
	static Thread *thread = Thread::myself();

	return thread;
}


/*
 * pthread
 */
void pthread::Thread_object::entry()
{
	/* obtain stack attributes of new thread */
	Thread::Stack_info info = Thread::mystack();
	_stack_addr = (void *)info.base;
	_stack_size = info.top - info.base;

	pthread_exit(_start_routine(_arg));
}


void pthread::join(void **retval)
{
	struct Check : Libc::Suspend_functor
	{
		bool retry { false };

		pthread &_thread;

		Check(pthread &thread) : _thread(thread) { }
		
		bool suspend() override
		{
			retry = !_thread._exiting;
			return retry;
		}
	} check(*this);

	do {
		Libc::suspend(check);
	} while (check.retry);

	_join_lock.lock();

	if (retval)
		*retval = _retval;
}


void pthread::cancel()
{
	_exiting = true;
	Libc::resume_all();
	_join_lock.unlock();
}


/*
 * Registry
 */

void Pthread_registry::insert(pthread_t thread)
{
	/* prevent multiple insertions at the same location */
	static Genode::Lock insert_lock;
	Genode::Lock::Guard insert_lock_guard(insert_lock);

	for (unsigned int i = 0; i < MAX_NUM_PTHREADS; i++) {
		if (_array[i] == 0) {
			_array[i] = thread;
			return;
		}
	}

	Genode::error("pthread registry overflow, pthread_self() might fail");
}


void Pthread_registry::remove(pthread_t thread)
{
	for (unsigned int i = 0; i < MAX_NUM_PTHREADS; i++) {
		if (_array[i] == thread) {
			_array[i] = 0;
			return;
		}
	}

	Genode::error("could not remove unknown pthread from registry");
}


bool Pthread_registry::contains(pthread_t thread)
{
	for (unsigned int i = 0; i < MAX_NUM_PTHREADS; i++)
		if (_array[i] == thread)
			return true;

	return false;
}


Pthread_registry &pthread_registry()
{
	static Pthread_registry instance;
	return instance;
}


extern "C" {

	/* Thread */

	int pthread_join(pthread_t thread, void **retval)
	{
		thread->join(retval);

		destroy(object_alloc, thread);

		return 0;
	}


	int pthread_attr_init(pthread_attr_t *attr)
	{
		if (!attr)
			return EINVAL;

		*attr = new (object_alloc) pthread_attr;

		return 0;
	}


	int pthread_attr_destroy(pthread_attr_t *attr)
	{
		if (!attr || !*attr)
			return EINVAL;

		destroy(object_alloc, *attr);
		*attr = 0;

		return 0;
	}


	int pthread_cancel(pthread_t thread)
	{
		thread->cancel();
		return 0;
	}


	void pthread_exit(void *value_ptr)
	{
		pthread_self()->exit(value_ptr);
		Genode::sleep_forever();
	}


	/* special non-POSIX function (for example used in libresolv) */
	int _pthread_main_np(void)
	{
		return (Thread::myself() == main_thread());
	}


	pthread_t pthread_self(void)
	{
		try {
			pthread_t pthread_myself =
				static_cast<pthread_t>(&Thread::Tls::Base::tls());

			if (pthread_registry().contains(pthread_myself))
				return pthread_myself;
		}
		catch (Thread::Tls::Base::Undefined) { }

		/*
		 * We pass here if the main thread or an alien thread calls
		 * pthread_self(). So check for aliens (or other bugs) and opt-out
		 * early.
		 */
		if (!_pthread_main_np()) {
			error("pthread_self() called from alien thread named ",
			      "'", Thread::myself()->name().string(), "'");
			return nullptr;
		}

		/*
		 * We create a pthread object associated to the main thread's Thread
		 * object. We ensure the pthread object does never get deleted by
		 * allocating it in heap via new(). Otherwise, the static destruction
		 * of the pthread object would also destruct the 'Thread' of the main
		 * thread.
		 */
		static pthread *main = new (object_alloc) pthread(*Thread::myself());
		return main;
	}


	pthread_t thr_self(void) { return pthread_self(); }

	__attribute__((alias("thr_self")))
	pthread_t __sys_thr_self(void);


	int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
	{
		if (!attr || !*attr)
			return EINVAL;

		if (stacksize < 4096)
			return EINVAL;

		size_t max_stack = Thread::stack_virtual_size() - 4 * 4096;
		if (stacksize > max_stack) {
			warning(__func__, ": requested stack size is ", stacksize, " limiting to ", max_stack);
			stacksize = max_stack;
		}

		(*attr)->stack_size = Genode::align_addr(stacksize, 12);

		return 0;
	}


	int pthread_attr_getstack(const pthread_attr_t *attr,
	                          void **stackaddr,
	                          ::size_t *stacksize)
	{
		if (!attr || !*attr || !stackaddr || !stacksize)
			return EINVAL;

		*stackaddr = (*attr)->stack_addr;
		*stacksize = (*attr)->stack_size;

		return 0;
	}


	int pthread_attr_getstackaddr(const pthread_attr_t *attr, void **stackaddr)
	{
		size_t stacksize;
		return pthread_attr_getstack(attr, stackaddr, &stacksize);
	}


	int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
	{
		void *stackaddr;
		return pthread_attr_getstack(attr, &stackaddr, stacksize);
	}


	int pthread_attr_get_np(pthread_t pthread, pthread_attr_t *attr)
	{
		if (!attr)
			return EINVAL;

		(*attr)->stack_addr = pthread->stack_addr();
		(*attr)->stack_size = pthread->stack_size();

		return 0;
	}


	int pthread_equal(pthread_t t1, pthread_t t2)
	{
		return (t1 == t2);
	}


	/* Mutex */


	struct pthread_mutex_attr
	{
		int type;

		pthread_mutex_attr() : type(PTHREAD_MUTEX_NORMAL) { }
	};


	struct pthread_mutex
	{
		pthread_mutex_attr mutexattr;

		Lock mutex_lock;

		pthread_t owner;
		int       lock_count;
		Lock      owner_and_counter_lock;

		pthread_mutex(const pthread_mutexattr_t *__restrict attr)
		: owner(0),
		  lock_count(0)
		{
			if (attr && *attr)
				mutexattr = **attr;
		}

		int lock()
		{
			if (mutexattr.type == PTHREAD_MUTEX_RECURSIVE) {

				Lock::Guard lock_guard(owner_and_counter_lock);

				if (lock_count == 0) {
					owner = pthread_self();
					lock_count++;
					mutex_lock.lock();
					return 0;
				}

				/* the mutex is already locked */
				if (pthread_self() == owner) {
					lock_count++;
					return 0;
				} else {
					mutex_lock.lock();
					return 0;
				}
			}

			if (mutexattr.type == PTHREAD_MUTEX_ERRORCHECK) {

				Lock::Guard lock_guard(owner_and_counter_lock);

				if (lock_count == 0) {
					owner = pthread_self();
					mutex_lock.lock();
					return 0;
				}

				/* the mutex is already locked */
				if (pthread_self() != owner) {
					mutex_lock.lock();
					return 0;
				} else
					return EDEADLK;
			}

			/* PTHREAD_MUTEX_NORMAL or PTHREAD_MUTEX_DEFAULT */
			mutex_lock.lock();
			return 0;
		}

		int trylock()
		{
			if (mutexattr.type == PTHREAD_MUTEX_RECURSIVE) {

				Lock::Guard lock_guard(owner_and_counter_lock);

				if (lock_count == 0) {
					owner = pthread_self();
					lock_count++;
					mutex_lock.lock();
					return 0;
				}

				/* the mutex is already locked */
				if (pthread_self() == owner) {
					lock_count++;
					return 0;
				} else {
					return EBUSY;
				}
			}

			if (mutexattr.type == PTHREAD_MUTEX_ERRORCHECK) {

				Lock::Guard lock_guard(owner_and_counter_lock);

				if (lock_count == 0) {
					owner = pthread_self();
					mutex_lock.lock();
					return 0;
				}

				/* the mutex is already locked */
				if (pthread_self() != owner) {
					return EBUSY;
				} else
					return EDEADLK;
			}

			/* PTHREAD_MUTEX_NORMAL or PTHREAD_MUTEX_DEFAULT */
			Lock::Guard lock_guard(owner_and_counter_lock);

			if (lock_count == 0) {
				owner = pthread_self();
				mutex_lock.lock();
				return 0;
			}

			return EBUSY;
		}

		int unlock()
		{

			if (mutexattr.type == PTHREAD_MUTEX_RECURSIVE) {

				Lock::Guard lock_guard(owner_and_counter_lock);

				if (pthread_self() != owner)
					return EPERM;

				lock_count--;

				if (lock_count == 0) {
					owner = 0;
					mutex_lock.unlock();
				}

				return 0;
			}

			if (mutexattr.type == PTHREAD_MUTEX_ERRORCHECK) {

				Lock::Guard lock_guard(owner_and_counter_lock);

				if (pthread_self() != owner)
					return EPERM;

				owner = 0;
				mutex_lock.unlock();
				return 0;
			}

			/* PTHREAD_MUTEX_NORMAL or PTHREAD_MUTEX_DEFAULT */
			mutex_lock.unlock();
			return 0;
		}
	};


	int pthread_mutexattr_init(pthread_mutexattr_t *attr)
	{
		if (!attr)
			return EINVAL;

		*attr = new (object_alloc) pthread_mutex_attr;

		return 0;
	}


	int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
	{
		if (!attr || !*attr)
			return EINVAL;

		destroy(object_alloc, *attr);
		*attr = 0;

		return 0;
	}


	int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
	{
		if (!attr || !*attr)
			return EINVAL;

		(*attr)->type = type;

		return 0;
	}


	int pthread_mutex_init(pthread_mutex_t *__restrict mutex,
	                       const pthread_mutexattr_t *__restrict attr)
	{
		if (!mutex)
			return EINVAL;

		*mutex = new (object_alloc) pthread_mutex(attr);

		return 0;
	}


	int pthread_mutex_destroy(pthread_mutex_t *mutex)
	{
		if ((!mutex) || (*mutex == PTHREAD_MUTEX_INITIALIZER))
			return EINVAL;

		destroy(object_alloc, *mutex);
		*mutex = PTHREAD_MUTEX_INITIALIZER;

		return 0;
	}


	int pthread_mutex_lock(pthread_mutex_t *mutex)
	{
		if (!mutex)
			return EINVAL;

		if (*mutex == PTHREAD_MUTEX_INITIALIZER)
			pthread_mutex_init(mutex, 0);

		(*mutex)->lock();

		return 0;
	}


	int pthread_mutex_trylock(pthread_mutex_t *mutex)
	{
		if (!mutex)
			return EINVAL;

		if (*mutex == PTHREAD_MUTEX_INITIALIZER)
			pthread_mutex_init(mutex, 0);

		return (*mutex)->trylock();
	}


	int pthread_mutex_unlock(pthread_mutex_t *mutex)
	{
		if (!mutex)
			return EINVAL;

		if (*mutex == PTHREAD_MUTEX_INITIALIZER)
			pthread_mutex_init(mutex, 0);

		(*mutex)->unlock();

		return 0;
	}


	/* Condition variable */


	/*
	 * Implementation based on
	 * http://web.archive.org/web/20010914175514/http://www-classic.be.com/aboutbe/benewsletter/volume_III/Issue40.html#Workshop
	 */

	struct pthread_cond
	{
		int num_waiters;
		int num_signallers;
		Lock counter_lock;
		Libc::Timed_semaphore signal_sem { _global_timeout_ep() };
		Semaphore handshake_sem;

		pthread_cond() : num_waiters(0), num_signallers(0) { }
	};


	int pthread_condattr_init(pthread_condattr_t *attr)
	{
		if (!attr)
			return EINVAL;

		*attr = nullptr;

		return 0;
	}


	int pthread_condattr_destroy(pthread_condattr_t *attr)
	{
		/* assert that the attr was produced by the init no-op */
		if (!attr || *attr != nullptr)
			return EINVAL;

		return 0;
	}


	int pthread_condattr_setclock(pthread_condattr_t *attr,
	                              clockid_t clock_id)
	{
		/* assert that the attr was produced by the init no-op */
		if (!attr || *attr != nullptr)
			return EINVAL;

		warning(__func__, " not implemented yet");

		return 0;
	}


	static int cond_init(pthread_cond_t *__restrict cond,
	                     const pthread_condattr_t *__restrict attr)
	{
		static Genode::Lock cond_init_lock { };

		if (!cond)
			return EINVAL;

		try {
			Genode::Lock::Guard g(cond_init_lock);
			*cond = new (object_alloc) pthread_cond;
			return 0;
		} catch (...) { return ENOMEM; }
	}


	int pthread_cond_init(pthread_cond_t *__restrict cond,
	                      const pthread_condattr_t *__restrict attr)
	{
		return cond_init(cond, attr);
	}


	int pthread_cond_destroy(pthread_cond_t *cond)
	{
		if (!cond || !*cond)
			return EINVAL;

		destroy(object_alloc, *cond);
		*cond = 0;

		return 0;
	}


	static uint64_t timeout_ms(struct timespec currtime,
	                           struct timespec abstimeout)
	{
		enum { S_IN_MS = 1000, S_IN_NS = 1000 * 1000 * 1000 };

		if (currtime.tv_nsec >= S_IN_NS) {
			currtime.tv_sec  += currtime.tv_nsec / S_IN_NS;
			currtime.tv_nsec  = currtime.tv_nsec % S_IN_NS;
		}
		if (abstimeout.tv_nsec >= S_IN_NS) {
			abstimeout.tv_sec  += abstimeout.tv_nsec / S_IN_NS;
			abstimeout.tv_nsec  = abstimeout.tv_nsec % S_IN_NS;
		}

		/* check whether absolute timeout is in the past */
		if (currtime.tv_sec > abstimeout.tv_sec)
			return 0;

		uint64_t diff_ms = (abstimeout.tv_sec - currtime.tv_sec) * S_IN_MS;
		uint64_t diff_ns = 0;

		if (abstimeout.tv_nsec >= currtime.tv_nsec)
			diff_ns = abstimeout.tv_nsec - currtime.tv_nsec;
		else {
			/* check whether absolute timeout is in the past */
			if (diff_ms == 0)
				return 0;
			diff_ns  = S_IN_NS - currtime.tv_nsec + abstimeout.tv_nsec;
			diff_ms -= S_IN_MS;
		}

		diff_ms += diff_ns / 1000 / 1000;

		/* if there is any diff then let the timeout be at least 1 MS */
		if (diff_ms == 0 && diff_ns != 0)
			return 1;

		return diff_ms;
	}


	int pthread_cond_timedwait(pthread_cond_t *__restrict cond,
	                           pthread_mutex_t *__restrict mutex,
	                           const struct timespec *__restrict abstime)
	{
		int result = 0;

		if (!cond)
			return EINVAL;

		if (*cond == PTHREAD_COND_INITIALIZER)
			cond_init(cond, NULL);

		pthread_cond *c = *cond;

		c->counter_lock.lock();
		c->num_waiters++;
		c->counter_lock.unlock();

		pthread_mutex_unlock(mutex);

		if (!abstime)
			c->signal_sem.down();
		else {
			struct timespec currtime;
			clock_gettime(CLOCK_REALTIME, &currtime);

			Alarm::Time timeout = timeout_ms(currtime, *abstime);

			try {
				c->signal_sem.down(timeout);
			} catch (Libc::Timeout_exception) {
				result = ETIMEDOUT;
			} catch (Libc::Nonblocking_exception) {
				errno  = ETIMEDOUT;
				result = ETIMEDOUT;
			}
		}

		c->counter_lock.lock();
		if (c->num_signallers > 0) {
			if (result == ETIMEDOUT) /* timeout occured */
				c->signal_sem.down();
			c->handshake_sem.up();
			--c->num_signallers;
		}
		c->num_waiters--;
		c->counter_lock.unlock();

		pthread_mutex_lock(mutex);

		return result;
	}


	int pthread_cond_wait(pthread_cond_t *__restrict cond,
	                      pthread_mutex_t *__restrict mutex)
	{
		return pthread_cond_timedwait(cond, mutex, 0);
	}


	int pthread_cond_signal(pthread_cond_t *cond)
	{
		if (!cond || !*cond)
			return EINVAL;

		pthread_cond *c = *cond;

		c->counter_lock.lock();
		if (c->num_waiters > c->num_signallers) {
		  ++c->num_signallers;
		  c->signal_sem.up();
		  c->counter_lock.unlock();
		  c->handshake_sem.down();
		} else
		  c->counter_lock.unlock();

	   return 0;
	}


	int pthread_cond_broadcast(pthread_cond_t *cond)
	{
		if (!cond || !*cond)
			return EINVAL;

		pthread_cond *c = *cond;

		c->counter_lock.lock();
		if (c->num_waiters > c->num_signallers) {
			int still_waiting = c->num_waiters - c->num_signallers;
			c->num_signallers = c->num_waiters;
			for (int i = 0; i < still_waiting; i++)
				c->signal_sem.up();
			c->counter_lock.unlock();
			for (int i = 0; i < still_waiting; i++)
				c->handshake_sem.down();
		} else
			c->counter_lock.unlock();

		return 0;
	}

	/* TLS */


	struct Key_element : List<Key_element>::Element
	{
		const void *thread_base;
		const void *value;

		Key_element(const void *thread_base, const void *value)
		: thread_base(thread_base),
		  value(value) { }
	};


	static Lock key_list_lock;
	List<Key_element> key_list[PTHREAD_KEYS_MAX];

	int pthread_key_create(pthread_key_t *key, void (*destructor)(void*))
	{
		if (!key)
			return EINVAL;

		Lock_guard<Lock> key_list_lock_guard(key_list_lock);

		for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
			/*
			 * Find an empty key slot and insert an element for the current
			 * thread to mark the key slot as used.
			 */
			if (!key_list[k].first()) {
				Key_element *key_element = new (object_alloc) Key_element(Thread::myself(), 0);
				key_list[k].insert(key_element);
				*key = k;
				return 0;
			}
		}

		return EAGAIN;
	}


	int pthread_key_delete(pthread_key_t key)
	{
		if (key < 0 || key >= PTHREAD_KEYS_MAX || !key_list[key].first())
			return EINVAL;

		Lock_guard<Lock> key_list_lock_guard(key_list_lock);

		while (Key_element * element = key_list[key].first()) {
			key_list[key].remove(element);
			destroy(object_alloc, element);
		}

		return 0;
	}


	int pthread_setspecific(pthread_key_t key, const void *value)
	{
		if (key < 0 || key >= PTHREAD_KEYS_MAX)
			return EINVAL;

		void *myself = Thread::myself();

		Lock_guard<Lock> key_list_lock_guard(key_list_lock);

		for (Key_element *key_element = key_list[key].first(); key_element;
		     key_element = key_element->next())
			if (key_element->thread_base == myself) {
				key_element->value = value;
				return 0;
			}

		/* key element does not exist yet - create a new one */
		Key_element *key_element = new (object_alloc) Key_element(Thread::myself(), value);
		key_list[key].insert(key_element);
		return 0;
	}


	void *pthread_getspecific(pthread_key_t key)
	{
		if (key < 0 || key >= PTHREAD_KEYS_MAX)
			return nullptr;

		void *myself = Thread::myself();

		Lock_guard<Lock> key_list_lock_guard(key_list_lock);

		for (Key_element *key_element = key_list[key].first(); key_element;
		     key_element = key_element->next())
			if (key_element->thread_base == myself)
				return (void*)(key_element->value);

		return 0;
	}


	int pthread_once(pthread_once_t *once, void (*init_once)(void))
	{
		if (!once || ((once->state != PTHREAD_NEEDS_INIT) &&
		              (once->state != PTHREAD_DONE_INIT)))
			return EINTR;

		if (!once->mutex) {
			pthread_mutex_t p = new (object_alloc) pthread_mutex(0);
			/* be paranoid */
			if (!p)
				return EINTR;

			static Lock lock;

			lock.lock();
			if (!once->mutex) {
				once->mutex = p;
				p = nullptr;
			}
			lock.unlock();

			/*
			 * If another thread concurrently allocated a mutex and was faster,
			 * free our mutex since it is not used.
			 */
			if (p)
				destroy(object_alloc, p);
		}

		once->mutex->lock();

		if (once->state == PTHREAD_DONE_INIT) {
			once->mutex->unlock();
			return 0;
		}

		init_once();

		once->state = PTHREAD_DONE_INIT;

		once->mutex->unlock();

		return 0;
	}
}
