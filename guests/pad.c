// a static x86-64 guest that reads every pad format the host layer answers, and checks it is refused
// everything it should be.
//
// it is run with `--pad`, from a shell binary that no app pushes pad state into, so every port reads
// as no pad at all. that is enough for what this is for: **the formats themselves.** each read hands
// over a buffer filled with a marker, and a read that answers has to have overwritten it -- which is
// what tells "answered with an empty pad" from "answered without writing anything". run without
// `--pad`, every read is refused and the guest must fail, which is what says the passes are the bridge
// answering rather than something else.
//
// what it checks, in order, each failure with an exit code of its own:
//
//   - the contract 3 format (number 1, 12 bytes) is answered, and ignores whatever is in the port
//     argument, since the payloads that send it pass none
//   - the contract 4 format (number 4, 64 bytes) is answered on each of the four ports
//   - a read naming an unknown format, the wrong size for a known one, a fifth port or no buffer is
//     refused, and a refused read writes nothing
//   - the rumble with no port and the rumble on a port are both taken, and a rumble on a fifth port
//     and an unknown command are refused
//
// no libc, for the reason the other freestanding guests have none: what is under test is the host
// layer against the raw kernel ABI.

typedef unsigned long u64;

#define SYS_write 1
#define SYS_exit_group 231

#define PAD_MAGIC 0x50440000l
#define PAD_READ (PAD_MAGIC | 0)
#define PAD_RUMBLE (PAD_MAGIC | 1)
#define PAD_RUMBLE_PORT (PAD_MAGIC | 2)
#define PAD_UNKNOWN (PAD_MAGIC | 3)

#define CONTRACT3_NUMBER 1
#define CONTRACT3_SIZE 12
#define CONTRACT4_NUMBER 4
#define CONTRACT4_SIZE 64
#define PORTS 4

#define MARKER 0xAA

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

static u64 StringLength(const char* Text) {
  u64 Length = 0;
  while (Text[Length]) {
    ++Length;
  }
  return Length;
}

static void Print(const char* Text) {
  Syscall6(SYS_write, 1, (long)Text, (long)StringLength(Text), 0, 0, 0);
}

static void Fail(const char* Why, long Code) {
  Print("[guest] FAIL: ");
  Print(Why);
  Print("\n");
  Syscall6(SYS_exit_group, Code, 0, 0, 0, 0, 0);
}

static unsigned char Buffer[CONTRACT4_SIZE];

// volatile, so that the compiler cannot turn the fill into a call to a memset there is no libc to
// provide -- and so that a check is a load after the host layer's write rather than a value it assumed.
static volatile unsigned char* const Bytes = Buffer;

static void Mark(void) {
  for (int Index = 0; Index < CONTRACT4_SIZE; ++Index) {
    Bytes[Index] = MARKER;
  }
}

// whether the first Size bytes were all overwritten with zeros: an empty pad, written in full.
static int WrittenEmpty(int Size) {
  for (int Index = 0; Index < Size; ++Index) {
    if (Bytes[Index] != 0) {
      return 0;
    }
  }
  return 1;
}

static int Untouched(void) {
  for (int Index = 0; Index < CONTRACT4_SIZE; ++Index) {
    if (Bytes[Index] != MARKER) {
      return 0;
    }
  }
  return 1;
}

static long Read(long Number, void* Out, long Size, long Port) {
  return Syscall6(PAD_READ, Number, (long)Out, Size, Port, 0, 0);
}

void _start(void) {
  // --- the formats that are answered ---
  Mark();
  if (Read(CONTRACT3_NUMBER, Buffer, CONTRACT3_SIZE, 0) != 0) {
    Fail("the contract 3 format was not answered with an empty pad", 2);
  }
  if (!WrittenEmpty(CONTRACT3_SIZE)) {
    Fail("the contract 3 format answered without writing its 12 bytes", 3);
  }

  Mark();
  if (Read(CONTRACT3_NUMBER, Buffer, CONTRACT3_SIZE, 99) != 0 || !WrittenEmpty(CONTRACT3_SIZE)) {
    Fail("the contract 3 format did not ignore its port argument", 4);
  }

  for (long Port = 0; Port < PORTS; ++Port) {
    Mark();
    if (Read(CONTRACT4_NUMBER, Buffer, CONTRACT4_SIZE, Port) != 0) {
      Fail("the contract 4 format was not answered with an empty pad on one of the four ports", 5);
    }
    if (!WrittenEmpty(CONTRACT4_SIZE)) {
      Fail("the contract 4 format answered without writing its 64 bytes", 6);
    }
  }

  // --- the reads that are refused, and write nothing ---
  Mark();
  if (Read(2, Buffer, CONTRACT4_SIZE, 0) != -1) {
    Fail("a read naming a format nobody defined was answered", 7);
  }
  if (Read(CONTRACT4_NUMBER, Buffer, CONTRACT3_SIZE, 0) != -1) {
    Fail("the contract 4 format was answered at the contract 3 format's size", 8);
  }
  if (Read(CONTRACT3_NUMBER, Buffer, CONTRACT4_SIZE, 0) != -1) {
    Fail("the contract 3 format was answered at the contract 4 format's size", 9);
  }
  if (Read(CONTRACT4_NUMBER, Buffer, CONTRACT4_SIZE, PORTS) != -1) {
    Fail("the contract 4 format was answered for a fifth port", 10);
  }
  if (Read(CONTRACT4_NUMBER, 0, CONTRACT4_SIZE, 0) != -1) {
    Fail("a read with nowhere to write was answered", 11);
  }
  if (!Untouched()) {
    Fail("a refused read wrote into the guest's buffer", 12);
  }

  // --- rumble ---
  if (Syscall6(PAD_RUMBLE, 255, 0, 0, 0, 0, 0) != 0) {
    Fail("the rumble with no port was not taken", 13);
  }
  if (Syscall6(PAD_RUMBLE_PORT, PORTS - 1, 10, 20, 0, 0, 0) != 0) {
    Fail("a rumble on port 4 was not taken", 14);
  }
  if (Syscall6(PAD_RUMBLE_PORT, PORTS, 10, 20, 0, 0, 0) != -1) {
    Fail("a rumble on a fifth port was taken", 15);
  }
  if (Syscall6(PAD_UNKNOWN, 0, 0, 0, 0, 0, 0) != -1) {
    Fail("a command nobody defined was answered", 16);
  }

  Print("[guest] PASS\n");
  Syscall6(SYS_exit_group, 0, 0, 0, 0, 0, 0);
}
