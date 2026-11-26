// The kernel guard (opad/kernel_guard.hpp): a hardware fault inside a guarded call is an error on that thread, the
// process carries on, exceptions pass through, guards nest, and other threads keep their own.
#include <atomic>
#include <stdexcept>
#include <thread>

#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <csetjmp>

#include "check.hpp"
#include "opad/kernel_guard.hpp"

using namespace opad;

namespace {
// A read through a null pointer the compiler cannot prove null.
int fault_now(volatile int* volatile* where) { return **where; }
volatile int* volatile g_null = nullptr;
}  // namespace

TEST(guard_returns_the_value) {
  const auto v = kernel_guarded([] { return 42; });
  CHECK(v.has_value());
  CHECK_EQ(*v, 42);
}

TEST(guard_turns_an_access_violation_into_an_error) {
  const unsigned long before = kernel_faults();
  KernelFault fault;
  const auto v = kernel_guarded([] { return fault_now(&g_null); }, &fault);
  CHECK(!v.has_value());
  CHECK(fault.code != 0);
  CHECK(fault.text().find("access violation") != std::string::npos);
  CHECK_EQ(kernel_faults(), before + 1);
  // And again: the guard is reusable after a fault, and the thread is fine.
  CHECK(!kernel_guarded([] { return fault_now(&g_null); }).has_value());
  CHECK_EQ(*kernel_guarded([] { return 7; }), 7);
}

TEST(guard_passes_exceptions_through) {
  bool caught = false;
  try {
    (void)kernel_guarded([]() -> int { throw std::runtime_error("refused"); });
  } catch (const std::runtime_error& e) {
    caught = std::string(e.what()) == "refused";
  }
  CHECK(caught);
  CHECK(!kernel_guarded([] { return fault_now(&g_null); }).has_value());
}

TEST(guards_nest) {
  const auto outer = kernel_guarded([] {
    const auto inner = kernel_guarded([] { return fault_now(&g_null); });
    return inner.has_value() ? 1 : 2;  // the inner guard took it, the outer one sees a value
  });
  CHECK(outer.has_value());
  CHECK_EQ(*outer, 2);
}

TEST(guard_is_per_thread) {
  std::atomic<int> faulted{0}, fine{0};
  std::thread a([&] {
    for (int i = 0; i < 20; ++i)
      if (!kernel_guarded([] { return fault_now(&g_null); })) ++faulted;
  });
  std::thread b([&] {
    for (int i = 0; i < 20; ++i)
      if (kernel_guarded([] { return 1; })) ++fine;
  });
  a.join();
  b.join();
  CHECK_EQ(faulted.load(), 20);
  CHECK_EQ(fine.load(), 20);
}

// A fault inside the kernel's own try block (OCC_CATCH_SIGNALS spelled out, as OCCT compiles it) comes out there as a
// Standard_Failure, the way the fillet's ChFi3d_Builder::Compute takes it; the guard itself sees a value.
int kernel_like_call() {
  try {
    Standard_ErrorHandler handler;
    if (setjmp(handler.Label())) {
      handler.Catches(STANDARD_TYPE(Standard_Failure));
      handler.Error()->Reraise();
    }
    return fault_now(&g_null);
  } catch (const Standard_Failure& e) {
    return std::string(e.GetMessageString()).find("access violation") != std::string::npos ? -1 : -2;
  }
}

TEST(a_fault_in_the_kernels_try_block_is_its_failure) {
  const auto v = kernel_guarded([] { return kernel_like_call(); });
  CHECK(v.has_value());
  CHECK_EQ(*v, -1);
}

// Nothing is left in OCCT's handler list: outside a guard no try block is active, and the kernel's own bookkeeping
// (type registration takes a mutex that registers with the innermost handler) keeps working.
TEST(no_handler_is_left_behind) {
  for (int i = 0; i < 5; ++i) {
    (void)kernel_guarded([] { return fault_now(&g_null); });
    (void)kernel_guarded([] { return kernel_like_call(); });
  }
  CHECK(!Standard_ErrorHandler::IsInTryBlock());
  bool caught = false;
  try {
    Standard_Failure::NewInstance("plain")->Reraise();
  } catch (const Standard_Failure&) {
    caught = true;
  }
  CHECK(caught);
}

CHECK_MAIN()
