/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <random>
#include <cctype>
#include <string>

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <ws2tcpip.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(
    net_host_overrides, "", "Network",
    "Redirect the title's name lookups without touching the system hosts file: "
    "a comma- or semicolon-separated list of host=target pairs, e.g. "
    "\"gosredirector.stest.ea.com=192.168.1.20; skate3_web=my-server.lan\". "
    "The target may be an IPv4 address or another hostname, and host matching "
    "is case-insensitive.");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {0};

static void FillRandom(void* out, size_t length) {
  static std::mutex mutex;
  static std::mt19937_64 engine{std::random_device{}()};
  std::lock_guard<std::mutex> lock(mutex);
  auto* bytes = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < length; ++i) {
    bytes[i] = static_cast<uint8_t>(engine() >> 56);
  }
}

// This PC's outbound IPv4 address in network byte order, or loopback while
// there is no route (or Winsock isn't started yet). Connecting a UDP socket
// selects the source address without sending anything.
static uint32_t LocalIPv4() {
  static std::atomic<uint32_t> cached{0};
  if (const uint32_t address = cached.load(std::memory_order_relaxed)) {
    return address;
  }
#if REX_PLATFORM_WIN32
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    return htonl(INADDR_LOOPBACK);
  }
#else
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) {
    return htonl(INADDR_LOOPBACK);
  }
#endif
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(53);
  target.sin_addr.s_addr = htonl(0x01010101);  // 1.1.1.1
  sockaddr_in local{};
  socklen_t local_len = sizeof(local);
  uint32_t address = 0;
  if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0 &&
      getsockname(s, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
    address = local.sin_addr.s_addr;
  }
#if REX_PLATFORM_WIN32
  closesocket(s);
#else
  close(s);
#endif
  if (address == 0) {
    return htonl(INADDR_LOOPBACK);
  }
  cached.store(address, std::memory_order_relaxed);
  return address;
}

// Hands the host socket error to the title's WSAGetLastError. The 360 uses the
// Winsock error numbers and titles act on them: DirtySock only keeps a
// non-blocking connect/recv alive when it sees WSAEWOULDBLOCK (10035).
static uint32_t NativeSocketError() {
#if REX_PLATFORM_WIN32
  return static_cast<uint32_t>(WSAGetLastError());
#else
  uint32_t code = 0;
  switch (errno) {
    case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
    case EINPROGRESS:
      code = 10035;  // WSAEWOULDBLOCK
      break;
    case EALREADY:
      code = 10037;  // WSAEALREADY
      break;
    case EISCONN:
      code = 10056;  // WSAEISCONN
      break;
    case ENOTCONN:
      code = 10057;  // WSAENOTCONN
      break;
    case ECONNREFUSED:
      code = 10061;  // WSAECONNREFUSED
      break;
    case ECONNRESET:
      code = 10054;  // WSAECONNRESET
      break;
    case ETIMEDOUT:
      code = 10060;  // WSAETIMEDOUT
      break;
    case ENETUNREACH:
      code = 10051;  // WSAENETUNREACH
      break;
    case EHOSTUNREACH:
      code = 10065;  // WSAEHOSTUNREACH
      break;
    default:
      break;
  }
  return code;
#endif
}

static void SetLastSocketError() {
  XThread::SetLastError(NativeSocketError());
}

// TEMPORARY UDP trace for online connection debugging (remove once peers connect).
static std::atomic<int> udp_trace_budget{2000};
static bool UdpTraceAllowed() {
  return udp_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}
static std::string FormatIPv4(uint32_t ip_host_order, uint16_t port) {
  return fmt::format("{}.{}.{}.{}:{}", (ip_host_order >> 24) & 0xFF, (ip_host_order >> 16) & 0xFF,
                     (ip_host_order >> 8) & 0xFF, ip_host_order & 0xFF, port);
}
static std::string HexPrefix(const uint8_t* data, size_t length) {
  std::string out;
  const size_t count = std::min<size_t>(length, 32);
  for (size_t i = 0; i < count; ++i) {
    out += fmt::format("{:02X}", data[i]);
  }
  return out;
}

// --- Overlapped receives (WSARecv / WSARecvFrom with an OVERLAPPED) ---------
//
// DirtySock's receive thread arms one overlapped receive per socket, waits on
// the OVERLAPPED events with WSAWaitForMultipleEvents(INFINITE) and collects
// the data with WSAGetOverlappedResult. A receive that can't finish right
// away is left pending (WSA_IO_PENDING) and completed by a host thread that
// polls the native sockets.

