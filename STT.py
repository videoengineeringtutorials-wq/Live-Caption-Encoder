
import sounddevice as sd
import whisper
from scipy.io import wavfile
import numpy as np
import argparse
import os
import threading
import queue
import time
import socket
import unicodedata
import warnings

warnings.filterwarnings("ignore", category=UserWarning)

# ------------------------------ CLI ------------------------------
parser = argparse.ArgumentParser(description="parameters for audioWhisper.py")
parser.add_argument('--devices', default='False', type=str, help='print all available devices id')
parser.add_argument('--model', type=str,
                    choices=['tiny', 'tiny.en', 'small', 'small.en', 'medium', 'medium.en', 'large'],
                    default='large', help='Whisper model to use')
parser.add_argument('--task', type=str, choices=['transcribe', 'translate'], default='translate',
                    help='Whisper task: transcribe (same language) or translate (to English)')
parser.add_argument('--device_index', default=1, type=int, help='the id of the input device')
parser.add_argument('--channel', default=1, type=int, help='number of channels for the device')
parser.add_argument('--rate', default=44100, type=int, help="sample rate for recording")
parser.add_argument('--audioseconds', default=5, type=int, help='length of each recorded chunk (seconds)')
parser.add_argument('--audiocounts', default=5, type=int, help='Number of rotating files kept in output_dir')
parser.add_argument('--output_dir', default="audio", type=str, help='directory for saved wav chunks')
parser.add_argument('--condition_on_previous_text', type=bool, default=True,
                    help='Whisper: condition on previous text to reduce hallucinations')
# New: caption transport
parser.add_argument('--cc_host', default='127.0.0.1', type=str, help='cc_injector UDP host')
parser.add_argument('--cc_port', default=54001, type=int, help='cc_injector UDP port')
parser.add_argument('--send_cc', default='True', type=str, help='send caption lines via UDP (True/False)')
parser.add_argument('--print_chunks', default='True', type=str, help='print caption chunks to console (True/False)')
parser.add_argument('--maxlen', default=32, type=int, help='max characters per caption line (CEA-608 cap is 32)')
args = parser.parse_args()

# ------------------------------ Utils ------------------------------
def str2bool(string):
    str2val = {"true": True, "false": False}
    if string.lower() in str2val:
        return str2val[string.lower()]
    else:
        raise ValueError(f"Expected one of {set(str2val.keys())}, got {string}")

SEND_CC = str2bool(args.send_cc)
PRINT_CHUNKS = str2bool(args.print_chunks)

def cea608_sanitize(s: str, limit: int = 32) -> str:
    """
    Strip accents/non-ASCII, keep printable ASCII 0x20..0x7E, collapse controls to space,
    clamp to `limit`.
    """
    if not s:
        return ""
    # Normalize accents → ASCII
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode("ascii", "ignore")
    # Keep printable
    out = []
    for ch in s:
        oc = ord(ch)
        if 0x20 <= oc <= 0x7E:
            out.append(ch)
        elif ch == '\t':
            out.append(' ')
        elif ch in ('\r', '\n'):
            break  # end of line
        # else drop/replace control chars silently
        if len(out) >= limit:
            break
    # Trim spaces
    sanitized = "".join(out).strip()
    if len(sanitized) > limit:
        sanitized = sanitized[:limit]
    return sanitized

def chunk_text_for_captions(text, limit=32):
    """
    Splits text into chunks <= `limit` characters, breaking at spaces when possible.
    """
    words = (text or "").strip().split()
    chunks = []
    current = ""

    for w in words:
        candidate = w if not current else (current + " " + w)
        if len(candidate) > limit:
            if current:
                chunks.append(current)
                current = w
            else:
                # Word itself longer than limit: hard split
                start = 0
                while start < len(w):
                    chunks.append(w[start:start+limit])
                    start += limit
                current = ""
        else:
            current = candidate

    if current:
        chunks.append(current)

    return chunks

# ------------------------------ Audio capture ------------------------------
def record_audio(out_queue, rate, seconds, channels):
    """
    Continuously records audio chunks and places them on a queue.
    """
    if args.device_index is not None:
        sd.default.device = args.device_index

    # Use blocking record per chunk to keep it simple
    while True:
        try:
            recording = sd.rec(frames=rate * seconds, samplerate=rate, channels=channels, dtype=np.float32)
            sd.wait()
            out_queue.put(recording)
        except Exception as e:
            print(f"[record] error: {e}")
            time.sleep(0.5)

# ------------------------------ Processing + UDP send ------------------------------
def process_audio(in_queue, model, task, rate, output_dir, cc_host, cc_port, maxlen):
    """
    Reads audio chunks from queue, writes WAV, runs Whisper, splits into <=32-char chunks,
    prints (optional) and sends each chunk via UDP (optional).
    """
    audio_model = whisper.load_model(model)
    index = 0

    # UDP socket (created once)
    sock = None
    if SEND_CC:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        except Exception as e:
            print(f"[cc] Failed to create UDP socket: {e}")
            sock = None

    while True:
        recording = in_queue.get()
        audio_file_path = f"{output_dir}/audio{index}.wav"

        try:
            wavfile.write(audio_file_path, rate=rate, data=recording)

            result = audio_model.transcribe(
                audio_file_path,
                task=task,
                no_speech_threshold=0.6,
                compression_ratio_threshold=2.0,
                condition_on_previous_text=args.condition_on_previous_text,
                fp16=False  # safer on CPU
            )
            text = (result or {}).get('text', '').strip()

            # Chunk and sanitize for 608
            chunks = chunk_text_for_captions(text, limit=maxlen)
            for c in chunks:
                c608 = cea608_sanitize(c, limit=maxlen)
                if not c608:
                    continue
                if PRINT_CHUNKS:
                    print(c608)
                if SEND_CC and sock is not None:
                    try:
                        # One datagram per line; injector reads the last line
                        payload = (c608 + "\n").encode("ascii", "ignore")
                        sock.sendto(payload, (cc_host, cc_port))
                    except Exception as se:
                        print(f"[cc] send error: {se}")

        except Exception as e:
            print(f"[process] Error: {e}")

        index = (index + 1) % args.audiocounts

def main():
    rate = args.rate
    seconds = args.audioseconds
    channels = args.channel
    output_dir = args.output_dir
    model = args.model
    task = args.task

    os.makedirs(output_dir, exist_ok=True)

    audio_queue = queue.Queue(maxsize=3)
    recording_thread = threading.Thread(
        target=record_audio, args=(audio_queue, rate, seconds, channels), daemon=True
    )
    processing_thread = threading.Thread(
        target=process_audio, args=(audio_queue, model, task, rate, output_dir, args.cc_host, args.cc_port, args.maxlen), daemon=True
    )

    recording_thread.start()
    processing_thread.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping...")

if __name__ == '__main__':
    if str2bool(args.devices) is True:
        print(sd.query_devices())
    else:
        main()

