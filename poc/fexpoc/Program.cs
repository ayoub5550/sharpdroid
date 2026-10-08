// FexPoc: the FexCpuBackend proof of concept from docs/native-arm64-backend.md, section 4.
//
// loads guest.bin (x86-64, linked at 0x8_0000_0000) and checks the contract a native arm64
// SharpEmu needs from FEXCore-as-a-library -- HLE imports with SharpEmu's register contract, guest
// pointers dereferenced directly by C#, host->guest callbacks nested inside imports, context
// transfer, blocking and resuming a guest thread, guest threads on several host threads during GC,
// .NET's own fault handling kept intact -- then times the same guest loops under both models.
//
//   model "fex": this process is native arm64; libsharpfex runs only the guest through FEXCore.
//   model "x64": this process is x86-64 and runs the guest natively, with SharpEmu's trampoline
//                design. on a phone it runs under the host layer, i.e. FEX translates all of it.
//
// usage: FexPoc [--n N] [--rounds R] [--blob path] [--lib path] [--fex "Name=V;..."] [--unaligned]
//               [--tests-only] [--threads W] [--thread-n N] [--thread-timeout S]

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace FexPoc;

[StructLayout(LayoutKind.Sequential)]
unsafe struct SfxRegs
{
    public fixed ulong Gpr[16];
    public ulong Rip;
    public uint Mxcsr;
    public uint Reserved;
    public fixed ulong Xmm[16];
}

[StructLayout(LayoutKind.Sequential)]
struct SfxStats
{
    public ulong HleCalls, Callbacks, Blocks, Transfers, ForeignSyscalls, UnalignedFixups, JitRestarts, SignalsChained;
}

static class R
{
    public const int Rax = 0, Rcx = 1, Rdx = 2, Rbx = 3, Rsp = 4, Rbp = 5, Rsi = 6, Rdi = 7;
    public const int R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15;
}

static unsafe class Libc
{
    public const int ProtRead = 1, ProtWrite = 2, ProtExec = 4;
    public const int MapPrivate = 2, MapAnonymous = 0x20, MapFixedNoReplace = 0x100000;

    [DllImport("libc", EntryPoint = "mmap")]
    public static extern void* Mmap(void* addr, nuint length, int prot, int flags, int fd, nint offset);
}

// the guest blob's header (guest.S): magic, slot count and stride, then offsets in this order.
enum Sym
{
    Slots, CbRet, TestArgs, TestRegs, TestCallback, GuestCbPlain, TestTransfer, BenchHle, BenchHash, BenchCb,
    BenchCompute, WorkerYield, Unaligned, X64Enter, X64GatewayCtx, X64GatewayFn, X64HostRsp, X64Tramps,
    StrHello, HashBuf, CodeEnd, BlobEnd, Scratch, BenchThunk, Thunks, BenchSse, Count,
}

static unsafe class Guest
{
    public const ulong Base = 0x8_0000_0000;
    public const ulong StackRegion = Base + 0x100_0000; // 16 MiB up: per-thread guest stacks
    public const ulong StackSize = 0x10_0000;
    public const int Threads = 17; // stack 0: main, 1-16: workers
    public static uint SlotCount, SlotStride;
    static readonly ulong[] Offsets = new ulong[(int)Sym.Count];

    public static ulong A(Sym s) => Base + Offsets[(int)s];
    public static ulong StackTop(int index) => StackRegion + (ulong)(index + 1) * StackSize - 64;

    public static void Load(string path)
    {
        byte[] blob = File.ReadAllBytes(path);
        if (blob.Length < 0x100 || BitConverter.ToUInt64(blob, 0) != 0x313043_4F50_5846_53UL)
            throw new InvalidDataException("guest.bin: bad magic");
        SlotCount = BitConverter.ToUInt32(blob, 8);
        SlotStride = BitConverter.ToUInt32(blob, 12);
        for (int i = 0; i < (int)Sym.Count; i++)
            Offsets[i] = BitConverter.ToUInt64(blob, 16 + i * 8);

        ulong size = (Offsets[(int)Sym.BlobEnd] + 0xFFFF) & ~0xFFFFUL;
        // PROT_EXEC only for the x64 model, where the host CPU (or the host layer) runs these bytes
        // itself. under FEX the pages stay RW: FEX reads them and emits arm64 elsewhere.
        int prot = Libc.ProtRead | Libc.ProtWrite | (Program.Fex ? 0 : Libc.ProtExec);
        Map(Base, size, prot, "image");
        Map(StackRegion, StackSize * Threads, Libc.ProtRead | Libc.ProtWrite, "stacks");
        fixed (byte* src = blob)
            Buffer.MemoryCopy(src, (void*)Base, (long)size, blob.Length);
    }

    static void Map(ulong at, ulong size, int prot, string what)
    {
        void* p = Libc.Mmap((void*)at, (nuint)size, prot, Libc.MapPrivate | Libc.MapAnonymous | Libc.MapFixedNoReplace, -1, 0);
        if ((ulong)p != at)
            throw new InvalidOperationException(
                $"could not map the guest {what} at 0x{at:X} (got 0x{(ulong)p:X}): something in this process already owns it");
    }