static constexpr uint32_t kWsaIoPending = 997;
static constexpr uint32_t kWsaIoIncomplete = 996;
static constexpr uint32_t kWsaWouldBlock = 10035;
static constexpr uint32_t kWsaNotSocket = 10038;

struct OverlappedReceive {
  object_ref<XSocket> socket;
  uint32_t buffer_ptr = 0;
  uint32_t buffer_len = 0;
  uint32_t from_ptr = 0;     // XSOCKADDR_IN* (WSARecvFrom only)
  uint32_t fromlen_ptr = 0;  // int*
  uint32_t overlapped_ptr = 0;
  bool done = false;
  uint32_t bytes = 0;
  uint32_t error = 0;  // Winsock error of a failed receive
};

static std::mutex overlapped_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<OverlappedReceive>> overlapped_receives;

// > 0 readable, 0 not yet, < 0 the socket is gone.
static int NativeSocketReadable(uint64_t native_handle) {
  fd_set set;
  FD_ZERO(&set);
  timeval timeout{0, 0};
#if REX_PLATFORM_WIN32
  const auto handle = static_cast<SOCKET>(native_handle);
  FD_SET(handle, &set);
  return select(0, &set, nullptr, nullptr, &timeout);
#else
  const auto handle = static_cast<int>(native_handle);
  FD_SET(handle, &set);
  return select(handle + 1, &set, nullptr, nullptr, &timeout);
#endif
}

// Performs the receive if data (or an error) is ready. Returns false while it
// would still block.
static bool TryReceive(OverlappedReceive& op) {
  const uint64_t native = op.socket->native_handle();
  const int readable = NativeSocketReadable(native);
  if (readable == 0) {
    return false;
  }
  int ret = -1;
  uint32_t error = 0;
  if (readable > 0) {
    auto* buffer = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(op.buffer_ptr);
#if REX_PLATFORM_WIN32
    const auto handle = static_cast<SOCKET>(native);
#else
    const auto handle = static_cast<int>(native);
#endif
    if (op.from_ptr) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      ret = recvfrom(handle, reinterpret_cast<char*>(buffer), op.buffer_len, 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
      if (ret >= 0) {
        // XSOCKADDR_IN: port and address stay in network byte order.
        auto* out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(op.from_ptr);
        memory::store_and_swap<uint16_t>(out, static_cast<uint16_t>(from.sin_family));
        std::memcpy(out + 2, &from.sin_port, 2);
        std::memcpy(out + 4, &from.sin_addr.s_addr, 4);
        std::memset(out + 8, 0, 8);
        if (UdpTraceAllowed()) {
          REXKRNL_INFO("[udp] overlapped recv socket={:08X} from {} len={} data={}",
                       op.socket->handle(),
                       FormatIPv4(ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)), ret,
                       HexPrefix(buffer, static_cast<size_t>(ret)));
        }
        if (op.fromlen_ptr) {
          memory::store_and_swap<uint32_t>(
              REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(op.fromlen_ptr), 16);
        }
      }
    } else {
      ret = recv(handle, reinterpret_cast<char*>(buffer), op.buffer_len, 0);
    }
    if (ret < 0) {
      error = NativeSocketError();
      if (error == kWsaWouldBlock) {
        return false;
      }
    }
  } else {
    error = NativeSocketError();
    if (error == 0) {
      error = kWsaNotSocket;
    }
  }
  op.bytes = ret > 0 ? static_cast<uint32_t>(ret) : 0;
  op.error = error;
  return true;
}

// Fills the guest OVERLAPPED and signals its event (after `done` is visible to
// WSAGetOverlappedResult: the waiter resets the event once it wakes).
static void SignalOverlapped(const OverlappedReceive& op) {
  auto* overlapped = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(op.overlapped_ptr);
  memory::store_and_swap<uint32_t>(overlapped + 0, op.error ? 0xC0000001u : 0u);  // Internal
  memory::store_and_swap<uint32_t>(overlapped + 4, op.bytes);                     // InternalHigh
  const uint32_t event_handle = memory::load_and_swap<uint32_t>(overlapped + 16);
  if (event_handle) {
    if (auto event = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle)) {
      event->Set(0, false);
    }
  }
}

