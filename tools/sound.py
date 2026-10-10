#!/usr/bin/env python3
"""Music and sound effects: sound/*.hps, sound/*.ssm, sound_data.sem.

    python tools/sound.py                       # one line per file
    python tools/sound.py --check               # verify the ADPCM decoder
    python tools/sound.py cubog03.hps --wav build/cubog03.wav --seconds 10
    python tools/sound.py ALWAYS.ssm --wav build/always   # one .wav per sample

Everything is Nintendo DSP-ADPCM, the GameCube's own codec: 8-byte frames
of one header byte (high nibble picks a coefficient pair, low nibble is a
scale) and 14 four-bit samples, decoded against two history samples and a
per-sample table of 16 coefficients.

HPS, the streamed music (73 files, 474 MB, all stereo, 48 kHz but one):
  0x00  " HALPST\\0", u32 sample rate, u32 channel count
  0x10  per channel 0x38: two u32, u32 nibble count, u32, 16 s16
        coefficients, then two u32
  0x80  the first block. A block is
          u32 data size, u32 nibble count, u32 next block offset
          (ABSOLUTE, 0xFFFFFFFF at the end), then per channel
          s16 ps, s16 hist1, s16 hist2, s16 pad, padded to 0x20
        and its data, in which each channel's bytes are CONTIGUOUS
        halves -- not interleaved per frame.
  64 of the 73 point their last block back at an earlier one, which is the
  loop; the other 9 end.

SSM, the sample banks (59 files, 392 samples):
  0x00  u32 data offset RELATIVE TO 0x10, u32 data size, u32 count, u32 0
  0x10  the records, then the shared sample data
  A record is u32 channel count, u32 sample rate, u32 loop flag, and the
  loop start, end and current positions as NIBBLE offsets into that shared
  data, then 16 s16 coefficients and a u32. That is 0x48 bytes for a mono
  sample and another 0x40 per extra channel: 382 of the 392 are mono and
  10 are stereo. Read that way all 59 banks tile exactly from 0x10 to the
  data, which is what confirms the layout.

The decoder is checked against the files themselves: every HPS block
stores the decoder history it should start from, so decoding one block and
comparing with the next block's stored history either matches exactly or
does not. See --check.

For the 3DS: libctru's NDSP takes this codec as-is -- NDSP_ENCODING_ADPCM
is documented in channel.h as "DSPADPCM (GameCube format)" and
ndspChnSetAdpcmCoefs() wants exactly the 16 coefficients these files
carry. So converting is repackaging, not re-encoding, and nothing is lost.
The catch is that ADPCM there is MONO ONLY: channel.h has
NDSP_FORMAT_MONO_ADPCM but no stereo equivalent, so a stereo stream needs
two NDSP channels panned apart. That suits HPS, whose blocks already hold
each channel's bytes separately.

sound_data.sem is the sound-effect map, 1232 u32: a table of ids followed
by pairs that look like (bank << 24 | sample, flags). Not decoded -- what
it indexes needs the DOL, and nothing needs it until sound is wired up.
"""
import argparse, struct, sys, wave
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).parent))
from models import FILES

SOUND = FILES / 'sound'
HPS_MAGIC = b' HALPST\0'

