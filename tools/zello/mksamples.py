#!/usr/bin/env python3
"""Makes the saved samples of tools/zello/samples, which zello_test.py reads; run once, the files are kept in git so
the test reads the same bytes on every PC (a new libopus could encode a tone a little differently):

    python3 -I -B mksamples.py

- channel_events.jsonl: what Zello's server says on a channel, a JSON message a line, in the shapes of Zello's
  Channel API (API.md): the logon's answer, the channel's state, two talkers one over the other, a picture (not audio,
  left alone), an error, the channel going offline. Made up here, never taken from zello.io.
- audio_frame.bin: one binary frame as the server sends it, {type 1, stream_id 7, packet_id 42, an Opus packet of
  20 ms of a 440 Hz tone at 16 kHz}, from the PC's libopus.
- server_stream.bin: the bytes of a websocket from a server: a text frame, a text message in three fragments with a
  ping between them, the audio frame (a 16-bit length), a pong, a close (1000, "bye").
"""
import base64
import ctypes
import ctypes.util
import json
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "samples")

EVENTS = [
    {"seq": 1, "success": True, "refresh_token": "sample-refresh-token"},
    {"command": "on_channel_status", "channel": "Test channel", "status": "online", "users_online": 3,
     "images_supported": True, "texting_supported": True, "locations_supported": True},
    {"command": "on_stream_start", "type": "audio", "codec": "opus", "codec_header": "gD4BPA==",
     "packet_duration": 60, "stream_id": 7, "channel": "Test channel", "from": "alice"},
    {"command": "on_stream_start", "type": "audio", "codec": "opus", "codec_header": "gD4BFA==",
     "packet_duration": 20, "stream_id": 8, "channel": "Test channel", "from": "bob"},
    {"command": "on_stream_stop", "stream_id": 7},
    {"command": "on_stream_stop", "stream_id": 8},
    {"command": "on_stream_stop", "stream_id": 99},
    {"command": "on_image", "channel": "Test channel", "from": "carol", "message_id": 5, "type": "jpeg",
     "height": 100, "width": 100, "source": "camera"},
    {"command": "on_stream_start", "type": "image", "stream_id": 9, "from": "carol"},
    {"command": "on_error", "error": "channel is not ready"},
    {"command": "on_channel_status", "channel": "Test channel", "status": "offline"},
]


def opus_tone():
    lib = ctypes.CDLL(ctypes.util.find_library("opus") or "libopus.so.0")
    lib.opus_encoder_create.restype = ctypes.c_void_p
    lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_char_p,
                                ctypes.c_int32]
    err = ctypes.c_int()
    enc = lib.opus_encoder_create(16000, 1, 2048, ctypes.byref(err))
    out = ctypes.create_string_buffer(1500)
    pkt = b""
    for k in range(5):      # the fifth frame: the encoder settled
        pcm = (ctypes.c_int16 * 320)(*[int(8000 * math.sin(2 * math.pi * 440 * (k * 320 + i) / 16000))
                                       for i in range(320)])
        n = lib.opus_encode(enc, pcm, 320, out, 1500)
        pkt = out.raw[:n]
    return pkt


def frame(op, payload, fin=True):
    n = len(payload)
    head = bytes([(0x80 if fin else 0) | op])
    head += bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n)
    return head + payload


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "channel_events.jsonl"), "w") as f:
        for m in EVENTS:
            f.write(json.dumps(m, separators=(",", ":")) + "\n")
    audio = struct.pack(">BII", 1, 7, 42) + opus_tone()
    with open(os.path.join(OUT, "audio_frame.bin"), "wb") as f:
        f.write(audio)
    status = json.dumps(EVENTS[1], separators=(",", ":")).encode()
    start = json.dumps(EVENTS[2], separators=(",", ":")).encode()
    padded = audio + b"\x00" * max(0, 130 - len(audio))     # past 125 bytes: the 16-bit length
    stream = (frame(1, status) + frame(1, start[:10], fin=False) + frame(9, b"ping!") +
              frame(0, start[10:30], fin=False) + frame(0, start[30:]) + frame(2, padded) + frame(10, b"pong") +
              frame(8, struct.pack(">H", 1000) + b"bye"))
    with open(os.path.join(OUT, "server_stream.bin"), "wb") as f:
        f.write(stream)
    print("audio frame", len(audio), "bytes, stream", len(stream), "bytes; codec headers",
          base64.b64decode("gD4BPA==").hex(), base64.b64decode("gD4BFA==").hex())


if __name__ == "__main__":
    main()