static void PollOverlappedReceives() {
  for (;;) {
    std::vector<std::shared_ptr<OverlappedReceive>> pending;
    {
      std::lock_guard<std::mutex> lock(overlapped_mutex);
      for (auto& [overlapped_ptr, op] : overlapped_receives) {
        if (!op->done) {
          pending.push_back(op);
        }
      }
    }
    for (auto& op : pending) {
      if (!TryReceive(*op)) {
        continue;
      }
      if (UdpTraceAllowed()) {
        REXKRNL_INFO("[udp] pending receive completed socket={:08X} bytes={} error={}",
                     op->socket->handle(), op->bytes, op->error);
      }
      {
        std::lock_guard<std::mutex> lock(overlapped_mutex);
        op->done = true;
      }
      SignalOverlapped(*op);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

static u32 StartReceive(u32 socket_handle, ppc_ptr_t<XWSABUF> buffers, u32 buffer_count,
                        mapped_u32 num_bytes_recv, mapped_u32 flags_ptr, u32 from_ptr,
                        u32 fromlen_ptr, u32 overlapped_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }
  if (!buffers || buffer_count == 0) {
    XThread::SetLastError(0x2726);  // WSAEINVAL
    return -1;
  }

  auto op = std::make_shared<OverlappedReceive>();
  op->socket = socket;
  op->buffer_ptr = buffers[0].buf_ptr;  // titles hand DirtySock a single buffer
  op->buffer_len = buffers[0].len;
  op->from_ptr = from_ptr;
  op->fromlen_ptr = fromlen_ptr;
  op->overlapped_ptr = overlapped_ptr;
  if (flags_ptr) {
    *flags_ptr = 0;
  }

  if (UdpTraceAllowed()) {
    REXKRNL_INFO("[udp] arm receive socket={:08X} local_port={} overlapped={:08X} len={} from={}",
                 socket_handle, socket->bound_port(), overlapped_ptr, op->buffer_len,
                 from_ptr ? "yes" : "no");
  }
  if (TryReceive(*op)) {
    if (op->error && UdpTraceAllowed()) {
      REXKRNL_INFO("[udp] receive failed immediately socket={:08X} error={}", socket_handle,
                   op->error);
    }
    // Finished immediately: like Winsock, still complete the OVERLAPPED.
    if (num_bytes_recv) {
      *num_bytes_recv = op->bytes;
    }
    if (overlapped_ptr) {
      SignalOverlapped(*op);
    }
    if (op->error) {
      XThread::SetLastError(op->error);
      return -1;
    }
    return 0;
  }

  if (!overlapped_ptr) {
    XThread::SetLastError(kWsaWouldBlock);
    return -1;
  }

  static std::once_flag poller_started;
  std::call_once(poller_started, [] { std::thread(PollOverlappedReceives).detach(); });
  {
    std::lock_guard<std::mutex> lock(overlapped_mutex);
    overlapped_receives[overlapped_ptr] = op;
  }
  XThread::SetLastError(kWsaIoPending);
  return -1;
}

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  // Titles derive session ids and keys from this; constant bytes would make
  // every online client generate the same ones.
  FillRandom(buffer_ptr, length);

  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             u32 from_ptr, u32 fromlen_ptr, u32 overlapped_ptr,
                             u32 completion_routine_ptr) {
  return StartReceive(socket_handle, buffers_ptr, buffer_count, num_bytes_recv, flags_ptr,
                      from_ptr, fromlen_ptr, overlapped_ptr);
}

u32 NetDll_WSARecv_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr,
                         u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                         u32 overlapped_ptr, u32 completion_routine_ptr) {
  return StartReceive(socket_handle, buffers_ptr, buffer_count, num_bytes_recv, flags_ptr, 0, 0,
                      overlapped_ptr);
}

