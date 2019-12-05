/*
 * Copyright (c) 2015, Nils Asmussen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 */

#include "arch/x86/m3/system.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/isa_traits.hh"
#include "arch/vtophys.hh"
#include "base/trace.hh"
#include "base/loader/object_file.hh"
#include "cpu/thread_context.hh"
#include "debug/DtuTlb.hh"
#include "mem/port_proxy.hh"
#include "mem/dtu/pt_unit.hh"
#include "mem/dtu/tlb.hh"
#include "mem/dtu/dtu.hh"
#include "params/M3X86System.hh"
#include "sim/byteswap.hh"

#include <libgen.h>

using namespace LittleEndianGuest;
using namespace X86ISA;

const unsigned M3X86System::RES_PAGES =
    (STACK_AREA + STACK_SIZE) >> DtuTlb::PAGE_BITS;

M3X86System::NoCMasterPort::NoCMasterPort(M3X86System &_sys)
  : QueuedMasterPort("noc_master_port", &_sys, reqQueue, snoopRespQueue),
    reqQueue(_sys, *this),
    snoopRespQueue(_sys, *this)
{ }

M3X86System::M3X86System(Params *p)
    : X86System(p),
      nocPort(*this),
      pes(p->pes),
      commandLine(p->boot_osflags),
      coreId(p->core_id),
      memPe(p->memory_pe),
      memOffset(p->memory_offset),
      memSize(p->memory_size),
      modOffset(p->mod_offset),
      modSize(p->mod_size),
      // don't reuse root pt
      nextFrame(RES_PAGES)
{
}

M3X86System::~M3X86System()
{
}

BaseMasterPort&
M3X86System::getMasterPort(const std::string &if_name, PortID idx)
{
    if (if_name == "noc_master_port")
        return nocPort;
    return System::getMasterPort(if_name, idx);
}

size_t
M3X86System::getArgc() const
{
    const char *cmd = commandLine.c_str();
    size_t argc = 0;
    size_t len = 0;
    while (*cmd)
    {
        if (isspace(*cmd))
        {
            if(len > 0)
                argc++;
            len = 0;
        }
        else
            len++;
        cmd++;
    }
    if(len > 0)
        argc++;

    return argc;
}

bool
M3X86System::isKernelArg(const std::string &arg)
{
    if (arg == "daemon")
        return true;
    if (arg.find("requires=") == 0)
        return true;
    if (arg.find("core=") == 0)
        return true;
    if (arg.find("pes=") == 0)
        return true;
    if (arg.find("repeat=") == 0)
        return true;
    return false;
}

void
M3X86System::writeArg(Addr &args, size_t &i, Addr argv, const char *cmd, const char *begin)
{
    const char zero[] = {0};
    // write argument pointer
    uint64_t argvPtr = args;
    physProxy.writeBlob(argv + i * sizeof(uint64_t), (uint8_t*)&argvPtr, sizeof(argvPtr));
    // write argument
    physProxy.writeBlob(args, (uint8_t*)begin, cmd - begin);
    args += cmd - begin;
    physProxy.writeBlob(args, (uint8_t*)zero, 1);
    args++;
    i++;
}

void
M3X86System::writeRemote(Addr dest, const uint8_t *data, size_t size)
{
    Request req(dest, size, 0, Request::funcMasterId);
    Packet pkt(&req, MemCmd::WriteReq);
    pkt.dataStaticConst(data);

    auto senderState = new Dtu::NocSenderState();
    senderState->packetType = Dtu::NocPacketType::CACHE_MEM_REQ_FUNC;
    senderState->result = Dtu::NONE;

    pkt.pushSenderState(senderState);

    nocPort.sendFunctional(&pkt);

    delete senderState;
}

Addr
M3X86System::loadModule(const std::string &path, const std::string &name, Addr addr)
{
    std::string filename = path + "/" + name;
    FILE *f = fopen(filename.c_str(), "r");
    if(!f)
        panic("Unable to open '%s' for reading", filename.c_str());

    fseek(f, 0L, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0L, SEEK_SET);

    auto data = new uint8_t[sz];
    if(fread(data, 1, sz, f) != sz)
        panic("Unable to read '%s'", filename.c_str());
    writeRemote(addr, data, sz);
    delete[] data;
    fclose(f);

    return sz;
}