    public static void PatchSlot(uint index, ReadOnlySpan<byte> code)
    {
        var slot = new Span<byte>((void*)(A(Sym.Slots) + index * SlotStride), (int)SlotStride);
        slot.Fill(0xCC);
        code.CopyTo(slot);
    }
}

interface IBackend
{
    string Describe();
    void Init();
    // a host->guest call on the current host thread's guest thread. rspHint 0 = top level.
    ulong Call(ulong rip, ulong rspHint, ulong a0 = 0, ulong a1 = 0, ulong a2 = 0);
    void BindThread(int stackIndex);
    void UnbindThread();
}

// --- model "fex": libsharpfex in this native arm64 process ----------------------------------------

unsafe sealed class FexBackend : IBackend
{
    delegate* unmanaged<byte*, int> _init;
    delegate* unmanaged<byte*> _describe;
    delegate* unmanaged<delegate* unmanaged<void*, uint, SfxRegs*, int>, void*, void> _setHle;
    delegate* unmanaged<ulong, uint, uint, int> _setSlots;
    delegate* unmanaged<ulong, int> _setCbRet;
    delegate* unmanaged<ulong, ulong, int> _markExec;
    delegate* unmanaged<ulong, ulong, ulong, void*> _threadNew;
    delegate* unmanaged<void*, void> _threadFree;
    public delegate* unmanaged<void*, int> Run;
    delegate* unmanaged<void*, ulong, ulong, ulong*, int, ulong> _call;
    public delegate* unmanaged<void*, SfxRegs*, void> GetRegs;
    delegate* unmanaged<SfxStats*, void> _stats;
    delegate* unmanaged<uint, void*, void*, int> _setSlotFn;
    delegate* unmanaged<byte*, void*> _builtin;
    delegate* unmanaged<void*, ulong, ulong, ulong, ulong> _benchCall;
    delegate* unmanaged<uint, byte*, uint, int> _thunkCode;

    [ThreadStatic] static nint t_thread;
    public static nint CurrentThread => t_thread;
    public double InitMs;
    string _options = "";

    public FexBackend(string lib, string options)
    {
        nint h = NativeLibrary.Load(lib);
        _init = (delegate* unmanaged<byte*, int>)NativeLibrary.GetExport(h, "sfx_init");
        _describe = (delegate* unmanaged<byte*>)NativeLibrary.GetExport(h, "sfx_describe");
        _setHle = (delegate* unmanaged<delegate* unmanaged<void*, uint, SfxRegs*, int>, void*, void>)NativeLibrary.GetExport(h, "sfx_set_hle_handler");
        _setSlots = (delegate* unmanaged<ulong, uint, uint, int>)NativeLibrary.GetExport(h, "sfx_set_import_slots");
        _setCbRet = (delegate* unmanaged<ulong, int>)NativeLibrary.GetExport(h, "sfx_set_callback_return");
        _markExec = (delegate* unmanaged<ulong, ulong, int>)NativeLibrary.GetExport(h, "sfx_mark_exec");
        _threadNew = (delegate* unmanaged<ulong, ulong, ulong, void*>)NativeLibrary.GetExport(h, "sfx_thread_new");
        _threadFree = (delegate* unmanaged<void*, void>)NativeLibrary.GetExport(h, "sfx_thread_free");
        Run = (delegate* unmanaged<void*, int>)NativeLibrary.GetExport(h, "sfx_run");
        _call = (delegate* unmanaged<void*, ulong, ulong, ulong*, int, ulong>)NativeLibrary.GetExport(h, "sfx_call");
        GetRegs = (delegate* unmanaged<void*, SfxRegs*, void>)NativeLibrary.GetExport(h, "sfx_get_regs");
        _stats = (delegate* unmanaged<SfxStats*, void>)NativeLibrary.GetExport(h, "sfx_get_stats");
        _setSlotFn = (delegate* unmanaged<uint, void*, void*, int>)NativeLibrary.GetExport(h, "sfx_set_slot_fn");
        _builtin = (delegate* unmanaged<byte*, void*>)NativeLibrary.GetExport(h, "sfx_builtin");
        _benchCall = (delegate* unmanaged<void*, ulong, ulong, ulong, ulong>)NativeLibrary.GetExport(h, "sfx_bench_call");
        _thunkCode = (delegate* unmanaged<uint, byte*, uint, int>)NativeLibrary.GetExport(h, "sfx_thunk_code");
        _options = options;
    }

    public string Describe() => Marshal.PtrToStringUTF8((nint)_describe()) ?? "";

