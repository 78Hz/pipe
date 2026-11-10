#!/usr/bin/env python3
"""Downloads and prepares the measured rooms that ship inside Pipe.

Source: OpenAIR (Audiolab, University of York), each set licensed CC BY 4.0.
The files are fetched from webfiles.york.ac.uk, checked against pinned SHA-256
sums, reduced to horizontal first-order B-format (W, X, Y) at 48 kHz, trimmed
to the end of the measurable decay and normalised so that the direct sound has
unit energy. The engine replaces that direct sound with its own model.

Output (in --out):
  ir_blob.bin   sample data, embedded into the DSP binary
  IrMeta.hpp    names, distances and decay times, used by the DSP and the UI

  python3 tools/prepare_irs.py --out build/irs              # download (CI)
  python3 tools/prepare_irs.py --out build/irs --synthetic  # offline test rooms
  python3 tools/prepare_irs.py --out build/irs --empty      # no measured rooms
"""
import argparse, hashlib, io, os, re, struct, sys, urllib.request

BASE = "https://webfiles.york.ac.uk/OPENAIR/IRs/"

# room index matches RoomPreset in src/engine/Params.hpp
ROOMS = [
    dict(name="ST ANDREW'S LYNDHURST", credit="St Andrew's Church, Lyndhurst - Damian T. Murphy",
         files=[("st-andrews-church/b-format/lyd3_000_bformat_48k.wav", "CENTRE OF NAVE", 11.5,
                 "8da3f2af4fc669b484e25076ca8e14a1e5beede8cdf7c5a938b1e09022608c90")], ab=None),
    dict(name="ST MARGARET'S YORK", credit="St Margaret's Church, York (NCEM) - Aglaia Foteinou, Simon Shelley",
         files=[("st-margarets-church-national-centre-early-music/b-format/r13_3rd_configuration.wav", "FRONT PEWS", None,
                 "b4583182182266db3eee7c25aa8316382b40480530322245d80bfb7929f997db"),
                ("st-margarets-church-national-centre-early-music/b-format/r4_3rd_configuration.wav", "MID NAVE LEFT", 8.5, None),
                ("st-margarets-church-national-centre-early-music/b-format/r5_3rd_configuration.wav", "MID NAVE RIGHT", 8.5, None),
                ("st-margarets-church-national-centre-early-music/b-format/r1_3rd_configuration.wav", "BACK OF NAVE", 11.0, None)],
         ab=(1, 2)),
    dict(name="YORK MINSTER", credit="York Minster - Damian T. Murphy",
         files=[("york-minster/b-format/minster1_bformat_48k.wav", "CENTRE OF NAVE", 23.69,
                 "3c5b080d2bbbb37432ebc41de851af76f720e0df2e4707f9c6664fc9d53542b3")], ab=None),
    dict(name="JACK LYONS HALL", credit="Jack Lyons Concert Hall, University of York",
         files=[("jack-lyons-concert-hall-university-york/b-format/rir_jack_lyons_lp1_96k.wav", "CONDUCTOR", 3.0,
                 "dd4b4b45a7466787fca4076e361b206334f1731c029254494b5254c2847750aa"),
                ("jack-lyons-concert-hall-university-york/b-format/rir_jack_lyons_lp2_96k.wav", "CENTRE SEATS", 10.0,
                 "edc6dd9e978ccdc2ac6719f22498a4e11c1613b76ab631bc01564ade8ab406fc"),
                ("jack-lyons-concert-hall-university-york/b-format/rir_jack_lyons_lp3_96k.wav", "STAGE-RIGHT SEATS", 10.0,
                 "4c07da968bf21f18f23fea040d127353111f85dd50add1210ea7b62e48783e89"),
                ("jack-lyons-concert-hall-university-york/b-format/rir_jack_lyons_lp4_96k.wav", "STAGE-LEFT SEATS", 10.0,
                 "d507b9c2cfefe2563716be99fcdbb5cb49765653e7d450fbdb60c7f956e09521")],
         ab=(2, 3)),
]
FS = 48000


