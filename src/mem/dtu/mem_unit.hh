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

#ifndef __MEM_DTU_MEM_UNIT_HH__
#define __MEM_DTU_MEM_UNIT_HH__

#include "mem/dtu/dtu.hh"

class MemoryUnit
{
  private:

    struct ContinueEvent : public Event
    {
        MemoryUnit& memUnit;

        Dtu::Command cmd;

        bool read;

        ContinueEvent(MemoryUnit& _memUnit)
            : memUnit(_memUnit), cmd(), read()
        {}

        void process() override
        {
            if (read)
                memUnit.startRead(cmd);
            else
                memUnit.startWrite(cmd);
        }

        const char* description() const override { return "ContinueEvent"; }

        const std::string name() const override { return memUnit.dtu.name(); }
    };

  public:

    MemoryUnit(Dtu &_dtu) : dtu(_dtu), continueEvent(*this) {}

    /**
     * Starts a read -> NoC request
     */
    void startRead(const Dtu::Command& cmd);

    /**
     * Starts a write -> Mem request
     */
    void startWrite(const Dtu::Command& cmd);

    /**
     * Read: response from remote DTU
     */
    void readComplete(PacketPtr pkt, Dtu::Error error);

    /**
     * Write: response from remote DTU
     */
    void writeComplete(PacketPtr pkt, Dtu::Error error);


    /**
     * Functional access from NoC
     */
    void recvFunctionalFromNoc(PacketPtr pkt);

    /**
     * Received read/write request from NoC -> Mem/regfile request
     */
    Dtu::Error recvFromNoc(PacketPtr pkt);

  private:

    Dtu &dtu;

    ContinueEvent continueEvent;
};

#endif
