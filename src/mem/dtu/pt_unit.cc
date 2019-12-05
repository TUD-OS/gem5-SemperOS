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

#include "debug/Dtu.hh"
#include "debug/DtuPf.hh"
#include "mem/dtu/dtu.hh"
#include "mem/dtu/pt_unit.hh"

const std::string
PtUnit::TranslateEvent::name() const
{
    return unit.dtu.name();
}

void
PtUnit::TranslateEvent::requestPTE()
{
    auto pkt = unit.createPacket(virt, ptAddr, level);
    if (!pkt)
    {
        finish(false, NocAddr(0));
        return;
    }

    unit.dtu.sendMemRequest(pkt,
                            -1,  // no virtual address here
                            reinterpret_cast<Addr>(this),
                            Dtu::MemReqType::TRANSLATION,
                            Cycles(0));
}

void
PtUnit::TranslateEvent::process()
{
    if (pf)
    {
        requestPTE();
        return;
    }

    // first check the TLB again; maybe we don't need to do a translation
    NocAddr phys;
    DtuTlb::Result res = unit.dtu.tlb->lookup(virt, access, &phys);
    if (res == DtuTlb::HIT)
        finish(true, phys);
    else if (res == DtuTlb::NOMAP)
        finish(false, phys);
    else if (res == DtuTlb::PAGEFAULT)
    {
        if (!unit.sendPagefaultMsg(this, virt, access))
            finish(false, NocAddr(0));
    }
    else
        requestPTE();
}

void
PtUnit::TranslateEvent::recvFromMem(PacketPtr pkt)
{
    Addr phys;
    uint flags = access;
    bool success = unit.finishTranslate(pkt, virt, level, &flags, &phys);

    if (success)
    {
        if (level > 0)
        {
            level--;
            ptAddr = phys;
            requestPTE();
            return;
        }

        unit.mkTlbEntry(virt, NocAddr(phys), flags);

        finish(success, NocAddr(phys + (virt & DtuTlb::PAGE_MASK)));
    }
    else
    {
        if (!unit.sendPagefaultMsg(this, virt, access))
            finish(false, NocAddr(0));
    }
}

bool
PtUnit::translateFunctional(Addr virt, uint access, NocAddr *phys)
{
    Addr ptePhys;
    Addr ptAddr = dtu.regs().get(DtuReg::ROOT_PT);
    for (int level = DtuTlb::LEVEL_CNT - 1; level >= 0; --level)
    {
        auto pkt = createPacket(virt, ptAddr, level);
        if (!pkt)
            return false;

        dtu.sendFunctionalMemRequest(pkt);

        uint acccopy = access;
        if (!finishTranslate(pkt, virt, level, &acccopy, &ptePhys))
            return false;

        ptAddr = ptePhys;
    }
    *phys = NocAddr(ptePhys + (virt & DtuTlb::PAGE_MASK));
    return true;
}

const char *
PtUnit::describeAccess(uint access)
{
    static char rwx[4];
    rwx[0] = (access & DtuTlb::READ ) ? 'r' : '-';
    rwx[1] = (access & DtuTlb::WRITE) ? 'w' : '-';
    rwx[2] = (access & DtuTlb::EXEC ) ? 'x' : '-';
    return rwx;
}