def read_wav(data):
    import numpy as np
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    i, chunks = 12, {}
    while i + 8 <= len(data):
        cid = data[i:i + 4]; sz = struct.unpack("<I", data[i + 4:i + 8])[0]
        chunks[cid] = (i + 8, sz); i += 8 + sz + (sz & 1)
    o, _ = chunks[b"fmt "]
    tag, ch, sr, _, _, bits = struct.unpack("<HHIIHH", data[o:o + 16])
    if tag == 0xFFFE:
        tag = struct.unpack("<H", data[o + 24:o + 26])[0]
    do, dsz = chunks[b"data"]
    raw = data[do:do + dsz]
    n = dsz // (ch * bits // 8)
    if bits == 24:
        a = np.frombuffer(raw[:n * ch * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        x = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
        x = np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23)
    elif bits == 16:
        x = np.frombuffer(raw[:n * ch * 2], dtype="<i2") / 32768.0
    elif bits == 32 and tag == 3:
        x = np.frombuffer(raw[:n * ch * 4], dtype="<f4").astype(np.float64)
    elif bits == 32:
        x = np.frombuffer(raw[:n * ch * 4], dtype="<i4") / 2.0 ** 31
    elif bits == 64:
        x = np.frombuffer(raw[:n * ch * 8], dtype="<f8")
    else:
        raise ValueError(f"unsupported WAV: {bits} bit")
    return sr, x.reshape(-1, ch)


def t30(sig, sr):
    import numpy as np
    e = np.cumsum((sig ** 2)[::-1])[::-1]
    db = 10 * np.log10(e / (e[0] + 1e-30) + 1e-30)
    def at(level):
        idx = np.where(db < level)[0]
        return idx[0] / sr if len(idx) else None
    a, b = at(-5), at(-35)
    if a is None or b is None:
        b = at(-25)
        return 3.0 * (b - a) if (a is not None and b is not None) else 0.0
    return 2.0 * (b - a)


def band(sig, f0, sr):
    from scipy.signal import butter, sosfilt
    sos = butter(4, [f0 / 2 ** 0.5, min(f0 * 2 ** 0.5, sr * 0.45)], btype="band", fs=sr, output="sos")
    return sosfilt(sos, sig)


def prepare(sr, x, distance):
    """-> (W,X,Y float32 array at 48k starting 1 ms before the direct sound, info)"""
    import numpy as np
    from scipy.signal import resample_poly
    if x.shape[1] < 3:
        raise ValueError("expected B-format")
    x = x[:, :3].astype(np.float64)
    if sr != FS:
        from math import gcd
        g = gcd(sr, FS)
        x = resample_poly(x, FS // g, sr // g, axis=0)
    w = x[:, 0]
    onset = int(np.argmax(np.abs(w) > 0.3 * np.max(np.abs(w))))
    start = max(0, onset - FS // 1000)
    x = x[start:]
    w = x[:, 0]
    # end of the measurable decay: where the backwards energy reaches the noise
    tail = w[int(len(w) * 0.9):]
    noise = np.mean(tail ** 2) + 1e-20
    e = np.cumsum((w ** 2)[::-1])[::-1]
    remaining = e - noise * np.arange(len(w), 0, -1)
    rel = remaining / (remaining[0] + 1e-30)
    idx = np.where(rel < 10 ** (-65 / 10))[0]
    end = int(idx[0]) if len(idx) else len(w)
    end = min(len(w), max(end, FS // 4))
    x = x[:end].copy()
    fade = min(len(x) // 4, int(0.05 * FS))
    x[-fade:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, fade)))[:, None]
    # direct sound energy (W, -0.5 ms .. +2.5 ms around the onset) -> 1
    d0 = FS // 1000
    direct = x[max(0, d0 - FS // 2000): d0 + int(0.0025 * FS), 0]
    x /= np.sqrt(np.sum(direct ** 2) + 1e-30)
    lo = t30(band(x[:, 0], 125, FS), FS)
    hi = t30(band(x[:, 0], 2000, FS), FS)
    est = (onset / sr) * 343.0
    return x.astype(np.float32), dict(onset=d0, t30lo=lo, t30hi=hi, distance=distance if distance else est)


def synthetic_room(seconds, rt, distance, seed):
    """B-format-like test response: direct sound + diffuse exponentially decaying noise."""
    import numpy as np
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    env = 10 ** (-3 * t / rt)
    x = rng.standard_normal((n, 3)) * env[:, None] * 0.08
    x[:int(0.004 * FS)] *= 0
    x[FS // 1000, :] = [1 / 2 ** 0.5, 1.0, 0.0]
    return 48000, x, distance


def write_outputs(out, entries, rooms_meta):
    import numpy as np
    os.makedirs(out, exist_ok=True)
    blob = io.BytesIO()
    header_size = 12 + 32 * len(entries)
    blob.write(b"PIPEIR01" + struct.pack("<I", len(entries)))
    offset = header_size
    datas = []
    for e in entries:
        x = e["data"]
        peak = float(np.max(np.abs(x))) or 1.0
        q = np.clip(np.round(x / peak * 32767), -32767, 32767).astype("<i2")
        raw = q.tobytes()
        blob.write(struct.pack("<IIfIIIfI", e["room"], e["pos"], e["distance"], x.shape[0], x.shape[1], e["onset"], peak / 32767.0, offset))
        datas.append(raw)
        offset += len(raw)
    for raw in datas:
        blob.write(raw)
    with open(os.path.join(out, "ir_blob.bin"), "wb") as f:
        f.write(blob.getvalue())
    lines = ["// Generated by tools/prepare_irs.py - do not edit.",
             "// Impulse responses: OpenAIR, Audiolab, University of York (www.openairlib.net), CC BY 4.0.",
             "#pragma once", "namespace pipe { namespace irmeta {",
             "struct Position { int room; int index; const char* name; float distance; float t30Low; float t30High; };",
             f"constexpr int kCount = {len(entries)};",
             "static const Position kPositions[] = {"]
    for e in entries:
        lines.append(f'    {{ {e["room"]}, {e["pos"]}, "{e["label"]}", {e["distance"]:.2f}f, {e["t30lo"]:.2f}f, {e["t30hi"]:.2f}f }},')
    if not entries:
        lines.append('    { -1, -1, "", 0.f, 0.f, 0.f },')
    lines.append("};")
    lines.append("// AB spaced pair (position indices) per measured room, -1 = none")
    lines.append("static const int kAbPair[4][2] = {" + ", ".join(
        "{ %d, %d }" % (r.get("ab") or (-1, -1)) for r in rooms_meta) + " };")
    lines.append("static const char* const kCredits[4] = {" + ", ".join('"%s"' % r["credit"].replace('"', "'") for r in rooms_meta) + " };")
    lines.append("}} // namespace pipe::irmeta")
    with open(os.path.join(out, "IrMeta.hpp"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {len(entries)} positions, blob {offset/1e6:.1f} MB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default=None, help="folder for downloaded files")
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--empty", action="store_true")
    a = ap.parse_args()
    entries, failed = [], []
    if a.empty:
        write_outputs(a.out, [], ROOMS)
        return
    cache = a.cache or os.path.join(a.out, "download")
    os.makedirs(cache, exist_ok=True)
    for ri, room in enumerate(ROOMS):
        for pi, (path, label, dist, sha) in enumerate(room["files"]):
            if a.synthetic:
                rt = [1.4, 1.7, 7.5, 2.0][ri]
                sr, x, dist = synthetic_room(min(9.0, rt * 1.2), rt, dist or 6.0, ri * 10 + pi)
            else:
                local = os.path.join(cache, path.replace("/", "_"))
                try:
                    if not os.path.exists(local):
                        print("download", path, flush=True)
                        with urllib.request.urlopen(BASE + path, timeout=120) as r, open(local + ".part", "wb") as f:
                            f.write(r.read())
                        os.replace(local + ".part", local)
                    data = open(local, "rb").read()
                    digest = hashlib.sha256(data).hexdigest()
                    if sha and digest != sha:
                        os.remove(local)
                        raise ValueError(f"checksum mismatch: {digest}")
                    if not sha:
                        print(f"  (not yet pinned) {path} sha256={digest}")
                    sr, x = read_wav(data)
                except Exception as e:
                    failed.append(path)
                    print(f"::warning::skipping {path}: {e}", flush=True)
                    continue
            y, info = prepare(sr, x, dist)
            print(f"  {room['name']} / {label}: {len(y)/FS:.2f} s, distance {info['distance']:.1f} m, "
                  f"T30 125 Hz {info['t30lo']:.2f} s, 2 kHz {info['t30hi']:.2f} s", flush=True)
            entries.append(dict(room=ri, pos=pi, label=label, data=y, **info))
    if not entries:
        sys.exit("no impulse responses could be prepared")
    if failed:
        print(f"{len(failed)} file(s) skipped", flush=True)
    write_outputs(a.out, entries, ROOMS)


if __name__ == "__main__":
    main()
