#include "linux_syscalls.h"

#include "guest_files.h"
#include "guest_log.h"
#include "guest_threads.h"

#include <FEXCore/Core/CoreState.h>
// for CpuStateFrame::Thread->FrontendPtr, which is how a syscall finds the guest thread issuing it.
#include <FEXCore/Debug/InternalThreadState.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

namespace HostLayer {

namespace {

// guest linux x86-64 syscall numbers. only the ones handled below are named.
enum GuestSyscall : uint64_t {
  SYS_x64_read = 0,
  SYS_x64_write = 1,
  SYS_x64_open = 2,
  SYS_x64_close = 3,
  SYS_x64_stat = 4,
  SYS_x64_fstat = 5,
  SYS_x64_lstat = 6,
  SYS_x64_poll = 7,
  SYS_x64_lseek = 8,
  SYS_x64_mmap = 9,
  SYS_x64_mprotect = 10,
  SYS_x64_munmap = 11,
  SYS_x64_brk = 12,
  SYS_x64_rt_sigaction = 13,
  SYS_x64_rt_sigprocmask = 14,
  SYS_x64_rt_sigreturn = 15,
  SYS_x64_ioctl = 16,
  SYS_x64_pread64 = 17,
  SYS_x64_pwrite64 = 18,
  SYS_x64_readv = 19,
  SYS_x64_writev = 20,
  SYS_x64_access = 21,
  SYS_x64_msync = 26,
  SYS_x64_mincore = 27,
  SYS_x64_madvise = 28,
  SYS_x64_nanosleep = 35,
  SYS_x64_sched_yield = 24,
  SYS_x64_mremap = 25,
  SYS_x64_dup = 32,
  SYS_x64_dup2 = 33,
  SYS_x64_getpid = 39,
  SYS_x64_socket = 41,
  SYS_x64_connect = 42,
  SYS_x64_accept = 43,
  SYS_x64_sendto = 44,
  SYS_x64_recvfrom = 45,
  SYS_x64_sendmsg = 46,
  SYS_x64_recvmsg = 47,
  SYS_x64_shutdown = 48,
  SYS_x64_bind = 49,
  SYS_x64_listen = 50,
  SYS_x64_getsockname = 51,
  SYS_x64_getpeername = 52,
  SYS_x64_socketpair = 53,
  SYS_x64_setsockopt = 54,
  SYS_x64_getsockopt = 55,
  SYS_x64_accept4 = 288,
  SYS_x64_exit = 60,
  SYS_x64_kill = 62,
  SYS_x64_tkill = 200,
  SYS_x64_uname = 63,
  SYS_x64_fcntl = 72,
  SYS_x64_flock = 73,
  SYS_x64_fsync = 74,
  SYS_x64_fdatasync = 75,
  SYS_x64_truncate = 76,
  SYS_x64_ftruncate = 77,
  SYS_x64_clone = 56,
  SYS_x64_getcwd = 79,
  SYS_x64_chdir = 80,
  SYS_x64_rename = 82,
  SYS_x64_mkdir = 83,
  SYS_x64_rmdir = 84,
  SYS_x64_creat = 85,
  SYS_x64_link = 86,
  SYS_x64_unlink = 87,
  SYS_x64_symlink = 88,
  SYS_x64_chmod = 90,
  SYS_x64_fchmod = 91,
  SYS_x64_umask = 95,
  SYS_x64_readlink = 89,
  SYS_x64_gettimeofday = 96,
  SYS_x64_getrlimit = 97,
  SYS_x64_getrusage = 98,
  SYS_x64_sysinfo = 99,
  SYS_x64_times = 100,
  SYS_x64_getuid = 102,
  SYS_x64_getgid = 104,
  SYS_x64_geteuid = 107,
  SYS_x64_getegid = 108,
  SYS_x64_getppid = 110,
  SYS_x64_getpgrp = 111,
  SYS_x64_getpgid = 121,
  SYS_x64_getsid = 124,
  SYS_x64_sigaltstack = 131,
  SYS_x64_mlock = 149,
  SYS_x64_munlock = 150,
  SYS_x64_mlockall = 151,
  SYS_x64_munlockall = 152,
  SYS_x64_setrlimit = 160,
  SYS_x64_statfs = 137,
  SYS_x64_fstatfs = 138,
  SYS_x64_sched_getparam = 143,
  SYS_x64_sched_setscheduler = 144,
  SYS_x64_sched_getscheduler = 145,
  SYS_x64_sched_get_priority_max = 146,
  SYS_x64_sched_get_priority_min = 147,
  SYS_x64_mknodat = 259,
  SYS_x64_arch_prctl = 158,
  SYS_x64_prctl = 157,
  SYS_x64_gettid = 186,
  SYS_x64_time = 201,
  SYS_x64_futex = 202,
  SYS_x64_sched_setaffinity = 203,
  SYS_x64_sched_getaffinity = 204,
  SYS_x64_select = 23,
  SYS_x64_epoll_wait = 232,
  SYS_x64_epoll_ctl = 233,
  SYS_x64_pselect6 = 270,
  SYS_x64_ppoll = 271,
  SYS_x64_epoll_pwait = 281,
  SYS_x64_eventfd2 = 290,
  SYS_x64_epoll_create1 = 291,
  SYS_x64_getdents64 = 217,
  SYS_x64_set_tid_address = 218,
  SYS_x64_clock_gettime = 228,
  SYS_x64_clock_getres = 229,
  SYS_x64_clock_nanosleep = 230,
  SYS_x64_exit_group = 231,
  SYS_x64_tgkill = 234,
  SYS_x64_set_robust_list = 273,
  SYS_x64_openat = 257,
  SYS_x64_mkdirat = 258,
  SYS_x64_fchownat = 260,
  SYS_x64_newfstatat = 262,
  SYS_x64_unlinkat = 263,
  SYS_x64_renameat = 264,
  SYS_x64_linkat = 265,
  SYS_x64_symlinkat = 266,
  SYS_x64_readlinkat = 267,
  SYS_x64_fchmodat = 268,
  SYS_x64_faccessat = 269,
  SYS_x64_utimensat = 280,
  SYS_x64_fallocate = 285,
  SYS_x64_dup3 = 292,
  SYS_x64_pipe2 = 293,
  SYS_x64_prlimit64 = 302,
  SYS_x64_renameat2 = 316,
  SYS_x64_getrandom = 318,
  SYS_x64_memfd_create = 319,
  SYS_x64_membarrier = 324,
  SYS_x64_statx = 332,
  SYS_x64_rseq = 334,
  SYS_x64_clone3 = 435,
  SYS_x64_faccessat2 = 439,
};

uint64_t FromHost(long Result) {
  return Result == -1 ? static_cast<uint64_t>(-errno) : static_cast<uint64_t>(Result);
}

// --- the guest's monotonic clock, with the pauses left out ---------------------------------------
//
// **a paused guest sees the pause the way a linux program sees the machine being suspended**: its
// monotonic clocks stop, and CLOCK_REALTIME and CLOCK_BOOTTIME carry on. that is the kernel's own
// answer to the same question, and it is what lets a guest come back from minutes away without
// concluding it has hung -- the emulator's own stall watchdog ends a run that makes no progress for
// twenty seconds by CLOCK_MONOTONIC, and every clock it hands a game reads that one too.
//
// so the time the guest reads is the host's less the total time paused, and a deadline it gives the
// kernel on that clock is the host's plus it. the total moves only while every guest thread is
// parked -- see Threads::Resume -- so no thread sees it change under a reading.
//
// the one clock this cannot reach is the cycle counter. a guest's own `rdtsc` is translated to a
// read of the host's counter inside a block, and the emulator does not use it on linux.

bool IsMonotonic(clockid_t Clock) {
  return Clock == CLOCK_MONOTONIC || Clock == CLOCK_MONOTONIC_RAW || Clock == CLOCK_MONOTONIC_COARSE;
}

void ShiftTimespec(struct timespec& Time, int64_t Nanos) {
  int64_t Total = static_cast<int64_t>(Time.tv_nsec) + Nanos % 1000000000;
  Time.tv_sec += Nanos / 1000000000 + Total / 1000000000;
  Total %= 1000000000;
  if (Total < 0) {
    Total += 1000000000;
    --Time.tv_sec;
  }
  Time.tv_nsec = Total;
}

// the futex operations whose timeout is an absolute time, on CLOCK_MONOTONIC unless the guest says
// CLOCK_REALTIME. FUTEX_WAIT's own is relative, FUTEX_LOCK_PI's is always the wall clock, and in the
// requeue and wake-op operations that slot is not a time at all.
bool FutexHasMonotonicDeadline(uint64_t Operation) {
  if (Operation & FUTEX_CLOCK_REALTIME) {
    return false;
  }
  const uint64_t Command = Operation & FUTEX_CMD_MASK;
  return Command == FUTEX_WAIT_BITSET || Command == FUTEX_WAIT_REQUEUE_PI || Command == 13 /* FUTEX_LOCK_PI2 */;
}

// x86-64's struct stat. it is not the arm64 one -- the field order diverges after st_ino and
// the padding differs -- so every stat-shaped syscall has to write this layout by hand.
struct GuestStat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint64_t st_nlink;
  uint32_t st_mode;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t __pad0;
  uint64_t st_rdev;
  int64_t st_size;
  int64_t st_blksize;
  int64_t st_blocks;
  // spelled st_atim_sec rather than st_atime_nsec and friends because bionic's <sys/stat.h>
  // defines those as macros expanding to st_atim.tv_nsec, which turns a field declaration here
  // into a syntax error.
  int64_t st_atim_sec;
  uint64_t st_atim_nsec;
  int64_t st_mtim_sec;
  uint64_t st_mtim_nsec;
  int64_t st_ctim_sec;
  uint64_t st_ctim_nsec;
  int64_t __unused_[3];
};
static_assert(sizeof(GuestStat) == 144, "x86-64 struct stat is 144 bytes");

void TranslateStat(const struct stat& Host, GuestStat* Guest) {
  std::memset(Guest, 0, sizeof(*Guest));
  Guest->st_dev = Host.st_dev;
  Guest->st_ino = Host.st_ino;
  Guest->st_nlink = Host.st_nlink;
  Guest->st_mode = Host.st_mode;
  Guest->st_uid = Host.st_uid;
  Guest->st_gid = Host.st_gid;
  Guest->st_rdev = Host.st_rdev;
  Guest->st_size = Host.st_size;
  Guest->st_blksize = Host.st_blksize;
  Guest->st_blocks = Host.st_blocks;
  Guest->st_atim_sec = Host.st_atim.tv_sec;
  Guest->st_atim_nsec = Host.st_atim.tv_nsec;
  Guest->st_mtim_sec = Host.st_mtim.tv_sec;
  Guest->st_mtim_nsec = Host.st_mtim.tv_nsec;
  Guest->st_ctim_sec = Host.st_ctim.tv_sec;
  Guest->st_ctim_nsec = Host.st_ctim.tv_nsec;
}

// **`struct statx` is the one stat-shaped structure that needs no guest layout of its own.** it is
// fixed-width and identical on every architecture -- that is what it was added for -- so the guest's
// and the host's are the same 256 bytes, and `<linux/stat.h>` is where it comes from: a kernel UAPI
// header the NDK ships whatever the API level, unguarded, and which `<sys/stat.h>` includes
// unconditionally. only the `statx()` *function* is `__INTRODUCED_IN(30)`, and nothing here calls it.
// contrast `GuestStat` above, which is hand-written precisely because the two do differ.
//
// it is only ever filled for a path the guest file layer owns. every other statx is still the raw
// syscall, unchanged, because the kernel fills that one correctly on its own.
static_assert(sizeof(struct statx) == 256, "struct statx is 256 bytes on every architecture");

void FillStatx(const struct stat& Host, struct statx* Guest) {
  std::memset(Guest, 0, sizeof(*Guest));
  // everything a plain stat would have answered, which is everything the layer below can know. a
  // caller asking for STATX_BTIME is told, correctly, that it was not answered.
  Guest->stx_mask = STATX_BASIC_STATS;
  Guest->stx_blksize = static_cast<uint32_t>(Host.st_blksize);
  Guest->stx_nlink = static_cast<uint32_t>(Host.st_nlink);
  Guest->stx_uid = Host.st_uid;
  Guest->stx_gid = Host.st_gid;
  Guest->stx_mode = static_cast<uint16_t>(Host.st_mode);
  Guest->stx_ino = Host.st_ino;
  Guest->stx_size = static_cast<uint64_t>(Host.st_size);
  Guest->stx_blocks = static_cast<uint64_t>(Host.st_blocks);
  Guest->stx_atime = {Host.st_atim.tv_sec, static_cast<uint32_t>(Host.st_atim.tv_nsec)};
  Guest->stx_ctime = {Host.st_ctim.tv_sec, static_cast<uint32_t>(Host.st_ctim.tv_nsec)};
  Guest->stx_mtime = {Host.st_mtim.tv_sec, static_cast<uint32_t>(Host.st_mtim.tv_nsec)};
  // major/minor of the fabricated device the layer reports. it is split the way statx splits it
  // rather than passed whole, because a caller reassembling it expects the halves to make sense.
  Guest->stx_dev_major = static_cast<uint32_t>((Host.st_dev >> 8) & 0xFFF);
  Guest->stx_dev_minor = static_cast<uint32_t>(Host.st_dev & 0xFF);
}

// four O_* bits sit at different values on x86-64 than on the asm-generic architectures arm64
// uses. everything below O_DSYNC agrees, so only these need moving; passing them through
// unchanged would silently turn an O_DIRECTORY open into O_LARGEFILE.
int TranslateOpenFlags(uint64_t GuestFlags) {
  constexpr uint64_t GuestO_DIRECT = 0x4000;
  constexpr uint64_t GuestO_LARGEFILE = 0x8000;
  constexpr uint64_t GuestO_DIRECTORY = 0x10000;
  constexpr uint64_t GuestO_NOFOLLOW = 0x20000;

  uint64_t Flags = GuestFlags & ~(GuestO_DIRECT | GuestO_LARGEFILE | GuestO_DIRECTORY | GuestO_NOFOLLOW);
  if (GuestFlags & GuestO_DIRECT) {
    Flags |= O_DIRECT;
  }
  if (GuestFlags & GuestO_DIRECTORY) {
    Flags |= O_DIRECTORY;
  }
  if (GuestFlags & GuestO_NOFOLLOW) {
    Flags |= O_NOFOLLOW;
  }
  // O_LARGEFILE is meaningless on a 64-bit host; bionic's is already implied.
  return static_cast<int>(Flags);
}

// PROT_EXEC never reaches the host kernel -- see VMA::HostProt, which is where the rule lives now.
//
// the guest cannot tell. it never reads back its own protections, mprotect reports success, and
// the VMA tracker remembers what was really asked for, so FEXCore is not fooled either.
int TranslateProt(uint64_t GuestProt) {
  return VMA::HostProt(static_cast<int>(GuestProt));
}

// MAP_32BIT and MAP_ABOVE4G are x86-only placement hints; those two bits mean nothing on the
// asm-generic arm64 flag set and would be rejected as unknown. everything else -- down to
// MAP_FIXED_NOREPLACE at 0x100000 -- holds the same value on both architectures.
int TranslateMapFlags(uint64_t GuestFlags) {
  constexpr uint64_t GuestMAP_32BIT = 0x40;
  constexpr uint64_t GuestMAP_ABOVE4G = 0x80;
  return static_cast<int>(GuestFlags & ~(GuestMAP_32BIT | GuestMAP_ABOVE4G));
}

constexpr uint64_t BrkArenaSize = 512ULL * 1024 * 1024;

uint64_t PageSize() {
  static const uint64_t Size = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
  return Size;
}

uint64_t AlignUp(uint64_t Value, uint64_t Alignment) {
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

// --- what the guest asks of one directory subtree ----------------------------------------------
//
// off unless --trace-files names a prefix, and the reason it exists is that a directory is not
// always reached by a path. when a game comes from a storage access framework grant rather than
// from a real path, every call counted here is one the host layer has to answer itself, out of a
// provider, across binder -- so these counts are what that costs, and the only way to compare the
// two ways of reaching the same game is to count both.
//
// the split is the point rather than the total. a descriptor the framework hands back is a real fd
// on a real file, so read, pread, lseek, mmap and fstat on one cost nothing extra and stay
// pass-throughs; it is the path-taking calls that become lookups, and a *directory*, which has no
// descriptor to hand back at all. hence the fd table -- it is the only way to tell a getdents on the
// game from a getdents on anything else, since the dispatcher otherwise never learns where an fd
// came from.
namespace FileProbe {

// the gate, and it is an atomic rather than the string below because every guest read, close and
// lseek passes it. one relaxed load and a branch that predicts perfectly -- the same cost the
// dispatcher already pays for `if (Trace)` on every syscall, and the same reason it is acceptable.
// written once before any guest thread exists, read from all of them.
std::atomic<bool> Active {false};

std::mutex Lock;
std::string Root;
std::unordered_map<int, bool> Tracked; // fd -> opened with O_DIRECTORY
std::set<std::string> Paths;
struct timespec LastReport {};

struct {
  uint64_t Opens, DirOpens, WriteOpens, OpenFails;
  uint64_t Stats, Accesses, Readlinks, Statfs;
  uint64_t Getdents, Reads, Writes, Seeks, Fstats, Mmaps;
  uint64_t ReadBytes;
} Count {};

// every entry point below takes one of these, after the gate and before anything that can fail.
// the probe prints, printf is allowed to set errno, and the caller's next statement is FromHost,
// which reads it -- so a probe that did not restore errno would turn a successful syscall into a
// random failure, and it would look like a host-layer bug rather than like the probe.
struct KeepErrno {
  int Saved {errno};
  ~KeepErrno() {
    errno = Saved;
  }
};

bool Enabled() {
  return Active.load(std::memory_order_relaxed);
}

bool Matches(const char* Path) {
  return Path != nullptr && Root.compare(0, Root.size(), Path, 0, Root.size()) == 0;
}

// every counter is behind the one lock. a diagnostic that races is a diagnostic that gets
// disbelieved, and this is nowhere near a hot path once the filter has rejected a call.
void Report(bool Force) {
  struct timespec Now {};
  ::clock_gettime(CLOCK_MONOTONIC, &Now);
  if (!Force && Now.tv_sec - LastReport.tv_sec < 5) {
    return;
  }
  LastReport = Now;
  std::printf("[files] paths=%zu opens=%llu (dir %llu, write %llu, failed %llu) stat=%llu access=%llu "
              "readlink=%llu statfs=%llu | getdents=%llu read=%llu (%llu KB) write=%llu lseek=%llu "
              "fstat=%llu mmap=%llu\n",
              Paths.size(), (unsigned long long)Count.Opens, (unsigned long long)Count.DirOpens,
              (unsigned long long)Count.WriteOpens, (unsigned long long)Count.OpenFails, (unsigned long long)Count.Stats,
              (unsigned long long)Count.Accesses, (unsigned long long)Count.Readlinks, (unsigned long long)Count.Statfs,
              (unsigned long long)Count.Getdents, (unsigned long long)Count.Reads,
              (unsigned long long)(Count.ReadBytes / 1024), (unsigned long long)Count.Writes,
              (unsigned long long)Count.Seeks, (unsigned long long)Count.Fstats, (unsigned long long)Count.Mmaps);
  std::fflush(stdout);
}

void OnOpen(const char* Path, uint64_t Flags, int64_t Result) {
  if (!Enabled() || !Matches(Path)) {
    return;
  }
  KeepErrno Restore;
  constexpr uint64_t GuestO_DIRECTORY = 0200000;
  constexpr uint64_t GuestO_WRONLY = 1, GuestO_RDWR = 2, GuestO_CREAT = 0100;
  const bool Directory = (Flags & GuestO_DIRECTORY) != 0;

  std::lock_guard Guard(Lock);
  ++Count.Opens;
  if (Directory) ++Count.DirOpens;
  if (Flags & (GuestO_WRONLY | GuestO_RDWR | GuestO_CREAT)) ++Count.WriteOpens;
  if (Result < 0) ++Count.OpenFails;

  // the distinct paths, capped: the question is how many files a game touches, and past a few
  // thousand the answer is "a lot" and the set is only costing memory.
  if (Paths.size() < 4096 && Paths.insert(Path).second) {
    std::printf("[files] open%s%s \"%s\" -> %lld\n", Directory ? " DIR" : "",
                (Flags & (GuestO_WRONLY | GuestO_RDWR | GuestO_CREAT)) ? " WRITE" : "", Path, (long long)Result);
    std::fflush(stdout);
  }
  if (Result >= 0) {
    Tracked[static_cast<int>(Result)] = Directory;
  }
  Report(false);
}

void OnPath(const char* Path, uint64_t& Counter) {
  if (!Enabled() || !Matches(Path)) {
    return;
  }
  KeepErrno Restore;
  std::lock_guard Guard(Lock);
  ++Counter;
  Report(false);
}

// true when this fd came from a path under the root, so the caller can count what was done to it.
bool OnFD(int FD, uint64_t& Counter, uint64_t Bytes = 0) {
  if (!Enabled()) {
    return false;
  }
  KeepErrno Restore;
  std::lock_guard Guard(Lock);
  auto Found = Tracked.find(FD);
  if (Found == Tracked.end()) {
    return false;
  }
  ++Counter;
  Count.ReadBytes += Bytes;
  Report(false);
  return true;
}

void OnClose(int FD) {
  if (!Enabled()) {
    return;
  }
  std::lock_guard Guard(Lock);
  Tracked.erase(FD);
}

} // namespace FileProbe

} // namespace

void LinuxSyscallHandler::SetFileProbeRoot(const char* Root) {
  std::lock_guard Guard(FileProbe::Lock);
  FileProbe::Root = Root ? Root : "";
  ::clock_gettime(CLOCK_MONOTONIC, &FileProbe::LastReport);
  // last, and after the string it guards is in place: from here on any guest thread may read it.
  FileProbe::Active.store(!FileProbe::Root.empty(), std::memory_order_relaxed);
  std::printf("[files] counting guest file access under \"%s\"\n", FileProbe::Root.c_str());
  std::fflush(stdout);
}

// up to FEX-2608 this set OSABI = OS_LINUX64, which was what made the JIT marshal guest
// RAX/RDI/... into the handler's arguments. FEX-2609 removed the OSABI selection along with the
// marshalling; the frame is read in HandleSyscall instead.
LinuxSyscallHandler::LinuxSyscallHandler() = default;

void LinuxSyscallHandler::SetBrkBase(uint64_t Base) {
  std::lock_guard Lock {BrkLock};
  BrkBase = BrkCurrent = Base;
  BrkArenaEnd = Base;

  // the guest heap gets a reservation of its own rather than being grown with plain mmap on
  // demand. brk must return contiguous memory, and in a 39-bit address space shared with
  // FEXCore, .NET and bionic there is no guarantee the next page past the image will still be
  // free by the time the guest asks for it. reserving PROT_NONE up front costs address space
  // and no memory.
  void* Arena = ::mmap(reinterpret_cast<void*>(Base), BrkArenaSize, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (Arena != MAP_FAILED && reinterpret_cast<uint64_t>(Arena) == Base) {
    BrkArenaEnd = Base + BrkArenaSize;
    // the whole reservation, PROT_NONE. brk hands pages out of it by mprotect, and each of those
    // re-records the range it touched -- so the tracker always describes the heap as the guest
    // sees it, with the untouched tail correctly unreadable rather than quietly executable.
    VMA::Record(Base, BrkArenaSize, PROT_NONE);
  } else if (Arena != MAP_FAILED) {
    // the kernel put the reservation somewhere else; brk has to be contiguous with the image,
    // so that is no use.
    ::munmap(Arena, BrkArenaSize);
  }
}

uint64_t LinuxSyscallHandler::HandleBrk(uint64_t NewBreak) {
  std::lock_guard Lock {BrkLock};
  // brk(0) is the idiomatic "where is the break?" query, and brk always returns the resulting
  // break rather than an error -- a failed request is reported by the break not having moved.
  if (NewBreak == 0 || NewBreak < BrkBase || NewBreak > BrkArenaEnd) {
    return BrkCurrent;
  }

  const uint64_t WantEnd = AlignUp(NewBreak, PageSize());
  const uint64_t HaveEnd = AlignUp(BrkCurrent, PageSize());
  if (WantEnd > HaveEnd) {
    if (::mprotect(reinterpret_cast<void*>(HaveEnd), WantEnd - HaveEnd, PROT_READ | PROT_WRITE) != 0) {
      return BrkCurrent;
    }
    VMA::Record(HaveEnd, WantEnd - HaveEnd, PROT_READ | PROT_WRITE);
  } else if (WantEnd < HaveEnd) {
    // shrinking: hand the pages back but keep the reservation, so the address range stays ours.
    ::mmap(reinterpret_cast<void*>(WantEnd), HaveEnd - WantEnd, PROT_NONE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    // back to PROT_NONE reservation, and anything compiled out of the freed pages goes with it.
    VMA::Record(WantEnd, HaveEnd - WantEnd, PROT_NONE);
    VMA::Invalidate(nullptr, WantEnd, HaveEnd - WantEnd);
  }

  BrkCurrent = NewBreak;
  return BrkCurrent;
}

FEXCore::HLE::ExecutableRangeInfo LinuxSyscallHandler::QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) {
  return VMA::Query(Address);
}

void LinuxSyscallHandler::MarkGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Start, uint64_t Length) {
  VMA::MarkExecutable(Start, Length);
}

void LinuxSyscallHandler::InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {
  // FEXCore calls this from more places than the name suggests, and one of them is load-bearing
  // for SMCChecks=full: the byte-comparison guard emitted into every block calls
  // `_ThreadRemoveCodeEntry`, which lands here. leaving it as the base class's empty default does
  // not merely lose an optimisation -- the guard detects the change, invalidates nothing, returns
  // to the same entrypoint, finds the same stale block and spins forever.
  VMA::Invalidate(Thread, Start, Length);
}

void LinuxSyscallHandler::HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) {
  // the whole of the FEX-2609 change in one place: the arguments come out of the spilled frame,
  // and the result goes back into it, because the JIT no longer does either -- and since
  // FEX-2609.1 so does the step past the `syscall` instruction, because the JIT now resumes from
  // CPUState::rip (see syscall_args.h). a path that does not return -- rt_sigreturn, a signal
  // delivered at the syscall's exit, a thread's exit -- writes the state it needs itself.
  auto Args = SyscallArguments::FromFrame(Frame);
  CompleteSyscall(Frame, Handle(Frame, &Args));
}