u32 NetDll_WSAGetOverlappedResult_entry(u32 caller, u32 socket_handle, u32 overlapped_ptr,
                                        mapped_u32 bytes_ptr, u32 wait, mapped_u32 flags_ptr) {
  std::shared_ptr<OverlappedReceive> op;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(overlapped_mutex);
      auto it = overlapped_receives.find(overlapped_ptr);
      if (it == overlapped_receives.end()) {
        XThread::SetLastError(0x2726);  // WSAEINVAL
        return 0;
      }
      if (it->second->done) {
        op = it->second;
        overlapped_receives.erase(it);
        break;
      }
    }
    if (!wait) {
      XThread::SetLastError(kWsaIoIncomplete);
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (flags_ptr) {
    *flags_ptr = 0;
  }
  if (bytes_ptr) {
    *bytes_ptr = op->bytes;
  }
  if (UdpTraceAllowed()) {
    REXKRNL_INFO("[udp] WSAGetOverlappedResult socket={:08X} overlapped={:08X} bytes={} error={}",
                 socket_handle, overlapped_ptr, op->bytes, op->error);
  }
  if (op->error) {
    XThread::SetLastError(op->error);
    return 0;
  }
  return 1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  int sent = socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);
  if (UdpTraceAllowed()) {
    REXKRNL_INFO("[udp] WSASendTo socket={:08X} local_port={} to {} len={} ret={} data={}",
                 socket_handle, socket->bound_port(),
                 FormatIPv4(static_cast<uint32_t>(native_to.sin_addr),
                            static_cast<uint16_t>(native_to.sin_port)),
                 combined_buffer_size, sent,
                 HexPrefix(combined_buffer_mem.data(), combined_buffer_size));
  }

  // TODO: Instantly complete overlapped

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events == 0 || num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;                 // WSA_WAIT_FAILED
  }

  // fWaitAll selects the NT wait type (WaitAll = 0, WaitAny = 1). Passing it
  // straight through turned DirtySock's wait-any into a wait-all that never
  // returned while its receive thread held the network lock. The timeout is
  // milliseconds; NT waits take relative 100ns ticks as a negative value.
  const uint32_t wait_type = wait_all ? 0 : 1;
  uint64_t timeout_ticks = static_cast<uint64_t>(-static_cast<int64_t>(timeout) * 10000);

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(
        num_events, events, wait_type, 1, alertable,
        timeout != 0xFFFFFFFFu ? &timeout_ticks : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (result == X_STATUS_TIMEOUT) {
    return 0x102;  // WSA_WAIT_TIMEOUT
  }
  if (result == X_STATUS_USER_APC) {
    return 0xC0;  // WSA_WAIT_IO_COMPLETION
  }
  if (result < num_events) {
    return result;  // WSA_WAIT_EVENT_0 + index of the signaled event (0 for wait-all)
  }
  if (result >= X_STATUS_ABANDONED_WAIT_0 && result < X_STATUS_ABANDONED_WAIT_0 + num_events) {
    return result - X_STATUS_ABANDONED_WAIT_0;
  }
  XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(result));
  return ~0u;  // WSA_WAIT_FAILED
}

u32 NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  // The PC's real address: recomp sockets are plain IP, so an XNADDR simply
  // carries the IP peers and servers reach this machine at.
  const uint32_t ip = LocalIPv4();
  addr_ptr->ina.s_addr = ip;
  addr_ptr->inaOnline.s_addr = ip;
  addr_ptr->wPortOnline = 3074;

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  // Locally administered MAC built from the IP so machines differ.
  addr_ptr->abEnet[0] = 0x02;
  addr_ptr->abEnet[1] = 0xCC;
  std::memcpy(addr_ptr->abEnet + 2, &ip, 4);

  std::memset(addr_ptr->abOnline, 0, 20);

  // ETHERNET | STATIC | GATEWAY | DNS | ONLINE: DirtySock only trusts ina
  // once GATEWAY or DNS is reported.
  return 0x02 | 0x04 | 0x20 | 0x40 | 0x80;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  rex::string::rex_strcpy(string_out, string_size, "666.666.666.666");
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
// An XNADDR's ina is the peer's real IP (see XNetGetTitleXnAddr), so the
// "secure" address is that IP itself.
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr_ptr) {
  if (!xn_addr) {
    return 0x2726;  // WSAEINVAL
  }
  std::memcpy(in_addr_ptr, &xn_addr->ina, sizeof(xn_addr->ina));
  return 0;
}

// Does the reverse of the above.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 ina, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  if (!xn_addr) {
    return 0x2726;  // WSAEINVAL
  }
  xn_addr.Zero();
  memory::store_and_swap<uint32_t>(reinterpret_cast<uint8_t*>(&xn_addr->ina), ina);
  xn_addr->inaOnline = xn_addr->ina;
  return 0;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE | XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
         XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
}

// Keys and security associations only exist for the console's encrypted
// transport. Recomp sockets are plain, so keys are accepted and every peer is
// reported connected.
u32 NetDll_XNetCreateKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  FillRandom(xnkid, 8);   // XNKID
  FillRandom(xnkey, 16);  // XNKEY
  return 0;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, mapped_void xnkid) {
  return 0;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 ina) {
  return 0;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 ina) {
  return 2;  // XNET_CONNECT_STATUS_CONNECTED
}

// There are no Xbox Live title servers (LSP); titles fall back to DNS.
u32 NetDll_XNetServerToInAddr_entry(u32 caller, u32 ina, u32 service_id, mapped_void in_addr_ptr) {
  return 0x2726;  // WSAEINVAL
}

