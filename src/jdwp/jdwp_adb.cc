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

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "jdwp/jdwp_priv.h"

#ifdef HAVE_ANDROID_OS
#include "cutils/sockets.h"
#endif

/*
 * The JDWP <-> ADB transport protocol is explained in detail
 * in system/core/adb/jdwp_service.c. Here's a summary.
 *
 * 1/ when the JDWP thread starts, it tries to connect to a Unix
 *    domain stream socket (@jdwp-control) that is opened by the
 *    ADB daemon.
 *
 * 2/ it then sends the current process PID as a string of 4 hexadecimal
 *    chars (no terminating zero)
 *
 * 3/ then, it uses recvmsg to receive file descriptors from the
 *    daemon. each incoming file descriptor is a pass-through to
 *    a given JDWP debugger, that can be used to read the usual
 *    JDWP-handshake, etc...
 */

#define kJdwpControlName    "\0jdwp-control"
#define kJdwpControlNameLen (sizeof(kJdwpControlName)-1)

namespace art {

namespace JDWP {

struct JdwpNetState : public JdwpNetStateBase {
  int                 controlSock;
  bool                shuttingDown;
  int                 wakeFds[2];

  socklen_t           controlAddrLen;
  union {
    sockaddr_un  controlAddrUn;
    sockaddr     controlAddrPlain;
  } controlAddr;