bool
PtUnit::sendPagefaultMsg(TranslateEvent *ev, Addr virt, uint access)
{
    if (~dtu.regs().get(DtuReg::STATUS) & static_cast<int>(Status::PAGEFAULTS))
    {
        DPRINTFS(DtuPf, (&dtu),
            "Pagefault (%s @ %p), but pagefault sending is disabled\n",
            describeAccess(access), virt);

        if (!pfqueue.empty())
        {
            DPRINTFS(DtuPf, (&dtu),
                "Dropping all pending pagefaults (%lu)\n",
                pfqueue.size());

            for (auto qev = pfqueue.begin(); qev != pfqueue.end(); ++qev)
                (*qev)->finish(false, NocAddr(0));
            pfqueue.clear();
        }
        return false;
    }

    // remove all access rights to ensure that all accesses fault until we have
    // resolved it. this is required, because it seems that the LSQUnit does
    // not forward loads to the store buffer properly, if the store has already
    // been sent to cache, but took longer because of a DTU PF
    dtu.tlb->block(virt, true);

    int pfep = ev->toKernel ? Dtu::SYSCALL_EP : dtu.regs().get(DtuReg::PF_EP);
    assert(pfep < dtu.numEndpoints);
    SendEp ep = dtu.regs().getSendEp(pfep);

    // fall back to syscall EP, if the PF ep is invalid
    if(ep.maxMsgSize == 0)
    {
        ev->toKernel = true;
        pfep = Dtu::SYSCALL_EP;
        ep = dtu.regs().getSendEp(pfep);
    }

    size_t size = sizeof(Dtu::MessageHeader) + sizeof(PagefaultMessage);
    assert(size <= ep.maxMsgSize);

    if (pfqueue.empty())
        pfqueue.push_back(ev);
    else if (pfqueue.front() != ev)
    {
        uint page = virt >> DtuTlb::PAGE_BITS;
        for(auto qev = pfqueue.begin(); qev != pfqueue.end(); ++qev)
        {
            // can we merge the requests?
            if (access == (*qev)->access &&
                page == ((*qev)->virt >> DtuTlb::PAGE_BITS))
            {
                DPRINTFS(DtuPf, (&dtu),
                    "Adding Pagefault @ %#x to running request (%s @ %#x)\n",
                    virt, describeAccess(access), (*qev)->virt);

                (*qev)->trans.push_back(*ev->trans.begin());
                delete ev;
                return true;
            }
        }

        // try again later
        DPRINTFS(DtuPf, (&dtu),
            "Appending Pagefault (%s @ %#x) to queue\n",
            describeAccess(access), virt);

        ev->pf = true;
        pfqueue.push_back(ev);
        return true;
    }

    // create packet
    NocAddr nocAddr = NocAddr(ep.targetCore, ep.vpeId, ep.targetEp);
    auto pkt = dtu.generateRequest(nocAddr.getAddr(),
                                   size,
                                   MemCmd::WriteReq);

    // build the message and put it in the packet
    Dtu::MessageHeader header;
    header.length = sizeof(PagefaultMessage);
    header.flags = Dtu::PAGEFAULT | Dtu::REPLY_ENABLED;
    header.label = ep.label;
    header.senderEpId = pfep;
    header.senderCoreId = dtu.coreId;
    header.replyLabel = reinterpret_cast<uint64_t>(ev);
    // not used
    header.replyEpId = 0;

    memcpy(pkt->getPtr<uint8_t>(),
           &header,
           sizeof(header));

    PagefaultMessage msg;
    msg.opcode = PagefaultMessage::OPCODE_PF;
    msg.virt = virt;
    msg.access = access;

    memcpy(pkt->getPtr<uint8_t>() + sizeof(header),
           &msg,
           sizeof(msg));

    DPRINTFS(Dtu, (&dtu),
             "\e[1m[sd -> %u]\e[0m with EP%u for Pagefault (%s @ %p)\n",
             ep.targetCore, pfep, describeAccess(access), virt);

    DPRINTFS(Dtu, (&dtu),
        "  header: flags=%#x tgtEP=%u lbl=%#018lx rpLbl=%#018lx rpEP=%u\n",
        header.flags, ep.targetEp, header.label,
        header.replyLabel, header.replyEpId);

    // send the packet
    Cycles delay = dtu.transferToNocLatency;
    // delay += dtu.ticksToCycles(headerDelay);
    // pkt->payloadDelay = payloadDelay;
    dtu.printPacket(pkt);
    dtu.sendNocRequest(Dtu::NocPacketType::PAGEFAULT, pkt, delay);
    return true;
}

void
PtUnit::sendingPfFailed(PacketPtr pkt, int error)
{
    Dtu::MessageHeader* header = pkt->getPtr<Dtu::MessageHeader>();
    TranslateEvent *ev = reinterpret_cast<TranslateEvent*>(header->replyLabel);

    DPRINTFS(DtuPf, (&dtu),
        "Sending Pagefault (%s @ %p) failed (%d); notifying kernel\n",
        describeAccess(ev->access), ev->virt, error);

    if (error == Dtu::VPE_GONE)
    {
        ev->pf = true;
        ev->toKernel = true;
        dtu.schedule(ev, dtu.clockEdge(Cycles(1)));
    }
    else
    {
        panic("Unable to resolve pagefault (%s @ %p)",
            describeAccess(ev->access), ev->virt);
    }

    nextPagefault(ev);
}

