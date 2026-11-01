"""Decrypts (and re-encrypts) FM2 profile save files: ForzaProfile, PlayerDatabase and the other secure streams.

  savecrypt.py decrypt <file> <xuid hex> [out]     -> the plain data (checks the HMAC first)
  savecrypt.py encrypt <plain> <xuid hex> <out>    -> a file the game accepts for that XUID
  savecrypt.py list <ForzaProfile> <xuid>                    -> every value: section/name type value
  savecrypt.py set <ForzaProfile> <xuid> <Section/Name> <value>   -> changes one value in place
  savecrypt.py unpack <PlayerDatabase> <xuid> [out.sqlite]   -> the career as a SQLite database
  savecrypt.py pack <in.sqlite> <xuid> <PlayerDatabase>      -> back into a save the game reads

Worked out from the decompiled game (TU4 addresses):
  IOSys::CSecurityStream (82451F58 writer, 82451C10 reader, 82452080 writes on close) keeps a 40-byte header
  then the AES-CBC body:
     0..19   HMAC-SHA1 of header[20..40] + body
     20..23  plain length XOR the four IV words (big endian)
     24..39  IV (random per save)
  The keys come from the profile's XUID, 16 bytes each, constant[i] ^ xuid[(i + shift) & 7]:
     ForzaProfile (82232A00)     AES 820076A0 shift 1,  HMAC 820076B0 shift 0
     PlayerDatabase              AES 820076C0 shift 3,  HMAC 820076D0 shift 2
  PlayerDatabase's plain data is then [u32 size][u32 packed size][u32 size] (big endian) and the SQLite
  database as raw deflate (IOSys::CCompressionStream).
  (the console's XeKeys calls only obscure them in memory; they don't change them.)
The XUID is the profile's, as in profiles\\<name>\\xuid or a console save's CON header.
"""
import ctypes
import hashlib
import hmac
import os
import struct
import sys
import zlib

IMAGE = os.environ.get('FM2_IMAGE', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'tu4', 'image.bin'))


# Each kind of file: where its two key constants are in the image, and the XUID byte each key byte uses.
KINDS = {
    'profile': (0x76A0, 1, 0x76B0, 0),      # ForzaProfile: 82232A00 / 82236E88
    'database': (0x76C0, 3, 0x76D0, 2),     # PlayerDatabase (CDatabase::vf13); found by trying constants
    'livery': (0xD1E0, -3, 0xD1D0, -4),     # 8226AF90 (the livery code); not checked against a file yet
}


def keys(xuid, kind='profile'):
    """(AES key, HMAC key): constant[i] ^ xuid[(i + shift) & 7], the constants read from the game's own TU4
    image (not copied into this file)."""
    img = open(IMAGE, 'rb').read()
    aes_at, aes_shift, mac_at, mac_shift = KINDS[kind]
    x = struct.pack('>Q', xuid)
    aes = bytes(img[aes_at + i] ^ x[(i + aes_shift) & 7] for i in range(16))
    mac = bytes(img[mac_at + i] ^ x[(i + mac_shift) & 7] for i in range(16))
    return aes, mac


def aes_cbc(key, iv, data, encrypt):
    """AES-128-CBC without padding, through Windows' BCrypt (no Python packages needed)."""
    bc = ctypes.WinDLL('bcrypt')
    alg, hkey = ctypes.c_void_p(), ctypes.c_void_p()
    if bc.BCryptOpenAlgorithmProvider(ctypes.byref(alg), ctypes.c_wchar_p('AES'), None, 0):
        raise OSError('BCryptOpenAlgorithmProvider')
    try:
        mode = ctypes.create_unicode_buffer('ChainingModeCBC')
        bc.BCryptSetProperty(alg, ctypes.c_wchar_p('ChainingMode'), mode, ctypes.sizeof(mode), 0)
        kb = ctypes.create_string_buffer(key, 16)
        if bc.BCryptGenerateSymmetricKey(alg, ctypes.byref(hkey), None, 0, kb, 16, 0):
            raise OSError('BCryptGenerateSymmetricKey')
        try:
            src = ctypes.create_string_buffer(data, len(data))
            out = ctypes.create_string_buffer(len(data))
            ivb = ctypes.create_string_buffer(iv, 16)
            got = ctypes.c_ulong()
            fn = bc.BCryptEncrypt if encrypt else bc.BCryptDecrypt
            if fn(hkey, src, len(data), None, ivb, 16, out, len(data), ctypes.byref(got), 0):
                raise OSError('BCrypt %scrypt' % ('en' if encrypt else 'de'))
            return out.raw[:got.value]
        finally:
            bc.BCryptDestroyKey(hkey)
    finally:
        bc.BCryptCloseAlgorithmProvider(alg, 0)


