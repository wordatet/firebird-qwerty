#ifndef USBIP_SERVER_H
#define USBIP_SERVER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

// USBIP Protocol Constants
#define USBIP_VERSION 0x0111

#define OP_REQ_DEVLIST 0x8005
#define OP_REP_DEVLIST 0x0005
#define OP_REQ_IMPORT 0x8003
#define OP_REP_IMPORT 0x0003

#define USBIP_CMD_SUBMIT 0x0001
#define USBIP_CMD_UNLINK 0x0002
#define USBIP_RET_SUBMIT 0x0003
#define USBIP_RET_UNLINK 0x0004

// Descriptor constants for TI-Nspire CX II
#define CXII_VENDOR_ID 0x0451
#define CXII_PRODUCT_ID 0xE022

class USBIPServer {
public:
  static USBIPServer &instance();

  void start();
  void stop();
  bool isRunning() const { return m_running; }

  void setVerbose(bool v) { m_verbose = v; }
  bool getVerbose() const { return m_verbose; }

  // Called by usb_cx2 when a packet is received from the calculator (IN
  // transfer) or when an OUT transfer generated a response/ACK that needs to be
  // signaled? Actually, USBIP works by submitting URBs. For IN transfers, we
  // wait for the calc to provide data, then return RET_SUBMIT. For OUT
  // transfers, we send data to calc, then return RET_SUBMIT (ack).

  void onPacketFromCalc(int ep, const uint8_t *data, size_t size);

private:
  USBIPServer();
  ~USBIPServer();

  void serverLoop();
  void clientHandler(int clientSock);

  // Protocol handlers
  void handleReqDevList(int sock);
  void handleReqImport(int sock, const uint8_t *buf);
  void handleCmdSubmit(int sock, const uint8_t *header);
  void handleCmdUnlink(int sock, const uint8_t *header);

  // Helper to send URB completion
  void sendRetSubmit(int sock, uint32_t seqnum, uint32_t devid,
                     uint32_t direction, uint32_t ep, int32_t status,
                     int32_t actual_length, const uint8_t *data);

  struct PendingRequest {
    uint32_t seqnum;
    uint32_t ep;
    uint32_t len;
  };

  std::mutex m_queueMutex;
  std::vector<PendingRequest> m_pendingRequests;

  std::atomic<bool> m_running;
  std::thread m_serverThread;
  std::atomic<int> m_serverSock;

  // Active connection state
  std::atomic<int> m_clientSock;
  std::atomic<bool> m_verbose;
  std::mutex m_clientMutex;
  std::mutex m_sendMutex;

  // We need to track pending URBs to match responses?
  // For simplicity, we might handle them synchronously where possible,
  // or store them in a map if async handling (waiting for calc) is needed.
};

#endif // USBIP_SERVER_H
