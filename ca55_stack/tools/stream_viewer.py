#!/usr/bin/env python3
"""
AI Camera Stream Viewer
=======================
Receives JPEG-over-UDP stream from RZ/V2H board and displays with OpenCV.

The board sends JPEG frames fragmented into UDP chunks with a 10-byte header:
  - uint32 frame_id
  - uint16 chunk_index
  - uint16 total_chunks
  - uint16 chunk_size
  - [payload: up to 1400 bytes]

Usage:
  python3 stream_viewer.py [--board <ip>] [--port 50002]
  # If --board is omitted, uses env RDK_IP or BOARD_IP (fallback: 192.168.1.140)

Controls:
  q     - Quit
  s     - Save screenshot
  +/-   - Zoom in/out
"""
import socket
import struct
import time
import sys
import argparse
import threading
import os

import cv2
import numpy as np

# Must match ChunkHeader in image_streamer.cpp
HEADER_FMT = '<IHHH'  # frame_id(u32), chunk_index(u16), total_chunks(u16), chunk_size(u16)
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 10 bytes
MAX_PACKET = 1500


class FrameAssembler:
    """Reassembles fragmented JPEG frames from UDP chunks."""

    def __init__(self, timeout=0.5):
        self.frames = {}           # frame_id -> {chunks: dict, total: int, ts: float}
        self.timeout = timeout
        self.last_completed_id = -1

    def add_chunk(self, frame_id, chunk_index, total_chunks, chunk_data):
        """Add a chunk. Returns complete JPEG bytes if frame is now complete, else None."""
        # Discard stale frames
        if frame_id <= self.last_completed_id:
            return None

        if frame_id not in self.frames:
            self.frames[frame_id] = {
                'chunks': {},
                'total': total_chunks,
                'ts': time.time()
            }

        entry = self.frames[frame_id]
        entry['chunks'][chunk_index] = chunk_data

        # Check if all chunks received
        if len(entry['chunks']) == entry['total']:
            # Reassemble in order
            jpeg_data = b''.join(entry['chunks'][i] for i in range(entry['total']))
            self.last_completed_id = frame_id

            # Cleanup this and older frames
            stale = [fid for fid in self.frames if fid <= frame_id]
            for fid in stale:
                del self.frames[fid]
            return jpeg_data

        # Garbage collect timed-out incomplete frames
        now = time.time()
        stale = [fid for fid, e in self.frames.items()
                 if now - e['ts'] > self.timeout]
        for fid in stale:
            del self.frames[fid]

        return None


def start_heartbeat(board_ip, board_port, interval=2.0):
    """Send periodic heartbeat to board so it learns our IP (unicast > broadcast).

    The board's hil_udp_receive_thread listens on port 50001. When it
    receives a packet from a non-local IP, it stores that IP in
    learned_host_ip and switches the image streamer from broadcast to
    unicast — dramatically reducing UDP packet loss.
    """
    hb_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    hb_msg = b'STREAM_VIEWER_HEARTBEAT'

    def _loop():
        while True:
            try:
                hb_sock.sendto(hb_msg, (board_ip, board_port))
            except Exception:
                pass
            time.sleep(interval)

    t = threading.Thread(target=_loop, daemon=True)
    t.start()
    return t


