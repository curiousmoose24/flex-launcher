#!/usr/bin/env python3
"""Generate the default navigation sounds in assets/sounds.

The sounds are synthesized here (no samples), so they are covered by the
project's license. Run from the repository root: python3 tools/generate_sounds.py
"""
import math
import os
import struct
import wave

RATE = 48000


def envelope(t, attack, decay):
    """Short linear attack followed by an exponential decay."""
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / decay)


def tone(freq, t, harmonics=((1.0, 1.0),)):
    return sum(amp * math.sin(2 * math.pi * freq * mult * t) for mult, amp in harmonics)


def move(t):
    # Soft, short tick
    return envelope(t, 0.002, 0.009) * tone(1800, t, ((1.0, 1.0), (2.0, 0.25)))


def select(t):
    # Two-note rising chime
    bell = ((1.0, 1.0), (2.0, 0.3), (3.0, 0.1))
    first = envelope(t, 0.003, 0.06) * tone(880.0, t, bell)
    second = envelope(t - 0.07, 0.003, 0.09) * tone(1318.5, t, bell) if t >= 0.07 else 0.0
    return first + second


def off(t):
    # Two-note falling chime, the reverse of select (played when sounds are turned off)
    bell = ((1.0, 1.0), (2.0, 0.3), (3.0, 0.1))
    first = envelope(t, 0.003, 0.06) * tone(1318.5, t, bell)
    second = envelope(t - 0.07, 0.003, 0.09) * tone(880.0, t, bell) if t >= 0.07 else 0.0
    return first + second


def back(t):
    # Short falling tone
    duration = 0.12
    freq = 1046.5 - (1046.5 - 659.3) * min(t / duration, 1.0)
    phase = 2 * math.pi * (1046.5 * t - (1046.5 - 659.3) * min(t, duration) ** 2 / (2 * duration))
    return envelope(t, 0.003, 0.05) * (math.sin(phase) + 0.2 * math.sin(2 * phase))


SOUNDS = {'move': (move, 0.06), 'select': (select, 0.45), 'back': (back, 0.25), 'off': (off, 0.45)}


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'sounds')
    os.makedirs(out_dir, exist_ok=True)
    for name, (func, length) in SOUNDS.items():
        samples = [func(i / RATE) for i in range(int(length * RATE))]
        peak = max(abs(s) for s in samples) or 1.0
        data = b''.join(struct.pack('<h', int(s / peak * 0.6 * 32767)) for s in samples)
        with wave.open(os.path.join(out_dir, f'{name}.wav'), 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(data)
        print(f'{name}.wav: {length * 1000:.0f} ms')


if __name__ == '__main__':
    main()
