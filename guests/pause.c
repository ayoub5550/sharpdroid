// a static x86-64 guest that checks it was paused properly, from the inside.
//
// it is run with `--pause-selftest`, which has the host layer pause the guest for half a second a
// little after it starts. the guest has no way to be told, and that is the point: what it can see
// is everything a game would see, and it passes only if all of that is right.
//
// two things are checked, from the two clocks a linux program has across a suspend:
//
//   1. **the pause happened, and the monotonic clock left it out.** CLOCK_BOOTTIME carries on
//      through a pause and CLOCK_MONOTONIC does not, so one short sleep somewhere in the loop below
//      takes half a second by the first and a few milliseconds by the second. no such sleep is a
//      fail, which is what makes running this without the self-test a negative control.
//   2. **a thread spinning in translated code was stopped too.** it makes no syscalls at all once it
//      is running, so the only thing that can stop it is the host layer's poke landing inside a
//      block. its counter is compared across the sleep the pause fell in: had it gone on spinning
//      it would have advanced for the whole half second rather than for the few milliseconds the
//      monotonic clock says passed.
//
// no libc, for the reason the other freestanding guests have none: what is under test is the host
// layer against the raw kernel ABI.

typedef unsigned long u64;
typedef long i64;

#define SYS_write 1
#define SYS_mmap 9
#define SYS_nanosleep 35
#define SYS_clone 56
#define SYS_clock_gettime 228
#define SYS_exit_group 231

#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME 7

#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000

static long Syscall6(long Number, long A, long B, long C, long D, long E, long F) {
  long Result;
  register long R10 __asm__("r10") = D;
  register long R8 __asm__("r8") = E;
  register long R9 __asm__("r9") = F;
  __asm__ volatile("syscall"
                   : "=a"(Result)
                   : "a"(Number), "D"(A), "S"(B), "d"(C), "r"(R10), "r"(R8), "r"(R9)
                   : "rcx", "r11", "memory");
  return Result;
}

static long Syscall4(long Number, long A, long B, long C, long D) {
  return Syscall6(Number, A, B, C, D, 0, 0);
}

static u64 StringLength(const char* Text) {
  u64 Length = 0;
  while (Text[Length]) {
    ++Length;
  }
  return Length;
}

static void Print(const char* Text) {
  Syscall4(SYS_write, 1, (long)Text, (long)StringLength(Text), 0);
}

static void PrintDec(u64 Value) {
  char Buffer[21];
  int Index = 20;
  Buffer[Index] = 0;
  do {
    Buffer[--Index] = (char)('0' + (Value % 10));
    Value /= 10;
  } while (Value);
  Print(&Buffer[Index]);
}

struct TimeSpec {
  i64 Seconds;
  i64 Nanoseconds;
};

static u64 Now(int Clock) {
  struct TimeSpec Time;
  Syscall4(SYS_clock_gettime, Clock, (long)&Time, 0, 0);
  return (u64)Time.Seconds * 1000000000ul + (u64)Time.Nanoseconds;
}

static void SleepMilliseconds(long Milliseconds) {
  struct TimeSpec Request;
  Request.Seconds = Milliseconds / 1000;
  Request.Nanoseconds = (Milliseconds % 1000) * 1000000;
  Syscall4(SYS_nanosleep, (long)&Request, 0, 0, 0);
}

// --- the spinning thread ----------------------------------------------------------------------

#define THREAD_STACK_SIZE (256 * 1024)

static volatile u64 SpinCounter = 0;
static volatile int Stop = 0;

// clone with a function to run, done by hand -- see asyncsig.c, where the stack arithmetic is
// explained.
extern long StartThread(u64 Flags, void* StackTop, void* Entry);
__asm__(".globl StartThread\n"
        "StartThread:\n"
        "  mov %rdx, %r9\n"
        "  mov %rsi, %r10\n"
        "  sub $24, %r10\n"
        "  mov %r9, (%r10)\n"
        "  mov %rdi, %r11\n"
        "  mov $56, %eax\n"
        "  mov %r11, %rdi\n"
        "  mov %r10, %rsi\n"
        "  xor %rdx, %rdx\n"
        "  xor %r10, %r10\n"
        "  xor %r8, %r8\n"
        "  syscall\n"
        "  test %rax, %rax\n"
        "  jnz 1f\n"
        "  pop %rax\n"
        "  xor %rbp, %rbp\n"
        "  call *%rax\n"
        "  mov $60, %eax\n"
        "  xor %edi, %edi\n"
        "  syscall\n"
        "  hlt\n"
        "1:\n"
        "  ret\n");