    public void Init()
    {
        long t0 = Stopwatch.GetTimestamp();
        byte[] opts = System.Text.Encoding.UTF8.GetBytes(_options + "\0");
        int rc;
        fixed (byte* p = opts)
            rc = _init(_options.Length == 0 ? null : p);
        if (rc != 0)
            throw new InvalidOperationException($"sfx_init failed: {rc}");
        InitMs = Stopwatch.GetElapsedTime(t0).TotalMilliseconds;
        _setHle(&HleEntry, null);
        _setSlots(Guest.A(Sym.Slots), Guest.SlotCount, Guest.SlotStride);
        _setCbRet(Guest.A(Sym.CbRet));
        _markExec(Guest.Base, Guest.A(Sym.CodeEnd) - Guest.Base);
        // mov [rsp-8],rcx; mov [rsp-16],r11; syscall; ret -- see sharpfex.h
        ReadOnlySpan<byte> stub = [0x48, 0x89, 0x4C, 0x24, 0xF8, 0x4C, 0x89, 0x5C, 0x24, 0xF0, 0x0F, 0x05, 0xC3];
        for (uint i = 0; i < Guest.SlotCount; i++)
            Guest.PatchSlot(i, stub);

        // slot 6 goes through a thunk slot instead: `jmp thunk0`, and thunk0 = 0f 3f + name (sharpfex.h).
        byte* thunk = (byte*)Guest.A(Sym.Thunks);
        if (_thunkCode(ThunkSlot, thunk, 64) != 34)
            throw new InvalidOperationException("sfx_thunk_code failed");
        ulong slot = Guest.A(Sym.Slots) + ThunkSlot * Guest.SlotStride;
        Span<byte> jmp = stackalloc byte[5];
        jmp[0] = 0xE9; // jmp rel32
        BitConverter.TryWriteBytes(jmp.Slice(1), (int)((long)Guest.A(Sym.Thunks) - (long)(slot + 5)));
        Guest.PatchSlot(ThunkSlot, jmp);
    }

    public const uint ThunkSlot = 6;

    // n host->guest calls made by C++ inside libsharpfex: the entry and exit alone, no managed code.
    public ulong BenchCall(ulong rip, ulong n) => _benchCall((void*)t_thread, rip, t_stackTop, n);

    [UnmanagedCallersOnly]
    static int HleEntry(void* user, uint index, SfxRegs* regs) => Hle.Handle(index, regs);

    // slot functions: FEX's register array, nothing copied (sharpfex.h, sfx_slot_fn).
    [UnmanagedCallersOnly]
    static void AddDirect(void* user, ulong* gpr) => gpr[R.Rax] = gpr[R.Rdi] + gpr[R.Rsi];

    public void SetSlotFn(uint index, void* fn)
    {
        if (_setSlotFn(index, fn, null) != 0)
            throw new InvalidOperationException("sfx_set_slot_fn failed");
    }

    public void* Builtin(string name)
    {
        byte[] b = System.Text.Encoding.UTF8.GetBytes(name + "\0");
        fixed (byte* p = b)
            return _builtin(p);
    }

    public static void* AddDirectPtr => (delegate* unmanaged<void*, ulong*, void>)&AddDirect;

    public void BindThread(int stackIndex)
    {
        if (t_thread != 0)
            return;
        t_stackTop = Guest.StackTop(stackIndex);
        t_thread = (nint)_threadNew(0, t_stackTop, 0);
        if (t_thread == 0)
            throw new InvalidOperationException("sfx_thread_new failed");
    }

    public void UnbindThread()
    {
        if (t_thread != 0)
            _threadFree((void*)t_thread);
        t_thread = 0;
    }

    public nint NewThread(ulong rip, int stackIndex) => (nint)_threadNew(rip, Guest.StackTop(stackIndex) - 8, 0);
    public void FreeThread(nint t) => _threadFree((void*)t);

    public ulong Call(ulong rip, ulong rspHint, ulong a0 = 0, ulong a1 = 0, ulong a2 = 0)
    {
        ulong* args = stackalloc ulong[3] { a0, a1, a2 };
        // at top level the thread's own stack top. nested inside an import, 0: the shim knows the
        // live guest rsp and goes below its red zone.
        return _call((void*)t_thread, rip, rspHint == 0 ? t_stackTop : 0, args, 3);
    }

    [ThreadStatic] static ulong t_stackTop;

    public SfxStats Stats()
    {
        SfxStats s;
        _stats(&s);
        return s;
    }
}

// --- model "x64": today's design, guest code run by the process's own CPU --------------------------

unsafe sealed class X64Backend : IBackend
{
    delegate* unmanaged<ulong, ulong, ulong, ulong, ulong, ulong> _enter;
    [ThreadStatic] static ulong t_stackTop;

    public string Describe() => "native x86-64 (SharpEmu's trampoline model)";

