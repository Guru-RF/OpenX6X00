#!/usr/bin/env python3
"""A TCI client of the plainest kind, for the host test of src/tci_server.c (see Makefile):
a raw WebSocket on 127.0.0.1:50001 (TCI_TEST_PORT) that walks through what the VFO-Knob does
on a connection. Standard library only."""
import base64
import hashlib
import os
import socket
import struct
import sys
import time

HOST, PORT = "127.0.0.1", int(os.environ.get("TCI_TEST_PORT", "50001"))


class WS:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET / HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.s.recv(1024)
            if not chunk:
                raise RuntimeError("closed during the handshake")
            resp += chunk
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        assert b"101" in head.split(b"\r\n")[0], head
        assert want.encode() in head, "bad Sec-WebSocket-Accept"

    def send(self, opcode, payload):
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.s.sendall(head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

    def text(self, s):
        self.send(1, s.encode())

    def _fill(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("closed")
            self.buf += chunk

    def frame(self):
        """(opcode, payload) of the next frame"""
        self._fill(2)
        b0, b1 = self.buf[0], self.buf[1]
        n, pos = b1 & 0x7F, 2
        if n == 126:
            self._fill(4)
            n, pos = struct.unpack(">H", self.buf[2:4])[0], 4
        elif n == 127:
            self._fill(10)
            n, pos = struct.unpack(">Q", self.buf[2:10])[0], 10
        assert not (b1 & 0x80), "server frames must not be masked"
        self._fill(pos + n)
        payload = self.buf[pos:pos + n]
        self.buf = self.buf[pos + n:]
        return b0 & 0x0F, payload

    def wait_text(self, prefix, timeout=3.0):
        """The next text frame starting with prefix; binary frames and others are kept aside"""
        end = time.time() + timeout
        while time.time() < end:
            op, p = self.frame()
            if op == 1:
                line = p.decode()
                if line.startswith(prefix):
                    return line
            elif op == 2:
                self.binary.append(p)
            elif op == 0xA:
                self.pongs.append(p)
        raise AssertionError(f"no '{prefix}' within {timeout} s")

    binary = []
    pongs = []


def main():
    ws = WS()
    greeting = []
    while True:
        op, p = ws.frame()
        assert op == 1, f"unexpected frame {op} in the greeting"
        greeting.append(p.decode())
        if greeting[-1] == "ready;":
            break
    print("greeting:", " ".join(greeting))
    assert greeting[0].startswith("protocol:"), greeting[0]
    assert "device:OpenX6X00;" in greeting
    assert any(g.startswith("vfo:0,0,") for g in greeting)
    assert any(g.startswith("modulation:0,") for g in greeting)
    assert any(g.startswith("rx_filter_band:0,") for g in greeting)

    ws.text("vfo:0,0,14074000;")
    print(ws.wait_text("vfo:0,0,14074000;"))
    ws.text("vfo:0,0,14074000;")                     # the same again: echoed to us
    print(ws.wait_text("vfo:0,0,14074000;"))
    ws.text("modulation:0,usb;")
    print(ws.wait_text("modulation:0,usb;"))
    ws.text("rx_filter_band:0,200,2400;")
    print(ws.wait_text("rx_filter_band:0,200,2400;"))
    ws.text("modulation:0,lsb;")
    print(ws.wait_text("modulation:0,lsb;"))
    print(ws.wait_text("rx_filter_band:0,-2400,-200;"))   # the passband below the carrier
    ws.text("agc_mode:0,slow;")
    print(ws.wait_text("agc_mode:0,slow;"))
    ws.text("agc_mode:0;")
    print(ws.wait_text("agc_mode:0,slow;"))
    ws.text("rit_offset:0,120;")
    print(ws.wait_text("rit_enable:0,true;"))
    print(ws.wait_text("rit_offset:0,120;"))
    ws.text("rit_enable:0,false;")
    print(ws.wait_text("rit_offset:0,0;"))
    ws.text("drive:0,50;")
    print(ws.wait_text("drive:0,50;"))

    ws.text("trx:0,true;")
    print(ws.wait_text("trx:0,true;"))
    ws.text("tx_sensors_enable:true;")
    print(ws.wait_text("tx_sensors:0,"))
    ws.text("trx:0,false;")
    print(ws.wait_text("trx:0,false;"))

    ws.text("rx_sensors_enable:true;")
    print(ws.wait_text("rx_sensors_enable:true;"))
    print(ws.wait_text("rx_smeter:0,0,"))
    print(ws.wait_text("rx_channel_sensors:0,0,"))

    ws.text("audio_samplerate:24000;")
    ws.text("audio_stream_sample_type:int16;")
    ws.text("audio_start:0;")
    print(ws.wait_text("audio_start:0;"))
    end = time.time() + 3
    while not ws.binary and time.time() < end:
        op, p = ws.frame()
        if op == 2:
            ws.binary.append(p)
    assert ws.binary, "no RX audio frame"
    f = ws.binary[0]
    hdr = struct.unpack("<8I", f[:32])
    print(f"audio frame: {len(f)} bytes, rate {hdr[1]}, format {hdr[2]}, length {hdr[5]}, type {hdr[6]}, ch {hdr[7]}")
    assert len(f) == 64 + 1024 and hdr[6] == 1 and hdr[2] == 0 and hdr[5] == 512
    ws.text("audio_stop:0;")
    print(ws.wait_text("audio_stop:0;"))

    # TCI audio transmission: the server asks for frames with TX_CHRONO, we answer with silence
    ws.text("trx:0,true,tci;")
    print(ws.wait_text("trx:0,true;"))
    chronos = 0
    end = time.time() + 1
    while time.time() < end and chronos < 10:
        op, p = ws.frame()
        if op == 2 and struct.unpack("<8I", p[:32])[6] == 3:
            chronos += 1
            hdr = struct.pack("<8I", 0, 24000, 0, 0, 0, 512, 2, 1) + bytes(32)
            ws.send(2, hdr + bytes(1024))
    print(f"chrono frames answered: {chronos}")
    assert chronos >= 5
    ws.text("trx:0,false;")
    print(ws.wait_text("trx:0,false;"))

    ws.send(9, b"hello")
    end = time.time() + 2
    while not ws.pongs and time.time() < end:
        op, p = ws.frame()
        if op == 0xA:
            ws.pongs.append(p)
    assert ws.pongs and ws.pongs[0] == b"hello", "no pong"
    print("pong ok")
    ws.send(8, struct.pack(">H", 1000))
    print("ALL OK")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("FAIL:", e)
        sys.exit(1)
