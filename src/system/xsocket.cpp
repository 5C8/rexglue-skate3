/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include <rex/kernel/xam/module.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/select.h>
#include <sys/socket.h>
#if REX_PLATFORM_MAC
#ifdef IPPROTO_TCP
#undef IPPROTO_TCP
#endif
#ifdef IPPROTO_UDP
#undef IPPROTO_UDP
#endif
#endif
#endif

namespace rex::system {

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  int ret = rex::net::socket_close(native_handle_);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  // FIONBIO: the Xbox 360 socket API uses the same value as native Winsock.
  // Track it so Connect() can tell whether the title is already handling
  // non-blocking connects itself (DirtySock's normal path) or still expects
  // a synchronous connect() to just work.
  constexpr uint32_t kFIONBIO = 0x8004667E;
  if (cmd == kFIONBIO && arg_ptr) {
    nonblocking_ = (*reinterpret_cast<uint32_t*>(arg_ptr)) != 0;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  if (nonblocking_) {
    // The title already put this socket in non-blocking mode (DirtySock's
    // normal path), so connect() already returns immediately and the title
    // polls for completion itself. Behave exactly as before.
    int ret = connect(native_handle_, (sockaddr*)name, name_len);
    if (ret < 0) {
      return X_STATUS_UNSUCCESSFUL;
    }
    return X_STATUS_SUCCESS;
  }

  // Still a blocking socket: a plain connect() here would hang this thread
  // for the OS's default TCP connect timeout (~20-30s on Windows) if the
  // destination never responds, stalling the whole boot. Bound it to a few
  // seconds instead, then restore blocking mode so Send/Recv on this socket
  // keep behaving exactly as the title expects.
#if REX_PLATFORM_WIN32
  u_long nb_mode = 1;
  ioctlsocket(native_handle_, FIONBIO, &nb_mode);
#else
  int prev_flags = fcntl(native_handle_, F_GETFL, 0);
  fcntl(native_handle_, F_SETFL, prev_flags | O_NONBLOCK);
#endif

  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  bool connected = ret == 0;
  if (ret < 0) {
#if REX_PLATFORM_WIN32
    const bool in_progress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
    const bool in_progress = errno == EINPROGRESS;
#endif
    if (in_progress) {
      fd_set write_set;
      FD_ZERO(&write_set);
      FD_SET(native_handle_, &write_set);
      timeval timeout{};
      timeout.tv_sec = 5;
      timeout.tv_usec = 0;
#if REX_PLATFORM_WIN32
      const int nfds = 0;  // ignored by Winsock's select()
#else
      const int nfds = static_cast<int>(native_handle_) + 1;
#endif
      if (select(nfds, nullptr, &write_set, nullptr, &timeout) > 0) {
        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        if (getsockopt(native_handle_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error),
                       &so_error_len) == 0) {
          connected = so_error == 0;
        }
      }
    }
  }

#if REX_PLATFORM_WIN32
  nb_mode = 0;
  ioctlsocket(native_handle_, FIONBIO, &nb_mode);
#else
  fcntl(native_handle_, F_SETFL, prev_flags);
#endif

  return connected ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  int ret = bind(native_handle_, (sockaddr*)name, name_len);
  // Type::SOCK_DGRAM (2); spelled numerically because SOCK_DGRAM is a socket macro.
  if (ret < 0 && static_cast<int>(type_) == 2) {
    // A console owns its fixed game ports; on a PC another program (or a
    // second instance of the title) may already hold them. Datagram sockets
    // fall back to a free port: peers and relays answer the observed source
    // address, so the exact local port doesn't matter to them.
    // Also covers "access denied": binding a specific (loopback) address on a
    // port another process holds on all addresses, e.g. a game server relay
    // running on the same PC.
#if REX_PLATFORM_WIN32
    const int bind_error = WSAGetLastError();
    const bool in_use = bind_error == WSAEADDRINUSE || bind_error == WSAEACCES;
#else
    const int bind_error = errno;
    const bool in_use = bind_error == EADDRINUSE || bind_error == EACCES;
#endif
    if (in_use) {
      N_XSOCKADDR_IN any_port = *name;
      any_port.sin_port = 0;
      ret = bind(native_handle_, (sockaddr*)&any_port, name_len);
    }
  }
  if (ret < 0) {
#if REX_PLATFORM_WIN32
    const int failure = WSAGetLastError();
#else
    const int failure = errno;
#endif
    REXKRNL_INFO("[udp] bind type={} port={} FAILED error={}", static_cast<int>(type_),
                 static_cast<uint16_t>(name->sin_port), failure);
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;
  sockaddr_in actual{};
  socklen_t actual_len = sizeof(actual);
  if (getsockname(native_handle_, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
    bound_port_ = ntohs(actual.sin_port);
  }
  // TEMPORARY trace for online connection debugging
  REXKRNL_INFO("[udp] bind type={} requested_port={} bound_port={}", static_cast<int>(type_),
               static_cast<uint16_t>(name->sin_port), bound_port_);

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = ntohs(nfrom.sin_port);  // BE <- network order
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  // N_XSOCKADDR_IN holds the port and address as big-endian values: convert
  // them to network byte order explicitly. Assigning them straight into the
  // native sockaddr stored them byte-swapped (127.0.0.2:6000 became
  // 2.0.0.127:28695), so no datagram ever reached its peer.
  sockaddr_in nto{};
  if (to) {
    nto.sin_family = static_cast<decltype(nto.sin_family)>(to->sin_family);
    nto.sin_port = htons(static_cast<uint16_t>(to->sin_port));
    nto.sin_addr.s_addr = htonl(static_cast<uint32_t>(to->sin_addr));
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to ? static_cast<int>(sizeof(nto)) : 0);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
