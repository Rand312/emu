/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifndef __QEMU_PIPE_STREAM_H
#define __QEMU_PIPE_STREAM_H

/* This file implements an IOStream that uses a QEMU fast-pipe
 * to communicate with the emulator's 'opengles' service. See
 * <hardware/qemu_pipe.h> for more details.
 */
#include <stdlib.h>
#include <memory>

#include "gfxstream/guest/IOStream.h"

#include <qemu_pipe_bp.h>

class QemuPipeStream : public gfxstream::guest::IOStream {
public:
 typedef enum { ERR_INVALID_SOCKET = -1000 } QemuPipeStreamError;

 explicit QemuPipeStream(size_t bufsize = 10000);
 ~QemuPipeStream();

 virtual int connect(const char* serviceName = nullptr);
 virtual uint64_t processPipeInit();

 virtual void* allocBuffer(size_t minSize);
 virtual int commitBuffer(size_t size);
 virtual const unsigned char* readFully(void* buf, size_t len);
 virtual const unsigned char* commitBufferAndReadFully(size_t size, void* buf, size_t len);
 virtual const unsigned char* read(void* buf, size_t* inout_len);

 bool valid() { return qemu_pipe_valid(m_sock); }
 int recv(void* buf, size_t len);

 virtual int writeFully(const void* buf, size_t len);

 QEMU_PIPE_HANDLE getSocket() const;
private:
    QEMU_PIPE_HANDLE m_sock;
    size_t m_bufsize;
    unsigned char *m_buf;
    size_t m_read;
    size_t m_readLeft;
    QemuPipeStream(QEMU_PIPE_HANDLE sock, size_t bufSize);
};

#endif