    public void Init()
    {
        _enter = (delegate* unmanaged<ulong, ulong, ulong, ulong, ulong, ulong>)Guest.A(Sym.X64Enter);
        *(ulong*)Guest.A(Sym.X64GatewayCtx) = 0;
        *(ulong*)Guest.A(Sym.X64GatewayFn) = (ulong)(delegate* unmanaged<nint, uint, byte*, void>)&Gateway;
        ulong* tramps = (ulong*)Guest.A(Sym.X64Tramps);
        Span<byte> stub = stackalloc byte[12];
        for (uint i = 0; i < Guest.SlotCount; i++)
        {
            // movabs rax, tramp; jmp rax -- PatchImportStub's 16-byte slot
            stub[0] = 0x48; stub[1] = 0xB8;
            BitConverter.TryWriteBytes(stub.Slice(2, 8), tramps[i]);
            stub[10] = 0xFF; stub[11] = 0xE0;
            Guest.PatchSlot(i, stub);
        }
    }

    public void BindThread(int stackIndex) => t_stackTop = Guest.StackTop(stackIndex) & ~15UL;
    public void UnbindThread() { }

    public ulong Call(ulong rip, ulong rspHint, ulong a0 = 0, ulong a1 = 0, ulong a2 = 0)
    {
        // nested: below the trampoline's frame (0x100 bytes under the return address) and a red zone.
        ulong rsp = rspHint == 0 ? t_stackTop : (rspHint - 0x100 - 0x100) & ~15UL;
        return _enter(rip, rsp, a0, a1, a2);
    }

    // the frame x64_tramp_common hands over: xmm0-7, r10, r11, mxcsr, rax-out, then the arg pack.
    [UnmanagedCallersOnly]
    static void Gateway(nint ctx, uint index, byte* frame)
    {
        SfxRegs r;
        ulong* pack = (ulong*)(frame + 0xA0);
        r.Gpr[R.Rdi] = pack[0]; r.Gpr[R.Rsi] = pack[1]; r.Gpr[R.Rdx] = pack[2]; r.Gpr[R.Rcx] = pack[3];
        r.Gpr[R.R8] = pack[4]; r.Gpr[R.R9] = pack[5]; r.Gpr[R.Rbx] = pack[6]; r.Gpr[R.Rbp] = pack[7];
        r.Gpr[R.R12] = pack[8]; r.Gpr[R.R13] = pack[9]; r.Gpr[R.R14] = pack[10]; r.Gpr[R.R15] = pack[11];
        r.Gpr[R.Rax] = 0;
        r.Gpr[R.R10] = *(ulong*)(frame + 0x80);
        r.Gpr[R.R11] = *(ulong*)(frame + 0x88);
        r.Gpr[R.Rsp] = (ulong)(frame + 0x100);
        ulong ret = pack[12];
        r.Rip = ret;
        r.Mxcsr = *(uint*)(frame + 0x90);
        r.Reserved = 0;
        for (int i = 0; i < 16; i++)
            r.Xmm[i] = ((ulong*)frame)[i];

        Hle.Handle(index, &r);

        pack[0] = r.Gpr[R.Rdi]; pack[1] = r.Gpr[R.Rsi]; pack[2] = r.Gpr[R.Rdx]; pack[3] = r.Gpr[R.Rcx];
        pack[4] = r.Gpr[R.R8]; pack[5] = r.Gpr[R.R9]; pack[6] = r.Gpr[R.Rbx]; pack[7] = r.Gpr[R.Rbp];
        pack[8] = r.Gpr[R.R12]; pack[9] = r.Gpr[R.R13]; pack[10] = r.Gpr[R.R14]; pack[11] = r.Gpr[R.R15];
        *(ulong*)(frame + 0x80) = r.Gpr[R.R10];
        *(ulong*)(frame + 0x88) = r.Gpr[R.R11];
        *(uint*)(frame + 0x90) = r.Mxcsr;
        *(ulong*)(frame + 0x98) = r.Gpr[R.Rax];
        for (int i = 0; i < 16; i++)
            ((ulong*)frame)[i] = r.Xmm[i];
        if (r.Rip != ret)
            pack[12] = r.Rip; // a transfer: the trampoline's `ret` goes there with the return address dropped
    }
}

// --- the HLE side: one handler for both models ------------------------------------------------------

static unsafe class Hle
{
    public const int Continue = 0, Block = 1;
    public static string? LastString;
    public static readonly List<int> Yields = new();

