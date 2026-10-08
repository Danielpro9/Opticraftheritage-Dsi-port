// dsi_atomic_fallback.c -- replaces -latomic, which the real BlocksDS/
// Wonderful Toolchain linker rejects outright for this target ("ld: cannot
// find -latomic: Invalid argument" -- confirmed against the actual
// toolchain in CI, not assumed; its arm9 multilib does not ship one).
//
// The ARM946E-S (ARMv5TE) has no LDREXD/STREXD, so GCC lowers certain
// std::atomic operations it cannot do as a single native instruction into
// calls to __atomic_* library functions instead of inlining them. Confirmed
// call sites from the real linker's own "undefined reference" errors,
// before -latomic was tried:
//   __atomic_load_8              <- Random::Random()'s std::atomic seed state
//   __atomic_exchange_4          <- ThreadDownloadImageData's destructor
//   __atomic_compare_exchange_8  <- Random::Random() too (same seed state's
//                                    compare_exchange, found only after the
//                                    first two were already resolved -- the
//                                    linker reports one missing symbol at a
//                                    time, not the whole set up front)
//   __atomic_fetch_add_4         <- GuiMultiplayer::incrementThreadsPending()/
//   __atomic_fetch_sub_4            decrementThreadsPending()'s std::atomic<int_t>
//                                    threadsPending, dead code (so never linked)
//                                    until NO_NETWORK was removed made this class's
//                                    real multiplayer UI reachable
//
// This is not a general-purpose libatomic replacement: it only defines the
// symbols actually referenced, and it is correct ONLY because DSi has
// no real concurrency to race against. Confirmed empirically against this
// same toolchain (see the "thread-probe" CI job and PlatformConfig.h's
// PLATFORM_ASYNC_FILE_IO comment): std::thread here has no constructor able
// to launch a callable at all, so nothing on this platform ever runs two
// flows of execution over the same memory at once. A plain non-atomic
// load/exchange is therefore behaviourally identical to a real atomic one
// here -- there is no interrupt handler or second core that could observe a
// torn read the way there would be on real multi-core hardware.
//
// If a future change (a real ARM7/ARM9 IPC path touching these same
// variables, for instance) reintroduces actual concurrency, these need
// revisiting -- they are not a general substitute for real atomics, just an
// honest fit for what this platform actually does today.
#ifdef DSI_PLATFORM

#include <stdint.h>
#include <stdbool.h>

unsigned long long __atomic_load_8(const volatile void *ptr, int memorder)
{
	(void)memorder;
	return *(const volatile unsigned long long *)ptr;
}

unsigned int __atomic_exchange_4(volatile void *ptr, unsigned int val, int memorder)
{
	(void)memorder;
	volatile unsigned int *p = (volatile unsigned int *)ptr;
	unsigned int old = *p;
	*p = val;
	return old;
}

// GCC's __atomic_compare_exchange_N ABI: if *ptr == *expected, writes
// desired into *ptr and returns 1 (success). Otherwise writes the current
// *ptr into *expected and returns 0. "weak" (spurious-failure allowed) does
// not matter here -- there is nothing to spuriously fail against without
// real concurrency, so this always behaves like the "strong" form.
bool __atomic_compare_exchange_8(volatile void *ptr, void *expected, unsigned long long desired,
	bool weak, int success_memorder, int failure_memorder)
{
	(void)weak;
	(void)success_memorder;
	(void)failure_memorder;
	volatile unsigned long long *p = (volatile unsigned long long *)ptr;
	unsigned long long *exp = (unsigned long long *)expected;
	if (*p == *exp)
	{
		*p = desired;
		return true;
	}
	*exp = *p;
	return false;
}

// GCC's __atomic_fetch_add_N/__atomic_fetch_sub_N ABI: apply val to *ptr,
// return the value *ptr held before the update.
unsigned int __atomic_fetch_add_4(volatile void *ptr, unsigned int val, int memorder)
{
	(void)memorder;
	volatile unsigned int *p = (volatile unsigned int *)ptr;
	unsigned int old = *p;
	*p = old + val;
	return old;
}

unsigned int __atomic_fetch_sub_4(volatile void *ptr, unsigned int val, int memorder)
{
	(void)memorder;
	volatile unsigned int *p = (volatile unsigned int *)ptr;
	unsigned int old = *p;
	*p = old - val;
	return old;
}

#endif // DSI_PLATFORM
