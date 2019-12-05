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

#ifndef __MEM_DTU_NOC_ADDR_HH__
#define __MEM_DTU_NOC_ADDR_HH__

#define ID_BITS         64
#define RESERVED_BITS   5
#define VALID_BITS      1
#define CORE_BITS       10
#define VPE_BITS        11
#define OFFSET_BITS     (ID_BITS - RESERVED_BITS - VALID_BITS - CORE_BITS - VPE_BITS)

#define VALID_SHIFT     (ID_BITS - RESERVED_BITS)
#define CORE_SHIFT      (VALID_SHIFT - CORE_BITS)
#define VPE_SHIFT       (CORE_SHIFT - VPE_BITS)

#include "base/types.hh"

/**
 *
 *  64    59        48      37         0
 *   -----------------------------------
 *   |res|V| coreId  | vpeId | offset  |
 *   -----------------------------------
 */
class NocAddr
{
  public:

    explicit NocAddr() : valid(), coreId(), offset()
    {}

    explicit NocAddr(Addr addr)
        : valid(addr >> VALID_SHIFT),
          coreId((addr >> CORE_SHIFT) & ((1 << CORE_BITS) - 1)),
          vpeId((addr >> VPE_SHIFT) & ((1 << VPE_BITS) - 1)),
          offset(addr & ((static_cast<Addr>(1) << OFFSET_BITS) - 1))
    {}

    explicit NocAddr(unsigned _coreId, unsigned _vpeId, Addr _offset)
        : valid(1), coreId(_coreId), vpeId(_vpeId), offset(_offset)
    {}

    Addr getAddr() const
    {
        assert((coreId & ~((1 << CORE_BITS) - 1)) == 0);
        assert((vpeId & ~((1 << VPE_BITS) - 1)) == 0);
        assert((offset & ~((static_cast<Addr>(1) << OFFSET_BITS) - 1)) == 0);

        Addr res = static_cast<Addr>(valid) << VALID_SHIFT;
        res |= static_cast<Addr>(coreId) << CORE_SHIFT;
        res |= static_cast<Addr>(vpeId) << VPE_SHIFT;
        res |= offset;
        return res;
    }

    bool valid;

    unsigned coreId;

    unsigned vpeId;

    Addr offset;
};

#endif
