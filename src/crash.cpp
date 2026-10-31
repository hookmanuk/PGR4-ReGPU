// Unhandled-exception report: host address, symbol (sub_XXXXXXXX is the
// guest function), and the guest address of a bad memory access.
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <string>

#include "kernel/imports.h"
#include "kernel/thread.h"
#include "live_switch.h"
#include "log.h"
#include "memory.h"

namespace
{
void Describe(DWORD64 address, char* out, size_t size)
{
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
    {
        if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line))
        {
            const char* file = strrchr(line.FileName, '\\');
            snprintf(out, size, "%s+%llX (%s:%lu)", symbol->Name, displacement, file ? file + 1 : line.FileName, line.LineNumber);
        }
        else
            snprintf(out, size, "%s+%llX", symbol->Name, displacement);
    }
    else
        snprintf(out, size, "%016llX", address);
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
{
    const auto* record = info->ExceptionRecord;
    char where[300];
    Describe(DWORD64(record->ExceptionAddress), where, sizeof(where));
    GuestThread* thread = CurrentGuestThread();
    Log("CRASH: exception %08lX at %s on guest thread %u", record->ExceptionCode, where, thread ? thread->id : 0);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const uint64_t fault = record->ExceptionInformation[1];
        const uint64_t guest = fault - uint64_t(g_base);
        Log("CRASH: %s host %016llX = guest %llX", record->ExceptionInformation[0] ? "write to" : "read from", fault, guest);
    }
    if (thread && thread->ctx)
    {
        Log("CRASH: guest lr %08llX r1 %08X r3 %08X", thread->ctx->lr, thread->ctx->r1.u32, thread->ctx->r3.u32); // (ctr is a local of the recompiled code)
        // What the object registers point at (a wiped object shows here as zeros or as whatever
        // overwrote it).
        const PPCRegister* regs[] = { &thread->ctx->r3, &thread->ctx->r11, &thread->ctx->r28, &thread->ctx->r29, &thread->ctx->r30, &thread->ctx->r31 };
        const char* names[] = { "r3", "r11", "r28", "r29", "r30", "r31" };
        for (int i = 0; i < 6; i++)
        {
            const uint32_t a = regs[i]->u32;
            if (a < 0x40000000 || a >= 0x7F000000 || !GuestAllocationSize(a & ~0xFu))
            {
                Log("CRASH: %s %08X", names[i], a);
                continue;
            }
            char line[200];
            int n = snprintf(line, sizeof(line), "CRASH: %s %08X:", names[i], a);
            for (uint32_t k = 0; k < 48 && n < int(sizeof(line)) - 10; k += 4)
                n += snprintf(line + n, sizeof(line) - n, " %08X", LoadBE32((a & ~3u) + k));
            Log("%s", line);
        }
    }

    // Host call stack.
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; i++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
            break;
        Describe(frame.AddrPC.Offset, where, sizeof(where));
        Log("CRASH:   #%d %s", i, where);
    }
    Log("CRASH: other threads:");
    LogGuestThreads();
    return EXCEPTION_EXECUTE_HANDLER;
}
}

// Host stack of another thread (suspended while walking); names of
// recompiled frames are the guest functions.
std::string HostStack(void* threadHandle, int maxFrames)
{
    std::string out;
    if (GetThreadId(threadHandle) == GetCurrentThreadId())
        return "(this thread)";
    if (SuspendThread(threadHandle) == DWORD(-1))
        return out;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(threadHandle, &context))
    {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        char where[300];
        for (int i = 0; i < maxFrames; i++)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), threadHandle, &frame, &context, nullptr,
                    SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
                break;
            Describe(frame.AddrPC.Offset, where, sizeof(where));
            char* name = where;
            if (!strncmp(name, "__imp__", 7))
                name += 7;
            if (char* plus = strchr(name, '+'))
                *plus = 0;
            if (!out.empty())
                out += " < ";
            out += name;
        }
    }
    ResumeThread(threadHandle);
    return out;
}

void CrashHandlerInit()
{
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SetUnhandledExceptionFilter(OnCrash);
}

