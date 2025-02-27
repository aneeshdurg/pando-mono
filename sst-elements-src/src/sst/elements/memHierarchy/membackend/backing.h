// Copyright 2009-2023 NTESS. Under the terms
// Copyright (c) 2023 University of Washington
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2023, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

/*
   <mrutt@cs.washington.edu> Sun Nov 12 21:57:34 2023 -0800:
   membackend/backing.h:
   Makes all fields public for class BackingMMAP.
*/
#ifndef __SST_MEMH_BACKEND_BACKING
#define __SST_MEMH_BACKEND_BACKING

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "sst/elements/memHierarchy/util.h"

namespace SST {
namespace MemHierarchy {
namespace Backend {

class Backing {
public:
    Backing( ) { }
    virtual ~Backing() { }

    virtual void set( Addr addr, uint8_t value ) = 0;
    virtual void set( Addr addr, size_t size, std::vector<uint8_t>& data) = 0;

    virtual uint8_t get( Addr addr) = 0;
    virtual void get( Addr addr, size_t size, std::vector<uint8_t>& data) = 0;
};

class BackingMMAP : public Backing {
public:
    BackingMMAP(std::string memoryFile, size_t size, size_t offset = 0) : Backing(), m_fd(-1), m_size(size), m_offset(offset) {
        int flags = MAP_SHARED;
        if ( ! memoryFile.empty() ) {
            m_fd = open(memoryFile.c_str(), O_RDWR);
            if ( m_fd < 0) {
                throw 1;
            }
        } else {
            flags  |= MAP_ANON;
        }
        m_buffer = (uint8_t*)mmap(NULL, size, PROT_READ|PROT_WRITE, flags, m_fd, 0);

        if ( m_buffer == MAP_FAILED) {
            throw 2;
        }
    }

    ~BackingMMAP() {
        munmap( m_buffer, m_size );
        if ( -1 != m_fd ) {
            close( m_fd );
        }
    }

    void set( Addr addr, uint8_t value ) {
        m_buffer[addr - m_offset ] = value;
    }

    void set (Addr addr, size_t size, std::vector<uint8_t> &data) {
        for (size_t i = 0; i < size; i++)
            m_buffer[addr + i] = data[i];
    }

    uint8_t get( Addr addr ) {
        return m_buffer[addr - m_offset];
    }

    void get( Addr addr, size_t size, std::vector<uint8_t> &data) {
        for (size_t i = 0; i < size; i++)
            data[i] = m_buffer[addr + i];
    }

  ////////////////////////////////////////////////////////////////////////////////////////////
  // Code here is added by UW and subject to the copyright statement at the top of the file //
  ////////////////////////////////////////////////////////////////////////////////////////////
    //private:
    uint8_t* m_buffer;
    int m_fd;
    int m_size;
    size_t m_offset;
};

class BackingMalloc : public Backing {
public:
    BackingMalloc(size_t size, bool init = false ) : m_init(init) {
        m_allocUnit = size;
        /* Alloc unit needs to be pwr-2 */
        if (!isPowerOfTwo(m_allocUnit)) {
            Output out("", 1, 0, Output::STDOUT);
            out.fatal(CALL_INFO, -1, "BackingMalloc: Error - size must be a power of two. Got: %zu\n", size);
        }
        m_shift = log2Of(m_allocUnit);
    }

    void set( Addr addr, uint8_t value ) {
        Addr bAddr = addr >> m_shift;
        Addr offset = addr - (bAddr << m_shift);
        allocIfNeeded(bAddr);
        m_buffer[bAddr][offset] = value;
    }

    void set( Addr addr, size_t size, std::vector<uint8_t> &data ) {
        /* Account for size exceeding alloc unit size */
        Addr bAddr = addr >> m_shift;
        Addr offset = addr - (bAddr << m_shift);
        size_t dataOffset = 0;

        allocIfNeeded(bAddr);

        while (dataOffset != size) {
            m_buffer[bAddr][offset] = data[dataOffset];
            offset++;
            dataOffset++;

            if (offset == m_allocUnit) {
                offset = 0;
                bAddr++;
                allocIfNeeded(bAddr);
            }
        }
    }

    void get (Addr addr, size_t size, std::vector<uint8_t> &data) {
        Addr bAddr = addr >> m_shift;
        Addr offset = addr - (bAddr << m_shift);
        size_t dataOffset = 0;

        allocIfNeeded(bAddr);

        while (dataOffset != size) {
            data[dataOffset] = m_buffer[bAddr][offset];
            offset++;
            dataOffset++;
            if (offset == m_allocUnit) {
                bAddr++;
                offset = 0;
                allocIfNeeded(bAddr);
            }
        }
    }

    uint8_t get( Addr addr ) {
        Addr bAddr = addr >> m_shift;
        Addr offset = addr - (bAddr << m_shift);
        allocIfNeeded(bAddr);
        return m_buffer[bAddr][offset];
    }

private:
    void allocIfNeeded(Addr bAddr) {
        if (m_buffer.find(bAddr) == m_buffer.end()) {
            uint8_t* data = (uint8_t*) malloc(sizeof(uint8_t)*m_allocUnit);
            if ( m_init ) {
                bzero( data, m_allocUnit );
            }
            if (!data) {
                Output out("", 1, 0, Output::STDOUT);
                out.fatal(CALL_INFO, -1, "BackingMalloc: Error - malloc failed.\n");
            }
            m_buffer[bAddr] = data;
        }
    }

    std::unordered_map<Addr,uint8_t*> m_buffer;
    unsigned int m_allocUnit;
    unsigned int m_shift;
    bool m_init;
};

}
}
}

#endif
