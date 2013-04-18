/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * JDWP internal interfaces.
 */
#ifndef ART_JDWP_JDWPPRIV_H_
#define ART_JDWP_JDWPPRIV_H_

#include "debugger.h"
#include "jdwp/jdwp.h"
#include "jdwp/jdwp_event.h"

#include <pthread.h>
#include <sys/uio.h>

/*
 * JDWP constants.
 */
#define kJDWPHeaderLen  11
#define kJDWPFlagReply  0x80

#define kMagicHandshake     "JDWP-Handshake"
#define kMagicHandshakeLen  (sizeof(kMagicHandshake)-1)

/* DDM support */
#define kJDWPDdmCmdSet  199     /* 0xc7, or 'G'+128 */
#define kJDWPDdmCmd     1

namespace art {

namespace JDWP {

struct JdwpState;

/*
 * Transport functions.
 */
struct JdwpTransport {
  bool (*startup)(JdwpState* state, const JdwpOptions* options);
  bool (*accept)(JdwpState* state);
  bool (*establish)(JdwpState* state, const JdwpOptions* options);
  void (*shutdown)(JdwpState* state);
  void (*free)(JdwpState* state);
  bool (*processIncoming)(JdwpState* state);
};

const JdwpTransport* SocketTransport();
const JdwpTransport* AndroidAdbTransport();

/*
 * Base class for the adb and socket JdwpNetState implementations.
 */
class JdwpNetStateBase {
 public:
  int clientSock;     /* active connection to debugger */

  enum { kInputBufferSize = 8192 };

  unsigned char inputBuffer[kInputBufferSize];
  size_t inputCount;

  JdwpNetStateBase();

  void ConsumeBytes(size_t byte_count);

  bool IsConnected();

  bool IsAwaitingHandshake();
  void SetAwaitingHandshake(bool new_state);

  bool HaveFullPacket();

  void Close();

  ssize_t WritePacket(ExpandBuf* pReply);
  ssize_t WriteBufferedPacket(const iovec* iov, int iov_count);

 private:
  // Used to serialize writes to the socket.
  Mutex socket_lock_;

  // Are we waiting for the JDWP handshake?
  bool awaiting_handshake_;
};

}  // namespace JDWP

}  // namespace art

#endif  // ART_JDWP_JDWPPRIV_H_