// nothing but translated guest code once it is running: the only way into it is an interrupt
// landing inside a block.
static void SpinWorker(void) {
  while (!Stop) {
    SpinCounter = SpinCounter + 1;
  }
}

// --- the check ------------------------------------------------------------------------------------

// long enough on the monotonic clock to reach well past where the self-test pauses, which is a few
// hundred milliseconds after the guest starts.
#define RUN_MS 1200
#define STEP_MS 5
// a pause shorter than this does not count as the one the self-test made, which is half a second.
#define PAUSE_SEEN_MS 300

void _start(void) {
  const long Stack = Syscall6(SYS_mmap, 0, THREAD_STACK_SIZE, 3, 0x22, -1, 0);
  if (Stack <= 0) {
    Print("[guest] mmap failed\n");
    Syscall4(SYS_exit_group, 1, 0, 0, 0);
  }
  StartThread(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD,
              (void*)(Stack + THREAD_STACK_SIZE), (void*)&SpinWorker);

  // the step the pause fell in, and the counter's rate over every step that had no pause in it.
  u64 PausedFor = 0, PausedStepMono = 0, PausedStepSpins = 0;
  u64 QuietMono = 0, QuietSpins = 0;

  const u64 MonoStart = Now(CLOCK_MONOTONIC);
  u64 Mono = MonoStart;
  u64 Boot = Now(CLOCK_BOOTTIME);
  u64 Spins = SpinCounter;
  while (Mono - MonoStart < (u64)RUN_MS * 1000000ul) {
    SleepMilliseconds(STEP_MS);
    const u64 NextMono = Now(CLOCK_MONOTONIC);
    const u64 NextBoot = Now(CLOCK_BOOTTIME);
    const u64 NextSpins = SpinCounter;
    const u64 MonoStep = NextMono - Mono;
    const u64 BootStep = NextBoot - Boot;
    const u64 Hidden = BootStep > MonoStep ? BootStep - MonoStep : 0;
    if (Hidden > PausedFor) {
      PausedFor = Hidden;
      PausedStepMono = MonoStep;
      PausedStepSpins = NextSpins - Spins;
    } else if (Hidden < 2000000ul) {
      QuietMono += MonoStep;
      QuietSpins += NextSpins - Spins;
    }
    Mono = NextMono;
    Boot = NextBoot;
    Spins = NextSpins;
  }
  Stop = 1;

  Print("[guest] longest stretch the monotonic clock left out: ");
  PrintDec(PausedFor / 1000000ul);
  Print(" ms\n");
  if (PausedFor < (u64)PAUSE_SEEN_MS * 1000000ul) {
    Print("[guest] FAIL: no pause was seen -- this guest is meant to be run with --pause-selftest\n");
    Syscall4(SYS_exit_group, 2, 0, 0, 0);
  }

  // what the spinner would have managed in the step the pause fell in, had it stopped with the
  // guest: the quiet steps' rate times the monotonic length of that step. had it kept spinning, it
  // would have managed that rate times the half second as well.
  const u64 RatePerMs = QuietMono ? QuietSpins * 1000000ul / QuietMono : 0;
  const u64 Expected = RatePerMs * (PausedStepMono / 1000000ul + 1);
  const u64 IfRunning = RatePerMs * (PausedFor / 1000000ul);
  Print("[guest] the spinner managed ");
  PrintDec(PausedStepSpins);
  Print(" in that step, against about ");
  PrintDec(Expected);
  Print(" stopped and ");
  PrintDec(IfRunning);
  Print(" had it kept running\n");
  if (RatePerMs == 0) {
    Print("[guest] FAIL: the spinner never ran\n");
    Syscall4(SYS_exit_group, 3, 0, 0, 0);
  }
  // a third of the way to running is far from both: generous towards the noise in one short step,
  // and nowhere near a spinner that carried on through half a second.
  if (PausedStepSpins > Expected + (IfRunning - Expected) / 3) {
    Print("[guest] FAIL: the spinning thread kept running through the pause\n");
    Syscall4(SYS_exit_group, 4, 0, 0, 0);
  }
  Print("[guest] PASS\n");
  Syscall4(SYS_exit_group, 0, 0, 0, 0);
}