void
M3X86System::mapPage(Addr virt, Addr phys, uint access)
{
    typedef PtUnit::PageTableEntry pte_t;
    Addr ptAddr = getRootPt().getAddr();
    for (int i = DtuTlb::LEVEL_CNT - 1; i >= 0; --i)
    {
        Addr idx = virt >> (DtuTlb::PAGE_BITS + i * DtuTlb::LEVEL_BITS);
        idx &= DtuTlb::LEVEL_MASK;

        Addr pteAddr = ptAddr + (idx << DtuTlb::PTE_BITS);
        pte_t entry = physProxy.read<pte_t>(pteAddr);
        assert(i > 0 || entry.ixwr == 0);
        if(!entry.ixwr)
        {
            // determine phys address
            Addr offset;
            if (i == 0)
                offset = memOffset + phys;
            else
                offset = memOffset + (nextFrame++ << DtuTlb::PAGE_BITS);
            NocAddr addr(memPe, 0, offset);

            // clear pagetables
            if (i > 0)
                physProxy.memsetBlob(addr.getAddr(), 0, DtuTlb::PAGE_SIZE);

            // insert entry
            entry.base = addr.getAddr() >> DtuTlb::PAGE_BITS;
            entry.ixwr = i == 0 ? access : DtuTlb::RWX;
            DPRINTF(DtuTlb,
                "Creating level %d PTE for virt=%#018x @ %#018x: %#018x\n",
                i, virt, pteAddr, entry);
            physProxy.write(pteAddr, entry);
        }

        ptAddr = entry.base << DtuTlb::PAGE_BITS;
    }
}

void
M3X86System::mapSegment(Addr start, Addr size, unsigned perm)
{
    Addr virt = start;
    size_t count = divCeil(size, DtuTlb::PAGE_SIZE);
    for(size_t i = 0; i < count; ++i)
    {
        mapPage(virt, virt, perm);

        virt += DtuTlb::PAGE_SIZE;
    }
}

void
M3X86System::mapMemory()
{
    // clear root pt
    physProxy.memsetBlob(getRootPt().getAddr(), 0, DtuTlb::PAGE_SIZE);

    // let the last entry in the root pt point to the root pt itself
    PtUnit::PageTableEntry entry = 0;
    entry.base = getRootPt().getAddr() >> DtuTlb::PAGE_BITS;
    // not internally accessible
    entry.ixwr = DtuTlb::RWX;
    size_t off = DtuTlb::PAGE_SIZE - sizeof(entry);
    DPRINTF(DtuTlb,
        "Creating recursive level %d PTE @ %#018x: %#018x\n",
        DtuTlb::LEVEL_CNT - 1, getRootPt().getAddr() + off, entry);
    physProxy.write(getRootPt().getAddr() + off, entry);

    // TODO check whether the size of idle fits before the RT_SPACE

    // program segments
    mapSegment(kernel->textBase(), kernel->textSize(),
        DtuTlb::INTERN | DtuTlb::RX);
    mapSegment(kernel->dataBase(), kernel->dataSize(),
        DtuTlb::INTERN | DtuTlb::RW);
    mapSegment(kernel->bssBase(), kernel->bssSize(),
        DtuTlb::INTERN | DtuTlb::RW);

    // idle doesn't need that stuff
    if (modOffset)
    {
        // initial heap
        Addr bssEnd = roundUp(kernel->bssBase() + kernel->bssSize(),
            DtuTlb::PAGE_SIZE);
        mapSegment(bssEnd, HEAP_SIZE, DtuTlb::INTERN | DtuTlb::RW);

        // state and stack
        mapSegment(RT_START, RT_SIZE, DtuTlb::INTERN | DtuTlb::RW);
        mapSegment(STACK_AREA, STACK_SIZE, DtuTlb::INTERN | DtuTlb::RW);
    }
    else
    {
        // map a large portion of the address space on app PEs
        // TODO this is temporary to still support clone and VPEs without AS
        mapSegment(RT_START, memSize - RT_START, DtuTlb::IRWX);
    }
}

