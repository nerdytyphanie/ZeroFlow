// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "arch/Arch.h"
#include "base/Log.h"
#include "base/EventQueue.h"
#include "server/ClientProxy1_3.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include <QCoreApplication>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

class MemoryStream final : public deskflow::IStream {
public:
  std::string bytes;
  void close() override { bytes.clear(); }
  uint32_t read(void *out, uint32_t size) override {
    size = (std::min)(size, uint32_t(bytes.size()));
    if (out) std::memcpy(out, bytes.data(), size);
    bytes.erase(0, size);
    return size;
  }
  void write(const void *data, uint32_t size) override { bytes.append(static_cast<const char *>(data), size); }
  void flush() override {}
  void shutdownInput() override {}
  void shutdownOutput() override {}
  void *getEventTarget() const override { return nullptr; }
  bool isReady() const override { return !bytes.empty(); }
  uint32_t getSize() const override { return uint32_t(bytes.size()); }
};

void require(bool condition) { if (!condition) throw std::runtime_error("clipboard limit regression"); }

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  Arch arch; arch.init();
  Log log;
  MemoryStream stream;
  ClipboardChunkAssemblyState state;
  std::string cached;
  ClipboardID id;
  uint32_t sequence;
  constexpr size_t limit = 3 * 1024 * 1024;
  auto receive = [&](uint8_t mark, const std::string &data) {
    ProtocolUtil::writef(&stream, kMsgDClipboard + 4, 0, 42, mark, &data);
    return ClipboardChunk::assemble(&stream, cached, id, sequence, state, limit);
  };

  // Reproduce the actual test failure without a clipboard or network peer.
  require(receive(ChunkType::DataStart, "17971490") == TransferState::InProgress);
  const std::string chunk(32768, 'x');
  for (size_t sent = 0; sent < 17971490; sent += chunk.size()) {
    require(receive(ChunkType::DataChunk, chunk.substr(0, (std::min)(chunk.size(), size_t(17971490) - sent))) == TransferState::InProgress);
    require(cached.empty());
  }
  require(receive(ChunkType::DataEnd, "") == TransferState::InProgress);
  require(!state.active && !state.discarding && cached.empty());

  // The next normal clipboard transfer still completes on the same stream.
  require(receive(ChunkType::DataStart, "5") == TransferState::Started);
  require(receive(ChunkType::DataChunk, "hello") == TransferState::InProgress);
  require(receive(ChunkType::DataEnd, "") == TransferState::Finished);
  require(cached == "hello" && id == 0 && sequence == 42);
  std::cout << "PASS: oversized clipboard discarded without error or retained payload; subsequent transfer succeeds\n";

  EventQueue events;
  auto *wire = new MemoryStream;
  ClientProxy1_3 client("test", wire, &events);
  client.resetOptions();
  auto *stopTimer = events.newOneShotTimer(4.5, nullptr);
  events.addHandler(EventTypes::Timer, stopTimer, [&](const auto &) { events.addEvent(Event(EventTypes::Quit)); });
  events.loop();
  events.removeHandler(EventTypes::Timer, stopTimer);
  events.deleteTimer(stopTimer);
  require(wire->bytes.find(kMsgCKeepAlive) != std::string::npos);
  std::cout << "PASS: protocol keep-alive timer runs alongside headless status timer\n";
}
