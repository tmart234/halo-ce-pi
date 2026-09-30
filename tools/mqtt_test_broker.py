"""A minimal MQTT 3.1.1 broker for testing internet play's signalling
(port/linux/src/p2p_signal.c) on one computer, without the public brokers.

It knows what the game sends: CONNECT, SUBSCRIBE and UNSUBSCRIBE (exact
topics, QoS 0), PUBLISH at QoS 0, PINGREQ and DISCONNECT. Every PUBLISH goes
to every connection subscribed to its topic.

    python tools/mqtt_test_broker.py --port 18830
    HALO_NET_BROKERS=127.0.0.1:18830 HALO_NET_STUN= build/linux/halo
"""

import argparse
import selectors
import socket
import sys
from typing import Dict, Optional, Set


def encode_length(length: int) -> bytes:
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        out.append(byte | (0x80 if length else 0))
        if not length:
            return bytes(out)


def packet(first: int, body: bytes) -> bytes:
    return bytes([first]) + encode_length(len(body)) + body


class Broker:
    def __init__(self, port: int, address: str = "127.0.0.1", log=None):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind((address, port))
        self.listener.listen(64)
        self.listener.setblocking(False)
        self.port = self.listener.getsockname()[1]
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.listener, selectors.EVENT_READ)
        self.buffers: Dict[socket.socket, bytearray] = {}
        self.subscriptions: Dict[socket.socket, Set[str]] = {}
        self.published = 0
        self.log = log or (lambda text: None)

    def close(self, connection: socket.socket) -> None:
        self.selector.unregister(connection)
        self.buffers.pop(connection, None)
        self.subscriptions.pop(connection, None)
        connection.close()

    def send(self, connection: socket.socket, data: bytes) -> None:
        try:
            connection.sendall(data)
        except OSError:
            pass

    def handle(self, connection: socket.socket, first: int, body: bytes) -> None:
        kind = first >> 4
        if kind == 1:  # CONNECT
            self.send(connection, packet(0x20, b"\x00\x00"))
        elif kind == 8 or kind == 10:  # SUBSCRIBE, UNSUBSCRIBE
            identifier = body[:2]
            at = 2
            topics = []
            while at + 2 <= len(body):
                size = int.from_bytes(body[at:at + 2], "big")
                topics.append(body[at + 2:at + 2 + size].decode("utf-8", "replace"))
                at += 2 + size + (1 if kind == 8 else 0)
            subscribed = self.subscriptions.setdefault(connection, set())
            if kind == 8:
                subscribed.update(topics)
                self.send(connection, packet(0x90, identifier + b"\x00" * len(topics)))
            else:
                subscribed.difference_update(topics)
                self.send(connection, packet(0xB0, identifier))
            self.log(f"{'subscribe' if kind == 8 else 'unsubscribe'} {topics}")
        elif kind == 3:  # PUBLISH (QoS 0)
            size = int.from_bytes(body[:2], "big")
            topic = body[2:2 + size].decode("utf-8", "replace")
            self.published += 1
            for other, topics in list(self.subscriptions.items()):
                if topic in topics:
                    self.send(other, packet(0x30, body))
        elif kind == 12:  # PINGREQ
            self.send(connection, packet(0xD0, b""))
        elif kind == 14:  # DISCONNECT
            self.close(connection)

    def readable(self, connection: socket.socket) -> None:
        try:
            data = connection.recv(65536)
        except OSError:
            data = b""
        if not data:
            self.close(connection)
            return
        buffer = self.buffers[connection]
        buffer.extend(data)
        while len(buffer) >= 2:
            length, multiplier, at = 0, 1, 1
            while at < len(buffer):
                byte = buffer[at]
                length += (byte & 0x7F) * multiplier
                multiplier *= 128
                at += 1
                if not byte & 0x80:
                    break
            else:
                return
            if len(buffer) < at + length:
                return
            first, body = buffer[0], bytes(buffer[at:at + length])
            del buffer[:at + length]
            self.handle(connection, first, body)
            if connection not in self.buffers:
                return

    def serve(self, seconds: Optional[float] = None) -> None:
        import time
        deadline = None if seconds is None else time.monotonic() + seconds
        while deadline is None or time.monotonic() < deadline:
            for key, _ in self.selector.select(timeout=0.2):
                if key.fileobj is self.listener:
                    connection, _ = self.listener.accept()
                    connection.setblocking(True)
                    self.buffers[connection] = bytearray()
                    self.selector.register(connection, selectors.EVENT_READ)
                else:
                    self.readable(key.fileobj)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=18830)
    parser.add_argument("--address", default="127.0.0.1")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    broker = Broker(args.port, args.address, (lambda text: print(text, flush=True)) if args.verbose else None)
    print(f"MQTT test broker on {args.address}:{broker.port}", flush=True)
    try:
        broker.serve()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