    public static int Handle(uint index, SfxRegs* r)
    {
        switch (index)
        {
            case 2: // add(a, b)
            case 6: // the same, behind a thunk slot in the fex model
                r->Gpr[R.Rax] = r->Gpr[R.Rdi] + r->Gpr[R.Rsi];
                return Continue;
            case 0: // args(str, 1, 2, 3, 4, 5, [rsp+8]=6, xmm0=2.5)
            {
                byte* s = (byte*)r->Gpr[R.Rdi]; // a guest pointer is a host pointer
                int len = 0;
                while (s[len] != 0)
                    len++;
                LastString = new string((sbyte*)s, 0, len);
                ulong stackArg = *(ulong*)(r->Gpr[R.Rsp] + 8);
                double x = BitConverter.UInt64BitsToDouble(r->Xmm[0]);
                ulong sum = r->Gpr[R.Rsi] + r->Gpr[R.Rdx] + r->Gpr[R.Rcx] + r->Gpr[R.R8] + r->Gpr[R.R9] + stackArg;
                r->Gpr[R.Rax] = sum + (ulong)len * 1000 + (ulong)(long)(x * 100);
                r->Xmm[0] = BitConverter.DoubleToUInt64Bits(x * 2);
                r->Xmm[1] = 0;
                return Continue;
            }
            case 1: // callback(fn, x): fn(x, 7) + 1, run on the guest from inside this import
                r->Gpr[R.Rax] = Program.Backend.Call(r->Gpr[R.Rdi], r->Gpr[R.Rsp], r->Gpr[R.Rsi], 7) + 1;
                return Continue;
            case 3: // yield(i): park this guest thread
                Yields.Add((int)r->Gpr[R.Rdi]);
                r->Gpr[R.Rax] = 0;
                return Block;
            case 4: // hash(ptr, len): FNV-1a over guest memory
                r->Gpr[R.Rax] = Fnv((byte*)r->Gpr[R.Rdi], (int)r->Gpr[R.Rsi]);
                return Continue;
            case 5: // transfer(target): continue at target, dropping the return address
                r->Rip = r->Gpr[R.Rdi];
                r->Gpr[R.Rsp] += 8;
                return Continue;
        }
        r->Gpr[R.Rax] = ulong.MaxValue;
        return Continue;
    }

    public static ulong Fnv(byte* p, int len)
    {
        ulong h = 0xCBF29CE484222325UL;
        for (int i = 0; i < len; i++)
        {
            h ^= p[i];
            h *= 0x100000001B3UL;
        }
        return h;
    }
}

static unsafe class Program
{
    public static bool Fex;
    public static IBackend Backend = null!;
    static int _failures;

    static void Check(string name, bool ok, string detail = "")
    {
        if (!ok)
            _failures++;
        Console.WriteLine($"[TEST] {name}: {(ok ? "OK" : "FAIL")}{(detail.Length > 0 ? " (" + detail + ")" : "")}");
    }