// Hardware write watchpoint on a guest byte (PGR4_WATCHW=addr or
// PGR4_WATCHW=ptr,offset to watch [ptr]+offset). Logs the writing thread's
// host stack, i.e. the guest functions, for the first writes.
namespace
{
std::atomic<int> g_watchHits{ 0 };

LONG WINAPI OnWatchHit(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 7))
        return EXCEPTION_CONTINUE_SEARCH;
    // PGR4_WATCH_SKIP=n: log from the n-th hit on (past the boot's writes).
    static const int watchSkip = getenv("PGR4_WATCH_SKIP") ? atoi(getenv("PGR4_WATCH_SKIP")) : 0;
    static const int watchLimit = (getenv("PGR4_WATCHRW") ? 3000 : 40) + watchSkip;
    if (g_watchHits < watchSkip && g_watchHits++ >= 0)
    {
        info->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_watchHits++ < watchLimit)
    {
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        std::string out;
        char where[300];
        for (int i = 0; i < 6 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++)
        {
            Describe(frame.AddrPC.Offset, where, sizeof(where));
            out += out.empty() ? "" : " < ";
            out += where;
        }
        GuestThread* t = CurrentGuestThread();
        // (Reading the byte here would trigger a read watchpoint again.)
        static LiveSwitch readsToo("PGR4_WATCHRW");
        Log("watchpoint: thread %u %s %02X (dr6 %llX): %s", t ? t->id : 0, readsToo ? "touched" : "wrote",
            readsToo ? 0u : unsigned(*reinterpret_cast<uint8_t*>(info->ContextRecord->Dr0)), info->ContextRecord->Dr6, out.c_str());
        static int ringDumps = 0;
        if (const char* ring = getenv("PGR4_DUMP_RING"); ring && (out.find("sub_829560A0") != std::string::npos || out.find("sub_82957260") != std::string::npos) && ringDumps++ == 0)
        {
            const uint32_t a = uint32_t(strtoul(ring, nullptr, 16)), b = uint32_t(strtoul(strchr(ring, ',') + 1, nullptr, 16));
            if (FILE* f = fopen("out/ring.bin", "wb"))
            {
                fwrite(g_base + a, 1, b - a, f);
                fclose(f);
                Log("watchpoint: dumped ring %08X..%08X at the first hit", a, b);
            }
        }
        if (t && t->ctx)
        {
            const PPCContext& g = *t->ctx;
            Log("watchpoint:   r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X r11 %08X r12 %08X", g.r3.u32, g.r4.u32, g.r5.u32,
                g.r6.u32, g.r7.u32, g.r8.u32, g.r9.u32, g.r10.u32, g.r11.u32, g.r12.u32);
            Log("watchpoint:   r24 %08X r25 %08X r26 %08X r27 %08X r28 %08X r29 %08X r30 %08X r31 %08X lr %08llX", g.r24.u32, g.r25.u32, g.r26.u32,
                g.r27.u32, g.r28.u32, g.r29.u32, g.r30.u32, g.r31.u32, g.lr);
        }
    }
    info->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
}

void ArmWatchpoint(void* threadHandle)
{
    static uint64_t address = [] {
        const char* v = getenv("PGR4_WATCHW");
        if (!v)
            return uint64_t(0);
        AddVectoredExceptionHandler(1, OnWatchHit);
        uint32_t a = uint32_t(strtoul(v, nullptr, 16));
        if (const char* comma = strchr(v, ','))
            a = LoadBE32(a) + uint32_t(strtoul(comma + 1, nullptr, 16));
        Log("watchpoint on guest %08X", a);
        return uint64_t(g_base + a);
    }();
    if (!address)
        return;
    if (SuspendThread(threadHandle) == DWORD(-1))
        return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(threadHandle, &c);
    c.Dr0 = address;
    static LiveSwitch readsToo("PGR4_WATCHRW"); // trap reads as well as writes
    c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | ((readsToo ? 3ull : 1ull) << 16); // L0, write (or read/write), 1 byte
    const uint32_t guest = uint32_t(address - uint64_t(g_base));
    if (guest >= 0xA0000000)
    {
        // Physical memory is mapped three times; watch every alias.
        const uint32_t offset = guest & 0x1FFFFFFF;
        uint64_t* regs[] = { &c.Dr1, &c.Dr2 };
        int n = 0;
        for (uint32_t view : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
        {
            if (view + offset == guest || n == 2)
                continue;
            *regs[n] = uint64_t(g_base + view + offset);
            c.Dr7 |= (1ull << (2 + n * 2)) | (1ull << (20 + n * 4));
            n++;
        }
    }
    SetThreadContext(threadHandle, &c);
    ResumeThread(threadHandle);
}

// Host call stack of the calling thread (recompiled frames name the guest functions).
std::string CurrentHostStack(int maxFrames)
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    std::string out;
    char where[300];
    for (int i = 0; i < maxFrames + 1; i++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr, SymFunctionTableAccess64,
                SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
            break;
        if (i == 0)
            continue; // this function
        Describe(frame.AddrPC.Offset, where, sizeof(where));
        out += out.empty() ? "" : " < ";
        out += where;
    }
    return out;
}

// An indirect call to a guest address with no recompiled function (ppc_hooks.h PPC_CALL_INDIRECT_FUNC):
// the target, the caller and the first arguments, then the usual crash report.
void PPCIndirectMiss(PPCContext& ctx, uint32_t target)
{
    Log("CRASH: indirect call to %08X, which is not a recompiled function (caller lr %08llX, r3 %08X r4 %08X r5 %08X)", target,
        (unsigned long long)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    fflush(nullptr);
    __builtin_trap();
}