void M3X86System::extractModules(std::string &args, std::vector<std::string> &sep, std::string &krnlPath,
    Addr argsAddr, Addr argv, std::vector<std::pair<std::string, std::string> > &mods, size_t lvl)
{
    const char *arg = args.c_str();
    const char *begin = arg;
    const char *separator = sep[lvl].c_str();
    size_t i = 0;
    std::string prog;
    std::string argstr;
    while (*arg)
    {
        if (isspace(*arg))
        {
            if (arg > begin)
            {
                // the first is the kernel; remember the path
                if (lvl == 0 && i == 0)
                {
                    std::string path(begin, arg - begin);
                    char *copy = strdup(path.c_str());
                    krnlPath = dirname(copy);
                    free(copy);
                }
                else if (modOffset)
                {
                    if (strncmp(begin, separator, sep[lvl].length()) == 0)
                    {
                        // look for modules needed by other kernels than the initial one
                        if(prog.compare(0, 6, "kernel") == 0) {
                            extractModules(argstr, sep, krnlPath, argsAddr, argv, mods, lvl + 1);
                        }
                        // only put in each module once
                        bool found = false;
                        for(auto it = mods.begin(); it != mods.end(); it++) {
                            if(prog.compare(it->first) == 0) {
                                found = true;
                                break;
                            }
                        }
                        if(!found && !prog.empty())
                            mods.push_back(std::make_pair(prog, argstr));
                        prog = "";
                        argstr = "";
                    }
                    else if(strncmp(begin, "pes=", 4) == 0) {
                        // PE assignment not allowed for initial kernel
                        assert(!(prog.empty() && lvl == 0));
                        std::string singleArg(begin, arg - begin);
                        if (!argstr.empty())
                            argstr += ' ';
                        argstr += singleArg;
                    }
                    else if(strncmp(begin, "repeat=", sizeof("repeat=") - 1) == 0 &&
                        (prog.compare(0, 6, "kernel") == 0)) {
                        std::string singleArg(begin, arg - begin);
                        if (!argstr.empty())
                            argstr += ' ';
                        argstr += singleArg;
                    }
                    else if (prog.empty())
                        prog = std::string(begin, arg - begin);
                    else
                    {
                        std::string singleArg(begin, arg - begin);
                        // TODO
                        // don't cut off kernel args that belong to a kernel of a different level
                        // currently this prevents multilevel kernel startup
                        if (!isKernelArg(singleArg))
                        {
                            if (!argstr.empty())
                                argstr += ' ';
                            argstr += singleArg;
                        }
                    }
                }

                if(lvl == 0)
                    writeArg(argsAddr, i, argv, arg, begin);
            }
            begin = arg + 1;
        }
        arg++;
    }

    if (arg > begin)
    {
        if (prog.empty() && strncmp(begin, "pes=", 4) != 0)
            prog = std::string(begin, arg - begin);
        else
        {
            std::string singleArg(begin, arg - begin);
            if (!isKernelArg(singleArg))
            {
                if (!argstr.empty())
                    argstr += ' ';
                argstr += singleArg;
            }
        }

        // look for modules needed by other kernels than the initial one
        if(prog.compare(0, 6, "kernel") == 0) {
            extractModules(argstr, sep, krnlPath, argsAddr, argv, mods, lvl + 1);
        }
        // only put in each module once
        bool found = false;
        for(auto it = mods.begin(); it != mods.end(); it++) {
            if(prog.compare(it->first) == 0) {
                found = true;
                break;
            }
        }
        if(!found && !prog.empty())
            mods.push_back(std::make_pair(prog, argstr));

        if(lvl == 0)
            writeArg(argsAddr, i, argv, arg, begin);
    }
}