    static int Main(string[] args)
    {
        long n = 1_000_000;
        int rounds = 5;
        bool unaligned = false, testsOnly = false;
        string dir = AppContext.BaseDirectory;
        string blob = Path.Combine(dir, "guest.bin");
        string lib = Path.Combine(dir, "libsharpfex.so");
        string fexOptions = "";
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--n": n = long.Parse(args[++i]); break;
                case "--rounds": rounds = int.Parse(args[++i]); break;
                case "--blob": blob = args[++i]; break;
                case "--lib": lib = args[++i]; break;
                case "--fex": fexOptions = args[++i]; break;
                case "--unaligned": unaligned = true; break;
                case "--tests-only": testsOnly = true; break;
                case "--skip-threads": SkipThreads = true; break;
                case "--skip-slotfn": SkipSlotFn = true; break;
                case "--threads": ThreadWorkers = int.Parse(args[++i]); break;
                case "--thread-n": ThreadImports = long.Parse(args[++i]); break;
                case "--thread-timeout": ThreadTimeoutSec = int.Parse(args[++i]); break;
            }
        }

        Fex = RuntimeInformation.ProcessArchitecture == Architecture.Arm64;
        long t0 = Stopwatch.GetTimestamp();
        Guest.Load(blob);
        Backend = Fex ? new FexBackend(lib, fexOptions) : new X64Backend();
        Backend.Init();
        Backend.BindThread(0);
        double setupMs = Stopwatch.GetElapsedTime(t0).TotalMilliseconds;
        Console.WriteLine($"[POC] model={(Fex ? "fex" : "x64")} process={RuntimeInformation.ProcessArchitecture} " +
                          $"backend=\"{Backend.Describe()}\" setup={setupMs:F1}ms" +
                          (Backend is FexBackend f ? $" sfx_init={f.InitMs:F1}ms" : ""));
        Console.WriteLine($"[POC] guest image 0x{Guest.Base:X}, {Guest.SlotCount} import slots, code {Guest.A(Sym.CodeEnd) - Guest.Base} bytes");

        RunTests(unaligned);
        if (!testsOnly)
            RunBenches(n, rounds);

        if (Backend is FexBackend fs)
        {
            var s = fs.Stats();
            Console.WriteLine($"[POC] sharpfex stats: hle={s.HleCalls} callbacks={s.Callbacks} blocks={s.Blocks} transfers={s.Transfers} " +
                              $"foreign_syscalls={s.ForeignSyscalls} unaligned_fixups={s.UnalignedFixups} jit_restarts={s.JitRestarts} " +
                              $"signals_chained={s.SignalsChained}");
        }
        Console.WriteLine($"[POC] result: {(_failures == 0 ? "PASS" : $"FAIL ({_failures})")}");
        return _failures == 0 ? 0 : 1;
    }

    static void RunTests(bool unaligned)
    {
        long t0 = Stopwatch.GetTimestamp();
        ulong rc = Backend.Call(Guest.A(Sym.TestArgs), 0);
        double firstMs = Stopwatch.GetElapsedTime(t0).TotalMilliseconds;
        Check("args: 6 int + stack arg + xmm0 + guest string read by C#", rc == 0 && Hle.LastString == "hello from guest",
              $"rc={rc} string=\"{Hle.LastString}\" first call incl. translation {firstMs:F2} ms");

        rc = Backend.Call(Guest.A(Sym.TestRegs), 0);
        Check("regs: rbx rbp r12-r15 rcx r10 r11 survive an import", rc == 0, $"bad-register mask=0x{rc:X}");

        rc = Backend.Call(Guest.A(Sym.TestCallback), 0, 6);
        Check("callback: guest -> HLE -> guest -> HLE -> guest", rc == 6 * 7 + 2, $"rax={rc}");

        rc = Backend.Call(Guest.A(Sym.TestTransfer), 0);
        Check("transfer: HLE rewrites rip/rsp (longjmp/fiber shape)", rc == 0x77, $"rax=0x{rc:X}");

        if (Backend is FexBackend fb)
        {
            // a guest thread that blocks in an import four times: the FEX thread state is the continuation.
            nint worker = fb.NewThread(Guest.A(Sym.WorkerYield), 1);
            var results = new List<int>();
            int run;
            int guard = 0;
            const int RunBlocked = 1, RunReturned = 0;
            while ((run = fb.Run((void*)worker)) == RunBlocked && guard++ < 10)
                results.Add(run);
            SfxRegs regs;
            fb.GetRegs((void*)worker, &regs);
            fb.FreeThread(worker);
            fb.BindThread(0);
            bool ok = run == RunReturned && results.Count == 4 && Hle.Yields.Count == 4 && Hle.Yields[3] == 3 && regs.Gpr[R.Rax] == 0x600D;
            Check("block/resume: a guest thread parks 4x in an import and is resumed", ok,
                  $"blocked {results.Count}x, yields=[{string.Join(",", Hle.Yields)}], final rax=0x{regs.Gpr[R.Rax]:X}");

            // slot 6 is a `jmp` to FEX's thunk op (0f 3f + name): an inline host call, then `ret`.
            rc = fb.Call(Guest.A(Sym.BenchThunk), 0, 1000);
            Check("thunk slot: 1000 imports through FEX's thunk op", rc == 500500, $"rax={rc}");

            if (SkipThreads)
                Console.WriteLine("[TEST] threads: skipped (--skip-threads)");
            else
                ThreadsUnderGc(fb);
        }
        else
        {
            Console.WriteLine("[TEST] block/resume: n/a in the x64 model (SharpEmu uses its own continuation stubs)");
            Console.WriteLine("[TEST] threads: n/a in the x64 model (the PoC keeps the host rsp in one global, not TLS)");
        }

        bool caught = false;
        try { Deref(null); }
        catch (NullReferenceException) { caught = true; }
        Check(".NET null dereference still becomes NullReferenceException", caught);

        if (unaligned)
        {
            ulong p = Guest.A(Sym.Scratch) + 12; // 8 bytes at +12 straddle a 16-byte boundary
            *(ulong*)p = 41;
            rc = Backend.Call(Guest.A(Sym.Unaligned), 0, p);
            Check("unaligned locked add across 16 bytes", rc == 42, $"rax={rc}");
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Deref(int[]? a) => a![0];

    public static bool SkipThreads, SkipSlotFn;
    public static int ThreadWorkers = 4;
    public static long ThreadImports = 20_000;
    public static int ThreadTimeoutSec = 300;

    // guest threads on their own host threads while the main thread keeps the GC busy. the point is
    // correctness under GC (stack walks across FEX frames, suspension of threads inside imports),
    // not throughput, so the pressure is a steady drip -- a forced collection every few ms -- and a
    // watchdog reports progress, so a hang and a slow emulator can be told apart.
    static void ThreadsUnderGc(FexBackend fb)
    {
        int workers = Math.Clamp(ThreadWorkers, 1, Guest.Threads - 1);
        long perThread = ThreadImports;
        const long callbacks = 1000;
        var ok = new bool[workers];
        var done = new int[workers];
        var threads = new Thread[workers];
        for (int i = 0; i < workers; i++)
        {
            int id = i;
            threads[i] = new Thread(() =>
            {
                fb.BindThread(id + 1);
                ulong sum = fb.Call(Guest.A(Sym.BenchHle), 0, (ulong)perThread);
                ulong cb = fb.Call(Guest.A(Sym.BenchCb), 0, (ulong)callbacks);
                ok[id] = sum == (ulong)perThread * (ulong)(perThread + 1) / 2 && cb == ExpectedCb(callbacks);
                fb.UnbindThread();
                Volatile.Write(ref done[id], 1);
            }) { IsBackground = true };
        }
        long gc0 = GC.CollectionCount(0), gc2 = GC.CollectionCount(2);
        ulong hle0 = fb.Stats().HleCalls;
        long t0 = Stopwatch.GetTimestamp();
        foreach (var t in threads)
            t.Start();
        long iter = 0, nextReport = 5;
        bool timedOut = false;
        var keep = new List<byte[]>();
        while (!AllDone(done))
        {
            Thread.Sleep(1);
            var junk = new byte[16 * 1024];
            junk[iter & 0x3FFF] = (byte)iter;
            keep.Add(junk); // survives a while, so collections have real work and objects move
            if (keep.Count > 64)
                keep.RemoveRange(0, 32);
            if (++iter % 4 == 0)
                GC.Collect(iter % 64 == 0 ? 2 : 0, GCCollectionMode.Forced, blocking: true, compacting: true);
            double secs = Stopwatch.GetElapsedTime(t0).TotalSeconds;
            if (secs >= nextReport)
            {
                nextReport += 5;
                Console.WriteLine($"[THREADS] t={secs:F0}s imports={fb.Stats().HleCalls - hle0} done={string.Join("", Array.ConvertAll(done, d => d.ToString()))} " +
                                  $"gen0={GC.CollectionCount(0) - gc0} gen2={GC.CollectionCount(2) - gc2}");
            }
            if (secs > ThreadTimeoutSec)
            {
                timedOut = true;
                break;
            }
        }
        double total = Stopwatch.GetElapsedTime(t0).TotalSeconds;
        ulong imports = fb.Stats().HleCalls - hle0;
        // leave a quiet heap behind, so the benches that follow do not share the machine with a GC.
        keep.Clear();
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        Check($"threads: {workers} guest threads on {workers} host threads while the main thread forces GCs",
              !timedOut && Array.TrueForAll(ok, x => x),
              $"{(timedOut ? "TIMED OUT, " : "")}{GC.CollectionCount(0) - gc0} GCs ({GC.CollectionCount(2) - gc2} full, compacting), " +
              $"{perThread} imports + {callbacks} callbacks per thread, {imports} imports seen, {total:F1} s");
    }

    static bool AllDone(int[] done)
    {
        for (int i = 0; i < done.Length; i++)
            if (Volatile.Read(ref done[i]) == 0)
                return false;
        return true;
    }

    static ulong ExpectedCb(long n) => 7UL * (ulong)n * (ulong)(n - 1) / 2 + 2UL * (ulong)n;
    static ulong ExpectedPlain(long n) => 7UL * (ulong)n * (ulong)(n - 1) / 2 + (ulong)n;

    // --- benches ----------------------------------------------------------------------------------

    static void RunBenches(long n, int rounds)
    {
        long nHash = Math.Max(1, n / 4), nCb = Math.Max(1, n / 4), nCompute = n * 10;
        ulong hashBuf = Guest.A(Sym.HashBuf);
        ulong hash = Hle.Fnv((byte*)hashBuf, 64);
        ulong computeExpected = ComputeReference(nCompute);

        Bench("hle_add", "guest loop of import calls, C# body a+b", n, rounds,
              () => Backend.Call(Guest.A(Sym.BenchHle), 0, (ulong)n), (ulong)n * (ulong)(n + 1) / 2);
        Bench("hle_hash64", "imports that hash 64 bytes of guest memory in C#", nHash, rounds,
              () => Backend.Call(Guest.A(Sym.BenchHash), 0, (ulong)nHash, hashBuf, 64), (ulong)nHash * hash);
        Bench("hle_callback", "guest -> HLE -> guest callback -> HLE -> guest", nCb, rounds,
              () => Backend.Call(Guest.A(Sym.BenchCb), 0, (ulong)nCb), ExpectedCb(nCb));
        // a host->guest call with no import around it: C# calls guest_cb_plain(i, 7) at top level.
        ulong cbPlain = Guest.A(Sym.GuestCbPlain);
        Bench("call_top", "C# -> guest function -> C#, top level (no import around it)", nCb, rounds, () =>
        {
            ulong acc = 0;
            for (long i = 0; i < nCb; i++)
                acc += Backend.Call(cbPlain, 0, (ulong)i, 7);
            return acc;
        }, ExpectedPlain(nCb));
        if (Backend is FexBackend fc)
        {
            // the same calls made by C++ inside the shim: the guest entry and exit alone.
            Bench("call_builtin", "C++ -> guest function -> C++ (sfx_call's entry and exit alone)", nCb, rounds,
                  () => fc.BenchCall(cbPlain, (ulong)nCb), ExpectedPlain(nCb));
            if (!SkipSlotFn)
            {
                // hle_callback with slot 1 handled in C++: the nested call without managed code.
                fc.SetSlotFn(1, fc.Builtin("callback"));
                Bench("hle_callback_builtin", "hle_callback, slot 1 a C++ slot function (no managed code)", nCb, rounds,
                      () => Backend.Call(Guest.A(Sym.BenchCb), 0, (ulong)nCb), ExpectedCb(nCb));
                fc.SetSlotFn(1, null);
            }
            // the import boundary as FEX's thunk op (sharpfex.h, thunk slots): slot 6 = jmp to a 0f 3f.
            Bench("hle_add_thunk", "hle_add through a thunk slot, C# handler (sfx_regs copy)", n, rounds,
                  () => Backend.Call(Guest.A(Sym.BenchThunk), 0, (ulong)n), (ulong)n * (ulong)(n + 1) / 2);
            if (!SkipSlotFn)
            {
                fc.SetSlotFn(FexBackend.ThunkSlot, FexBackend.AddDirectPtr);
                Bench("hle_add_thunk_direct", "thunk slot, C# slot function (no copy)", n, rounds,
                      () => Backend.Call(Guest.A(Sym.BenchThunk), 0, (ulong)n), (ulong)n * (ulong)(n + 1) / 2);
                fc.SetSlotFn(FexBackend.ThunkSlot, fc.Builtin("add"));
                Bench("hle_add_thunk_builtin", "thunk slot, C++ slot function (the thunk boundary alone)", n, rounds,
                      () => Backend.Call(Guest.A(Sym.BenchThunk), 0, (ulong)n), (ulong)n * (ulong)(n + 1) / 2);
                fc.SetSlotFn(FexBackend.ThunkSlot, null);
            }
        }
        if (Backend is FexBackend fb && !SkipSlotFn)
        {
            // the same guest loop with slot 2 rerouted: what the copy costs, and what FEX's boundary costs alone.
            fb.SetSlotFn(2, FexBackend.AddDirectPtr);
            Bench("hle_add_direct", "same loop, C# slot function on FEX's registers (no copy)", n, rounds,
                  () => Backend.Call(Guest.A(Sym.BenchHle), 0, (ulong)n), (ulong)n * (ulong)(n + 1) / 2);
            fb.SetSlotFn(2, fb.Builtin("add"));
            Bench("hle_add_builtin", "same loop, C++ slot function (FEX's boundary alone)", n, rounds,
                  () => Backend.Call(Guest.A(Sym.BenchHle), 0, (ulong)n), (ulong)n * (ulong)(n + 1) / 2);
            fb.SetSlotFn(2, null);
        }
        Bench("guest_compute", "pure guest loop, no imports (the control)", nCompute, rounds,
              () =>
              {
                  new Span<byte>((void*)Guest.A(Sym.Scratch), 512).Clear();
                  return Backend.Call(Guest.A(Sym.BenchCompute), 0, (ulong)nCompute);
              }, computeExpected);
        // scalar SSE in the guest: the control for sfx.AFP=0 (FEX needs more instructions without AFP).
        Bench("guest_sse", "pure guest scalar SSE loop, no imports (the AFP control)", nCompute / 4, rounds,
              () => Backend.Call(Guest.A(Sym.BenchSse), 0, (ulong)(nCompute / 4)), SseReference(nCompute / 4));
        // the C# body alone, no guest at all: the floor every model pays.
        Bench("csharp_body", "the hle_add handler called directly from C#", n, rounds, () =>
        {
            SfxRegs r;
            ulong acc = 0;
            for (long i = 0; i < n; i++)
            {
                r.Gpr[R.Rdi] = (ulong)i;
                r.Gpr[R.Rsi] = 1;
                Hle.Handle(2, &r);
                acc += r.Gpr[R.Rax];
            }
            return acc;
        }, (ulong)n * (ulong)(n + 1) / 2);
    }

    // f_bench_sse in C#: IEEE single, round to nearest, no contraction -- bit-exact with SSE.
    static ulong SseReference(long n)
    {
        float x = 1.0f, k = 0.999f, a = 0.5f, big = 1.0e30f, eps = 1.0e-9f;
        for (long c = n; c > 0; c--)
        {
            x = x * k;
            x = x + a;
            x = x < big ? x : big;
            float t = (float)(int)c;
            t = t * eps;
            x = x + t;
        }
        return BitConverter.SingleToUInt32Bits(x);
    }

    static ulong ComputeReference(long n)
    {
        var table = new ulong[64];
        ulong x = 0x9E3779B97F4A7C15UL;
        for (long i = 0; i < n; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            int slot = (int)(x & 0xF8) / 8;
            x += table[slot];
            table[slot] = x;
        }
        return x;
    }

    static void Bench(string name, string what, long n, int rounds, Func<ulong> run, ulong expected)
    {
        // one untimed round compiles every block the loop touches (and warms the code cache).
        ulong first = run();
        var ns = new double[rounds];
        bool ok = first == expected;
        for (int i = 0; i < rounds; i++)
        {
            long t0 = Stopwatch.GetTimestamp();
            ulong got = run();
            ns[i] = Stopwatch.GetElapsedTime(t0).TotalNanoseconds / n;
            ok &= got == expected;
        }
        Array.Sort(ns);
        if (!ok)
            _failures++;
        Console.WriteLine($"[BENCH] {name} n={n} min={ns[0]:F1}ns med={ns[rounds / 2]:F1}ns per op{(ok ? "" : " WRONG RESULT")} -- {what}");
    }
}
