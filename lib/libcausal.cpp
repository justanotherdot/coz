#include <dlfcn.h>
#include <linux/limits.h>
#include <pthread.h>
#include <stdlib.h>

#include <sstream>
#include <string>
#include <unordered_set>

#include "causal/inspect.h"
#include "causal/profiler.h"
#include "causal/progress_point.h"
#include "causal/real.h"
#include "causal/util.h"

#include "ccutil/log.h"

using namespace std;

/// The type of a main function
typedef int (*main_fn_t)(int, char**, char**);

/// The program's real main function
main_fn_t real_main;

bool initialized = false;

/**
 * Called by the application to register a progress point
 */
extern "C" void __causal_register_counter(progress_point::kind k,
                                          size_t* counter,
                                          size_t* backoff,
                                          const char* name) {
  if(k == progress_point::kind::progress) {
    progress_point* p = new source_progress_point(name, counter);
    profiler::get_instance().register_progress_point(p);
  } else {
    WARNING << "Ignored unsupported progress point type";
  }
}

/**
 * Pass the real __libc_start_main this main function, then run the real main
 * function. This allows Causal to shut down when the real main function returns.
 */
int wrapped_main(int argc, char** argv, char** env) {
  // Remove causal from LD_PRELOAD. Just clearing LD_PRELOAD for now FIXME!
  unsetenv("LD_PRELOAD");

  // Read settings out of environment variables
  string output_file = getenv_safe("COZ_OUTPUT", "profile.coz");
  
  vector<string> binary_scope_v = split(getenv_safe("COZ_BINARY_SCOPE"), '\t');
  unordered_set<string> binary_scope(binary_scope_v.begin(), binary_scope_v.end());
  
  vector<string> source_scope_v = split(getenv_safe("COZ_SOURCE_SCOPE"), '\t');
  unordered_set<string> source_scope(source_scope_v.begin(), source_scope_v.end());
  
  vector<string> progress_points_v = split(getenv_safe("COZ_PROGRESS_POINTS"), '\t');
  unordered_set<string> progress_points(progress_points_v.begin(), progress_points_v.end());
  
  bool end_to_end = getenv("COZ_END_TO_END");
  bool sample_only = getenv("COZ_SAMPLE_ONLY");
  string fixed_line_name = getenv_safe("COZ_FIXED_LINE", "");
  int fixed_speedup;
  stringstream(getenv_safe("COZ_FIXED_SPEEDUP", "-1")) >> fixed_speedup;

  // Replace 'MAIN' in the binary_scope with the real path of the main executable
  if(binary_scope.find("MAIN") != binary_scope.end()) {
    char buf[PATH_MAX];
    readlink("/proc/self/exe", buf, PATH_MAX);
    binary_scope.erase("MAIN");
    binary_scope.insert(string(buf));
  }

  // Build the memory map for all in-scope binaries
  memory_map::get_instance().build(binary_scope, source_scope);

  // Register any sampling progress points
  for(const string& line_name : progress_points) {
    shared_ptr<line> l = memory_map::get_instance().find_line(line_name);
    if(l) {
      progress_point* p = new sampling_progress_point(line_name, l);
      profiler::get_instance().register_progress_point(p);
    } else {
      WARNING << "Progress line \"" << line_name << "\" was not found.";
    }
  }

  shared_ptr<line> fixed_line;
  if(fixed_line_name != "") {
    fixed_line = memory_map::get_instance().find_line(fixed_line_name);
    PREFER(fixed_line) << "Fixed line \"" << fixed_line_name << "\" was not found.";
  }

  // Create an end-to-end progress point and register it if running in end-to-end mode
  end_progress_point* end_point = new end_progress_point();
  if(end_to_end) {
    profiler::get_instance().register_progress_point(end_point);
  }

  // Start the profiler
  profiler::get_instance().startup(output_file, fixed_line.get(), fixed_speedup, sample_only);

  // Synchronizations can be intercepted once the profiler has been initialized
  initialized = true;

  // Run the real main function
  int result = real_main(argc, argv, env);

  // Increment the end-to-end progress point just before shutdown
  end_point->done();

  // Shut down the profiler
  profiler::get_instance().shutdown();

  return result;
}

/**
 * Interpose on the call to __libc_start_main to run before libc constructors.
 */
extern "C" int __libc_start_main(main_fn_t, int, char**, void (*)(), void (*)(), void (*)(), void*) __attribute__((weak, alias("causal_libc_start_main")));

extern "C" int causal_libc_start_main(main_fn_t main_fn, int argc, char** argv,
    void (*init)(), void (*fini)(), void (*rtld_fini)(), void* stack_end) {
  // Find the real __libc_start_main
  auto real_libc_start_main = (decltype(__libc_start_main)*)dlsym(RTLD_NEXT, "__libc_start_main");
  // Save the program's real main function
  real_main = main_fn;
  // Run the real __libc_start_main, but pass in the wrapped main function
  int result = real_libc_start_main(wrapped_main, argc, argv, init, fini, rtld_fini, stack_end);

  return result;
}

