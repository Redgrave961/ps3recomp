import argparse
import hashlib
import hmac
import struct
import sys

from Crypto.Cipher import AES
from Crypto.Hash import CMAC

RAP_KEY = bytes.fromhex("869F7745C13FD890CCF29188E3CC3EDF")
RAP_PBOX = bytes.fromhex("0C030604010B0F0802070005 0A0E0D09".replace(" ", ""))
RAP_E1 = bytes.fromhex("A93E1FD67C55A329B75FDDA62A95C7A5")
RAP_E2 = bytes.fromhex("67D45DA3296D006A4E7C537BF5538C74")
EDAT_KEY = (bytes.fromhex("BE959CA8308DEFA2E5E180C63712A9AE"), bytes.fromhex("4CA9C14B01C95309969BEC68AA0BC081"))
EDAT_IV = bytes(16)
NP_KLIC_FREE = bytes.fromhex("72F990788F9CFF745725F08E4C128387")

FLAG_COMPRESSED = 0x00000001
FLAG_0x02 = 0x00000002
FLAG_ENCRYPTED_KEY = 0x00000008
FLAG_0x10 = 0x00000010
FLAG_0x20 = 0x00000020
FLAG_DEBUG = 0x80000000
FLAG_SDAT = 0x01000000


def rap_to_rif(rap):
    key = bytearray(AES.new(RAP_KEY, AES.MODE_CBC, bytes(16)).decrypt(rap))
    for _ in range(5):
        for i in range(16):
            p = RAP_PBOX[i]
            key[p] ^= RAP_E1[p]
        for i in range(15, 0, -1):
            key[RAP_PBOX[i]] ^= key[RAP_PBOX[i - 1]]
        o = 0
        for i in range(16):
            p = RAP_PBOX[i]
            kc = (key[p] - o) & 0xFF
            ec2 = RAP_E2[p]
            if o != 1 or kc != 0xFF:
                o = 1 if kc < ec2 else 0
                key[p] = (kc - ec2) & 0xFF
            else:
                key[p] = kc
    return bytes(key)


def aes_cbc_dec(key, iv, data):
    return AES.new(key, AES.MODE_CBC, iv).decrypt(data)


def read_headers(blob):
    if blob[:4] != b"NPD\0":
        raise ValueError("not an NPD file")
    version, license_, type_ = struct.unpack(">iii", blob[4:16])
    npd = {
        "version": version, "license": license_, "type": type_,
        "content_id": blob[16:64].rstrip(b"\0").decode("ascii", "replace"),
        "digest": blob[64:80], "title_hash": blob[80:96], "dev_hash": blob[96:112],
    }
    flags, block_size, file_size = struct.unpack(">iIQ", blob[0x80:0x90])
    edat = {"flags": flags & 0xFFFFFFFF, "block_size": block_size, "file_size": file_size}
    return npd, edat


def block_key(block, npd):
    src = bytes(16) if npd["version"] <= 1 else npd["dev_hash"]
    return src[:12] + struct.pack(">i", block)


def decrypt_blocks(blob, crypt_key):
    npd, edat = read_headers(blob)
    flags = edat["flags"]
    if flags & FLAG_SDAT:
        raise ValueError("SDATA files are handled with the SDAT key, not a licence")
    if flags & FLAG_COMPRESSED:
        raise ValueError("compressed EDAT")
    bs, size = edat["block_size"], edat["file_size"]
    total = (size + bs - 1) // bs
    meta_size = 0x20 if flags & FLAG_0x20 else 0x10
    version4 = npd["version"] == 4
    out = bytearray()
    for b in range(total):
        if flags & FLAG_0x20:
            sec = 0x100 + b * (meta_size + bs)
            meta = blob[sec:sec + 0x20]
            test_hash = bytes(meta[j] ^ meta[j + 0x10] for j in range(0x10)) + meta[0x10:0x14]
            offset, length = sec + 0x20, bs
        else:
            sec = 0x100 + b * meta_size
            test_hash = blob[sec:sec + 0x10]
            offset, length = 0x100 + b * bs + total * meta_size, bs
        if b == total - 1 and size % bs:
            length = size % bs
        pad = (length + 15) & ~15
        enc = blob[offset:offset + pad]
        key_result = AES.new(crypt_key, AES.MODE_ECB).encrypt(block_key(b, npd))
        hash_src = AES.new(crypt_key, AES.MODE_ECB).encrypt(key_result) if flags & FLAG_0x10 else key_result
        if flags & FLAG_DEBUG:
            out += enc[:length]
            continue
        iv = bytes(16) if npd["version"] <= 1 else npd["digest"]
        if flags & FLAG_ENCRYPTED_KEY:
            key_final = aes_cbc_dec(EDAT_KEY[version4], EDAT_IV, key_result)
            hash_key = aes_cbc_dec(EDAT_KEY[version4], EDAT_IV, hash_src)
        else:
            key_final = key_result
            hash_key = hash_src
        plain = enc if flags & FLAG_0x02 else aes_cbc_dec(key_final, iv, enc)
        if not flags & FLAG_0x10:
            ok = CMAC.new(hash_key[:16], enc, ciphermod=AES).digest() == test_hash[:16]
        elif not flags & FLAG_0x20:
            ok = hmac.new(hash_key[:16], enc, hashlib.sha1).digest()[:16] == test_hash[:16]
        else:
            ok = hmac.new(hash_key[:16] + bytes(4), enc, hashlib.sha1).digest() == test_hash[:20]
        if not ok:
            return None, npd, edat
        out += plain[:length]
    return bytes(out), npd, edat


def main():
    ap = argparse.ArgumentParser(description="Decrypt an NPDRM EDAT with a .rap licence, verifying every block")
    ap.add_argument("edat")
    ap.add_argument("--rap", action="append", default=[])
    ap.add_argument("--out")
    a = ap.parse_args()
    blob = open(a.edat, "rb").read()
    npd, edat = read_headers(blob)
    print(f"{a.edat}: content {npd['content_id']} version {npd['version']} license {npd['license']} "
          f"flags 0x{edat['flags']:08X} block 0x{edat['block_size']:X} size {edat['file_size']}")
    keys = [(r, rap_to_rif(open(r, "rb").read())) for r in a.rap]
    if npd["license"] & 3 == 3:
        keys.append(("free klicensee", NP_KLIC_FREE))
    for name, key in keys:
        data, _, _ = decrypt_blocks(blob, key)
        if data is None:
            print(f"  {name}: block hashes do not verify")
            continue
        print(f"  {name}: verified, {len(data)} bytes")
        if a.out:
            open(a.out, "wb").write(data)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
