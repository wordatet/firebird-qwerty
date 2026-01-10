#include "usbip_server.h"
#include "debug.h"
#include "emu.h"
#include "usb_cx2.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
#define close closesocket
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>

#define LOG_TAG "USBIP"
#define LOG(...)                                                               \
  do {                                                                         \
    gui_debug_printf(LOG_TAG ": " __VA_ARGS__);                                \
    gui_debug_printf("\n");                                                    \
    printf(LOG_TAG ": " __VA_ARGS__);                                          \
    printf("\n");                                                              \
  } while (0)
#define VLOG(...)                                                              \
  do {                                                                         \
    if (m_verbose) {                                                           \
      gui_debug_printf(LOG_TAG ": " __VA_ARGS__);                              \
      gui_debug_printf("\n");                                                  \
      printf(LOG_TAG ": " __VA_ARGS__);                                        \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

// Structs for protocol
struct op_common {
  uint16_t version;
  uint16_t code;
  uint32_t status;
} __attribute__((packed));

struct usbip_header_basic {
  uint32_t command;
  uint32_t seqnum;
  uint32_t devid;
  uint32_t direction;
  uint32_t ep;
} __attribute__((packed));

struct usbip_cmd_submit {
  usbip_header_basic base;
  uint32_t transfer_flags;
  uint32_t transfer_buffer_length;
  uint32_t start_frame;
  uint32_t number_of_packets;
  uint32_t interval;
  uint8_t setup[8];
} __attribute__((packed));

struct usbip_ret_submit {
  usbip_header_basic base;
  uint32_t status;
  uint32_t actual_length;
  uint32_t start_frame;
  uint32_t number_of_packets;
  uint32_t error_count;
  uint64_t padded; // Padding to align?
} __attribute__((packed));

// Device info
static const char *kPath = "/sys/devices/pci0000:00/0000:00:01.2/usb1/1-1";
static const char *kBusId = "1-1";

USBIPServer::USBIPServer()
    : m_running(false), m_serverSock(-1), m_clientSock(-1), m_verbose(false) {
#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

USBIPServer::~USBIPServer() { stop(); }

USBIPServer &USBIPServer::instance() {
  static USBIPServer s_instance;
  return s_instance;
}

void USBIPServer::start() {
  if (m_running)
    return;

  m_running = true;
  m_serverThread = std::thread(&USBIPServer::serverLoop, this);

  // Pulse reset to trigger enumeration in the emulated OS
  usb_cx2_set_present(true);
  std::thread([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    usb_cx2_set_present(false);
  }).detach();

  LOG("Server started on port 3240");
}

void USBIPServer::stop() {
  usb_cx2_set_present(false);
  m_running = false;

  if (m_serverSock != -1) {
    shutdown(m_serverSock, SHUT_RDWR);
    close(m_serverSock);
    m_serverSock = -1;
  }
  if (m_clientSock != -1) {
    shutdown(m_clientSock, SHUT_RDWR);
    close(m_clientSock);
    m_clientSock = -1;
  }
  if (m_serverThread.joinable()) {
    m_serverThread.join();
  }
}

void USBIPServer::serverLoop() {
  m_serverSock = socket(AF_INET, SOCK_STREAM, 0);
  if (m_serverSock < 0) {
    LOG("Failed to create socket");
    return;
  }

  int opt = 1;
  setsockopt(m_serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(3240);

  if (bind(m_serverSock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOG("Failed to bind socket");
    close(m_serverSock);
    return;
  }

  if (listen(m_serverSock, 1) < 0) {
    LOG("Failed to listen");
    close(m_serverSock);
    return;
  }

  while (m_running) {
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    int clientSock =
        accept(m_serverSock, (struct sockaddr *)&clientAddr, &clientLen);

    if (clientSock < 0) {
      if (m_running)
        LOG("Accept failed");
      continue;
    }

    LOG("Client connected from %s", inet_ntoa(clientAddr.sin_addr));

    {
      std::lock_guard<std::mutex> lock(m_clientMutex);
      if (m_clientSock != -1)
        close(m_clientSock);
      m_clientSock = clientSock;
    }

    // Clear old pending requests on new connection
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_pendingRequests.clear();
    }

    // Pulse USB reset so the OS sees a fresh connection
    usb_cx2_bus_reset_on();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    usb_cx2_bus_reset_off();

    clientHandler(clientSock);

    // Clear pending requests on disconnect
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_pendingRequests.clear();
    }

    {
      std::lock_guard<std::mutex> lock(m_clientMutex);
      if (m_clientSock == clientSock) {
        close(m_clientSock);
        m_clientSock = -1;
      }
    }
    LOG("Client disconnected");
  }
}

void USBIPServer::clientHandler(int sock) {
  int flag = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

  while (m_running) {
    op_common header;
    ssize_t received = recv(sock, &header, sizeof(header), MSG_WAITALL);
    if (received != sizeof(header))
      break;

    uint16_t code = ntohs(header.code);

    if (ntohs(header.version) == USBIP_VERSION) {
      switch (code) {
      case OP_REQ_DEVLIST:
        handleReqDevList(sock);
        break;
      case OP_REQ_IMPORT: {
        uint8_t busid[32];
        if (recv(sock, busid, 32, MSG_WAITALL) != 32)
          return;
        handleReqImport(sock, busid);
      } break;
      default:
        VLOG("Unknown OP %04x", code);
        break;
      }
    } else {
      // Handle CMD_SUBMIT / UNLINK
      uint32_t cmd_raw;
      memcpy(&cmd_raw, &header, 4);
      uint32_t cmd = ntohl(cmd_raw);

      if (cmd == USBIP_CMD_SUBMIT) {
        uint8_t buf[48];
        memcpy(buf, &header, 8);
        if (recv(sock, buf + 8, 40, MSG_WAITALL) != 40)
          return;
        handleCmdSubmit(sock, buf);
      } else if (cmd == USBIP_CMD_UNLINK) {
        uint8_t buf[48];
        memcpy(buf, &header, 8);
        if (recv(sock, buf + 8, 40, MSG_WAITALL) != 40)
          return;
        handleCmdUnlink(sock, buf);
      } else {
        VLOG("Unknown CMD %08x", cmd);
        return; // Desync
      }
    }
  }
}

void USBIPServer::handleReqDevList(int sock) {
  struct op_devlist_rep { // header + ...
    struct op_common common;
    uint32_t ndev;
  } __attribute__((packed));

  op_devlist_rep rep;
  rep.common.version = htons(USBIP_VERSION);
  rep.common.code = htons(OP_REP_DEVLIST);
  rep.ndev = htonl(1);

  {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    send(sock, &rep, sizeof(rep), 0);
  }

  struct device_desc {
    char path[256];
    char busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bConfigurationValue;
    uint8_t bNumConfigurations;
    uint8_t bNumInterfaces;
  } __attribute__((packed));

  device_desc dev;
  memset(&dev, 0, sizeof(dev));
  strncpy(dev.path, kPath, 255);
  strncpy(dev.busid, kBusId, 31);
  dev.busnum = htonl(1);
  dev.devnum = htonl(2);
  dev.speed = htonl(3); // High speed
  dev.idVendor = htons(CXII_VENDOR_ID);
  dev.idProduct = htons(CXII_PRODUCT_ID);
  dev.bcdDevice = htons(0x0100);
  dev.bDeviceClass = 0;
  dev.bDeviceSubClass = 0;
  dev.bDeviceProtocol = 0;
  dev.bConfigurationValue = 0;
  dev.bNumInterfaces = 1;

  {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    send(sock, &dev, sizeof(dev), 0);
  }

  struct if_desc {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
  } __attribute__((packed));

  if_desc iface;
  iface.bInterfaceClass = 0xFF; // Vendor specific
  iface.bInterfaceSubClass = 0;
  iface.padding = 0;

  {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    send(sock, &iface, sizeof(iface), 0);
  }
}

void USBIPServer::handleReqImport(int sock, const uint8_t *busid) {
  if (strncmp((const char *)busid, kBusId, 32) != 0) {
    op_common rep;
    rep.version = htons(USBIP_VERSION);
    rep.code = htons(OP_REP_IMPORT);
    rep.status = htonl(1);
    {
      std::lock_guard<std::mutex> lock(m_sendMutex);
      send(sock, &rep, sizeof(rep), 0);
    }
    return;
  }

  struct op_import_rep {
    op_common common;
  } __attribute__((packed));

  op_import_rep rep;
  rep.common.version = htons(USBIP_VERSION);
  rep.common.code = htons(OP_REP_IMPORT);
  rep.common.status = 0;
  {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    send(sock, &rep, sizeof(rep), 0);
  }

  struct device_desc {
    char path[256];
    char busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bConfigurationValue;
    uint8_t bNumConfigurations;
    uint8_t bNumInterfaces;
  } __attribute__((packed));

  device_desc dev;
  memset(&dev, 0, sizeof(dev));
  strncpy(dev.path, kPath, 255);
  strncpy(dev.busid, kBusId, 31);
  dev.busnum = htonl(1);
  dev.devnum = htonl(2);
  dev.speed = htonl(3);
  dev.idVendor = htons(CXII_VENDOR_ID);
  dev.idProduct = htons(CXII_PRODUCT_ID);
  dev.bcdDevice = htons(0x0100);
  dev.bDeviceClass = 0;
  dev.bDeviceSubClass = 0;
  dev.bDeviceProtocol = 0;
  dev.bConfigurationValue = 0;
  dev.bNumInterfaces = 1;
  {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    send(sock, &dev, sizeof(dev), 0);
  }
}

void USBIPServer::handleCmdSubmit(int sock, const uint8_t *header) {
  const usbip_cmd_submit *cmd = (const usbip_cmd_submit *)header;

  uint32_t seq = ntohl(cmd->base.seqnum);
  uint32_t ep = ntohl(cmd->base.ep);
  uint32_t dir = ntohl(cmd->base.direction); // 0=OUT, 1=IN
  uint32_t len = ntohl(cmd->transfer_buffer_length);

  VLOG("CMD_SUBMIT Seq %u EP %u Dir %u Len %u Setup: %02x %02x %02x %02x %02x "
       "%02x %02x %02x",
       seq, ep, dir, len, cmd->setup[0], cmd->setup[1], cmd->setup[2],
       cmd->setup[3], cmd->setup[4], cmd->setup[5], cmd->setup[6],
       cmd->setup[7]);

  if (ep == 0) {
    if (dir == 0) { // OUT
      usb_cx2_receive_setup_packet((const usb_setup *)cmd->setup);
      if (len > 0) {
        std::vector<uint8_t> data(len);
        recv(sock, data.data(), len, MSG_WAITALL);
        usb_cx2_packet_to_calc(0, data.data(), len);
      }
      sendRetSubmit(sock, seq, 0, dir, ep, 0, len, nullptr);
    } else { // IN
      usb_cx2_receive_setup_packet((const usb_setup *)cmd->setup);

      // Queue request for data
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_pendingRequests.push_back({seq, ep, len});

      // Note: usb_cx2 responds to setup packets (e.g. GetDescriptor) by writing
      // to FIFO via DMA. We capture this in usb_cx2_fdma_update ->
      // usb_cx2_packet_from_calc.
    }
  } else {
    if (dir == 0) { // OUT
      std::vector<uint8_t> data(len);
      recv(sock, data.data(), len, MSG_WAITALL);
      usb_cx2_packet_to_calc(ep, data.data(), len);
      sendRetSubmit(sock, seq, 0, dir, ep, 0, len, nullptr);
    } else { // IN
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_pendingRequests.push_back({seq, ep, len});
    }
  }
}

void USBIPServer::handleCmdUnlink(int sock, const uint8_t *header) {
  const usbip_header_basic *basic = (const usbip_header_basic *)header;
  uint32_t seq = ntohl(basic->seqnum);

  usbip_ret_submit ret;
  memset(&ret, 0, sizeof(ret));
  ret.base.command = htonl(USBIP_RET_UNLINK);
  ret.base.seqnum = htonl(seq);
  ret.base.devid = basic->devid;
  ret.base.direction = basic->direction;
  ret.status = htonl(0); // Success

  {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    send(sock, &ret, sizeof(ret), 0);
  }
}

void USBIPServer::sendRetSubmit(int sock, uint32_t seqnum, uint32_t devid,
                                uint32_t direction, uint32_t ep, int32_t status,
                                int32_t actual_length, const uint8_t *data) {
  usbip_ret_submit ret;
  memset(&ret, 0, sizeof(ret));
  ret.base.command = htonl(USBIP_RET_SUBMIT);
  ret.base.seqnum = htonl(seqnum);
  ret.base.devid = htonl(devid);
  ret.base.direction = htonl(direction);
  ret.base.ep = htonl(ep);
  ret.status = htonl(status);
  ret.actual_length = htonl(actual_length);

  {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    send(sock, &ret, sizeof(ret), 0);

    if (actual_length > 0 && data) {
      send(sock, data, actual_length, 0);
    }
  }
}

void USBIPServer::onPacketFromCalc(int ep, const uint8_t *data, size_t size) {
  std::lock_guard<std::mutex> lock(m_queueMutex);

  for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end();
       ++it) {
    if (it->ep == (uint32_t)ep) {
      // Match found!
      {
        std::lock_guard<std::mutex> sockLock(m_clientMutex);
        if (m_clientSock != -1) {
          // Truncate if needed?
          size_t sendSize = (size < it->len) ? size : it->len;
          sendRetSubmit(m_clientSock, it->seqnum, 0, 1, ep, 0, sendSize, data);
        }
      }
      m_pendingRequests.erase(it);
      return;
    }
  }

  // VLOG("Dropped calc packet EP%d Size %zu (No pending req)", ep, size);
}