void
PtUnit::finishPagefault(PacketPtr pkt)
{
    Dtu::MessageHeader* header = pkt->getPtr<Dtu::MessageHeader>();
    uint64_t *errorPtr = reinterpret_cast<uint64_t*>(header + 1);
    size_t expSize = sizeof(Dtu::MessageHeader) + sizeof(uint64_t);
    int error = pkt->getSize() == expSize ? *errorPtr : -1;

    TranslateEvent *ev = reinterpret_cast<TranslateEvent*>(header->label);

    DPRINTFS(Dtu, (&dtu),
        "\e[1m[rv <- %u]\e[0m %lu bytes for Pagefault (%s @ %p)\n",
        header->senderCoreId, header->length,
        describeAccess(ev->access), ev->virt);
    dtu.printPacket(pkt);

    pkt->makeResponse();

    Cycles delay = dtu.ticksToCycles(
        pkt->headerDelay + pkt->payloadDelay);
    delay += dtu.nocToTransferLatency;

    nextPagefault(ev, delay);

    if (!dtu.atomicMode)
    {
        pkt->headerDelay = 0;
        pkt->payloadDelay = 0;

        dtu.schedNocRequestFinished(dtu.clockEdge(Cycles(1)));
        dtu.schedNocResponse(pkt, dtu.clockEdge(delay));
    }

    if (error != 0)
    {
        if (pkt->getSize() != expSize)
            DPRINTFS(DtuPf, (&dtu), "Invalid response for pagefault\n");
        else
        {
            DPRINTFS(DtuPf, (&dtu),
                "Pagefault for %s @ %p could not been resolved: %d\n",
                describeAccess(ev->access), ev->virt, error);
        }

        // if the pagefault handler tells us that there is no mapping, just
        // store an entry with flags=0. this way, we will remember that we
        // already tried to access there with no success
        if (error == Dtu::NO_MAPPING)
            mkTlbEntry(ev->virt, NocAddr(0), 0);
        else
            dtu.tlb->block(ev->virt, false);

        ev->finish(false, NocAddr(0));
        return;
    }

    DPRINTFS(DtuPf, (&dtu),
        "Retrying pagetable walk for %s @ %p\n",
        describeAccess(ev->access), ev->virt);

    // retry the translation
    ev->pf = false;
    ev->toKernel = false;
    dtu.schedule(ev, dtu.clockEdge(Cycles(1)));
}

void PtUnit::mkTlbEntry(Addr virt, NocAddr phys, uint flags)
{
    Addr tlbVirt = virt & ~DtuTlb::PAGE_MASK;

    DPRINTFS(DtuPf, (&dtu),
        "Inserting into TLB: virt=%p phys=%p flags=%u\n",
        tlbVirt, phys.offset, flags);

    dtu.tlb->insert(tlbVirt, phys, flags);
}

void
PtUnit::nextPagefault(TranslateEvent *ev, Cycles delay)
{
    assert(pfqueue.front() == ev);
    pfqueue.pop_front();

    if (!pfqueue.empty())
    {
        TranslateEvent *ev = pfqueue.front();
        dtu.schedule(ev, dtu.clockEdge(delay));
    }
}

PacketPtr
PtUnit::createPacket(Addr virt, Addr ptAddr, int level)
{
    Addr idx = virt >> (DtuTlb::PAGE_BITS + level * DtuTlb::LEVEL_BITS);
    idx &= DtuTlb::LEVEL_MASK;

    NocAddr pteAddr(ptAddr + (idx << DtuTlb::PTE_BITS));
    auto pkt = dtu.generateRequest(pteAddr.getAddr(),
                                   sizeof(PtUnit::PageTableEntry),
                                   MemCmd::ReadReq);

    DPRINTFS(DtuPf, (&dtu), "Loading level %d PTE for %p from %p\n",
             level, virt, pteAddr.getAddr());

    return pkt;
}

bool
PtUnit::finishTranslate(PacketPtr pkt,
                        Addr virt,
                        int level,
                        uint *access,
                        Addr *phys)
{
    PageTableEntry *e = pkt->getPtr<PageTableEntry>();

    DPRINTFS(DtuPf, (&dtu), "Received level %d PTE for %p: %#x\n",
             level, virt, (uint64_t)*e);

    // for last-level PTEs, we need the desired access permissions
    if (level == 0)
    {
        if ((e->ixwr & *access) != *access)
            return false;
    }
    // for others, we don't need the INTERN bit
    else
    {
        uint pteAccess = *access & ~DtuTlb::INTERN;
        if ((e->ixwr & pteAccess) != pteAccess)
            return false;
    }

    *access = static_cast<uint>((uint64_t)e->ixwr);
    *phys = e->base << DtuTlb::PAGE_BITS;
    return true;
}

void
PtUnit::resolveFailed(Addr virt)
{
    if (virt == lastPfAddr)
    {
        if (++lastPfCnt == 100)
        {
            dtu.regs().set(DtuReg::LAST_PF, virt);
            // TODO make the vector configurable
            dtu.injectIRQ(0x41);
        }
    }
    else
    {
        lastPfAddr = virt;
        lastPfCnt = 1;
    }
}

void
PtUnit::startTranslate(Addr virt, uint access, Translation *trans, bool pf)
{
    TranslateEvent *event = new TranslateEvent(*this);
    event->level = DtuTlb::LEVEL_CNT - 1;
    event->virt = virt;
    event->access = access;
    event->trans.push_back(trans);
    event->ptAddr = dtu.regs().get(DtuReg::ROOT_PT);
    event->pf = pf;
    event->toKernel = false;

    dtu.schedule(event, dtu.clockEdge(Cycles(1)));
}