// QoS probes target Xbox Live infrastructure; a failed lookup is skipped.
u32 NetDll_XNetQosLookup_entry(u32 caller) {
  return 0x2726;  // WSAEINVAL
}

// net_host_overrides lookup: returns the configured target for `host`, or an
// empty string when the title's own name should be resolved. Parsed per call --
// name lookups are a handful per session, and reading the cvar each time means
// an edited value applies without a restart.
std::string HostOverrideFor(const std::string& host) {
  const std::string spec = REXCVAR_GET(net_host_overrides);
  if (spec.empty() || host.empty()) {
    return {};
  }
  const auto trim = [](std::string v) {
    const size_t b = v.find_first_not_of(" 	");
    if (b == std::string::npos) {
      return std::string();
    }
    return v.substr(b, v.find_last_not_of(" 	") - b + 1);
  };
  const auto lower = [](std::string v) {
    for (char& c : v) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v;
  };
  const std::string want = lower(host);
  for (size_t pos = 0; pos < spec.size();) {
    size_t end = spec.find_first_of(",;", pos);
    if (end == std::string::npos) {
      end = spec.size();
    }
    const std::string entry = spec.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = entry.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = trim(entry.substr(0, eq));
    const std::string target = trim(entry.substr(eq + 1));
    if (!target.empty() && lower(key) == want) {
      return target;
    }
  }
  return {};
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  if (!pdns) {
    if (event_handle) {
      auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
      assert_not_null(ev);
      ev->Set(0, false);
    }
    return 0;
  }

  auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
  auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
  std::memset(dns, 0, sizeof(XNDNS));
  dns->status = 10036;  // WSAEINPROGRESS: still resolving.
  *pdns = dns_guest;

  // Resolved on a background thread: XNetDnsLookup is an async API and the
  // title is expected to wait on event_handle, so getaddrinfo() must not
  // block the calling (guest) thread. A hosts-file entry is how servers get
  // redirected; resolution time still depends on the local resolver, so
  // this can legitimately take a while, but it no longer stalls the game.
  const std::string requested = host ? std::string(host.value()) : std::string();
  // An override target is resolved in place of the requested name; a literal
  // IPv4 address resolves to itself, so both forms take the same path.
  const std::string override_target = HostOverrideFor(requested);
  const std::string name = override_target.empty() ? requested : override_target;
  if (!override_target.empty()) {
    REXKRNL_INFO("XNetDnsLookup({}) -> overridden to {} (net_host_overrides)", requested,
                 override_target);
  }
  std::thread([name, dns, event_handle]() {
    uint32_t count = 0;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (!name.empty() && getaddrinfo(name.c_str(), nullptr, &hints, &results) == 0) {
      for (addrinfo* it = results; it != nullptr && count < 8; it = it->ai_next) {
        if (it->ai_family == AF_INET) {
          dns->aina[count++].s_addr = reinterpret_cast<sockaddr_in*>(it->ai_addr)->sin_addr.s_addr;
        }
      }
      freeaddrinfo(results);
    }
    dns->cina = count;
    dns->status = count ? 0 : 11001;  // WSAHOST_NOT_FOUND
    if (count) {
      const auto* ip = reinterpret_cast<const uint8_t*>(&dns->aina[0].s_addr);
      REXKRNL_INFO("XNetDnsLookup({}) -> {}.{}.{}.{}", name, ip[0], ip[1], ip[2], ip[3]);
    } else {
      REXKRNL_INFO("XNetDnsLookup({}) -> not found", name);
    }
    if (event_handle) {
      auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
      assert_not_null(ev);
      ev->Set(0, false);
    }
  }).detach();

  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    SetLastSocketError();
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  if (XFAILED(status)) {
    SetLastSocketError();
    return -1;
  }
  return 0;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    SetLastSocketError();
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    SetLastSocketError();
    return -1;
  }

  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    // Non-blocking connects end here with WSAEWOULDBLOCK (still in progress).
    SetLastSocketError();
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    SetLastSocketError();
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    SetLastSocketError();
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {0};
  fd_set native_readfds = {0};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {0};
  fd_set native_writefds = {0};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {0};
  fd_set native_exceptfds = {0};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (ret == -1) {
    SetLastSocketError();
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);
  if (ret >= 0 && UdpTraceAllowed()) {
    REXKRNL_INFO("[udp] recvfrom socket={:08X} from {} len={}", socket_handle,
                 FormatIPv4(static_cast<uint32_t>(native_from.sin_addr),
                            static_cast<uint16_t>(native_from.sin_port)),
                 ret);
  }

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
    SetLastSocketError();
  }

  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  int ret = socket->Send(buf_ptr, buf_len, flags);
  if (ret == -1) {
    SetLastSocketError();
  }
  return ret;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  int ret = socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
  if (ret == -1) {
    SetLastSocketError();
  }
  if (UdpTraceAllowed()) {
    REXKRNL_INFO("[udp] sendto socket={:08X} local_port={} to {} len={} ret={} err={} data={}",
                 socket_handle, socket->bound_port(),
                 FormatIPv4(static_cast<uint32_t>(native_to.sin_addr),
                            static_cast<uint16_t>(native_to.sin_port)),
                 buf_len, ret, ret == -1 ? XThread::GetLastError() : 0,
                 HexPrefix(static_cast<uint8_t*>(static_cast<void*>(buf_ptr)), buf_len));
  }
  return ret;
}