uint64_t LinuxSyscallHandler::Handle(FEXCore::Core::CpuStateFrame* Frame, HostLayer::SyscallArguments* Args) {
  auto* Self = static_cast<GuestThread*>(Frame->Thread->FrontendPtr);
  if (!Self) {
    return Dispatch(Frame, Args);
  }

  // the syscall is bracketed so that a pause can tell a thread that is blocked, and so already
  // stopped, from one running guest code -- see Threads::Pause. the exit half is also one of the two
  // places a paused thread parks, and the reason a call the pause interrupted is made again.
  //
  // only a real syscall is ever made twice. a thunk call is a library call: the library retries its
  // own EINTR, its result is a value of the library's that can equal -EINTR by coincidence, and the
  // vulkan thunk rewrites its own arguments on the way through.
  const uint64_t Number = Args->Argument[0];
  const bool Repeatable = !VulkanThunk::IsThunkCall(Number) && !AudioThunk::IsThunkCall(Number) &&
                          !PadBridge::IsThunkCall(Number);
  Threads::EnterSyscall(*Self);
  uint64_t Result = Dispatch(Frame, Args);
  while (Threads::LeaveSyscall(*Self, Result, Repeatable)) {
    Result = Dispatch(Frame, Args);
  }

  // a syscall boundary is the host layer's one unconditionally safe delivery point: CPUState
  // describes the guest exactly, no host lock is held, and an interrupted host call is still close
  // enough to its start to be restarted. so every syscall ends by asking whether a signal was
  // raised on this thread while it was somewhere it could not be redirected from -- which includes
  // the very common case of a thread parked in futex or poll, brought back with EINTR for exactly
  // this reason.
  //
  // it does not return if it delivers.
  Threads::DeliverPendingAtSyscallExit(*Self, Args->Argument[0], Result);
  return Result;
}