  JdwpNetState() {
    controlSock = -1;
    shuttingDown = false;
    wakeFds[0] = -1;
    wakeFds[1] = -1;

    controlAddr.controlAddrUn.sun_family = AF_UNIX;
    controlAddrLen = sizeof(controlAddr.controlAddrUn.sun_family) + kJdwpControlNameLen;
    memcpy(controlAddr.controlAddrUn.sun_path, kJdwpControlName, kJdwpControlNameLen);
  }
};

static JdwpNetState* GetNetState(JdwpState* state) {
  return reinterpret_cast<JdwpNetState*>(state->netState);
}

static void adbStateFree(JdwpNetState* netState) {
  if (netState == NULL) {
    return;
  }

  if (netState->clientSock >= 0) {
    shutdown(netState->clientSock, SHUT_RDWR);
    close(netState->clientSock);
  }
  if (netState->controlSock >= 0) {
    shutdown(netState->controlSock, SHUT_RDWR);
    close(netState->controlSock);
  }
  if (netState->wakeFds[0] >= 0) {
    close(netState->wakeFds[0]);
    netState->wakeFds[0] = -1;
  }
  if (netState->wakeFds[1] >= 0) {
    close(netState->wakeFds[1]);
    netState->wakeFds[1] = -1;
  }

  delete netState;
}

/*
 * Do initial prep work, e.g. binding to ports and opening files.  This
 * runs in the main thread, before the JDWP thread starts, so it shouldn't
 * do anything that might block forever.
 */
static bool startup(JdwpState* state, const JdwpOptions*) {
  VLOG(jdwp) << "ADB transport startup";
  state->netState = new JdwpNetState;
  return (state->netState != NULL);
}

/*
 * Receive a file descriptor from ADB.  The fd can be used to communicate
 * directly with a debugger or DDMS.
 *
 * Returns the file descriptor on success.  On failure, returns -1 and
 * closes netState->controlSock.
 */
static int  receiveClientFd(JdwpNetState*  netState) {
  msghdr    msg;
  cmsghdr*  cmsg;
  iovec     iov;
  char             dummy = '!';
  union {
    cmsghdr cm;
    char buffer[CMSG_SPACE(sizeof(int))];
  } cm_un;
  int              ret;

  iov.iov_base       = &dummy;
  iov.iov_len        = 1;
  msg.msg_name       = NULL;
  msg.msg_namelen    = 0;
  msg.msg_iov        = &iov;
  msg.msg_iovlen     = 1;
  msg.msg_flags      = 0;
  msg.msg_control    = cm_un.buffer;
  msg.msg_controllen = sizeof(cm_un.buffer);

  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_len   = msg.msg_controllen;
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type  = SCM_RIGHTS;
  ((int*)(void*)CMSG_DATA(cmsg))[0] = -1;

  do {
    ret = recvmsg(netState->controlSock, &msg, 0);
  } while (ret < 0 && errno == EINTR);

  if (ret <= 0) {
    if (ret < 0) {
      PLOG(WARNING) << "Receiving file descriptor from ADB failed (socket " << netState->controlSock << ")";
    }
    close(netState->controlSock);
    netState->controlSock = -1;
    return -1;
  }

  return ((int*)(void*)CMSG_DATA(cmsg))[0];
}

/*
 * Block forever, waiting for a debugger to connect to us.  Called from the
 * JDWP thread.
 *
 * This needs to un-block and return "false" if the VM is shutting down.  It
 * should return "true" when it successfully accepts a connection.
 */
static bool acceptConnection(JdwpState* state) {
  JdwpNetState* netState = GetNetState(state);
  int retryCount = 0;

  /* first, ensure that we get a connection to the ADB daemon */

 retry:
  if (netState->shuttingDown) {
    return false;
  }

  if (netState->controlSock < 0) {
    int        sleep_ms     = 500;
    const int  sleep_max_ms = 2*1000;
    char       buff[5];

    netState->controlSock = socket(PF_UNIX, SOCK_STREAM, 0);
    if (netState->controlSock < 0) {
      PLOG(ERROR) << "Could not create ADB control socket";
      return false;
    }

    if (pipe(netState->wakeFds) < 0) {
      PLOG(ERROR) << "pipe failed";
      return false;
    }

    snprintf(buff, sizeof(buff), "%04x", getpid());
    buff[4] = 0;

    for (;;) {
      /*
       * If adbd isn't running, because USB debugging was disabled or
       * perhaps the system is restarting it for "adb root", the
       * connect() will fail.  We loop here forever waiting for it
       * to come back.
       *
       * Waking up and polling every couple of seconds is generally a
       * bad thing to do, but we only do this if the application is
       * debuggable *and* adbd isn't running.  Still, for the sake
       * of battery life, we should consider timing out and giving
       * up after a few minutes in case somebody ships an app with
       * the debuggable flag set.
       */
      int  ret = connect(netState->controlSock, &netState->controlAddr.controlAddrPlain, netState->controlAddrLen);
      if (!ret) {
#ifdef HAVE_ANDROID_OS
        if (!socket_peer_is_trusted(netState->controlSock)) {
          if (shutdown(netState->controlSock, SHUT_RDWR)) {
            PLOG(ERROR) << "trouble shutting down socket";
          }
          return false;
        }
#endif

        /* now try to send our pid to the ADB daemon */
        ret = TEMP_FAILURE_RETRY(send(netState->controlSock, buff, 4, 0));
        if (ret >= 0) {
          VLOG(jdwp) << StringPrintf("PID sent as '%.*s' to ADB", 4, buff);
          break;
        }

        PLOG(ERROR) << "Weird, can't send JDWP process pid to ADB";
        return false;
      }
      if (VLOG_IS_ON(jdwp)) {
        PLOG(ERROR) << "Can't connect to ADB control socket";
      }

      usleep(sleep_ms * 1000);

      sleep_ms += (sleep_ms >> 1);
      if (sleep_ms > sleep_max_ms) {
        sleep_ms = sleep_max_ms;
      }
      if (netState->shuttingDown) {
        return false;
      }
    }
  }

  VLOG(jdwp) << "trying to receive file descriptor from ADB";
  /* now we can receive a client file descriptor */
  netState->clientSock = receiveClientFd(netState);
  if (netState->shuttingDown) {
    return false;       // suppress logs and additional activity
  }
  if (netState->clientSock < 0) {
    if (++retryCount > 5) {
      LOG(ERROR) << "adb connection max retries exceeded";
      return false;
    }
    goto retry;
  } else {
    VLOG(jdwp) << "received file descriptor " << netState->clientSock << " from ADB";
    netState->SetAwaitingHandshake(true);
    netState->inputCount = 0;
    return true;
  }
}

/*
 * Connect out to a debugger (for server=n).  Not required.
 */
static bool establishConnection(JdwpState*, const JdwpOptions*) {
  return false;
}

/*
 * Close all network stuff, including the socket we use to listen for
 * new connections.
 *
 * May be called from a non-JDWP thread, e.g. when the VM is shutting down.
 */
static void adbStateShutdown(JdwpNetState* netState) {
  int  controlSock;
  int  clientSock;

  if (netState == NULL) {
    return;
  }

  netState->shuttingDown = true;

  clientSock = netState->clientSock;
  if (clientSock >= 0) {
    shutdown(clientSock, SHUT_RDWR);
    netState->clientSock = -1;
  }

  controlSock = netState->controlSock;
  if (controlSock >= 0) {
    shutdown(controlSock, SHUT_RDWR);
    netState->controlSock = -1;
  }

  if (netState->wakeFds[1] >= 0) {
    VLOG(jdwp) << "+++ writing to wakePipe";
    TEMP_FAILURE_RETRY(write(netState->wakeFds[1], "", 1));
  }
}

static void netShutdown(JdwpState* state) {
  adbStateShutdown(GetNetState(state));
}

/*
 * Free up anything we put in state->netState.  This is called after
 * "netShutdown", after the JDWP thread has stopped.
 */
static void netFree(JdwpState* state) {
  adbStateFree(GetNetState(state));
}

/*
 * Process incoming data.  If no data is available, this will block until
 * some arrives.
 *
 * If we get a full packet, handle it.
 *
 * To take some of the mystery out of life, we want to reject incoming
 * connections if we already have a debugger attached.  If we don't, the
 * debugger will just mysteriously hang until it times out.  We could just
 * close the listen socket, but there's a good chance we won't be able to
 * bind to the same port again, which would confuse utilities.
 *
 * Returns "false" on error (indicating that the connection has been severed),
 * "true" if things are still okay.
 */
static bool processIncoming(JdwpState* state) {
  JdwpNetState* netState = GetNetState(state);
  int readCount;

  CHECK_GE(netState->clientSock, 0);

  if (!netState->HaveFullPacket()) {
    /* read some more, looping until we have data */
    errno = 0;
    while (1) {
      int selCount;
      fd_set readfds;
      int maxfd = -1;
      int fd;

      FD_ZERO(&readfds);

      /* configure fds; note these may get zapped by another thread */
      fd = netState->controlSock;
      if (fd >= 0) {
        FD_SET(fd, &readfds);
        if (maxfd < fd) {
          maxfd = fd;
        }
      }
      fd = netState->clientSock;
      if (fd >= 0) {
        FD_SET(fd, &readfds);
        if (maxfd < fd) {
          maxfd = fd;
        }
      }
      fd = netState->wakeFds[0];
      if (fd >= 0) {
        FD_SET(fd, &readfds);
        if (maxfd < fd) {
          maxfd = fd;
        }
      } else {
        LOG(INFO) << "NOTE: entering select w/o wakepipe";
      }

      if (maxfd < 0) {
        VLOG(jdwp) << "+++ all fds are closed";
        return false;
      }

      /*
       * Select blocks until it sees activity on the file descriptors.
       * Closing the local file descriptor does not count as activity,
       * so we can't rely on that to wake us up (it works for read()
       * and accept(), but not select()).
       *
       * We can do one of three things: (1) send a signal and catch
       * EINTR, (2) open an additional fd ("wakePipe") and write to
       * it when it's time to exit, or (3) time out periodically and
       * re-issue the select.  We're currently using #2, as it's more
       * reliable than #1 and generally better than #3.  Wastes two fds.
       */
      selCount = select(maxfd+1, &readfds, NULL, NULL, NULL);
      if (selCount < 0) {
        if (errno == EINTR) {
          continue;
        }
        PLOG(ERROR) << "select failed";
        goto fail;
      }

      if (netState->wakeFds[0] >= 0 && FD_ISSET(netState->wakeFds[0], &readfds)) {
        LOG(DEBUG) << "Got wake-up signal, bailing out of select";
        goto fail;
      }
      if (netState->controlSock >= 0 && FD_ISSET(netState->controlSock, &readfds)) {
        int  sock = receiveClientFd(netState);
        if (sock >= 0) {
          LOG(INFO) << "Ignoring second debugger -- accepting and dropping";
          close(sock);
        } else {
          CHECK_LT(netState->controlSock, 0);
          /*
           * Remote side most likely went away, so our next read
           * on netState->clientSock will fail and throw us out
           * of the loop.
           */
        }
      }
      if (netState->clientSock >= 0 && FD_ISSET(netState->clientSock, &readfds)) {
        readCount = read(netState->clientSock, netState->inputBuffer + netState->inputCount, sizeof(netState->inputBuffer) - netState->inputCount);
        if (readCount < 0) {
          /* read failed */
          if (errno != EINTR) {
            goto fail;
          }
          LOG(DEBUG) << "+++ EINTR hit";
          return true;
        } else if (readCount == 0) {
          /* EOF hit -- far end went away */
          VLOG(jdwp) << "+++ peer disconnected";
          goto fail;
        } else {
          break;
        }
      }
    }

    netState->inputCount += readCount;
    if (!netState->HaveFullPacket()) {
      return true;        /* still not there yet */
    }
  }

  /*
   * Special-case the initial handshake.  For some bizarre reason we're
   * expected to emulate bad tty settings by echoing the request back
   * exactly as it was sent.  Note the handshake is always initiated by
   * the debugger, no matter who connects to whom.
   *
   * Other than this one case, the protocol [claims to be] stateless.
   */
  if (netState->IsAwaitingHandshake()) {
    int cc;

    if (memcmp(netState->inputBuffer, kMagicHandshake, kMagicHandshakeLen) != 0) {
      LOG(ERROR) << StringPrintf("ERROR: bad handshake '%.14s'", netState->inputBuffer);
      goto fail;
    }

    errno = 0;
    cc = TEMP_FAILURE_RETRY(write(netState->clientSock, netState->inputBuffer, kMagicHandshakeLen));
    if (cc != kMagicHandshakeLen) {
      PLOG(ERROR) << "Failed writing handshake bytes (" << cc << " of " << kMagicHandshakeLen << ")";
      goto fail;
    }

    netState->ConsumeBytes(kMagicHandshakeLen);
    netState->SetAwaitingHandshake(false);
    VLOG(jdwp) << "+++ handshake complete";
    return true;
  }

  /*
   * Handle this packet.
   */
  return state->HandlePacket();

 fail:
  netState->Close();
  return false;
}

/*
 * Our functions.
 */
static const JdwpTransport adbTransport = {
  startup,
  acceptConnection,
  establishConnection,
  netShutdown,
  netFree,
  processIncoming,
};

/*
 * Return our set.
 */
const JdwpTransport* AndroidAdbTransport() {
  return &adbTransport;
}

}  // namespace JDWP

}  // namespace art
