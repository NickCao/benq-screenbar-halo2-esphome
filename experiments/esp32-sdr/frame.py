"""MSB-first Halo 2 frame construction and independent bitwise CRC checks."""


def crc_bits(bits):
    crc = 0xFFFF
    for bit in bits:
        feedback = ((crc >> 15) & 1) ^ int(bit)
        crc = (crc << 1) & 0xFFFF
        if feedback:
            crc ^= 0x1021
    return crc


def unpack(data, count):
    return [(data[b // 8] >> (7 - b % 8)) & 1 for b in range(count)]


def pack(bits):
    data = bytearray((len(bits) + 7) // 8)
    for b, bit in enumerate(bits):
        data[b // 8] |= bit << (7 - b % 8)
    return bytes(data)


def make_frame(sync, payload, pid=0):
    if not 0 < sync < 0xFFFFFFFF or len(payload) != 10 or not 0 <= pid <= 3:
        raise ValueError("Expected a mixed-bit address, ten payload bytes and PID 0..3")
    following = [0] + unpack(bytes([0x50 | (pid << 1)]) + payload, 88)
    crc = crc_bits(unpack(sync.to_bytes(4, "big"), 32) + following)
    return pack(following + unpack(crc.to_bytes(2, "big"), 16))


def check_crc(sync, data, bits):
    if bits != 105:
        return None
    following = unpack(data, bits)
    calculated = crc_bits(unpack(sync.to_bytes(4, "big"), 32) + following[:89])
    received = int.from_bytes(pack(following[89:]), "big")
    return {"calculated": f"{calculated:04X}", "received": f"{received:04X}", "valid": calculated == received}