def decrypt(blob, xuid, kind='profile'):
    aes, mac = keys(xuid, kind)
    head, body = blob[:40], blob[40:]
    want = hmac.new(mac, head[20:40] + body, hashlib.sha1).digest()
    if want != head[:20]:
        raise ValueError('HMAC mismatch: wrong XUID, or not a secure stream')
    w = struct.unpack('>5I', head[20:40])
    length = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4]
    return aes_cbc(aes, head[24:40], body, False)[:length]


def encrypt(plain, xuid, kind='profile', iv=None):
    aes, mac = keys(xuid, kind)
    iv = iv or os.urandom(16)
    body = aes_cbc(aes, iv, plain + b'\0' * (-len(plain) % 16), True)
    w = struct.unpack('>4I', iv)
    lenword = struct.pack('>I', len(plain) ^ w[0] ^ w[1] ^ w[2] ^ w[3])
    tail = lenword + iv
    return hmac.new(mac, tail + body, hashlib.sha1).digest() + tail + body


# ForzaProfile values: [u32 name length][name][u32 0x20][u32 0][u8 type][value]; type 15 is a section (u32 count,
# then its entries). The file holds several such trees behind small headers that aren't worked out yet, so
# values are found by their shape rather than by walking the trees.
VALUE_SIZE = {0: 1, 1: 1, 2: 2, 3: 4, 4: 8, 7: 4}


def values(plain):
    """(offset of the value, section, name, type, size) for every value, in file order."""
    out, section, o = [], '', 0
    while o + 17 < len(plain):
        n = struct.unpack_from('>I', plain, o)[0]
        if 1 <= n <= 64 and o + 4 + n + 9 <= len(plain):
            name = plain[o + 4:o + 4 + n]
            if all(48 <= c < 123 for c in name) and plain[o + 4 + n:o + 12 + n] == b'\0\0\0\x20\0\0\0\0':
                t = plain[o + 12 + n]
                if t == 15:
                    section = name.decode()
                    o += 17 + n
                    continue
                if t in VALUE_SIZE:
                    out.append((o + 13 + n, section, name.decode(), t, VALUE_SIZE[t]))
                    o += 13 + n + VALUE_SIZE[t]
                    continue
        o += 1
    return out


def unpack(blob, xuid):
    """PlayerDatabase -> SQLite bytes."""
    plain = decrypt(blob, xuid, 'database')
    size, packed, _ = struct.unpack('>3I', plain[:12])
    db = zlib.decompress(plain[12:12 + packed], -15)
    if len(db) != size:
        raise ValueError('inflated %d bytes, the header says %d' % (len(db), size))
    return db


def pack(db, xuid):
    """SQLite bytes -> PlayerDatabase."""
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = c.compress(db) + c.flush()
    return encrypt(struct.pack('>3I', len(db), len(packed), len(db)) + packed, xuid, 'database')


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    cmd, path, xuid = argv[1], argv[2], int(argv[3], 16)
    if cmd in ('unpack', 'pack'):
        data = open(path, 'rb').read()
        out = unpack(data, xuid) if cmd == 'unpack' else pack(data, xuid)
        dest = argv[4] if len(argv) > 4 else path + ('.sqlite' if cmd == 'unpack' else '.packed')
        open(dest, 'wb').write(out)
        print('%s: %d bytes -> %s' % (cmd, len(out), dest))
        return 0
    if cmd in ('list', 'set'):
        plain = bytearray(decrypt(open(path, 'rb').read(), xuid))
        found = values(plain)
        if cmd == 'list':
            for at, sec, name, t, size in found:
                v = int.from_bytes(plain[at:at + size], 'big')
                print('%s/%s	type %d	%d' % (sec, name, t, v))
            return 0
        want, value = argv[4], int(argv[5], 0)
        hits = [f for f in found if '%s/%s' % (f[1], f[2]) == want]
        if len(hits) != 1:
            raise SystemExit('%d values called %s' % (len(hits), want))
        at, _, _, _, size = hits[0]
        plain[at:at + size] = value.to_bytes(size, 'big')
        open(path, 'wb').write(encrypt(bytes(plain), xuid))
        print('%s = %d' % (want, value))
        return 0
    kind = 'database' if 'PlayerDatabase' in os.path.basename(path) else 'profile'
    data = open(path, 'rb').read()
    out = decrypt(data, xuid, kind) if cmd == 'decrypt' else encrypt(data, xuid, kind)
    dest = argv[4] if len(argv) > 4 else path + ('.plain' if cmd == 'decrypt' else '.enc')
    open(dest, 'wb').write(out)
    print('%s: %d bytes -> %s (%s)' % (cmd, len(out), dest, out[:16].hex() if cmd == 'decrypt' else 'ok'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
