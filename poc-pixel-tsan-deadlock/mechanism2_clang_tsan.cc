// C++ mirror of the Rust dlopen_loi repro, to test whether CLANG's TSAN runtime
// exhibits the same dl_iterate_phdr <-> interceptor-lock collapse.
//   thread group A: _Unwind_Backtrace loop (-> _Unwind_Find_FDE -> dl_iterate_phdr)
//   thread group B: dlopen/dlclose loop on a not-already-loaded lib (real map churn)
#include <unwind.h>
#include <dlfcn.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <ctime>

static std::atomic<uint64_t> BT{0}, DL{0};

static _Unwind_Reason_Code trace_fn(struct _Unwind_Context* ctx, void* arg) {
  int* n = static_cast<int*>(arg);
  (void)_Unwind_GetIP(ctx);
  // allocate inside the walk, like Rust's frame Vec push
  volatile char* p = static_cast<char*>(malloc(64));
  if (p) { p[0] = 1; free((void*)p); }
  if (++(*n) > 128) return _URC_END_OF_STACK;
  return _URC_NO_REASON;
}
static void actor_bt() { for (;;) { int n = 0; _Unwind_Backtrace(trace_fn, &n); BT.fetch_add(1, std::memory_order_relaxed); } }
static void actor_dl() { for (;;) { void* h = dlopen("libz.so.1", RTLD_NOW); if (h) dlclose(h); DL.fetch_add(1, std::memory_order_relaxed); } }

int main(int argc, char** argv) {
  int nbt = argc > 1 ? atoi(argv[1]) : 6;
  int ndl = argc > 2 ? atoi(argv[2]) : 6;
  int secs = argc > 3 ? atoi(argv[3]) : 20;
  fprintf(stderr, "[cpp_tsan] bt_threads=%d dl_threads=%d\n", nbt, ndl);
  for (int i = 0; i < nbt; i++) std::thread(actor_bt).detach();
  for (int i = 0; i < ndl; i++) std::thread(actor_dl).detach();
  uint64_t lb = 0, ld = 0;
  for (int i = 0; i < secs * 2; i++) {
    struct timespec ts{0, 500000000}; nanosleep(&ts, nullptr);
    uint64_t b = BT.load(), d = DL.load();
    bool collapsed = (b - lb) < 100;
    fprintf(stderr, "[cpp_tsan] bt=+%lu dl=+%lu%s\n", b - lb, d - ld, collapsed ? "   *** COLLAPSE ***" : "");
    lb = b; ld = d;
  }
  return 0;
}
