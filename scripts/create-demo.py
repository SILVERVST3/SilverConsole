"""Gera uma sessao de demonstracao original (6 faixas sintetizadas, 8 s) para o SilverConsole.

Uso: python scripts/create-demo.py [pasta_destino]
Somente biblioteca padrao do Python (roda no GitHub Actions sem instalar nada).
"""
import math
import random
import struct
import sys
import wave
from pathlib import Path

SR = 44100
SECONDS = 8
N = SR * SECONDS


def save(path: Path, left, right=None):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1 if right is None else 2)
        w.setsampwidth(2)
        w.setframerate(SR)
        frames = bytearray()
        for i in range(N):
            for v in ((left[i],) if right is None else (left[i], right[i])):
                frames += struct.pack("<h", int(max(-1.0, min(1.0, v)) * 32767))
        w.writeframes(bytes(frames))


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "Demo")
    out.mkdir(parents=True, exist_ok=True)
    beat = SR // 2
    rnd = random.Random(1)

    kick = [0.0] * N
    for s in range(0, N, beat):
        for k in range(min(6000, N - s)):
            t = k / SR
            kick[s + k] += 0.9 * math.sin(2 * math.pi * (50 + 80 * math.exp(-t * 40)) * t) * math.exp(-t * 9)
    save(out / "Bumbo.wav", kick)

    hat = [0.0] * N
    for s in range(beat // 2, N, beat // 2):
        for k in range(min(2500, N - s)):
            hat[s + k] += 0.25 * rnd.gauss(0, 1) * math.exp(-k / SR * 60)
    save(out / "Chimbal.wav", [v * 0.8 for v in hat], hat)

    notes = [55.0, 55.0, 65.41, 49.0]
    bass = [0.5 * math.tanh(2 * math.sin(2 * math.pi * notes[(i // (2 * SR)) % 4] * i / SR)) for i in range(N)]
    save(out / "Baixo.wav", bass)

    chord = [(220.0, 0.0), (277.18, 1.0), (329.63, 2.0), (440.0, 3.0)]
    pad = [sum(0.12 * math.sin(2 * math.pi * f * i / SR + ph) for f, ph in chord) for i in range(N)]
    save(out / "Pad.wav", [p * (0.8 + 0.2 * math.sin(i / SR)) for i, p in enumerate(pad)],
         [p * (0.8 + 0.2 * math.cos(i / SR)) for i, p in enumerate(pad)])

    lead = []
    for i in range(N):
        t = i / SR
        sq = 1.0 if math.sin(2 * math.pi * (440 + 20 * math.sin(2 * math.pi * 5 * t)) * t) >= 0 else -1.0
        lead.append(0.18 * sq * (0.5 + 0.5 * math.sin(2 * math.pi * 0.5 * t)))
    save(out / "Guitarra.wav", lead)

    voice = [0.35 * math.sin(2 * math.pi * 330 * i / SR + 3 * math.sin(2 * math.pi * 2 * i / SR))
             * (1.0 if math.sin(2 * math.pi * 0.25 * i / SR) > 0 else 0.0) for i in range(N)]
    save(out / "Voz.wav", voice)

    tracks = [
        ("Bumbo", "ff5ec8ff", -2, 0.0, 0, []),
        ("Chimbal", "fff2e05a", -8, 0.3, 0, []),
        ("Baixo", "ff7be08f", -4, 0.0, -1, []),
        ("Pad", "ffb48cff", -10, 0.0, -1, [(0, 1, -6, 0)]),
        ("Guitarra", "ffffb74d", -9, -0.45, -1, [(0, 1, -12, 0)]),
        ("Voz", "ffff7a8a", -3, 0.0, -1, [(0, 1, -9, 0), (1, 2, -14, 1)]),
    ]
    xml = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<SILVERCONSOLE version="1" loopOn="0" loopStart="2" loopEnd="6">']
    for name, colour, gain, pan, out_bus, sends in tracks:
        s = "".join(f'<SEND index="{i}" bus="{b}" levelDb="{l}" pre="{p}"/>' for i, b, l, p in sends)
        xml.append(f'  <TRACK name="{name}" colour="{colour}" gainDb="{gain}" pan="{pan}" mute="0" solo="0" phase="0" '
                   f'file="{name}.wav" relFile="{name}.wav" output="{out_bus}">{s}</TRACK>')
    for name, gain in (("Bateria", -1), ("Reverb", -4), ("Delay", -6)):
        xml.append(f'  <BUS name="{name}" colour="ffffc46b" gainDb="{gain}" pan="0" mute="0" solo="0" phase="0"/>')
    xml.append('  <MASTER name="MASTER" colour="ffe8edf2" gainDb="-1.5" pan="0" mute="0" solo="0" phase="0" mono="0"/>')
    xml.append('</SILVERCONSOLE>')
    (out / "Demo.sconsole").write_text("\n".join(xml) + "\n", encoding="utf-8")
    print(f"Demo criada em {out.resolve()}")


if __name__ == "__main__":
    main()
