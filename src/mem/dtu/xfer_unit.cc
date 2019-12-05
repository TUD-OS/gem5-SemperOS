/*
 * Copyright (c) 2015, Christian Menard
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
#include "debug/DtuBuf.hh"
#include "debug/DtuPackets.hh"
#include "debug/DtuSysCalls.hh"
#include "debug/DtuPower.hh"
#include "debug/DtuXfers.hh"
#include "debug/DtuTlb.hh"
#include "mem/dtu/xfer_unit.hh"

XferUnit::XferUnit(Dtu &_dtu,
                   size_t _blockSize,
                   size_t _bufCount,
                   size_t _bufSize)
    : dtu(_dtu),
      blockSize(_blockSize),
      bufCount(_bufCount),
      bufSize(_bufSize),
      bufs(new Buffer*[bufCount])
{
    for (size_t i = 0; i < bufCount; ++i)
        bufs[i] = new Buffer(*this, i, bufSize);
}

XferUnit::~XferUnit()
{
    for (size_t i = 0; i < bufCount; ++i)
        delete bufs[i];
    delete[] bufs;
}

void
XferUnit::TransferEvent::process()
{
    NocAddr phys(localAddr);
    if (xfer.dtu.tlb)
    {
        bool writing = type == Dtu::TransferType::REMOTE_WRITE ||
                       type == Dtu::TransferType::LOCAL_WRITE;
        bool remote  = type == Dtu::TransferType::REMOTE_WRITE ||
                       type == Dtu::TransferType::REMOTE_READ;

        uint access = writing ? DtuTlb::WRITE : DtuTlb::READ;
        access |= remote ? 0 : DtuTlb::INTERN;

        DtuTlb::Result res = xfer.dtu.tlb->lookup(localAddr, access, &phys);
        if (res != DtuTlb::HIT)
        {
            bool pf = res == DtuTlb::PAGEFAULT;
            assert(res != DtuTlb::NOMAP);
            DPRINTFS(DtuTlb, (&xfer.dtu),
                "%s for %s access to %p\n",
                res != DtuTlb::MISS ? "Pagefault" : "TLB-miss",
                access == DtuTlb::READ ? "read" : "write",
                localAddr);

            Translation *trans = new Translation(*this);
            xfer.dtu.startTranslate(localAddr, access, trans, pf);
            return;
        }
    }

    translateDone(true, phys);
}

void
XferUnit::TransferEvent::translateDone(bool success, const NocAddr &phys)
{
    // TODO handle error
    assert(success);

    assert(size > 0);

    Addr localOff = localAddr & (xfer.blockSize - 1);
    Addr reqSize = std::min(size, xfer.blockSize - localOff);

    bool writing = type == Dtu::TransferType::REMOTE_WRITE ||
                   type == Dtu::TransferType::LOCAL_WRITE;

    auto cmd = writing ? MemCmd::WriteReq : MemCmd::ReadReq;
    auto pkt = xfer.dtu.generateRequest(phys.getAddr(), reqSize, cmd);

    if (writing)
    {
        assert(buf->offset + reqSize <= xfer.bufSize);

        memcpy(pkt->getPtr<uint8_t>(), buf->bytes + buf->offset, reqSize);

        buf->offset += reqSize;
    }

    DPRINTFS(DtuXfers, (&xfer.dtu),
        "buf%d: %s %lu bytes @ %p->%p in local memory\n",
        buf->id,
        writing ? "Writing" : "Reading",
        reqSize,
        localAddr,
        phys.getAddr());

    xfer.dtu.sendMemRequest(pkt,
                            localAddr,
                            buf->id,
                            Dtu::MemReqType::TRANSFER,
                            xfer.dtu.transferToMemRequestLatency);

    // to next block
    localAddr += reqSize;
    size -= reqSize;
}

bool
XferUnit::startTransfer(Dtu::TransferType type,
                        NocAddr remoteAddr,
                        Addr localAddr,
                        Addr size,
                        PacketPtr pkt,
                        Dtu::MessageHeader* header,
                        Cycles delay,
                        uint flags)
{
    Buffer *buf = allocateBuf(flags & XferFlags::MSGRECV);

    bool writing = type == Dtu::TransferType::REMOTE_WRITE ||
                   type == Dtu::TransferType::LOCAL_WRITE;

    // try again later, if there is no free buffer
    if (!buf)
    {
        DPRINTFS(DtuXfers, (&dtu),
            "Delaying %s transfer of %lu bytes @ %p (all buffers busy)\n",
            writing ? "mem-write" : "mem-read",
            size,
            localAddr);

        auto event = new StartEvent(*this,
                                    type,
                                    remoteAddr,
                                    localAddr,
                                    size,
                                    pkt,
                                    header,
                                    flags);

        dtu.schedule(event, dtu.clockEdge(Cycles(delay + 1)));

        return false;
    }

    // use that buffer and start transferring the data into it
    assert(buf->event.size == 0);

    buf->event.type = type;
    buf->event.remoteAddr = remoteAddr;
    buf->event.localAddr = localAddr;
    buf->event.size = size;
    buf->event.pkt = NULL;
    buf->event.flags = flags;

    // if there is data to put into the buffer, do that now
    if (header)
    {
        // note that this causes no additional delay because we assume that we
        // create the header directly in the buffer (and if there is no one
        // free we just wait until there is)
        memcpy(buf->bytes, header, sizeof(Dtu::MessageHeader));
        buf->event.flags |= XferFlags::MESSAGE;

        // for the header
        buf->offset += sizeof(Dtu::MessageHeader);
        delete header;
    }
    else if (pkt)
    {
        // here is also no additional delay, because we are doing that in
        // parallel and are already paying for it at other places
        memcpy(buf->bytes, pkt->getPtr<uint8_t>(), pkt->getSize());
        buf->event.pkt = pkt;
    }

    DPRINTFS(DtuXfers, (&dtu),
        "buf%d: Starting %s transfer of %lu bytes @ %p\n",
        buf->id,
        writing ? "mem-write" : "mem-read",
        size,
        localAddr);

    dtu.schedule(buf->event, dtu.clockEdge(Cycles(delay + 1)));

    // finish the noc request now to make the port unbusy
    bool remote = type == Dtu::TransferType::REMOTE_READ ||
                  type == Dtu::TransferType::REMOTE_WRITE;
    if (remote)
        dtu.schedNocRequestFinished(dtu.clockEdge(Cycles(1)));

    return true;
}

void
XferUnit::recvMemResponse(size_t bufId,
                          const void* data,
                          Addr size,
                          Tick headerDelay,
                          Tick payloadDelay)
{
    Buffer *buf = bufs[bufId];

    assert(!buf->free);

    if (buf->event.type == Dtu::TransferType::LOCAL_READ ||
        buf->event.type == Dtu::TransferType::REMOTE_READ)
    {
        assert(buf->offset + size <= bufSize);

        memcpy(buf->bytes + buf->offset, data, size);

        buf->offset += size;
    }

    // nothing more to copy?
    if (buf->event.size == 0)
    {
        if (buf->event.type == Dtu::TransferType::LOCAL_READ)
        {
            DPRINTFS(DtuXfers, (&dtu),
                "buf%d: Sending NoC request of %lu bytes @ %p\n",
                buf->id,
                buf->offset,
                buf->event.remoteAddr.offset);

            auto pkt = dtu.generateRequest(buf->event.remoteAddr.getAddr(),
                                           buf->offset,
                                           MemCmd::WriteReq);
            memcpy(pkt->getPtr<uint8_t>(),
                   buf->bytes,
                   buf->offset);

            /*
             * See sendNocMessage() for an explanation of delay handling.
             */
            Cycles delay = dtu.transferToNocLatency;
            delay += dtu.ticksToCycles(headerDelay);
            pkt->payloadDelay = payloadDelay;
            dtu.printPacket(pkt);

            Dtu::NocPacketType pktType;
            if (buf->event.flags & MESSAGE)
                pktType = Dtu::NocPacketType::MESSAGE;
            else
                pktType = Dtu::NocPacketType::WRITE_REQ;
            dtu.sendNocRequest(pktType, pkt, delay);
        }
        else if (buf->event.type == Dtu::TransferType::LOCAL_WRITE)
        {
            if (buf->event.flags & LAST)
                dtu.scheduleFinishOp(Cycles(1));

            dtu.freeRequest(buf->event.pkt);
        }
        else
        {
            if (buf->event.flags & XferFlags::MSGRECV)
            {
                NocAddr addr(buf->event.pkt->getAddr());
                dtu.finishMsgReceive(addr.offset, buf->event.localAddr);
            }

            // TODO should we respond earlier for remote reads? i.e. as soon
            // as its in the buffer
            assert(buf->event.pkt != NULL);

            // some requests from the cache (e.g. cleanEvict) do not need a
            // response
            if (buf->event.pkt->needsResponse())
            {
                DPRINTFS(DtuXfers, (&dtu),
                    "buf%d: Sending NoC response of %lu bytes\n",
                    buf->id,
                    buf->offset);

                buf->event.pkt->makeResponse();

                if (buf->event.type == Dtu::TransferType::REMOTE_READ)
                {
                    memcpy(buf->event.pkt->getPtr<uint8_t>(),
                           buf->bytes,
                           buf->offset);
                }

                Cycles delay = dtu.transferToNocLatency;
                dtu.schedNocResponse(buf->event.pkt, dtu.clockEdge(delay));
            }
        }

        DPRINTFS(DtuXfers, (&dtu), "buf%d: Transfer done\n",
                 buf->id);

        // we're done with this buffer now
        buf->free = true;
    }
    else
        buf->event.process();
}

XferUnit::Buffer*
XferUnit::allocateBuf(bool recvmsg)
{
    // don't allow message receives in parallel. because otherwise we run into race conditions.
    // e.g., we could overwrite unread messages because we can't increase the message counter when
    // the receive starts (to not notify SW) and thus might start receiving without having space
    // another problem is that we might finish receiving the second message before the first and
    // then increase the message counter, so that the SW looks at the first message, which is not
    // ready yet.
    if (recvmsg)
    {
        for (size_t i = 0; i < bufCount; ++i)
        {
            if (!bufs[i]->free && (bufs[i]->event.flags & XferFlags::MSGRECV))
                return NULL;
        }
    }

    for (size_t i = 0; i < bufCount; ++i)
    {
        if (bufs[i]->free)
        {
            bufs[i]->free = false;
            bufs[i]->offset = 0;
            return bufs[i];
        }
    }

    return NULL;
}
