#!/usr/bin/env python3
"""ppmdiff.py a.ppm b.ppm -> maximale Abweichung je Kanal und Anteil abweichender Pixel"""
import sys

def load(p):
    data = open(p, 'rb').read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos+1].isspace():
            pos += 1
        if data[pos:pos+1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end+1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + w * h * 3]

wa, ha, a = load(sys.argv[1])
wb, hb, b = load(sys.argv[2])
if (wa, ha) != (wb, hb):
    print('Größe verschieden'); sys.exit(1)
mx, npix = 0, 0
for i in range(0, len(a), 3):
    d = max(abs(a[i] - b[i]), abs(a[i+1] - b[i+1]), abs(a[i+2] - b[i+2]))
    if d:
        npix += 1
        mx = max(mx, d)
print(f'max {mx}, {npix} von {wa*ha} Pixeln ({100.0*npix/(wa*ha):.2f} %)')