def decode_dsp(data, coefs, hist1=0, hist2=0):
    """DSP-ADPCM bytes -> (int16 samples, hist1, hist2)."""
    out = np.empty(len(data) // 8 * 14, np.int16)
    i = 0
    for k in range(0, len(data) - 7, 8):
        ps = data[k]
        idx = ((ps >> 4) & 7) * 2
        scale, c1, c2 = 1 << (ps & 0xF), coefs[idx], coefs[idx + 1]
        for nib in range(14):
            b = data[k + 1 + nib // 2]
            s = (b >> 4) if nib % 2 == 0 else (b & 0xF)
            if s > 7:
                s -= 16
            v = (((s * scale) << 11) + 1024 + c1 * hist1 + c2 * hist2) >> 11
            v = -32768 if v < -32768 else (32767 if v > 32767 else v)
            out[i] = v
            i += 1
            hist2, hist1 = hist1, v
    return out[:i], hist1, hist2

class Hps:
    def __init__(self, blob):
        if blob[:8] != HPS_MAGIC:
            raise ValueError('not an HPS stream')
        self.blob = blob
        self.rate, self.channels = struct.unpack_from('>2I', blob, 8)
        self.coefs = [list(struct.unpack_from('>16h', blob, 0x10 + 0x38 * c + 0x10))
                      for c in range(self.channels)]

    def blocks(self):
        """(offset, data size, next offset, [(ps, hist1, hist2) per channel])."""
        at, seen = 0x80, set()
        while at not in seen and at + 0x20 <= len(self.blob):
            seen.add(at)
            size, _, nxt = struct.unpack_from('>3I', self.blob, at)
            state = [struct.unpack_from('>4h', self.blob, at + 0x0C + 8 * c)[:3]
                     for c in range(self.channels)]
            yield at, size, nxt, state
            if nxt == 0xFFFFFFFF or nxt <= at:
                return
            at = nxt

    def _walk(self, max_blocks, keep):
        """Shared by decode() and check(): decode blocks, tracking history."""
        per = [[] for _ in range(self.channels)]
        hist, checked = [(0, 0)] * self.channels, [0, 0]
        for i, (at, size, nxt, state) in enumerate(self.blocks()):
            if max_blocks is not None and i >= max_blocks:
                break
            if i:
                for c in range(self.channels):
                    checked[(state[c][1], state[c][2]) != hist[c]] += 1
            data = self.blob[at + 0x20:at + 0x20 + size]
            half = size // self.channels
            for c in range(self.channels):
                s, h1, h2 = decode_dsp(data[c * half:(c + 1) * half], self.coefs[c],
                                       state[c][1], state[c][2])
                hist[c] = (h1, h2)
                if keep:
                    per[c].append(s)
        return per, checked

    def decode(self, max_blocks=None):
        """Interleaved int16 samples, shape (frames, channels)."""
        per, _ = self._walk(max_blocks, True)
        n = min(sum(len(x) for x in p) for p in per)
        return np.stack([np.concatenate(p)[:n] for p in per], 1)

    def check(self, max_blocks=None):
        """(matches, mismatches) of our decoder's history against the file's."""
        _, checked = self._walk(max_blocks, False)
        return checked[0], checked[1]

class Ssm:
    def __init__(self, blob):
        self.blob = blob
        off, self.data_size, count, _ = struct.unpack_from('>4I', blob, 0)
        self.data = 0x10 + off
        self.samples, at = [], 0x10
        for _ in range(count):
            ch, rate, loop, start, end, cur = struct.unpack_from('>6I', blob, at)
            coefs = [list(struct.unpack_from('>16h', blob, at + 0x18 + 0x40 * c))
                     for c in range(ch)]
            self.samples.append({'channels': ch, 'rate': rate, 'loop': loop,
                                 'start': start, 'end': end, 'coefs': coefs})
            at += 0x48 + 0x40 * (ch - 1)
        if at != self.data:
            raise ValueError(f'records end at {at:#x}, data starts at {self.data:#x}')

    def decode(self, i):
        s = self.samples[i]
        # start and end are NIBBLE addresses, and every start is 2 mod 16: the
        # frame's header byte is nibbles 0-1, so the first sample nibble is 2.
        # Slicing at start//2 would cut that header off and decode garbage.
        # start/end describe channel 0 only. A second channel follows it
        # immediately, the same length again: for 8 of the 10 stereo samples
        # lo + 2*len lands exactly on the next sample's start.
        lo = self.data + (s['start'] // 16) * 8
        hi = self.data + (s['end'] // 16 + 1) * 8
        size = hi - lo
        chans = [decode_dsp(self.blob[lo + c * size:lo + (c + 1) * size], s['coefs'][c])[0]
                 for c in range(s['channels'])]
        n = min(len(c) for c in chans)
        return np.stack([c[:n] for c in chans], 1), s['rate']

def write_wav(path, samples, rate):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(samples.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.astype('<i2').tobytes())

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('name', nargs='?', help='a file in sound/, e.g. cubog03.hps')
    ap.add_argument('--wav', help='decode to this .wav (a prefix, for a bank)')
    ap.add_argument('--seconds', type=float, help='how much of a stream to decode')
    ap.add_argument('--check', action='store_true', help='verify the decoder on every stream')
    ap.add_argument('--blocks', type=int, default=4, help='blocks per stream for --check')
    args = ap.parse_args()
    if args.check:
        ok = bad = 0
        for p in sorted(SOUND.glob('*.hps')):
            a, b = Hps(p.read_bytes()).check(max_blocks=args.blocks)
            ok += a
            bad += b
            if b:
                print(f'  {p.name}: {b} mismatches')
        print(f'block-history checks: {ok} matched, {bad} mismatched')
        return
    names = [args.name] if args.name else [p.name for p in sorted(SOUND.iterdir())]
    for name in names:
        p = SOUND / name
        if p.suffix == '.hps':
            h = Hps(p.read_bytes())
            blocks = list(h.blocks())
            loops = blocks[-1][2] != 0xFFFFFFFF
            print(f'{name:22s} stream  {h.rate:5d} Hz  {h.channels}ch  {len(blocks):4d} blocks'
                  f'  {"loops" if loops else "one-shot"}')
            if args.wav:
                n = None if args.seconds is None else max(1, int(args.seconds * h.rate / 57344) + 1)
                s = h.decode(max_blocks=n)
                if args.seconds:
                    s = s[:int(args.seconds * h.rate)]
                write_wav(args.wav, s, h.rate)
                print('   wrote', args.wav, s.shape)
        elif p.suffix == '.ssm':
            b = Ssm(p.read_bytes())
            rates = sorted({s['rate'] for s in b.samples})
            stereo = sum(s['channels'] > 1 for s in b.samples)
            print(f'{name:22s} bank    {len(b.samples):4d} samples  {stereo} stereo  rates {rates}')
            if args.wav:
                for i in range(len(b.samples)):
                    s, rate = b.decode(i)
                    write_wav(f'{args.wav}_{i:03d}.wav', s, rate)
                print('   wrote', len(b.samples), 'wavs')
        else:
            print(f'{name:22s} {p.stat().st_size} bytes (sound-effect map, not decoded)')

if __name__ == '__main__':
    main()
