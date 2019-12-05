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

#ifndef __MEM_DTU_MSG_UNIT_HH__
#define __MEM_DTU_MSG_UNIT_HH__

#include "mem/dtu/dtu.hh"

class MessageUnit
{
  private:

    struct MsgInfo
    {
        bool ready;
        bool unlimcred;
        uint8_t flags;
        unsigned targetCoreId;
        uint16_t targetVpeId;
        unsigned targetEpId;
        unsigned replyEpId;
        uint64_t label;
        uint64_t replyLabel;
    };

    struct Translation : PtUnit::Translation
    {
        MessageUnit& unit;

        Addr virt;

        unsigned epId;

        Translation(MessageUnit& _unit, Addr _virt, unsigned _epId)
            : unit(_unit), virt(_virt), epId(_epId)
        {}

        void finished(bool success, const NocAddr &phys) override
        {
            unit.requestHeaderWithPhys(epId, success, virt, phys);

            delete this;
        }
    };

  public:

    MessageUnit(Dtu &_dtu)
      : dtu(_dtu), info(), header(), flagsPhys(), offset() {}

    /**
     * Start message transmission -> Mem request
     */
    void startTransmission(const Dtu::Command& cmd);

    /**
     * Received response from local memory (header lookup)
     */
    void recvFromMem(const Dtu::Command& cmd, PacketPtr pkt);

    /**
     * Received a message from NoC -> Mem request
     */
    Dtu::Error recvFromNoc(PacketPtr pkt);

    /**
     * Finishes the reply-on-message command
     */
    void finishMsgReply(Dtu::Error error, unsigned epid);

    /**
     * Fetches the next message and returns the address or 0
     */
    Addr fetchMessage(unsigned epid);

    /**
     * Acknowledges the message in OFFSET register
     */
    void ackMessage(unsigned epId);

    /**
     * Finishes a message receive
     */
    void finishMsgReceive(unsigned epId,
                          Addr msgAddr);

  private:
    int allocSlot(size_t msgSize, unsigned epid, RecvEp &ep);

    void requestHeader(unsigned epid);

    void requestHeaderWithPhys(unsigned epid,
                               bool success,
                               Addr virt,
                               const NocAddr &phys);

    void startXfer(const Dtu::Command& cmd);

  private:

    Dtu &dtu;

    MsgInfo info;

    Dtu::MessageHeader header;
    Addr flagsPhys;
    Addr offset;
};

#endif
