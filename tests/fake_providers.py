#!/usr/bin/env python3
"""Stand-ins for the AI models chasm calls, so a real chasm backend can run a
whole turn on a machine without a GPU or API keys (tests/run_e2e.py):

  POST /v1/chat/completions            the LLM (chasm's "openai_compat" provider)
  POST /v1/audio/transcriptions        speech recognition (chasm's "openai" STT provider)
  POST /v1/text-to-speech/<voice>      speech synthesis (chasm's "elevenlabs" TTS provider):
                                       a tone as 24 kHz PCM16, as ElevenLabs answers

The LLM answers in chasm's structured format ({"speech": ..., "actions": [...]}),
choosing the follow action when the player asked to be followed. Every request
is appended to the log file (--log) as a JSON line, for the test to check what
chasm sent.

    python tests/fake_providers.py --port 5099 --log /tmp/providers.jsonl
"""

import argparse
import json
import math
import re
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TRANSCRIPT = "Johnson, follow me."  # (what the microphone "said")


def tone_pcm(text: str, rate: int = 24000) -> bytes:
    """About 60 ms a word, at least half a second: a tone that rises and falls."""
    seconds = max(0.5, 0.06 * len(text.split()))
    count = int(seconds * rate)
    samples = (int(9000 * math.sin(2 * math.pi * (180 + 40 * math.sin(i / 2400)) * i / rate)) for i in range(count))
    return b"".join(struct.pack("<h", s) for s in samples)


class Handler(BaseHTTPRequestHandler):
    log_path = None
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def record(self, body):
        with self.lock, open(self.log_path, "a", encoding="utf-8") as log:
            log.write(json.dumps({"path": self.path, "body": body}) + "\n")

    def read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length) if length else b""

    def reply(self, status: int, body: bytes, kind: str):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.rstrip("/").endswith("/models"):
            self.reply(200, json.dumps({"data": [{"id": "fake"}]}).encode(), "application/json")
        else:
            self.reply(404, b"{}", "application/json")

    def do_POST(self):
        raw = self.read_body()
        if self.path.endswith("/audio/transcriptions"):
            self.record({"bytes": len(raw), "wav": b"RIFF" in raw})
            self.reply(200, json.dumps({"text": TRANSCRIPT}).encode(), "application/json")
            return
        if "/text-to-speech/" in self.path:
            body = json.loads(raw or b"{}")
            self.record(body)
            self.reply(200, tone_pcm(body.get("text", "")), "audio/pcm")
            return
        if self.path.endswith("/chat/completions"):
            body = json.loads(raw or b"{}")
            self.record(body)
            self.chat(body)
            return
        self.reply(404, b"{}", "application/json")

    def chat(self, body):
        messages = body.get("messages", [])
        last_user = next((m.get("content", "") for m in reversed(messages) if m.get("role") == "user"), "")
        if isinstance(last_user, list):
            last_user = " ".join(part.get("text", "") for part in last_user if isinstance(part, dict))
        system = " ".join(str(m.get("content", "")) for m in messages if m.get("role") == "system")
        wants_structure = "speech" in json.dumps(body.get("response_format", "")) or '"speech"' in system or "speech" in system
        # (what was asked, as an action a character found and chose)
        orders = [
            (r"follow", "On your six, Chief. Marines, move it!", "movement.follow_target"),
            (r"hold|stay here", "Holding here, Chief. Nobody gets past us.", "ai.wait_here"),
            (r"warthog|get in", "Mounting up!", "halo.enter_vehicle"),
            (r"charge", "Marines! Charge!", "halo.berserk"),
            (r"weapon|gun", "Here, take it.", "world.give_item"),
            (r"stand down|truce", "Okay okay, me no shoot!", "combat.stop"),
        ]
        speech, actions = "Copy that, Chief.", []
        for pattern, line, action_id in orders:
            if re.search(pattern, last_user, re.I):
                speech = line
                actions = [{"id": action_id, "target": "player", "confidence": 0.93, "reason": f"Asked to: {last_user}"}]
                break
        content = json.dumps({"speech": speech, "actions": actions}) if wants_structure else speech
        if body.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            created = int(time.time())
            pieces = [content[i:i + 12] for i in range(0, len(content), 12)]
            for piece in pieces:
                chunk = {"id": "fake", "object": "chat.completion.chunk", "created": created, "model": "fake",
                         "choices": [{"index": 0, "delta": {"content": piece}, "finish_reason": None}]}
                self.wfile.write(f"data: {json.dumps(chunk)}\n\n".encode())
                self.wfile.flush()
            end = {"id": "fake", "object": "chat.completion.chunk", "created": created, "model": "fake",
                   "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                   "usage": {"prompt_tokens": 100, "completion_tokens": 20, "total_tokens": 120}}
            self.wfile.write(f"data: {json.dumps(end)}\n\ndata: [DONE]\n\n".encode())
            self.wfile.flush()
            return
        answer = {"id": "fake", "object": "chat.completion", "created": int(time.time()), "model": "fake",
                  "choices": [{"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": "stop"}],
                  "usage": {"prompt_tokens": 100, "completion_tokens": 20, "total_tokens": 120}}
        self.reply(200, json.dumps(answer).encode(), "application/json")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=5099)
    parser.add_argument("--log", required=True)
    arguments = parser.parse_args()
    Handler.log_path = arguments.log
    open(arguments.log, "w").close()
    server = ThreadingHTTPServer(("127.0.0.1", arguments.port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
