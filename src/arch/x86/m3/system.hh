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

#ifndef __ARCH_M3_X86_SYSTEM_HH__
#define __ARCH_M3_X86_SYSTEM_HH__

#include <string>
#include <vector>

#include "arch/x86/system.hh"
#include "params/M3X86System.hh"
#include "mem/qport.hh"
#include "mem/dtu/noc_addr.hh"

class M3X86System : public X86System
{
  protected:
    static const size_t MAX_MODS        = 64;
    static const size_t MAX_PES         = 1024;
    static const size_t MAX_MEM_MODS    = 4;
    static const size_t RT_SIZE         = 0x20000;
    static const uintptr_t RT_START     = 0x3000;
    static const size_t STACK_SIZE      = 0x1000;
    static const uintptr_t STACK_AREA   = RT_START + RT_SIZE;
    static const size_t HEAP_SIZE       = 0x1000;
    static const unsigned RES_PAGES;

    class NoCMasterPort : public QueuedMasterPort
    {
      protected:

        ReqPacketQueue reqQueue;

        SnoopRespPacketQueue snoopRespQueue;

      public:

        NoCMasterPort(M3X86System &_sys);

        bool recvTimingResp(PacketPtr) override
        {
            // unused
            return true;
        }
    };

    struct BootModule
    {
        char name[128];
        uint64_t addr;
        uint64_t size;
    } M5_ATTR_PACKED;

    struct MemPEDesc {
        size_t pe;
        uintptr_t offs;
        uintptr_t size;
    } M5_ATTR_PACKED;

    struct KernelEnv
    {
        enum
        {
            TYPE_IMEM    = 0,
            TYPE_EMEM    = 1,
            TYPE_MEM     = 2,
        };

        uint64_t mods[MAX_MODS];
        uint64_t pe_count;
        uint64_t pes[MAX_PES];
        uint32_t kernelId;
        uint32_t creatorKernelId;
        uint32_t creatorCore;
        int32_t creatorThread;
        int32_t creatorEp;
        MemPEDesc mem_mods[MAX_MEM_MODS];
        size_t memOffset;
        uint64_t memberTable;
        uint64_t ddlPartitions;
        size_t ddlPartitionsSize;
    } M5_ATTR_PACKED;

    struct StartEnv
    {
        uint64_t coreid;
        uint32_t argc;
        char **argv;

        uint64_t sp;
        uint64_t entry;
        uint64_t lambda;
        uint32_t pager_sess;
        uint32_t pager_gate;
        uint32_t mounts_len;
        uint64_t mounts;
        uint32_t fds_len;
        uint64_t fds;
        uint64_t eps;
        uint64_t caps;
        uint64_t exit;
        uint64_t heapsize;

        uint64_t backend;
        uint64_t kenv;
        uint64_t pe;
        uint64_t secondaryrcvfbuf;
    } M5_ATTR_PACKED;

    NoCMasterPort nocPort;
    std::vector<Addr> pes;
    std::string commandLine;

  public:
    const unsigned coreId;
    const unsigned memPe;
    const Addr memOffset;
    const Addr memSize;
    const Addr modOffset;
    const Addr modSize;
    unsigned nextFrame;

  public:
    typedef M3X86SystemParams Params;
    M3X86System(Params *p);
    ~M3X86System();

    BaseMasterPort& getMasterPort(const std::string &if_name,
                                  PortID idx = InvalidPortID) override;

    NocAddr getRootPt() const
    {
        return NocAddr(memPe, 0, memOffset);
    }

    void initState();

  private:
    bool isKernelArg(const std::string &arg);
    void mapPage(Addr virt, Addr phys, uint access);
    void mapSegment(Addr start, Addr size, unsigned perm);
    void mapMemory();
    size_t getArgc() const;
    void writeRemote(Addr dest, const uint8_t *data, size_t size);
    void writeArg(Addr &args, size_t &i, Addr argv, const char *cmd, const char *begin);
    Addr loadModule(const std::string &path, const std::string &name, Addr addr);
    void extractModules(std::string &args, std::vector<std::string> &sep, std::string &krnlPath,
            Addr argsAddr, Addr argv, std::vector<std::pair<std::string, std::string> > &mods,
            size_t lvl = 0);
};

#endif