// getpeername/getsockname: IPv4 only (the only family titles create).
static u32 StoreSocketName(u32 socket_handle, ppc_ptr_t<XSOCKADDR> name_ptr,
                           mapped_u32 namelen_ptr, bool peer) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }
  if (!name_ptr || !namelen_ptr || *namelen_ptr < sizeof(XSOCKADDR_IN)) {
    XThread::SetLastError(0x271E);  // WSAEFAULT
    return -1;
  }
  sockaddr_in native{};
  socklen_t native_len = sizeof(native);
#if REX_PLATFORM_WIN32
  const auto handle = static_cast<SOCKET>(socket->native_handle());
#else
  const auto handle = static_cast<int>(socket->native_handle());
#endif
  const int ret = peer ? getpeername(handle, reinterpret_cast<sockaddr*>(&native), &native_len)
                       : getsockname(handle, reinterpret_cast<sockaddr*>(&native), &native_len);
  if (ret != 0) {
    SetLastSocketError();
    return -1;
  }
  // Port and address are network byte order on both sides: copy them raw.
  auto* out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(name_ptr.guest_address());
  memory::store_and_swap<uint16_t>(out, static_cast<uint16_t>(native.sin_family));
  std::memcpy(out + 2, &native.sin_port, 2);
  std::memcpy(out + 4, &native.sin_addr.s_addr, 4);
  std::memset(out + 8, 0, 8);
  *namelen_ptr = sizeof(XSOCKADDR_IN);
  return 0;
}

u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name_ptr,
                             mapped_u32 namelen_ptr) {
  return StoreSocketName(socket_handle, name_ptr, namelen_ptr, true);
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name_ptr,
                             mapped_u32 namelen_ptr) {
  return StoreSocketName(socket_handle, name_ptr, namelen_ptr, false);
}

// Integer options (SO_SNDBUF, SO_RCVBUF, SO_ERROR, ...), returned big-endian.
u32 NetDll_getsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, mapped_u32 optlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }
  if (!optlen_ptr || *optlen_ptr < sizeof(uint32_t)) {
    XThread::SetLastError(0x271E);  // WSAEFAULT
    return -1;
  }
  int value = 0;
  socklen_t value_len = sizeof(value);
#if REX_PLATFORM_WIN32
  const auto handle = static_cast<SOCKET>(socket->native_handle());
#else
  const auto handle = static_cast<int>(socket->native_handle());
#endif
  if (getsockopt(handle, static_cast<int>(level), static_cast<int>(optname),
                 reinterpret_cast<char*>(&value), &value_len) != 0) {
    SetLastSocketError();
    return -1;
  }
  memory::store_and_swap<uint32_t>(static_cast<uint8_t*>(static_cast<void*>(optval_ptr)),
                                   static_cast<uint32_t>(value));
  *optlen_ptr = sizeof(uint32_t);
  return 0;
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSARecv, rex::kernel::xam::NetDll_WSARecv_entry)
REX_EXPORT(__imp__NetDll_WSAGetOverlappedResult,
           rex::kernel::xam::NetDll_WSAGetOverlappedResult_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus,
           rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT(__imp__NetDll_XNetServerToInAddr, rex::kernel::xam::NetDll_XNetServerToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetUnregisterInAddr);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT(__imp__NetDll_getpeername, rex::kernel::xam::NetDll_getpeername_entry)
REX_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
REX_EXPORT(__imp__NetDll_getsockopt, rex::kernel::xam::NetDll_getsockopt_entry)