def main():
    default_board_ip = os.getenv('RDK_IP') or os.getenv('BOARD_IP') or '192.168.1.140'

    parser = argparse.ArgumentParser(
        description='AI Camera Stream Viewer - receives JPEG-over-UDP from RZ/V2H board')
    parser.add_argument('--port', type=int, default=50002,
                        help='UDP port to listen on (default: 50002)')
    parser.add_argument('--board', type=str, default=default_board_ip,
                        help=f'Board IP for heartbeat registration (default: {default_board_ip})')
    args = parser.parse_args()

    # Send heartbeat so board learns our IP and switches to unicast
    start_heartbeat(args.board, 50001, interval=2.0)
    print(f"[Stream Viewer] Sending heartbeat to {args.board}:50001 (IP registration)")

    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Increase receive buffer to reduce packet drops
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(('0.0.0.0', args.port))
    sock.settimeout(2.0)

    print(f"[Stream Viewer] Listening on UDP port {args.port}")
    print(f"[Stream Viewer] Press 'q' to quit, 's' to save screenshot")

    assembler = FrameAssembler()

    # FPS tracking
    fps_counter = 0
    fps_timer = time.time()
    display_fps = 0.0
    frame_count = 0
    total_bytes = 0
    scale = 1.0

    while True:
        try:
            data, addr = sock.recvfrom(MAX_PACKET)
        except socket.timeout:
            # Show waiting message
            blank = np.zeros((480, 640, 3), dtype=np.uint8)
            cv2.putText(blank, "Waiting for stream...", (140, 230),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 255), 2)
            cv2.putText(blank, f"Listening on UDP port {args.port}", (160, 270),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (128, 128, 128), 1)
            cv2.imshow('AI Camera Stream', blank)
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
            # Detect window closed via X button
            if cv2.getWindowProperty('AI Camera Stream', cv2.WND_PROP_VISIBLE) < 1:
                break
            continue
        except KeyboardInterrupt:
            break

        if len(data) < HEADER_SIZE:
            continue

        # Parse chunk header
        frame_id, chunk_idx, total_chunks, chunk_size = struct.unpack(
            HEADER_FMT, data[:HEADER_SIZE])
        chunk_data = data[HEADER_SIZE:HEADER_SIZE + chunk_size]

        total_bytes += len(data)

        # Try to assemble complete frame
        jpeg_data = assembler.add_chunk(frame_id, chunk_idx, total_chunks, chunk_data)
        if jpeg_data is None:
            continue

        # Decode JPEG
        np_arr = np.frombuffer(jpeg_data, dtype=np.uint8)
        frame = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)
        if frame is None:
            continue

        # FPS calculation
        fps_counter += 1
        elapsed = time.time() - fps_timer
        if elapsed >= 1.0:
            display_fps = fps_counter / elapsed
            fps_counter = 0
            fps_timer = time.time()

        frame_count += 1

        # Draw receive stats overlay (bottom bar)
        h, w = frame.shape[:2]
        cv2.rectangle(frame, (0, h - 25), (w, h), (0, 0, 0), -1)
        stats_text = (f"RX: {display_fps:.1f} FPS | "
                      f"Frame #{frame_id} | "
                      f"JPEG: {len(jpeg_data) / 1024:.1f} KB | "
                      f"From: {addr[0]}")
        cv2.putText(frame, stats_text, (5, h - 7),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 255, 0), 1)

        # Scale if needed
        if scale != 1.0:
            new_w = int(w * scale)
            new_h = int(h * scale)
            frame = cv2.resize(frame, (new_w, new_h))

        # Update window title with FPS
        window_title = f"AI Camera Stream [{display_fps:.1f} FPS]"
        cv2.imshow('AI Camera Stream', frame)
        cv2.setWindowTitle('AI Camera Stream', window_title)

        # Handle keyboard and window-close (X button)
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):
            break
        if cv2.getWindowProperty('AI Camera Stream', cv2.WND_PROP_VISIBLE) < 1:
            break
        elif key == ord('s'):
            filename = f"stream_screenshot_{frame_count:05d}.jpg"
            cv2.imwrite(filename, frame)
            print(f"[Stream Viewer] Saved {filename}")
        elif key == ord('+') or key == ord('='):
            scale = min(scale + 0.25, 3.0)
            print(f"[Stream Viewer] Scale: {scale:.2f}x")
        elif key == ord('-'):
            scale = max(scale - 0.25, 0.25)
            print(f"[Stream Viewer] Scale: {scale:.2f}x")

    sock.close()
    cv2.destroyAllWindows()
    print(f"\n[Stream Viewer] Session stats:")
    print(f"  Frames received: {frame_count}")
    print(f"  Total data: {total_bytes / 1024 / 1024:.1f} MB")


if __name__ == '__main__':
    main()