/// Remove causal's required signals from a signal mask
void remove_causal_signals(sigset_t* set) {
  if(sigismember(set, SampleSignal)) {
    sigdelset(set, SampleSignal);
  }
  if(sigismember(set, SIGSEGV)) {
    sigdelset(set, SIGSEGV);
  }
  if(sigismember(set, SIGABRT)) {
    sigdelset(set, SIGABRT);
  }
}

/// Check if a signal is required by causal
bool is_causal_signal(int signum) {
  return signum == SampleSignal || signum == SIGSEGV || signum == SIGABRT;
}

extern "C" {
  /// Pass pthread_create calls to causal so child threads can inherit the parent's delay count
  int pthread_create(pthread_t* thread,
                     const pthread_attr_t* attr,
                     thread_fn_t fn,
                     void* arg) {
    return profiler::get_instance().handle_pthread_create(thread, attr, fn, arg);
  }

  /// Catch up on delays before exiting, possibly unblocking a thread joining this one
  void __attribute__((noreturn)) pthread_exit(void* result) {
	  profiler::get_instance().handle_pthread_exit(result);
  }

  /// Skip any delays added while waiting to join a thread
  int pthread_join(pthread_t t, void** retval) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_join(t, retval);
    if(initialized) profiler::get_instance().post_block(true);

    return result;
  }

  int pthread_tryjoin_np(pthread_t t, void** retval) throw() {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_tryjoin_np(t, retval);
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  int pthread_timedjoin_np(pthread_t t, void** ret, const struct timespec* abstime) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_timedjoin_np(t, ret, abstime);
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  /// Skip any global delays added while blocked on a mutex
  int pthread_mutex_lock(pthread_mutex_t* mutex) throw() {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_mutex_lock(mutex);
    if(initialized) profiler::get_instance().post_block(true);

    return result;
  }

  /// Catch up on delays before unblocking any threads waiting on a mutex
  int pthread_mutex_unlock(pthread_mutex_t* mutex) throw() {
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_mutex_unlock(mutex);
  }

  /// Skip any delays added while waiting on a condition variable
  int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_cond_wait(cond, mutex);
    if(initialized) profiler::get_instance().post_block(true);

    return result;
  }

  /**
   * Wait on a condvar for a fixed timeout. If the wait does *not* time out, skip any global
   * delays added during the waiting period.
   */
  int pthread_cond_timedwait(pthread_cond_t* cond,
                             pthread_mutex_t* mutex,
                             const struct timespec* time) {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_cond_timedwait(cond, mutex, time);

    // Skip delays only if the wait didn't time out
    if(initialized) profiler::get_instance().post_block(result == 0);

    return result;
  }

  /// Catchup on delays before waking a thread waiting on a condition variable
  int pthread_cond_signal(pthread_cond_t* cond) throw() {
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_cond_signal(cond);
  }

  /// Catch up on delays before waking any threads waiting on a condition variable
  int pthread_cond_broadcast(pthread_cond_t* cond) throw() {
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_cond_broadcast(cond);
  }

  /// Catch up before, and skip ahead after waking from a barrier
  int pthread_barrier_wait(pthread_barrier_t* barrier) throw() {
    if(initialized) profiler::get_instance().catch_up();
    if(initialized) profiler::get_instance().pre_block();

    int result = real::pthread_barrier_wait(barrier);

    if(initialized) profiler::get_instance().post_block(true);

    return result;
  }

  int pthread_rwlock_rdlock(pthread_rwlock_t* rwlock) throw() {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_rwlock_rdlock(rwlock);
    if(initialized) profiler::get_instance().post_block(true);
    return result;
  }

  int pthread_rwlock_timedrdlock(pthread_rwlock_t* rwlock, const struct timespec* abstime) throw() {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_rwlock_timedrdlock(rwlock, abstime);
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  int pthread_rwlock_wrlock(pthread_rwlock_t* rwlock) throw() {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_rwlock_wrlock(rwlock);
    if(initialized) profiler::get_instance().post_block(true);
    return result;
  }

  int pthread_rwlock_timedwrlock(pthread_rwlock_t* rwlock, const struct timespec* abstime) throw() {
    if(initialized) profiler::get_instance().pre_block();
    int result = real::pthread_rwlock_timedwrlock(rwlock, abstime);
    if(initialized) profiler::get_instance().post_block(result == 0);
    return result;
  }

  int pthread_rwlock_unlock(pthread_rwlock_t* rwlock) throw() {
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_rwlock_unlock(rwlock);
  }

  /// Run shutdown before exiting
  void __attribute__((noreturn)) exit(int status) throw() {
    profiler::get_instance().shutdown();
    real::exit(status);
  }

  /// Run shutdown before exiting
  void __attribute__((noreturn)) _exit(int status) {
    profiler::get_instance().shutdown();
  	real::_exit(status);
  }

  /// Run shutdown before exiting
  void __attribute__((noreturn)) _Exit(int status) throw() {
    profiler::get_instance().shutdown();
    real::_Exit(status);
  }

  /// Don't allow programs to set signal handlers for causal's required signals
  sighandler_t signal(int signum, sighandler_t handler) throw() {
    if(is_causal_signal(signum)) {
      return NULL;
    } else {
      return real::signal(signum, handler);
    }
  }

  /// Don't allow programs to set handlers or mask signals required for causal
  int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) throw() {
    if(is_causal_signal(signum)) {
      return 0;
    } else if(act != NULL) {
      struct sigaction my_act = *act;
      remove_causal_signals(&my_act.sa_mask);
      return real::sigaction(signum, &my_act, oldact);
    } else {
      return real::sigaction(signum, act, oldact);
    }
  }

  /// Ensure causal's signals remain unmasked
  int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) throw() {
    if(how == SIG_BLOCK || how == SIG_SETMASK) {
      if(set != NULL) {
        sigset_t myset = *set;
        remove_causal_signals(&myset);
        return real::sigprocmask(how, &myset, oldset);
      }
    }

    return real::sigprocmask(how, set, oldset);
  }

  /// Ensure causal's signals remain unmasked
  int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) throw() {
    if(how == SIG_BLOCK || how == SIG_SETMASK) {
      if(set != NULL) {
        sigset_t myset = *set;
        remove_causal_signals(&myset);

        return real::pthread_sigmask(how, &myset, oldset);
      }
    }

    return real::pthread_sigmask(how, set, oldset);
  }

  /// Catch up on delays before sending a signal to the current process
  int kill(pid_t pid, int sig) throw() {
    if(pid == getpid())
      profiler::get_instance().catch_up();
    return real::kill(pid, sig);
  }

  /// Catch up on delays before sending a signal to another thread
  int pthread_kill(pthread_t thread, int sig) throw() {
    // TODO: Don't allow threads to send causal's signals
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_kill(thread, sig);
  }

  int pthread_sigqueue(pthread_t thread, int sig, const union sigval val) throw() {
    if(initialized) profiler::get_instance().catch_up();
    return real::pthread_sigqueue(thread, sig, val);
  }

  /**
   * Ensure a thread cannot wait for causal's signals.
   * If the waking signal is delivered from the same process, skip any global delays added
   * while blocked.
   */
  int sigwait(const sigset_t* set, int* sig) {
    sigset_t myset = *set;
    remove_causal_signals(&myset);
    siginfo_t info;

    if(initialized) profiler::get_instance().pre_block();

    int result = real::sigwaitinfo(&myset, &info);

    // Woken up by another thread if the call did not fail, and the waking process is this one
    if(initialized) profiler::get_instance().post_block(result != -1 && info.si_pid == getpid());

    if(result == -1) {
      // If there was an error, return the error code
      return errno;
    } else {
      // If the sig pointer is not null, pass the received signal to the caller
      if(sig) *sig = result;
      return 0;
    }
  }

  /**
   * Ensure a thread cannot wait for causal's signals.
   * If the waking signal is delivered from the same process, skip any added global delays.
   */
  int sigwaitinfo(const sigset_t* set, siginfo_t* info) {
    sigset_t myset = *set;
    siginfo_t myinfo;
    remove_causal_signals(&myset);

    if(initialized) profiler::get_instance().pre_block();

    int result = real::sigwaitinfo(&myset, &myinfo);

    // Woken up by another thread if the call did not fail, and the waking process is this one
    if(initialized) profiler::get_instance().post_block(result > 0 && myinfo.si_pid == getpid());

    if(result > 0 && info)
      *info = myinfo;

    return result;
  }

  /**
   * Ensure a thread cannot wait for causal's signals.
   * If the waking signal is delivered from the same process, skip any global delays.
   */
  int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout) {
    sigset_t myset = *set;
    siginfo_t myinfo;
    remove_causal_signals(&myset);

    if(initialized) profiler::get_instance().pre_block();

    int result = real::sigtimedwait(&myset, &myinfo, timeout);

    // Woken up by another thread if the call did not fail, and the waking process is this one
    if(initialized) profiler::get_instance().post_block(result > 0 && myinfo.si_pid == getpid());

    if(result > 0 && info)
      *info = myinfo;

    return result;
  }

  /**
   * Set the process signal mask, suspend, then wake and restore the signal mask.
   * If the waking signal is delivered from within the process, skip any added global delays
   */
  int sigsuspend(const sigset_t* set) {
    sigset_t oldset;
    int sig;
    real::sigprocmask(SIG_SETMASK, set, &oldset);
    int rc = sigwait(set, &sig);
    real::sigprocmask(SIG_SETMASK, &oldset, nullptr);
    return rc;
  }
}