uint64_t LinuxSyscallHandler::Dispatch(FEXCore::Core::CpuStateFrame* Frame, HostLayer::SyscallArguments* Args) {
  const uint64_t Number = Args->Argument[0];

  // the vulkan thunk rides in on the syscall boundary rather than beside it, because the boundary
  // is what guarantees the guest's registers are all in CPUState -- which is where the thunk reads
  // its arguments from. the magic range is far above any real syscall number, so this test can
  // never shadow one. see vulkan_thunk.h.
  if (VulkanThunk::IsThunkCall(Number)) [[unlikely]] {
    return VulkanThunk::Handle(Frame, Args);
  }
  // and the audio thunk on the same boundary, for the same reason and in a magic range one along.
  // see audio_thunk.h.
  if (AudioThunk::IsThunkCall(Number)) [[unlikely]] {
    return AudioThunk::Handle(Frame, Args);
  }
  // and the pad bridge, one range along again. it is not a thunk -- nothing is forwarded to an NDK
  // library, the host layer answers it -- but it needs the same boundary for the same reason, since
  // that is where a guest thread's registers are all in CPUState. see pad_bridge.h.
  if (PadBridge::IsThunkCall(Number)) [[unlikely]] {
    return PadBridge::Handle(Frame, Args);
  }

  const uint64_t Arg0 = Args->Argument[1];
  const uint64_t Arg1 = Args->Argument[2];
  const uint64_t Arg2 = Args->Argument[3];
  const uint64_t Arg3 = Args->Argument[4];
  const uint64_t Arg4 = Args->Argument[5];
  const uint64_t Arg5 = Args->Argument[6];

  // the guest thread making this call, taken from the frame rather than from thread-local storage:
  // the frame is the syscall's own context and cannot describe anyone else. per-thread guest state
  // -- the signal mask, the alternate stack -- hangs off this.
  auto* Self = static_cast<GuestThread*>(Frame->Thread->FrontendPtr);

  if (Trace) {
    // path-taking syscalls get their path printed. a trace of bare pointers answers "how many
    // opens" and never "which file", and which file is the whole question when working out what
    // a guest's dynamic linker is searching for and failing to find.
    const char* Path = nullptr;
    switch (Number) {
    case SYS_x64_open:
    case SYS_x64_access:
    case SYS_x64_stat:
    case SYS_x64_lstat:
    case SYS_x64_readlink:
    case SYS_x64_statfs: Path = reinterpret_cast<const char*>(Arg0); break;
    case SYS_x64_openat:
    case SYS_x64_newfstatat:
    case SYS_x64_readlinkat:
    case SYS_x64_faccessat:
    case SYS_x64_faccessat2:
    case SYS_x64_statx:
    case SYS_x64_utimensat: Path = reinterpret_cast<const char*>(Arg1); break;
    default: break;
    }
    // the tid, because from here on there is more than one guest thread issuing syscalls and an
    // interleaved trace without it is unreadable -- two threads' library loads look like one
    // thread doing something incoherent.
    const int TID = ::gettid();
    if (Path) {
      std::printf("[syscall %d] %llu(\"%s\", 0x%llX, 0x%llX)\n", TID, static_cast<unsigned long long>(Number), Path,
                  static_cast<unsigned long long>(Arg2), static_cast<unsigned long long>(Arg3));
    } else {
      std::printf("[syscall %d] %llu(0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX)\n", TID,
                  static_cast<unsigned long long>(Number), static_cast<unsigned long long>(Arg0),
                  static_cast<unsigned long long>(Arg1), static_cast<unsigned long long>(Arg2),
                  static_cast<unsigned long long>(Arg3), static_cast<unsigned long long>(Arg4),
                  static_cast<unsigned long long>(Arg5));
    }
  }

  switch (Number) {
  // --- guest addresses are host addresses, so buffers pass straight through -----------------
  case SYS_x64_read: {
    const long Bytes = ::read(static_cast<int>(Arg0), reinterpret_cast<void*>(Arg1), Arg2);
    FileProbe::OnFD(static_cast<int>(Arg0), FileProbe::Count.Reads, Bytes > 0 ? Bytes : 0);
    return FromHost(Bytes);
  }
  // stdout and stderr go through the timestamper, which is a plain write unless --timestamps is on.
  // this is the only place SharpEmu's log ever reaches the outside world, so it is the only place a
  // stamp has to be applied to get a log the shape of the Windows release's.
  case SYS_x64_write:
    FileProbe::OnFD(static_cast<int>(Arg0), FileProbe::Count.Writes);
    return FromHost(GuestLog::Write(static_cast<int>(Arg0), reinterpret_cast<const void*>(Arg1), Arg2));
  case SYS_x64_pread64: {
    const long Bytes = ::pread(static_cast<int>(Arg0), reinterpret_cast<void*>(Arg1), Arg2, static_cast<off_t>(Arg3));
    FileProbe::OnFD(static_cast<int>(Arg0), FileProbe::Count.Reads, Bytes > 0 ? Bytes : 0);
    return FromHost(Bytes);
  }
  // struct iovec is {void* base; size_t len} on both architectures, so no translation.
  case SYS_x64_readv:
    return FromHost(::readv(static_cast<int>(Arg0), reinterpret_cast<const struct iovec*>(Arg1), static_cast<int>(Arg2)));
  case SYS_x64_writev:
    return FromHost(GuestLog::Writev(static_cast<int>(Arg0), reinterpret_cast<const struct iovec*>(Arg1), static_cast<int>(Arg2)));
  case SYS_x64_close:
    FileProbe::OnClose(static_cast<int>(Arg0));
    // the layer only ever holds *directories*, and forgetting one is all there is to do: the
    // descriptor underneath is a real one and the close below is what actually closes it.
    GuestFiles::Close(static_cast<int>(Arg0));
    return FromHost(::close(static_cast<int>(Arg0)));
  case SYS_x64_lseek:
    FileProbe::OnFD(static_cast<int>(Arg0), FileProbe::Count.Seeks);
    return FromHost(::lseek(static_cast<int>(Arg0), static_cast<off_t>(Arg1), static_cast<int>(Arg2)));
  case SYS_x64_dup: return FromHost(::dup(static_cast<int>(Arg0)));
  case SYS_x64_dup2: return FromHost(::dup2(static_cast<int>(Arg0), static_cast<int>(Arg1)));
  case SYS_x64_pipe2: return FromHost(::pipe2(reinterpret_cast<int*>(Arg0), static_cast<int>(Arg1)));
  // the F_* command numbers agree between the two architectures. what does not is the flag word
  // F_GETFL returns, which comes back in host O_* values -- a guest that inspects O_DIRECTORY or
  // O_NOFOLLOW in the result will read the wrong bit. left alone until something needs it.
  case SYS_x64_fcntl: return FromHost(::fcntl(static_cast<int>(Arg0), static_cast<int>(Arg1), Arg2));
  case SYS_x64_getcwd: {
    // getcwd's syscall returns the length written, where the libc wrapper returns the pointer.
    if (::getcwd(reinterpret_cast<char*>(Arg0), Arg1) == nullptr) {
      return static_cast<uint64_t>(-errno);
    }
    return std::strlen(reinterpret_cast<char*>(Arg0)) + 1;
  }
  case SYS_x64_getdents64:
    FileProbe::OnFD(static_cast<int>(Arg0), FileProbe::Count.Getdents);
    // a directory the file layer invented has nothing underneath it to enumerate, so the records
    // are written by hand. struct linux_dirent64 is byte-identical on both architectures, which is
    // why this is a synthesis and not also a translation.
    if (GuestFiles::OwnsFD(static_cast<int>(Arg0))) {
      return static_cast<uint64_t>(GuestFiles::GetDents(static_cast<int>(Arg0), reinterpret_cast<void*>(Arg1), Arg2));
    }
    return FromHost(::syscall(SYS_getdents64, Arg0, Arg1, Arg2));

  // --- creating and modifying files -----------------------------------------------------------
  //
  // .NET's single-file host extracts the runtime's native libraries beside itself before dlopen'ing
  // them, so the whole create/write/chmod/rename dance has to work. as with access/stat/lstat, the
  // legacy non-*at forms exist only on x86-64 and have to be routed onto the *at ones by hand.
  case SYS_x64_pwrite64:
    return FromHost(::pwrite(static_cast<int>(Arg0), reinterpret_cast<const void*>(Arg1), Arg2, static_cast<off_t>(Arg3)));
  case SYS_x64_creat:
    return FromHost(::openat(AT_FDCWD, reinterpret_cast<const char*>(Arg0), O_CREAT | O_WRONLY | O_TRUNC,
                             static_cast<mode_t>(Arg1)));
  case SYS_x64_mkdir: return FromHost(::mkdirat(AT_FDCWD, reinterpret_cast<const char*>(Arg0), static_cast<mode_t>(Arg1)));
  case SYS_x64_mkdirat:
    return FromHost(::mkdirat(static_cast<int>(Arg0), reinterpret_cast<const char*>(Arg1), static_cast<mode_t>(Arg2)));
  // the S_IF* type bits in the mode agree between the architectures, and so does dev_t.
  case SYS_x64_mknodat:
    return FromHost(::mknodat(static_cast<int>(Arg0), reinterpret_cast<const char*>(Arg1), static_cast<mode_t>(Arg2),
                              static_cast<dev_t>(Arg3)));
  case SYS_x64_rmdir: return FromHost(::unlinkat(AT_FDCWD, reinterpret_cast<const char*>(Arg0), AT_REMOVEDIR));
  case SYS_x64_unlink: return FromHost(::unlinkat(AT_FDCWD, reinterpret_cast<const char*>(Arg0), 0));
  case SYS_x64_unlinkat:
    return FromHost(::unlinkat(static_cast<int>(Arg0), reinterpret_cast<const char*>(Arg1), static_cast<int>(Arg2)));
  case SYS_x64_rename:
    return FromHost(::renameat(AT_FDCWD, reinterpret_cast<const char*>(Arg0), AT_FDCWD, reinterpret_cast<const char*>(Arg1)));
  case SYS_x64_renameat:
    return FromHost(::renameat(static_cast<int>(Arg0), reinterpret_cast<const char*>(Arg1), static_cast<int>(Arg2),
                               reinterpret_cast<const char*>(Arg3)));
  // RENAME_NOREPLACE and friends agree across architectures. bionic only declares renameat2 from
  // API 30, hence the raw syscall.
  case SYS_x64_renameat2: return FromHost(::syscall(SYS_renameat2, Arg0, Arg1, Arg2, Arg3, Arg4));
  case SYS_x64_link:
    return FromHost(::linkat(AT_FDCWD, reinterpret_cast<const char*>(Arg0), AT_FDCWD, reinterpret_cast<const char*>(Arg1), 0));
  case SYS_x64_linkat:
    return FromHost(::linkat(static_cast<int>(Arg0), reinterpret_cast<const char*>(Arg1), static_cast<int>(Arg2),
                             reinterpret_cast<const char*>(Arg3), static_cast<int>(Arg4)));
  case SYS_x64_symlink:
    return FromHost(::symlinkat(reinterpret_cast<const char*>(Arg0), AT_FDCWD, reinterpret_cast<const char*>(Arg1)));
  case SYS_x64_symlinkat:
    return FromHost(::symlinkat(reinterpret_cast<const char*>(Arg0), static_cast<int>(Arg1),
                                reinterpret_cast<const char*>(Arg2)));
  case SYS_x64_chmod:
    return FromHost(::fchmodat(AT_FDCWD, reinterpret_cast<const char*>(Arg0), static_cast<mode_t>(Arg1), 0));
  case SYS_x64_fchmod: return FromHost(::fchmod(static_cast<int>(Arg0), static_cast<mode_t>(Arg1)));
  case SYS_x64_fchmodat:
    return FromHost(::fchmodat(static_cast<int>(Arg0), reinterpret_cast<const char*>(Arg1), static_cast<mode_t>(Arg2), 0));
  case SYS_x64_fchownat:
    return FromHost(::fchownat(static_cast<int>(Arg0), reinterpret_cast<const char*>(Arg1), static_cast<uid_t>(Arg2),
                               static_cast<gid_t>(Arg3), static_cast<int>(Arg4)));
  case SYS_x64_truncate: return FromHost(::truncate(reinterpret_cast<const char*>(Arg0), static_cast<off_t>(Arg1)));
  case SYS_x64_ftruncate: return FromHost(::ftruncate(static_cast<int>(Arg0), static_cast<off_t>(Arg1)));
  case SYS_x64_fallocate:
    return FromHost(::fallocate(static_cast<int>(Arg0), static_cast<int>(Arg1), static_cast<off_t>(Arg2),
                                static_cast<off_t>(Arg3)));
  case SYS_x64_fsync: return FromHost(::fsync(static_cast<int>(Arg0)));
  case SYS_x64_fdatasync: return FromHost(::fdatasync(static_cast<int>(Arg0)));
  case SYS_x64_flock: return FromHost(::flock(static_cast<int>(Arg0), static_cast<int>(Arg1)));
  case SYS_x64_umask: return static_cast<uint64_t>(::umask(static_cast<mode_t>(Arg0)));
  case SYS_x64_chdir: {
    const char* Path = reinterpret_cast<const char*>(Arg0);
    // a working directory inside the mount is the one thing the file layer cannot answer for: the
    // kernel owns the cwd and there is no directory there for it to point at, so this fails with
    // ENOENT below. it says so rather than leaving a boot to fail somewhere further on, because
    // neither measured title does it and a third one that did would look like a broken dump.
    if (GuestFiles::OwnsAt(AT_FDCWD, Path)) {
      std::printf("[files] the guest tried to chdir into \"%s\", which the layer cannot answer for."
                  " every path it takes from here has to be an absolute one\n",
                  Path);
      std::fflush(stdout);
    }
    return FromHost(::chdir(Path));
  }
  case SYS_x64_dup3: return FromHost(::dup3(static_cast<int>(Arg0), static_cast<int>(Arg1), static_cast<int>(Arg2)));
  case SYS_x64_memfd_create: return FromHost(::syscall(SYS_memfd_create, Arg0, Arg1));
  // struct statfs is all 64-bit words on both LP64 architectures.
  case SYS_x64_statfs:
    FileProbe::OnPath(reinterpret_cast<const char*>(Arg0), FileProbe::Count.Statfs);
    return FromHost(::statfs(reinterpret_cast<const char*>(Arg0), reinterpret_cast<struct statfs*>(Arg1)));
  case SYS_x64_fstatfs: return FromHost(::fstatfs(static_cast<int>(Arg0), reinterpret_cast<struct statfs*>(Arg1)));
  // arm64 has no plain access/stat/lstat -- asm-generic dropped them in favour of the *at forms --
  // so on x86-64 these are the numbers glibc actually issues, and they have to be routed by hand.
  // ld.so probes /etc/ld.so.preload with access() before it does anything else.
  case SYS_x64_access:
  case SYS_x64_faccessat:
  case SYS_x64_faccessat2: {
    const int DirFD = Number == SYS_x64_access ? AT_FDCWD : static_cast<int>(Arg0);
    const char* Path = reinterpret_cast<const char*>(Number == SYS_x64_access ? Arg0 : Arg1);
    const uint64_t Mode = Number == SYS_x64_access ? Arg1 : Arg2;
    // only faccessat2 takes flags. the faccessat *syscall* is three arguments -- the fourth
    // argument bionic's wrapper has is a libc-level invention -- so reading Arg3 for it means
    // reading whatever the guest happened to leave in R10, and bionic rejects unknown flags
    // with EINVAL.
    const uint64_t Flags = Number == SYS_x64_faccessat2 ? Arg3 : 0;
    FileProbe::OnPath(Path, FileProbe::Count.Accesses);
    if (GuestFiles::OwnsAt(DirFD, Path)) {
      return static_cast<uint64_t>(GuestFiles::Access(DirFD, Path, static_cast<int>(Mode)));
    }
    if (const char* Substitute = Proc.Substitute(Path)) {
      Path = Substitute;
    }
    return FromHost(::faccessat(DirFD, Path, static_cast<int>(Mode), static_cast<int>(Flags)));
  }
  case SYS_x64_utimensat:
    return FromHost(::utimensat(static_cast<int>(Arg0), reinterpret_cast<const char*>(Arg1),
                                reinterpret_cast<const struct timespec*>(Arg2), static_cast<int>(Arg3)));
#ifdef SYS_statx
  // struct statx is fixed-width and identical on every architecture, so it needs no translation
  // -- unlike struct stat, which is the one that does.
  case SYS_x64_statx: {
    const char* Path = reinterpret_cast<const char*>(Arg1);
    FileProbe::OnPath(Path, FileProbe::Count.Stats);
    if (GuestFiles::OwnsAt(static_cast<int>(Arg0), Path)) {
      struct stat Host {};
      const int64_t Result = GuestFiles::Stat(static_cast<int>(Arg0), Path, static_cast<int>(Arg2), &Host);
      if (Result != 0) {
        return static_cast<uint64_t>(Result);
      }
      FillStatx(Host, reinterpret_cast<struct statx*>(Arg4));
      return 0;
    }
    return FromHost(::syscall(SYS_statx, Arg0, Arg1, Arg2, Arg3, Arg4));
  }
#endif

  // --- paths, with /proc/self answered about the guest rather than about us ------------------
  case SYS_x64_open:
  case SYS_x64_openat: {
    const int DirFD = Number == SYS_x64_open ? AT_FDCWD : static_cast<int>(Arg0);
    const char* Path = reinterpret_cast<const char*>(Number == SYS_x64_open ? Arg0 : Arg1);
    const uint64_t Flags = Number == SYS_x64_open ? Arg1 : Arg2;
    const uint64_t Mode = Number == SYS_x64_open ? Arg2 : Arg3;

    const int Synthetic = Proc.OpenSynthetic(Path);
    if (Synthetic >= 0) {
      return static_cast<uint64_t>(Synthetic);
    }
    // the game directory, when it came from a grant rather than from a path. what comes back for a
    // file is a real descriptor on a real file, which is why nothing downstream of here -- read,
    // pread, lseek, mmap, fstat -- had to learn anything about it.
    if (GuestFiles::OwnsAt(DirFD, Path)) {
      const int64_t Result = GuestFiles::Open(DirFD, Path, Flags);
      // the probe counts these exactly as it counts an open of a staged path, which is the only
      // reason the two ways of reaching the same game can be compared rather than argued about.
      FileProbe::OnOpen(Path, Flags, Result);
      return static_cast<uint64_t>(Result);
    }
    if (const char* Substitute = Proc.Substitute(Path)) {
      Path = Substitute;
    }
    const int Opened = ::openat(DirFD, Path, TranslateOpenFlags(Flags), static_cast<mode_t>(Mode));
    FileProbe::OnOpen(Path, Flags, Opened >= 0 ? Opened : -errno);
    return FromHost(Opened);
  }
  case SYS_x64_readlink:
  case SYS_x64_readlinkat: {
    const int DirFD = Number == SYS_x64_readlink ? AT_FDCWD : static_cast<int>(Arg0);
    const char* Path = reinterpret_cast<const char*>(Number == SYS_x64_readlink ? Arg0 : Arg1);
    char* Buffer = reinterpret_cast<char*>(Number == SYS_x64_readlink ? Arg1 : Arg2);
    const size_t Size = Number == SYS_x64_readlink ? Arg2 : Arg3;

    FileProbe::OnPath(Path, FileProbe::Count.Readlinks);
    // nothing under the mount is a symlink, because a document provider has none -- so this is
    // EINVAL for a file that is there and ENOENT for one that is not, which is what the kernel
    // would have said about a real directory with no symlinks in it.
    if (GuestFiles::OwnsAt(DirFD, Path)) {
      return static_cast<uint64_t>(GuestFiles::ReadLink(DirFD, Path));
    }
    if (const char* Target = Proc.ReadLinkTarget(Path)) {
      // readlink does not NUL-terminate and does not fail on truncation -- it writes what fits and
      // returns that. matching that exactly matters: the caller sizes its buffer from the result.
      const size_t Length = std::strlen(Target);
      const size_t Written = Length < Size ? Length : Size;
      std::memcpy(Buffer, Target, Written);
      return Written;
    }
    return FromHost(::readlinkat(DirFD, Path, Buffer, Size));
  }

  case SYS_x64_fstat: {
    struct stat Host {};
    FileProbe::OnFD(static_cast<int>(Arg0), FileProbe::Count.Fstats);
    // only a *directory* of the file layer's ever gets here. a file's descriptor is a real one and
    // bionic answers about it correctly; the memfd standing in for a directory would not, and a
    // guest checking S_ISDIR would conclude the directory it is holding open is not one.
    if (GuestFiles::OwnsFD(static_cast<int>(Arg0))) {
      const int64_t Result = GuestFiles::FStat(static_cast<int>(Arg0), &Host);
      if (Result != 0) {
        return static_cast<uint64_t>(Result);
      }
      TranslateStat(Host, reinterpret_cast<GuestStat*>(Arg1));
      return 0;
    }
    if (::fstat(static_cast<int>(Arg0), &Host) != 0) {
      return static_cast<uint64_t>(-errno);
    }
    TranslateStat(Host, reinterpret_cast<GuestStat*>(Arg1));
    return 0;
  }
  case SYS_x64_newfstatat: {
    struct stat Host {};
    const char* Path = reinterpret_cast<const char*>(Arg1);
    FileProbe::OnPath(Path, FileProbe::Count.Stats);
    if (GuestFiles::OwnsAt(static_cast<int>(Arg0), Path)) {
      const int64_t Result = GuestFiles::Stat(static_cast<int>(Arg0), Path, static_cast<int>(Arg3), &Host);
      if (Result != 0) {
        return static_cast<uint64_t>(Result);
      }
      TranslateStat(Host, reinterpret_cast<GuestStat*>(Arg2));
      return 0;
    }
    if (const char* Substitute = Proc.Substitute(Path)) {
      Path = Substitute;
    }
    // AT_EMPTY_PATH and AT_SYMLINK_NOFOLLOW hold the same values on both architectures.
    if (::fstatat(static_cast<int>(Arg0), Path, &Host, static_cast<int>(Arg3)) != 0) {
      return static_cast<uint64_t>(-errno);
    }
    TranslateStat(Host, reinterpret_cast<GuestStat*>(Arg2));
    return 0;
  }
  case SYS_x64_stat:
  case SYS_x64_lstat: {
    struct stat Host {};
    const char* Path = reinterpret_cast<const char*>(Arg0);
    FileProbe::OnPath(Path, FileProbe::Count.Stats);
    if (GuestFiles::OwnsAt(AT_FDCWD, Path)) {
      // stat and lstat are one answer here rather than being made into one: a document provider has
      // no symlinks, so there is never anything for AT_SYMLINK_NOFOLLOW to refuse to follow.
      const int64_t Result = GuestFiles::Stat(AT_FDCWD, Path, 0, &Host);
      if (Result != 0) {
        return static_cast<uint64_t>(Result);
      }
      TranslateStat(Host, reinterpret_cast<GuestStat*>(Arg1));
      return 0;
    }
    // lstat of /proc/self/exe would report the symlink itself, but the substitute is not a
    // symlink -- so an lstat of it answers about the guest binary. that is the more useful lie:
    // the only thing a caller learns from lstat'ing the link is its target's length, which it
    // gets from readlink anyway.
    if (const char* Substitute = Proc.Substitute(Path)) {
      Path = Substitute;
    }
    const int Flags = Number == SYS_x64_lstat ? AT_SYMLINK_NOFOLLOW : 0;
    if (::fstatat(AT_FDCWD, Path, &Host, Flags) != 0) {
      return static_cast<uint64_t>(-errno);
    }
    TranslateStat(Host, reinterpret_cast<GuestStat*>(Arg1));
    return 0;
  }

  // --- memory. PROT_* and MAP_* agree between x86-64 and arm64 apart from the two noted at
  // TranslateProt/TranslateMapFlags above ------------------------------------------------------
  //
  // every one of these tells the VMA tracker what happened, and every one of them tells it the
  // protection the *guest* asked for rather than the one bionic was given. the host kernel never
  // sees PROT_EXEC, so if the tracker is not told here the information does not exist anywhere.
  case SYS_x64_mmap: {
    FileProbe::OnFD(static_cast<int>(static_cast<int32_t>(Arg4)), FileProbe::Count.Mmaps);
    void* Result = ::mmap(reinterpret_cast<void*>(Arg0), Arg1, TranslateProt(Arg2), TranslateMapFlags(Arg3),
                          static_cast<int>(static_cast<int32_t>(Arg4)), static_cast<off_t>(Arg5));
    if (Result == MAP_FAILED) {
      return static_cast<uint64_t>(-errno);
    }
    VMA::Record(reinterpret_cast<uint64_t>(Result), Arg1, static_cast<int>(Arg2));
    return reinterpret_cast<uint64_t>(Result);
  }
  case SYS_x64_mremap: {
    void* Result = ::mremap(reinterpret_cast<void*>(Arg0), Arg1, Arg2, static_cast<int>(Arg3), reinterpret_cast<void*>(Arg4));
    if (Result == MAP_FAILED) {
      return static_cast<uint64_t>(-errno);
    }
    VMA::Remap(Arg0, Arg1, reinterpret_cast<uint64_t>(Result), Arg2);
    return reinterpret_cast<uint64_t>(Result);
  }
  case SYS_x64_mprotect: {
    const uint64_t Result = FromHost(::mprotect(reinterpret_cast<void*>(Arg0), Arg1, TranslateProt(Arg2)));
    if (Result == 0) {
      VMA::Reprotect(Arg0, Arg1, static_cast<int>(Arg2));
    }
    return Result;
  }
  case SYS_x64_munmap: {
    const uint64_t Result = FromHost(::munmap(reinterpret_cast<void*>(Arg0), Arg1));
    if (Result == 0) {
      VMA::Forget(Arg0, Arg1);
    }
    return Result;
  }
  case SYS_x64_madvise: {
    const uint64_t Result = FromHost(::madvise(reinterpret_cast<void*>(Arg0), Arg1, static_cast<int>(Arg2)));
    // MADV_DONTNEED does not unmap, so the mapping and its protection stand -- but the *contents*
    // are gone, and the next read of an anonymous page there gives zeroes. anything FEXCore
    // compiled out of those bytes is describing something that no longer exists.
    if (Result == 0 && static_cast<int>(Arg2) == MADV_DONTNEED) {
      VMA::Invalidate(Frame->Thread, Arg0, Arg1);
    }
    return Result;
  }
  case SYS_x64_msync: return FromHost(::msync(reinterpret_cast<void*>(Arg0), Arg1, static_cast<int>(Arg2)));
  case SYS_x64_mincore: return FromHost(::mincore(reinterpret_cast<void*>(Arg0), Arg1, reinterpret_cast<unsigned char*>(Arg2)));
  case SYS_x64_brk: return HandleBrk(Arg0);

  // CoreCLR's FlushProcessWriteBuffers wants a process-wide memory barrier. it asks membarrier
  // for one first and, if that is unavailable, falls back to touching a locked page's protection
  // -- which is why mlock sits next to it here rather than anywhere near the rest of the mm calls.
  // bionic declares neither membarrier nor its constants, hence the raw syscall.
  case SYS_x64_membarrier: return FromHost(::syscall(SYS_membarrier, Arg0, Arg1, Arg2));
  case SYS_x64_mlock: return FromHost(::mlock(reinterpret_cast<const void*>(Arg0), Arg1));
  case SYS_x64_munlock: return FromHost(::munlock(reinterpret_cast<const void*>(Arg0), Arg1));
  case SYS_x64_mlockall: return FromHost(::mlockall(static_cast<int>(Arg0)));
  case SYS_x64_munlockall: return FromHost(::munlockall());

  // --- thread-local storage ------------------------------------------------------------------
  // in 64-bit mode FEX keeps the FS and GS bases in CPUState rather than in a descriptor, and
  // reads them from there for every segment-prefixed access. writing them here is the whole of
  // arch_prctl. this works from inside a syscall because the JIT spills all statically
  // allocated registers to CPUState before calling us and refills them after -- so guest state
  // is genuinely live in memory for the duration of this function.
  case SYS_x64_arch_prctl: {
    constexpr uint64_t ARCH_SET_GS = 0x1001, ARCH_SET_FS = 0x1002, ARCH_GET_FS = 0x1003, ARCH_GET_GS = 0x1004;
    switch (Arg0) {
    case ARCH_SET_GS: Frame->State.gs_cached = Arg1; return 0;
    case ARCH_SET_FS: Frame->State.fs_cached = Arg1; return 0;
    case ARCH_GET_FS: *reinterpret_cast<uint64_t*>(Arg1) = Frame->State.fs_cached; return 0;
    case ARCH_GET_GS: *reinterpret_cast<uint64_t*>(Arg1) = Frame->State.gs_cached; return 0;
    default: return static_cast<uint64_t>(-EINVAL);
    }
  }

  // --- identity and misc ---------------------------------------------------------------------
  case SYS_x64_getpid: return static_cast<uint64_t>(::getpid());
  case SYS_x64_getppid: return static_cast<uint64_t>(::getppid());
  case SYS_x64_getpgrp: return static_cast<uint64_t>(::getpgrp());
  case SYS_x64_getpgid: return FromHost(::getpgid(static_cast<pid_t>(Arg0)));
  case SYS_x64_getsid: return FromHost(::getsid(static_cast<pid_t>(Arg0)));
  case SYS_x64_gettid: return static_cast<uint64_t>(::gettid());
  case SYS_x64_getuid: return static_cast<uint64_t>(::getuid());
  case SYS_x64_geteuid: return static_cast<uint64_t>(::geteuid());
  case SYS_x64_getgid: return static_cast<uint64_t>(::getgid());
  case SYS_x64_getegid: return static_cast<uint64_t>(::getegid());
  case SYS_x64_sched_yield: return FromHost(::sched_yield());
  case SYS_x64_getrandom: return FromHost(::getrandom(reinterpret_cast<void*>(Arg0), Arg1, static_cast<unsigned int>(Arg2)));
  case SYS_x64_ioctl:
    return FromHost(::syscall(SYS_ioctl, static_cast<int>(Arg0), Arg1, Arg2));
  // struct timespec, timeval and sysinfo are all plain 64-bit words on both architectures.
  case SYS_x64_clock_gettime: {
    auto* Time = reinterpret_cast<struct timespec*>(Arg1);
    const uint64_t Result = FromHost(::clock_gettime(static_cast<clockid_t>(Arg0), Time));
    // the pauses are taken out of the monotonic clocks -- see IsMonotonic. a run that has never
    // paused pays one load and a branch that is never taken.
    if (const uint64_t Paused = Threads::PausedNanos(); Paused && Result == 0 && Time &&
                                                        IsMonotonic(static_cast<clockid_t>(Arg0))) {
      ShiftTimespec(*Time, -static_cast<int64_t>(Paused));
    }
    return Result;
  }
  case SYS_x64_clock_getres:
    return FromHost(::clock_getres(static_cast<clockid_t>(Arg0), reinterpret_cast<struct timespec*>(Arg1)));
  case SYS_x64_clock_nanosleep: {
    // a deadline on the guest's monotonic clock is that much later on the host's. clock_nanosleep
    // reports failure by its return value rather than through errno, hence no FromHost.
    const auto* Asked = reinterpret_cast<const struct timespec*>(Arg2);
    struct timespec Deadline {};
    if (const uint64_t Paused = Threads::PausedNanos(); Paused && Asked && (Arg1 & TIMER_ABSTIME) &&
                                                        IsMonotonic(static_cast<clockid_t>(Arg0))) {
      Deadline = *Asked;
      ShiftTimespec(Deadline, static_cast<int64_t>(Paused));
      Asked = &Deadline;
    }
    return static_cast<uint64_t>(-static_cast<int64_t>(::clock_nanosleep(
      static_cast<clockid_t>(Arg0), static_cast<int>(Arg1), Asked, reinterpret_cast<struct timespec*>(Arg3))));
  }
  case SYS_x64_nanosleep:
    return FromHost(::nanosleep(reinterpret_cast<const struct timespec*>(Arg0), reinterpret_cast<struct timespec*>(Arg1)));
  case SYS_x64_gettimeofday:
    return FromHost(::gettimeofday(reinterpret_cast<struct timeval*>(Arg0), nullptr));
  case SYS_x64_time: {
    const time_t Now = ::time(nullptr);
    if (Arg0) {
      *reinterpret_cast<int64_t*>(Arg0) = Now;
    }
    return static_cast<uint64_t>(Now);
  }
  case SYS_x64_sysinfo: return FromHost(::sysinfo(reinterpret_cast<struct sysinfo*>(Arg0)));
  case SYS_x64_getrlimit: return FromHost(::getrlimit(static_cast<int>(Arg0), reinterpret_cast<struct rlimit*>(Arg1)));
  case SYS_x64_setrlimit: return FromHost(::setrlimit(static_cast<int>(Arg0), reinterpret_cast<const struct rlimit*>(Arg1)));
  // struct rusage and struct tms are all longs on both LP64 architectures, and the RLIMIT_* and
  // RUSAGE_* numbers agree, so these pass through.
  case SYS_x64_getrusage: return FromHost(::getrusage(static_cast<int>(Arg0), reinterpret_cast<struct rusage*>(Arg1)));
  case SYS_x64_times: return FromHost(::times(reinterpret_cast<struct tms*>(Arg0)));

  // PR_* option numbers are architecture-independent, including android's own PR_SET_VMA -- and
  // since the host is android too, a guest naming its mappings gets exactly what it asked for.
  case SYS_x64_prctl: {
    const uint64_t Result = FromHost(::prctl(static_cast<int>(Arg0), Arg1, Arg2, Arg3, Arg4));
    // the guest names its own threads, and nothing else in a log ties a tid to a name. it rides
    // with --log-tids because it answers the same question and is only wanted by the same caller:
    // note that this sees the runtime's threads and not the ones the emulator names itself, which
    // are tracked inside it and never reach a prctl.
    if (HostLayer::GuestLog::ThreadIdsEnabled() && Result == 0 && static_cast<int>(Arg0) == PR_SET_NAME && Arg1) {
      char Name[17] {};
      std::memcpy(Name, reinterpret_cast<const void*>(Arg1), sizeof(Name) - 1);
      std::printf("[host-layer] thread %d is named '%s'\n", static_cast<int>(::gettid()), Name);
      std::fflush(stdout);
    }
    return Result;
  }

  // scheduling. the guest is one host thread, so these are honest pass-throughs; cpu_set_t is a
  // plain bitmask with the same representation on both.
  case SYS_x64_sched_getscheduler: return FromHost(::sched_getscheduler(static_cast<pid_t>(Arg0)));
  case SYS_x64_sched_setscheduler:
    return FromHost(::sched_setscheduler(static_cast<pid_t>(Arg0), static_cast<int>(Arg1),
                                         reinterpret_cast<const struct sched_param*>(Arg2)));
  case SYS_x64_sched_getparam:
    return FromHost(::sched_getparam(static_cast<pid_t>(Arg0), reinterpret_cast<struct sched_param*>(Arg1)));
  case SYS_x64_sched_getaffinity: return FromHost(::syscall(SYS_sched_getaffinity, Arg0, Arg1, Arg2));
  case SYS_x64_sched_setaffinity: return FromHost(::syscall(SYS_sched_setaffinity, Arg0, Arg1, Arg2));
  // the SCHED_* policy numbers agree between the architectures, so the priority bounds do too.
  // .NET asks for these when it maps managed thread priorities onto the host's.
  case SYS_x64_sched_get_priority_max: return FromHost(::sched_get_priority_max(static_cast<int>(Arg0)));
  case SYS_x64_sched_get_priority_min: return FromHost(::sched_get_priority_min(static_cast<int>(Arg0)));

  // sockets. bionic's logging talks to /dev/socket/logdw over AF_UNIX, and .NET's diagnostics
  // server listens on an AF_UNIX socket of its own in TMPDIR. sockaddr, msghdr and iovec all have
  // identical layouts on the two architectures and the SOL_*/SO_* numbers agree, so these forward
  // rather than being stubbed -- a guest that cannot open a socket usually just gives up quietly,
  // which hides things.
  case SYS_x64_socket: return FromHost(::socket(static_cast<int>(Arg0), static_cast<int>(Arg1), static_cast<int>(Arg2)));
  case SYS_x64_socketpair:
    return FromHost(::socketpair(static_cast<int>(Arg0), static_cast<int>(Arg1), static_cast<int>(Arg2), reinterpret_cast<int*>(Arg3)));
  case SYS_x64_connect:
    return FromHost(::connect(static_cast<int>(Arg0), reinterpret_cast<const struct sockaddr*>(Arg1), static_cast<socklen_t>(Arg2)));
  case SYS_x64_bind:
    return FromHost(::bind(static_cast<int>(Arg0), reinterpret_cast<const struct sockaddr*>(Arg1), static_cast<socklen_t>(Arg2)));
  case SYS_x64_listen: return FromHost(::listen(static_cast<int>(Arg0), static_cast<int>(Arg1)));
  case SYS_x64_accept:
    return FromHost(::accept(static_cast<int>(Arg0), reinterpret_cast<struct sockaddr*>(Arg1), reinterpret_cast<socklen_t*>(Arg2)));
  case SYS_x64_accept4:
    return FromHost(::accept4(static_cast<int>(Arg0), reinterpret_cast<struct sockaddr*>(Arg1), reinterpret_cast<socklen_t*>(Arg2),
                              static_cast<int>(Arg3)));
  case SYS_x64_getsockname:
    return FromHost(::getsockname(static_cast<int>(Arg0), reinterpret_cast<struct sockaddr*>(Arg1), reinterpret_cast<socklen_t*>(Arg2)));
  case SYS_x64_getpeername:
    return FromHost(::getpeername(static_cast<int>(Arg0), reinterpret_cast<struct sockaddr*>(Arg1), reinterpret_cast<socklen_t*>(Arg2)));
  case SYS_x64_setsockopt:
    return FromHost(::setsockopt(static_cast<int>(Arg0), static_cast<int>(Arg1), static_cast<int>(Arg2),
                                 reinterpret_cast<const void*>(Arg3), static_cast<socklen_t>(Arg4)));
  case SYS_x64_getsockopt:
    return FromHost(::getsockopt(static_cast<int>(Arg0), static_cast<int>(Arg1), static_cast<int>(Arg2), reinterpret_cast<void*>(Arg3),
                                 reinterpret_cast<socklen_t*>(Arg4)));
  case SYS_x64_shutdown: return FromHost(::shutdown(static_cast<int>(Arg0), static_cast<int>(Arg1)));
  case SYS_x64_sendto:
    return FromHost(::sendto(static_cast<int>(Arg0), reinterpret_cast<const void*>(Arg1), Arg2, static_cast<int>(Arg3),
                             reinterpret_cast<const struct sockaddr*>(Arg4), static_cast<socklen_t>(Arg5)));
  case SYS_x64_recvfrom:
    return FromHost(::recvfrom(static_cast<int>(Arg0), reinterpret_cast<void*>(Arg1), Arg2, static_cast<int>(Arg3),
                               reinterpret_cast<struct sockaddr*>(Arg4), reinterpret_cast<socklen_t*>(Arg5)));
  case SYS_x64_sendmsg:
    return FromHost(::sendmsg(static_cast<int>(Arg0), reinterpret_cast<const struct msghdr*>(Arg1), static_cast<int>(Arg2)));
  case SYS_x64_recvmsg:
    return FromHost(::recvmsg(static_cast<int>(Arg0), reinterpret_cast<struct msghdr*>(Arg1), static_cast<int>(Arg2)));
  // struct rlimit64 is two 64-bit words on both architectures.
  case SYS_x64_prlimit64: return FromHost(::syscall(SYS_prlimit64, Arg0, Arg1, Arg2, Arg3));
  // FUTEX_* operation codes are architecture-independent.
  case SYS_x64_futex: {
    // an absolute deadline on the guest's monotonic clock is that much later on the host's -- the
    // shape CoreCLR's own timed waits take, since its condition variables run on CLOCK_MONOTONIC.
    uint64_t Timeout = Arg3;
    struct timespec Deadline {};
    if (const uint64_t Paused = Threads::PausedNanos(); Paused && Timeout && FutexHasMonotonicDeadline(Arg1)) {
      Deadline = *reinterpret_cast<const struct timespec*>(Timeout);
      ShiftTimespec(Deadline, static_cast<int64_t>(Paused));
      Timeout = reinterpret_cast<uint64_t>(&Deadline);
    }
    return FromHost(::syscall(SYS_futex, Arg0, Arg1, Arg2, Timeout, Arg4, Arg5));
  }

  case SYS_x64_uname: {
    // new_utsname is six 65-byte fields on both architectures; only the contents are ours to
    // choose. the machine string is the one that matters -- a guest that reads "aarch64" here
    // will make wrong decisions about the very code it is running.
    auto* Guest = reinterpret_cast<char(*)[65]>(Arg0);
    std::memset(Guest, 0, 65 * 6);
    std::strcpy(Guest[0], "Linux");
    std::strcpy(Guest[1], "localhost");
    std::strcpy(Guest[2], "6.6.0");
    std::strcpy(Guest[3], "#1 SMP sharpdroid");
    std::strcpy(Guest[4], "x86_64");
    std::strcpy(Guest[5], "(none)");
    return 0;
  }

  // --- signals ----------------------------------------------------------------------------------
  // these are the guest's own, tracked by the host layer and never installed on the host: the
  // host's SIGSEGV handler belongs to us, and handing it to the guest is what guest_signals.cpp
  // does. sigsetsize (Arg3) is checked because a guest passing anything but 8 is using an ABI we
  // are not implementing.
  case SYS_x64_rt_sigaction:
    if (Arg3 != 8) {
      return static_cast<uint64_t>(-EINVAL);
    }
    // process-wide: a handler installed on one thread is the handler every thread runs.
    return Guest->SigAction(static_cast<int>(Arg0), reinterpret_cast<const GuestABI::SigAction*>(Arg1),
                            reinterpret_cast<GuestABI::SigAction*>(Arg2));
  case SYS_x64_rt_sigprocmask:
    if (Arg3 != 8) {
      return static_cast<uint64_t>(-EINVAL);
    }
    // per-thread: pthread_sigmask and sigprocmask are the same syscall, and it has only ever
    // affected the calling thread.
    return Guest->SigProcMask(*Self, static_cast<int>(Arg0), reinterpret_cast<const uint64_t*>(Arg1),
                              reinterpret_cast<uint64_t*>(Arg2));
  case SYS_x64_sigaltstack:
    return Guest->SigAltStack(*Self, reinterpret_cast<const GuestABI::AltStack*>(Arg0),
                              reinterpret_cast<GuestABI::AltStack*>(Arg1));
  case SYS_x64_rt_sigreturn:
    // this one never returns a value to the guest: it *replaces* guest state wholesale, so
    // there is nothing to put in RAX and nowhere in this block to carry on from. RestartCurrent
    // re-dispatches from the restored RIP and does not come back.
    Guest->RestoreFromFrame(*Self);
    // the frame carried the mask the handler was entered with, so restoring it can unblock a
    // signal that arrived while the handler was running. that makes this a delivery point, and
    // missing it would leave the signal waiting for whatever the guest happened to do next.
    Threads::DeliverPendingNow(*Self);
    Threads::RestartCurrent();

  // --- waiting on file descriptors ---------------------------------------------------------------
  //
  // CoreCLR's PAL runs a dedicated signal-handling thread that blocks in poll() on a self-pipe
  // forever, so the very first thing the first cloned thread does is call this. struct pollfd,
  // fd_set, epoll_event and struct timeval are identical on x86-64 and arm64.
  //
  // the *p* variants take a signal mask, and it is deliberately dropped rather than forwarded.
  // guest signals are emulated entirely inside the host layer -- no guest handler is ever installed
  // on the host -- so the guest's mask and the host's have no relationship at all. handing the
  // guest's mask to the kernel would not change what the guest sees; it would blindfold the host
  // layer's own SIGSEGV handler, which is the one thing that must never be blocked.
  case SYS_x64_poll:
    return FromHost(::poll(reinterpret_cast<struct pollfd*>(Arg0), static_cast<nfds_t>(Arg1), static_cast<int>(Arg2)));
  case SYS_x64_ppoll:
    return FromHost(::ppoll(reinterpret_cast<struct pollfd*>(Arg0), static_cast<nfds_t>(Arg1),
                            reinterpret_cast<const struct timespec*>(Arg2), nullptr));
  case SYS_x64_select:
    return FromHost(::select(static_cast<int>(Arg0), reinterpret_cast<fd_set*>(Arg1), reinterpret_cast<fd_set*>(Arg2),
                             reinterpret_cast<fd_set*>(Arg3), reinterpret_cast<struct timeval*>(Arg4)));
  case SYS_x64_pselect6:
    return FromHost(::pselect(static_cast<int>(Arg0), reinterpret_cast<fd_set*>(Arg1), reinterpret_cast<fd_set*>(Arg2),
                              reinterpret_cast<fd_set*>(Arg3), reinterpret_cast<const struct timespec*>(Arg4), nullptr));
  case SYS_x64_eventfd2: return FromHost(::eventfd(static_cast<unsigned>(Arg0), static_cast<int>(Arg1)));
  case SYS_x64_epoll_create1: return FromHost(::epoll_create1(static_cast<int>(Arg0)));
  case SYS_x64_epoll_ctl:
    return FromHost(::epoll_ctl(static_cast<int>(Arg0), static_cast<int>(Arg1), static_cast<int>(Arg2),
                                reinterpret_cast<struct epoll_event*>(Arg3)));
  case SYS_x64_epoll_wait:
  case SYS_x64_epoll_pwait:
    return FromHost(::epoll_wait(static_cast<int>(Arg0), reinterpret_cast<struct epoll_event*>(Arg1), static_cast<int>(Arg2),
                                 static_cast<int>(Arg3)));

  // --- threads ----------------------------------------------------------------------------------
  //
  // the x86-64 argument order, which is not x86-32's: x86-32 selects CLONE_BACKWARDS in the kernel
  // and swaps tls with child_tid. taking the 32-bit order here would put a TLS pointer where the
  // CLONE_CHILD_CLEARTID word belongs, and the first pthread_join would wait on garbage.
  case SYS_x64_clone:
    return Threads::Clone(Frame, Arg0, Arg1, reinterpret_cast<int32_t*>(Arg2), reinterpret_cast<int32_t*>(Arg3), Arg4);
  // deliberately not implemented. glibc probes clone3 first and falls back to clone on ENOSYS all
  // by itself -- the trace confirms it does -- so implementing a second entry point into the same
  // machinery would buy nothing but a second way to get the argument marshalling wrong.
  case SYS_x64_clone3: return static_cast<uint64_t>(-ENOSYS);
  case SYS_x64_set_tid_address: return Threads::SetTidAddress(reinterpret_cast<int32_t*>(Arg0));

  // --- accepted and ignored --------------------------------------------------------------------
  // the robust futex list is glibc's own crash-recovery bookkeeping for pthread mutexes: the
  // kernel walks it if a thread dies holding one. our threads only die by asking to, so there is
  // nothing to recover, and accepting the registration silently is what a guest expects.
  case SYS_x64_set_robust_list: return 0;
  case SYS_x64_rseq: return static_cast<uint64_t>(-ENOSYS);

  // --- leaving --------------------------------------------------------------------------------
  // exit ends one guest thread; exit_group ends the process. before threads these were the same
  // thing and it did not matter which was which -- now the first is how every pthread finishes.
  case SYS_x64_exit: Threads::ExitCurrent(static_cast<int>(Arg0), false);
  case SYS_x64_exit_group: Threads::ExitCurrent(static_cast<int>(Arg0), true);
  // --- raising a signal -------------------------------------------------------------------------
  //
  // raise(), abort(), pthread_kill() and CoreCLR's activation injection all arrive here. none of
  // them delivers anything at this point, self-directed or not: raising records a bit on the target
  // thread and, if that is some other thread, pokes it. the signal is delivered where the target
  // can safely take it, which for this thread is the exit check a few lines below in HandleSyscall.
  case SYS_x64_tgkill: return Threads::SignalGuestThread(static_cast<int32_t>(Arg1), static_cast<int>(Arg2));
  case SYS_x64_tkill: return Threads::SignalGuestThread(static_cast<int32_t>(Arg0), static_cast<int>(Arg1));
  case SYS_x64_kill: {
    // a process-directed signal. linux delivers it to any one thread that is not blocking it; the
    // calling thread is always an acceptable choice and is the one raise() means. if that thread
    // happens to be blocking the signal it stays pending on it rather than moving to another,
    // which is a simplification and the only one here.
    const auto Target = static_cast<int32_t>(Arg0);
    if (Target == ::getpid() || Target == 0) {
      return Threads::SignalGuestThread(Self->TID, static_cast<int>(Arg1));
    }
    // some other process, and its pid means the same thing to us as to the guest: one address
    // space, one pid namespace.
    return FromHost(::kill(static_cast<pid_t>(Target), static_cast<int>(Arg1)));
  }

  default:
    Unhandled.fetch_add(1, std::memory_order_relaxed);
    LastUnhandled.store(Number, std::memory_order_relaxed);
    std::printf("[syscall %d] UNHANDLED %llu(0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX) rip=0x%llX\n", ::gettid(),
                static_cast<unsigned long long>(Number), static_cast<unsigned long long>(Arg0),
                static_cast<unsigned long long>(Arg1), static_cast<unsigned long long>(Arg2),
                static_cast<unsigned long long>(Arg3), static_cast<unsigned long long>(Arg4),
                static_cast<unsigned long long>(Arg5), static_cast<unsigned long long>(Frame->State.rip));
    return static_cast<uint64_t>(-ENOSYS);
  }
}

} // namespace HostLayer
