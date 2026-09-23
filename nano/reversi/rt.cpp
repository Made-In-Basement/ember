/*
  Reversi for Ember.  The drawing, platform and C++ runtime files here are
  the ones Chess uses (nano/chess/), copied so each game builds on its own.
*/

// rt.cpp - the pieces of a C++ runtime the games need and Ember lacks.
//
// Ember's N32 runtime is C: a heap, files, the keyboard, the screen.  A C++
// program also expects operator new, somewhere to send a pure virtual call,
// its global constructors run before main, and a handful of out-of-line
// helpers libc++'s headers refer to.  Only what the link actually asked for
// is here.  None of it runs on the host build, which has a real runtime.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
  void sys_puts(const char* s);
  void sys_log(const char* s);
  void sys_exit(int code) __attribute__((noreturn));
  int  vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
}

// Stop with a message on the text screen and in EMBER.LOG.
[[noreturn]] void ember_fatal(const char* what) {
  sys_log(what);
  sys_puts("\r\nCHESS: ");
  sys_puts(what);
  sys_puts("\r\n");
  sys_exit(1);
}

void* operator new(size_t n) {
  void* p = malloc(n ? n : 1);
  if (!p)
      ember_fatal("out of memory");
  return p;
}

void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return malloc(n ? n : 1); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

namespace std {
  const nothrow_t nothrow{};
}

extern "C" {

void* __dso_handle = nullptr;

// Destructors registered for global objects.  The program ends by leaving
// the machine to the shell, so there is nothing to run them for.
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

void __cxa_pure_virtual() { ember_fatal("pure virtual call"); }

void* memchr(const void* s, int c, size_t n) {
  const unsigned char* p = (const unsigned char*)s;
  for (size_t i = 0; i < n; ++i)
      if (p[i] == (unsigned char)c)
          return (void*)(p + i);
  return nullptr;
}

// Global constructors: the linker script gathers them between these two.
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

void ember_run_constructors(void) {
  for (void (**f)(void) = __init_array_start; f < __init_array_end; ++f)
      (*f)();
}

} // extern "C"

namespace std { inline namespace __1 {

  // libc++ asserts and "can't happen" paths end here.
  [[noreturn]] void __libcpp_verbose_abort(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ember_fatal(buf);
  }

  // Bucket counts for std::unordered_map (Stockfish's endgame table).  Any
  // prime at least n gives the same lookups; this is the smallest.
  size_t __next_prime(size_t n) {
    if (n < 2) return 2;
    for (;; ++n) {
        bool prime = true;
        for (size_t d = 2; d * d <= n; ++d)
            if (n % d == 0) { prime = false; break; }
        if (prime) return n;
    }
  }

}} // namespace std::__1


// std::string's members.  libc++'s headers declare them "extern template"
// and expect its compiled library to provide them; Ember has no such
// library, so they are instantiated here, once, from the same headers.
#include <string>
template class std::__1::basic_string<char>;
template std::__1::basic_string<char> std::__1::operator+<char, std::__1::char_traits<char>, std::__1::allocator<char>>(const char*, const std::__1::basic_string<char>&);

// std::to_string for integers, which libc++ also keeps in its library.
namespace std { inline namespace __1 {

  static string decimal(unsigned long long v, bool negative) {
    char tmp[24];
    int i = sizeof tmp;
    do { tmp[--i] = char('0' + v % 10); v /= 10; } while (v);
    if (negative) tmp[--i] = '-';
    return string(tmp + i, sizeof tmp - i);
  }

  string to_string(int v) { return decimal(v < 0 ? 0ULL - (unsigned long long)(long long)v : v, v < 0); }
  string to_string(long v) { return decimal(v < 0 ? 0ULL - (unsigned long long)(long long)v : v, v < 0); }
  string to_string(long long v) { return decimal(v < 0 ? 0ULL - (unsigned long long)v : v, v < 0); }
  string to_string(unsigned v) { return decimal(v, false); }
  string to_string(unsigned long v) { return decimal(v, false); }
  string to_string(unsigned long long v) { return decimal(v, false); }

}} // namespace std::__1