void
M3X86System::initState()
{
    X86System::initState();

    // no internal memory? then we use paging
    if ((pes[coreId] & ~1) == 0)
        mapMemory();

    StartEnv env;
    memset(&env, 0, sizeof(env));
    env.secondaryrcvfbuf = 0;
    env.coreid = coreId;
    env.argc = getArgc();
    Addr argv = RT_START + sizeof(env);
    // the kernel gets the kernel env behind the normal env
    if (modOffset)
        argv += sizeof(KernelEnv);
    Addr args = argv + sizeof(void*) * env.argc;
    env.argv = reinterpret_cast<char**>(argv);

    // with paging, the kernel gets an initial heap mapped
    if ((pes[coreId] & ~1) == 0)
        env.heapsize = HEAP_SIZE;
    // otherwise, he should use all internal memory
    else
        env.heapsize = 0;

    // check if there is enough space
    if (commandLine.length() + 1 + args > RT_START + RT_SIZE)
    {
        panic("Command line \"%s\" is longer than %d characters.\n",
                commandLine, RT_START + RT_SIZE - args - 1);
    }

    if (pes.size() > MAX_PES)
    {
        const size_t max = MAX_PES;
        panic("Too many PEs (%u vs. %u)", pes.size(), max);
    }

    std::string kernelPath;
    std::string prog;
    std::string argstr;
    std::vector<std::pair<std::string,std::string>> mods;

    // write arguments to state area and determine boot modules
    std::vector<std::string> separators {"--", "++"};
    extractModules(commandLine, separators, kernelPath, args, argv, mods);

    // modules for the kernel
    if (modOffset)
    {
        KernelEnv kenv;

        // idle is always needed
        mods.push_back(std::make_pair("idle", ""));

        if (mods.size() > MAX_MODS)
            panic("Too many modules");

        size_t i = 0;
        Addr addr = NocAddr(memPe, 0, modOffset).getAddr();
        for (const std::pair<std::string, std::string> &mod : mods)
        {
            Addr size = loadModule(kernelPath, mod.first, addr);

            // construct module info
            BootModule bmod;
            size_t cmdlen = mod.first.length() + 1;// mod.second.length() + 1;
            if (cmdlen >= sizeof(bmod.name))
                panic("Module name too long: %s", mod.first.c_str());
            strcpy(bmod.name, mod.first.c_str());
//            if (!mod.second.empty())
//            {
//                strcat(bmod.name, " ");
//                strcat(bmod.name, mod.second.c_str());
//            }
            bmod.addr = addr;
            bmod.size = size;

            inform("Loaded '%s' to %p .. %p",
                bmod.name, bmod.addr, bmod.addr + bmod.size);

            // store pointer to area module info and info itself
            kenv.mods[i] = roundUp(addr + size, sizeof(uint64_t));
            writeRemote(kenv.mods[i],
                reinterpret_cast<uint8_t*>(&bmod), sizeof(bmod));

            // to next
            addr = kenv.mods[i] + sizeof(bmod);
            addr += DtuTlb::PAGE_SIZE - 1;
            addr &= ~static_cast<Addr>(DtuTlb::PAGE_SIZE - 1);
            i++;
        }

        // termination
        kenv.mods[i] = 0;

        // build PE array
        kenv.pe_count = pes.size();
        memset(kenv.pes, 0, sizeof(kenv.pes));
        memset(kenv.mem_mods, 0, sizeof(kenv.mem_mods));
        size_t j = 0;
        for (size_t i = 0; i < pes.size(); ++i)
        {
            // 63..54: core id
            // 53..12: memsize in pages (0 = no internal memory)
            // 3..0  : PE type
            if (pes[i] & 1) {
                kenv.pes[i] = KernelEnv::TYPE_MEM;
                if(j > MAX_MEM_MODS)
                    panic("Too many memory modules");
                kenv.mem_mods[j++] = {i, 0, pes[i] & ~0x7};
                inform("Defined memory module. pe: %d size: %d", i, kenv.mem_mods[i].size);
            }
            else if(pes[i] & ~1)
                kenv.pes[i] = KernelEnv::TYPE_IMEM;
            else
                kenv.pes[i] = KernelEnv::TYPE_EMEM;
            kenv.pes[i] |= i << 54;
            kenv.pes[i] |= pes[i] & ~1;
        }

        // the kernel needs to PE info in its env
        env.pe = kenv.pes[coreId];

        // the initial kernel is identified by being created by itself
        kenv.kernelId = 0;
        kenv.creatorKernelId = 0;

        // write kenv
        env.kenv = addr;
        writeRemote(env.kenv, reinterpret_cast<uint8_t*>(&kenv), sizeof(kenv));
        addr += sizeof(kenv);

        // check size
        Addr end = NocAddr(memPe, 0, modOffset + modSize).getAddr();
        if (addr > end)
        {
            panic("Modules are too large (have: %lu, need: %lu)",
                modSize, addr - NocAddr(memPe, 0, modOffset).getAddr());
        }
    }

    // write env
    physProxy.writeBlob(
        RT_START, reinterpret_cast<uint8_t*>(&env), sizeof(env));
}

M3X86System *
M3X86SystemParams::create()
{
    return new M3X86System(this);
}
