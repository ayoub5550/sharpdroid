// the register decode, the errata and the two things that are not CPU properties at all.
//
// the field offsets and the minimum field values below are FEX's own, from its `FetchHostFeatures`;
// they are the architecture's rather than anyone's invention, and they are transcribed here rather
// than called because that probe lives in FEX's frontend, which a library host does not build.
// FEXCore is MIT and its notice already ships.
//
// the risk runs one way. understating an extension costs a longer instruction sequence; claiming one
// the CPU lacks is SIGILL at whatever point the emitter first uses it, with nothing between the two
// to catch it. so every boolean here is read from an ID register, and the three that are not say
// where they come from instead.

#include "host_features.h"

#include <FEXCore/Config/Config.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/auxv.h>
#include <unistd.h>

namespace HostLayer {
namespace HostFeatures {

namespace {

// --- the registers ----------------------------------------------------------------------------

// `mrs` of an ID_AA64* register is trapped and emulated by the kernel at EL0, which is what makes
// this readable from an ordinary android process at all. the kernel answers with a *sanitised*
// view -- fields it does not want userspace acting on read as zero -- and that is the right answer
// to describe FEXCore with, since it is also what the guest's threads will observe.
struct IDRegisters {
  uint64_t ISAR0 {};
  uint64_t ISAR1 {};
  uint64_t ISAR2 {};
  uint64_t PFR0 {};
  uint64_t PFR1 {};
  uint64_t MMFR0 {};
  uint64_t MMFR1 {};
  uint64_t MMFR2 {};
  uint64_t ZFR0 {};
  uint64_t MIDR {};
  uint64_t CTR {};
  uint64_t DCZID {};
  // the SVE vector length in bits, which is an instruction rather than a register.
  uint64_t SVEVL {};
};

#define DEFINE_SYSREG_READER(Name, Reg)                    \
  uint64_t Read##Name() {                                  \
    uint64_t Value = 0;                                    \
    __asm__ volatile("mrs %0, " #Reg : "=r"(Value));       \
    return Value;                                          \
  }

DEFINE_SYSREG_READER(ISAR0, ID_AA64ISAR0_EL1)
DEFINE_SYSREG_READER(ISAR1, ID_AA64ISAR1_EL1)
DEFINE_SYSREG_READER(ISAR2, ID_AA64ISAR2_EL1)
DEFINE_SYSREG_READER(PFR0, ID_AA64PFR0_EL1)
DEFINE_SYSREG_READER(PFR1, ID_AA64PFR1_EL1)
DEFINE_SYSREG_READER(MMFR0, ID_AA64MMFR0_EL1)
DEFINE_SYSREG_READER(MMFR1, ID_AA64MMFR1_EL1)
DEFINE_SYSREG_READER(MMFR2, ID_AA64MMFR2_EL1)
DEFINE_SYSREG_READER(MIDR, MIDR_EL1)
DEFINE_SYSREG_READER(CTR, CTR_EL0)
DEFINE_SYSREG_READER(DCZID, DCZID_EL0)
// by encoding rather than by name: the assembler refuses ID_AA64ZFR0_EL1 unless the translation
// unit is compiled for a target with SVE, which is precisely the thing being asked about.
DEFINE_SYSREG_READER(ZFR0, s3_0_c0_c4_4)

#undef DEFINE_SYSREG_READER

// `rdvl x0, #8` -- the vector length in bits. it cannot be written as a mnemonic for the same reason
// ZFR0 cannot be named, and it is only architecturally defined where SVE is implemented, so it is
// reached only after the ID registers have said so.
__attribute__((naked)) uint64_t ReadSVEVectorLengthInBits() {
  __asm__(R"(
  .word 0x04bf5100
  ret
  )");
}

// every feature field in these registers is four bits wide, and the offsets below are written as
// FEX writes them -- the field's index times four -- so the two can be read side by side.
constexpr uint64_t Field(uint64_t Register, unsigned Offset) {
  return (Register >> Offset) & 0b1111;
}

// only the fields that reach FEXCore::HostFeatures are named. a field absent from these lists is
// one nothing consumes, rather than one the register does not carry.
namespace ISAR0Field {
constexpr unsigned AES = 1 * 4;
constexpr unsigned SHA1 = 2 * 4;
constexpr unsigned SHA2 = 3 * 4;
constexpr unsigned CRC32 = 4 * 4;
constexpr unsigned Atomic = 5 * 4;
constexpr unsigned TS = 13 * 4;
constexpr unsigned RNDR = 15 * 4;
} // namespace ISAR0Field

namespace ISAR1Field {
constexpr unsigned FCMA = 4 * 4;
constexpr unsigned LRCPC = 5 * 4;
constexpr unsigned FRINTTS = 8 * 4;
} // namespace ISAR1Field

namespace ISAR2Field {
constexpr unsigned WFxt = 0 * 4;
constexpr unsigned RPRES = 1 * 4;
constexpr unsigned MOPS = 4 * 4;
constexpr unsigned CSSC = 13 * 4;
} // namespace ISAR2Field

namespace PFR0Field {
constexpr unsigned SVE = 8 * 4;
} // namespace PFR0Field

namespace MMFR0Field {
constexpr unsigned ECV = 15 * 4;
} // namespace MMFR0Field

namespace MMFR1Field {
constexpr unsigned AFP = 11 * 4;
} // namespace MMFR1Field

namespace ZFR0Field {
constexpr unsigned SVEver = 0 * 4;
constexpr unsigned BitPerm = 4 * 4;
} // namespace ZFR0Field

// DCZID_EL0 is not a feature register and does not follow the four-bit rule.
constexpr uint64_t DCZID_DataZeroProhibited = 0b1'0000;
constexpr uint64_t DCZID_BlockSizeMask = 0b0'1111;

IDRegisters ReadIDRegisters() {
  IDRegisters Registers {};
  Registers.ISAR0 = ReadISAR0();
  Registers.ISAR1 = ReadISAR1();
  Registers.ISAR2 = ReadISAR2();
  Registers.PFR0 = ReadPFR0();
  Registers.PFR1 = ReadPFR1();
  Registers.MMFR0 = ReadMMFR0();
  Registers.MMFR1 = ReadMMFR1();
  Registers.MMFR2 = ReadMMFR2();
  Registers.MIDR = ReadMIDR();
  Registers.CTR = ReadCTR();
  Registers.DCZID = ReadDCZID();

  if (Field(Registers.PFR0, PFR0Field::SVE) >= 0b0001) {
    Registers.ZFR0 = ReadZFR0();
    if (Field(Registers.ZFR0, ZFR0Field::SVEver) >= 0b0001) {
      Registers.SVEVL = ReadSVEVectorLengthInBits();
    }
  }
  return Registers;
}

// --- the same registers, named on the command line ----------------------------------------------

// FEXCore's own `CPUFeatureRegisters`, which is a comma-separated list of `name=hex`. it is the
// honest way to ask what this build would do on a CPU that is not this one -- a feature set can be
// described exactly, rather than a boolean being forced past the check that reads the hardware.
// unnamed registers keep the value the hardware gave, so one register can be overridden alone.
void ApplyRegisterOverride(IDRegisters& Registers, std::string_view Config) {
  while (!Config.empty()) {
    // a comma is the separator FEX documents, and a semicolon is accepted beside it because this
    // option travels here inside argument vectors that already split on commas -- the app's `--es
    // fex` does, so a comma-separated value arrives as several options and the second one refuses
    // the run by name. one spelling that survives the journey is worth more than one that is tidy.
    const size_t End = Config.find_first_of(",;");
    std::string_view Entry = Config.substr(0, End);
    Config = End == std::string_view::npos ? std::string_view {} : Config.substr(End + 1);

    const size_t Equals = Entry.find('=');
    if (Equals == std::string_view::npos || Equals == 0) {
      continue;
    }
    const std::string_view Key = Entry.substr(0, Equals);
    // strtoull rather than from_chars because the value is written with a 0x prefix by every
    // example of this option, and base 16 with a prefix is what strtoull's base 0 accepts.
    const std::string Value(Entry.substr(Equals + 1));
    const uint64_t Parsed = std::strtoull(Value.c_str(), nullptr, 0);

    if (Key == "isar0") {
      Registers.ISAR0 = Parsed;
    } else if (Key == "isar1") {
      Registers.ISAR1 = Parsed;
    } else if (Key == "isar2") {
      Registers.ISAR2 = Parsed;
    } else if (Key == "pfr0") {
      Registers.PFR0 = Parsed;
    } else if (Key == "pfr1") {
      Registers.PFR1 = Parsed;
    } else if (Key == "midr") {
      Registers.MIDR = Parsed;
    } else if (Key == "mmfr0") {
      Registers.MMFR0 = Parsed;
    } else if (Key == "mmfr1") {
      Registers.MMFR1 = Parsed;
    } else if (Key == "mmfr2") {
      Registers.MMFR2 = Parsed;
    } else if (Key == "zfr0") {
      Registers.ZFR0 = Parsed;
    } else if (Key == "dczid") {
      Registers.DCZID = Parsed;
    } else if (Key == "svevl") {
      Registers.SVEVL = Parsed;
    } else if (Key == "ctr") {
      // not one of FEX's names. the cache line sizes are the only part of this probe a register
      // override could not otherwise reach, and they are the part with a fallback to hide a
      // mistake in.
      Registers.CTR = Parsed;
    } else {
      std::fprintf(stderr, "[host-layer] CPUFeatureRegisters: no register named '%.*s'\n", static_cast<int>(Key.size()),
                   Key.data());
    }
  }
}

// --- the MIDRs --------------------------------------------------------------------------------

// one MIDR_EL1 per core, out of sysfs.
//
// this is not optional and understating it is not safe, which makes it the exception to the rule
// above. FEXCore's CPUID emulation sizes its per-core table from CPUMIDRs.size() and then indexes
// it with the *current* core number -- CPUIDEmu::Function_8000_0002h is `PerCPUData[GetCPUID()]`
// with no bounds check -- so leaving the vector empty is a wild read the moment a guest asks for
// CPUID leaf 0x8000_0002, the processor brand string.
//
// read from /sys rather than by pinning to each core and executing `mrs`: the kernel already
// publishes the value per cpu, and MIDR_EL1 in userspace is an emulated trap anyway. it matters
// that these are per-core and not one value copied around -- the Snapdragon 8 Elite really is
// hybrid, reporting 0x514F0014 on some cores and 0x513F0014 on the rest, and FEXCore decides
// whether to advertise a hybrid topology to the guest by comparing them.
void FillMIDRs(FEXCore::HostFeatures& Features) {
  const long Cores = ::sysconf(_SC_NPROCESSORS_CONF);
  if (Cores <= 0) {
    return;
  }
  Features.CPUMIDRs.resize(static_cast<size_t>(Cores));

  for (long i = 0; i < Cores; ++i) {
    char Path[128];
    std::snprintf(Path, sizeof(Path), "/sys/devices/system/cpu/cpu%ld/regs/identification/midr_el1", i);
    std::FILE* File = std::fopen(Path, "re");
    if (!File) {
      continue;
    }
    unsigned long long MIDR = 0;
    if (std::fscanf(File, "%llx", &MIDR) == 1) {
      // truncated to 32 bits, as FEXCore does: the top half of MIDR_EL1 is all reserved.
      Features.CPUMIDRs[static_cast<size_t>(i)] = static_cast<uint32_t>(MIDR);
    }
    std::fclose(File);
  }
}

constexpr uint32_t MIDRImplementer(uint32_t MIDR) {
  return (MIDR >> 24) & 0xFF;
}

constexpr uint32_t MIDRPartNum(uint32_t MIDR) {
  return (MIDR >> 4) & 0xFFF;
}

// --- errata -------------------------------------------------------------------------------------

// where the CPU advertises an extension it does not deliver. these are the cases understating is
// not merely allowed but required, and they are why the probe is a port of FEX's rather than a
// reading of the ID registers on their own -- the registers say yes to all three.
void HandleErrata(FEXCore::HostFeatures& Features) {
  constexpr uint32_t Implementer_ARM = 0x41;
  constexpr uint32_t PartNum_V2 = 0xd4f;
  constexpr uint32_t PartNum_V3 = 0xd84;
  constexpr uint32_t PartNum_V3AE = 0xd83;
  constexpr uint32_t PartNum_X3 = 0xd4e;
  constexpr uint32_t PartNum_X4 = 0xd82;
  constexpr uint32_t PartNum_X925 = 0xd85;
  constexpr uint32_t PartNum_C1Ultra = 0xd8c;
  constexpr uint32_t PartNum_C1Premium = 0xd90;

  constexpr uint32_t Implementer_QCOM = 0x51;
  constexpr uint32_t PartNum_Oryon1 = 0x001;
  constexpr uint32_t PartNum_Oryon3 = 0x002;

  constexpr uint32_t Implementer_Ampere = 0xc0;

  for (const uint32_t CoreMIDR : Features.CPUMIDRs) {
    const uint32_t Implementer = MIDRImplementer(CoreMIDR);
    const uint32_t PartNum = MIDRPartNum(CoreMIDR);

    // Qualcomm's Oryon implements the RAND extension with RNDR working and RNDRRS never returning
    // a random number. x86 allows RDSEED to fail spuriously but guarantees eventual success, so a
    // guest that retries until it succeeds never leaves the loop. the extension is switched off
    // rather than the instruction being special-cased, because CPUID is what a guest asks.
    if (Implementer == Implementer_QCOM && (PartNum == PartNum_Oryon1 || PartNum == PartNum_Oryon3)) {
      Features.SupportsRAND = false;
    }

    // LDAPUR, LDAPURB and LDAPURH execute with full load-acquire ordering on these cores instead of
    // the relaxed ordering their pseudocode describes, which costs far more than the addressing
    // mode saves. only the unscaled forms are affected, so LDAPR itself stays.
    const bool IgnoreLRCPC2 = Implementer == Implementer_ARM &&
                              (PartNum == PartNum_V2 || PartNum == PartNum_V3 || PartNum == PartNum_X3 || PartNum == PartNum_X4 ||
                               PartNum == PartNum_X925 || PartNum == PartNum_V3AE || PartNum == PartNum_C1Ultra ||
                               PartNum == PartNum_C1Premium);
    if (IgnoreLRCPC2) {
      Features.SupportsTSOImm9 = false;
    }

    // `dc zva` is faster than the alternative for zeroing the upper halves on Ampere's parts and
    // dramatically slower on Oryon's, which stall around barriers and overlapping zeroes. the
    // optimisation was written for the hardware it helps, so it is asked for there and nowhere else.
    if (Implementer == Implementer_Ampere) {
      Features.PreferZVAForVZero = Features.SupportsCLZERO;
    }
  }
}

// --- float exception trapping --------------------------------------------------------------------

// FPCR's exception-enable bits are architecturally RAZ/WI where the traps are not implemented, so
// the test is to ask for them and read back what stuck. FPCR is per-thread and is put back either
// way, so this leaves nothing behind on the thread that runs it.
bool ProbeFloatExceptions() {
  constexpr uint32_t ExceptionEnableTraps = (1u << 8) |  // invalid operation
                                            (1u << 9) |  // divide by zero
                                            (1u << 10) | // overflow
                                            (1u << 11) | // underflow
                                            (1u << 12) | // inexact
                                            (1u << 15);  // input denormal
  uint64_t Original = 0;
  __asm__ volatile("mrs %0, FPCR" : "=r"(Original));
  const uint64_t Wanted = Original | ExceptionEnableTraps;
  __asm__ volatile("msr FPCR, %0" ::"r"(Wanted));
  uint64_t After = 0;
  __asm__ volatile("mrs %0, FPCR" : "=r"(After));
  __asm__ volatile("msr FPCR, %0" ::"r"(Original));
  return (static_cast<uint32_t>(After) & ExceptionEnableTraps) == ExceptionEnableTraps;
}

// --- the conservative set -----------------------------------------------------------------------

FEXCore::HostFeatures MinimalFeatures() {
  FEXCore::HostFeatures Features {};
  const unsigned long HwCaps = ::getauxval(AT_HWCAP);
  Features.SupportsAES = (HwCaps & HWCAP_AES) != 0;
  Features.SupportsCRC = (HwCaps & HWCAP_CRC32) != 0;
  Features.SupportsAtomics = (HwCaps & HWCAP_ATOMICS) != 0;
  Features.SupportsRCPC = (HwCaps & HWCAP_LRCPC) != 0;
  Features.SupportsAVX = true;
  // gated on AVX in FEX's own probe too: VAES is a VEX encoding, so it is unreachable without one.
  Features.SupportsAES256 = Features.SupportsAVX && Features.SupportsAES;
  FillMIDRs(Features);
  return Features;
}

// --- the probe ------------------------------------------------------------------------------------

// what the last probe decoded, so that the two report lines describe one thing. re-reading the
// hardware there would print registers the feature line beside it was not computed from, which is
// exactly wrong under a CPUFeatureRegisters override -- the case the raw line exists for.
IDRegisters LastProbed {};
bool HaveProbed = false;

FEXCore::HostFeatures ProbedFeatures() {
  IDRegisters Registers = ReadIDRegisters();

  const auto Override = FEXCore::Config::Get(FEXCore::Config::CONFIG_CPUFEATUREREGISTERS);
  if (Override && !(*Override)->empty()) {
    ApplyRegisterOverride(Registers, std::string_view((*Override)->data(), (*Override)->size()));
  }

  LastProbed = Registers;
  HaveProbed = true;

  FEXCore::HostFeatures Features {};

  // the fields, in FEX's own order. each is a minimum field value rather than a bit, because these
  // fields count versions of an extension: LRCPC at 2 means LRCPC2, and AES at 2 means PMULL.
  Features.SupportsAES = Field(Registers.ISAR0, ISAR0Field::AES) >= 0b0001;
  Features.SupportsPMULL_128Bit = Field(Registers.ISAR0, ISAR0Field::AES) >= 0b0010;
  Features.SupportsSHA = Field(Registers.ISAR0, ISAR0Field::SHA1) >= 0b0001 && Field(Registers.ISAR0, ISAR0Field::SHA2) >= 0b0001;
  Features.SupportsCRC = Field(Registers.ISAR0, ISAR0Field::CRC32) >= 0b0001;
  Features.SupportsAtomics = Field(Registers.ISAR0, ISAR0Field::Atomic) >= 0b0010;
  Features.SupportsFlagM = Field(Registers.ISAR0, ISAR0Field::TS) >= 0b0001;
  Features.SupportsFlagM2 = Field(Registers.ISAR0, ISAR0Field::TS) >= 0b0010;
  Features.SupportsRAND = Field(Registers.ISAR0, ISAR0Field::RNDR) >= 0b0001;

  Features.SupportsFCMA = Field(Registers.ISAR1, ISAR1Field::FCMA) >= 0b0001;
  Features.SupportsRCPC = Field(Registers.ISAR1, ISAR1Field::LRCPC) >= 0b0001;
  Features.SupportsTSOImm9 = Field(Registers.ISAR1, ISAR1Field::LRCPC) >= 0b0010;
  Features.SupportsFRINTTS = Field(Registers.ISAR1, ISAR1Field::FRINTTS) >= 0b0001;

  Features.SupportsWFXT = Field(Registers.ISAR2, ISAR2Field::WFxt) >= 0b0010;
  Features.SupportsRPRES = Field(Registers.ISAR2, ISAR2Field::RPRES) >= 0b0001;
  Features.SupportsMOPS = Field(Registers.ISAR2, ISAR2Field::MOPS) >= 0b0001;
  Features.SupportsCSSC = Field(Registers.ISAR2, ISAR2Field::CSSC) >= 0b0001;

  Features.SupportsECV = Field(Registers.MMFR0, MMFR0Field::ECV) >= 0b0010;
  Features.SupportsAFP = Field(Registers.MMFR1, MMFR1Field::AFP) >= 0b0001;

  const bool SupportsSVE = Field(Registers.PFR0, PFR0Field::SVE) >= 0b0001;
  const bool SupportsSVE2 = SupportsSVE && Field(Registers.ZFR0, ZFR0Field::SVEver) >= 0b0001;
  Features.SupportsSVE128 = SupportsSVE2;
  Features.SupportsSVE256 = SupportsSVE2 && Registers.SVEVL >= 256;
  Features.SupportsSVEBitPerm = SupportsSVE && Field(Registers.ZFR0, ZFR0Field::BitPerm) >= 0b0001;

  // `dc zva` can stand in for a cacheline clear only where it zeroes exactly the cacheline x86
  // means by one.
  if ((Registers.DCZID & DCZID_DataZeroProhibited) == 0) {
    constexpr uint64_t CachelineSize = 64;
    Features.SupportsCLZERO = ((1u << (Registers.DCZID & DCZID_BlockSizeMask)) * sizeof(uint32_t)) == CachelineSize;
  }

  // not a capability. SupportsAVX is what decides whether FEXCore's decoder has a VEX table *to
  // decode with*: the Decoder constructor picks VEXTableOps + SVE256 if the host has it,
  // VEXTableOps_AVX128 -- 256-bit decomposed into pairs of 128-bit NEON, which any arm64 can run --
  // if it does not, and leaves both null otherwise. so with it unset every VEX-encoded instruction
  // is undecodable and raises #UD, which is fine for a guest that checks CPUID first and fatal for
  // one that does not. FEX sets it unconditionally on arm64 for the same reason.
  Features.SupportsAVX = true;
  Features.SupportsAES256 = Features.SupportsAVX && Features.SupportsAES;

  // 3DNow! is likewise a decode table rather than a claim about the host, and an opcode nothing
  // emits costs nothing to be able to decode.
  Features.Supports3DNow = true;

  // the cacheline maintenance ops, and the sizes they are emitted against. these two move together
  // and neither is safe alone: with the ops off, a guest cacheline clear becomes a bare barrier and
  // nothing is written back; with them on and a size of zero, the emitter divides by that zero to
  // work out how many lines to walk.
  //
  // since FEX-2609 the size is stored as CTR_EL0's own log2-of-words field (DminLine, 4 << it is the
  // line in bytes, read back through DCacheSize()) and the icache size is gone -- nothing emitted
  // against it. 4 is the 64-byte fallback, the same one FEX's own frontend uses (Common/HostFeatures.cpp).
  Features.SupportsCacheMaintenanceOps = true;
  if (Registers.CTR) {
    Features.DCacheLineLog2 = (Registers.CTR >> 16) & 0xF;
  } else {
    Features.DCacheLineLog2 = 4;
  }

  // a property of the compiler that built FEXCore, not of the CPU, and false here for a reason
  // that is checkable rather than assumed: the attribute is applied to FEXCore's own out-of-line
  // helpers by a compile definition its build sets only when a support check has run, and this
  // build assembles FEXCore's subdirectories directly rather than through the configuration that
  // runs that check. claiming it would tell the emitter it may skip spilling caller-saved registers
  // around calls into functions that do not in fact preserve them.
  Features.SupportsPreserveAllABI = false;

  // read by nothing that reaches a library host: FEXCore declares the field and consumes it
  // nowhere. it is filled because the measurement is free and a false one would be a claim.
  Features.SupportsFloatExceptions = ProbeFloatExceptions();

  // FEXCore reads the core index out of TPIDRRO_EL0 where a host guarantees it is there, and
  // linux makes no such guarantee.
  Features.SupportsCPUIndexInTPIDRRO = false;

  // decides whether `INT 0x80` is treated as a linux syscall, which is a 32-bit path a 64-bit guest
  // reaching is an error either way.
  Features.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;

  FillMIDRs(Features);
  HandleErrata(Features);
  return Features;
}

} // namespace

bool ParseMode(const char* Text, Mode& Out) {
  if (std::strcmp(Text, "probe") == 0) {
    Out = Mode::Probe;
    return true;
  }
  if (std::strcmp(Text, "minimal") == 0) {
    Out = Mode::Minimal;
    return true;
  }
  return false;
}

FEXCore::HostFeatures Build(Mode Chosen) {
  return Chosen == Mode::Probe ? ProbedFeatures() : MinimalFeatures();
}

void Report(const FEXCore::HostFeatures& Features, Mode Chosen) {
  if (Chosen == Mode::Probe && HaveProbed) {
    const IDRegisters& Registers = LastProbed;
    // the raw registers, so that every boolean on the next line can be recomputed away from the
    // device. a report about wrong codegen is worth far more with this line than without it.
    std::printf("[host-layer] id registers: isar0=%016llx isar1=%016llx isar2=%016llx pfr0=%016llx pfr1=%016llx "
                "mmfr0=%016llx mmfr1=%016llx mmfr2=%016llx zfr0=%016llx midr=%08llx ctr=%08llx dczid=%llx svevl=%llu\n",
                static_cast<unsigned long long>(Registers.ISAR0), static_cast<unsigned long long>(Registers.ISAR1),
                static_cast<unsigned long long>(Registers.ISAR2), static_cast<unsigned long long>(Registers.PFR0),
                static_cast<unsigned long long>(Registers.PFR1), static_cast<unsigned long long>(Registers.MMFR0),
                static_cast<unsigned long long>(Registers.MMFR1), static_cast<unsigned long long>(Registers.MMFR2),
                static_cast<unsigned long long>(Registers.ZFR0), static_cast<unsigned long long>(Registers.MIDR),
                static_cast<unsigned long long>(Registers.CTR), static_cast<unsigned long long>(Registers.DCZID),
                static_cast<unsigned long long>(Registers.SVEVL));
  }

  // named individually rather than as a bitfield, because the question asked of this line is
  // always "is <one extension> on", and a hex blob makes that a lookup.
  std::printf("[host-layer] host features (%s): AES=%d AES256=%d CRC=%d SHA=%d PMULL128=%d Atomics=%d RCPC=%d TSOImm9=%d "
              "AFP=%d FlagM=%d FlagM2=%d RPRES=%d FRINTTS=%d FCMA=%d ECV=%d RAND=%d CLZERO=%d CSSC=%d MOPS=%d WFXT=%d "
              "AVX=%d SVE128=%d SVE256=%d SVEBitPerm=%d 3DNow=%d PreserveAllABI=%d FloatExceptions=%d "
              "CacheOps=%d dcache=%u, %zu core(s)\n",
              Chosen == Mode::Probe ? "probe" : "minimal", Features.SupportsAES, Features.SupportsAES256, Features.SupportsCRC,
              Features.SupportsSHA, Features.SupportsPMULL_128Bit, Features.SupportsAtomics, Features.SupportsRCPC,
              Features.SupportsTSOImm9, Features.SupportsAFP, Features.SupportsFlagM, Features.SupportsFlagM2, Features.SupportsRPRES,
              Features.SupportsFRINTTS, Features.SupportsFCMA, Features.SupportsECV, Features.SupportsRAND, Features.SupportsCLZERO,
              Features.SupportsCSSC, Features.SupportsMOPS, Features.SupportsWFXT, Features.SupportsAVX, Features.SupportsSVE128,
              Features.SupportsSVE256, Features.SupportsSVEBitPerm, Features.Supports3DNow, Features.SupportsPreserveAllABI,
              Features.SupportsFloatExceptions, Features.SupportsCacheMaintenanceOps, Features.DCacheSize(),
              Features.CPUMIDRs.size());
}

} // namespace HostFeatures
} // namespace HostLayer
