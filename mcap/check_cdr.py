#!/usr/bin/env python3
"""
CDR decoder that simulates Foxglove's IDL parsing to diagnose the
"consumed fewer bytes" error.

Tests two hypotheses:
  H1: Foxglove resolves cross-section type refs (multi-section IDL works)
  H2: Foxglove does NOT resolve cross-section refs (unknown types → 0 bytes consumed)

Also decodes C-generated example.mcap and Python test files to verify byte counts.
"""
import struct, re, math
from mcap.reader import make_reader

# ---------------------------------------------------------------------------
# Minimal IDL parser → type registry
# ---------------------------------------------------------------------------

# CDR XCDR1 LE primitive sizes and alignments
PRIM = {  # type_name -> (size, align)
    'float':    (4, 4), 'double': (8, 8),
    'uint8':    (1, 1), 'uint16': (2, 2), 'uint32': (4, 4), 'uint64': (8, 8),
    'int8':     (1, 1), 'int16':  (2, 2), 'int32':  (4, 4), 'int64':  (8, 8),
    'boolean':  (1, 1), 'octet':  (1, 1), 'char':   (1, 1),
}

def parse_idl(schema_text):
    """
    Parse multi-section IDL schema text.
    Returns:
      structs: {name: [(field_name, type_str, count), ...]}  (in definition order)
      enums:   {name: (size, align)}   — @bit_bound(N) enums get size=N/8
    """
    structs, enums = {}, {}

    # Remove C/C++ block comments
    schema_text = re.sub(r'/\*.*?\*/', '', schema_text, flags=re.DOTALL)
    schema_text = re.sub(r'//[^\n]*', '', schema_text)

    # Enums: @bit_bound(N) enum Name { ... }
    for m in re.finditer(r'@bit_bound\s*\(\s*(\d+)\s*\)\s*enum\s+(\w+)', schema_text):
        nb = int(m.group(1))
        name = m.group(2)
        sz = (nb + 7) // 8
        enums[name] = (sz, sz)

    # Standard enums (no @bit_bound) → 4 bytes per XCDR1 spec
    for m in re.finditer(r'(?<![\w])enum\s+(\w+)', schema_text):
        name = m.group(1)
        if name not in enums:
            enums[name] = (4, 4)

    # Structs
    for m in re.finditer(r'struct\s+(\w+)\s*\{([^}]*)\}', schema_text, re.DOTALL):
        sname = m.group(1)
        body  = m.group(2)
        fields = []
        for fm in re.finditer(r'([\w:]+)\s+(\w+)(?:\[(\d+)\])?\s*;', body):
            type_str = fm.group(1)
            fname    = fm.group(2)
            count    = int(fm.group(3)) if fm.group(3) else 1
            fields.append((fname, type_str, count))
        structs[sname] = fields

    return structs, enums

def cdr_type_sa(type_name, structs, enums):
    """Return (size, align) for a type in the CDR stream.
    Returns (None, None) if the type is unknown (cross-section failure case)."""
    if type_name in PRIM:     return PRIM[type_name]
    if type_name in enums:    return enums[type_name]
    if type_name in structs:  return cdr_struct_sa(structs[type_name], structs, enums)
    return (None, None)  # unknown — cross-section resolution failure

def cdr_struct_sa(fields, structs, enums, start=0):
    """
    Compute (byte_size, max_alignment) of a CDR-encoded struct.
    start: absolute buffer position where this struct begins (for alignment).
    Returns (None, None) if any field type is unknown.
    """
    offset = start
    max_align = 1
    for fname, type_str, count in fields:
        elem_sz, elem_al = cdr_type_sa(type_str, structs, enums)
        if elem_sz is None:
            return (None, None)  # unknown nested type
        al = min(elem_al, 8)
        max_align = max(max_align, al)

        if type_str in structs:
            # Each array element of a struct: serialize its fields with proper alignment
            for _ in range(count):
                padding = (-offset) % al
                offset += padding
                _, _ = cdr_struct_sa(structs[type_str], structs, enums, offset)
                elem_actual, _ = cdr_struct_sa(structs[type_str], structs, enums, offset)
                if elem_actual is None:
                    return (None, None)
                offset += elem_actual
        else:
            # Primitive / enum: align once, then count copies with no inter-element padding
            padding = (-offset) % al
            offset += padding
            offset += elem_sz * count

    return (offset - start, max_align)

def cdr_message_size(root_type, structs, enums):
    """Expected CDR payload size (bytes after the 4-byte CDR header) for root_type."""
    sz, _ = cdr_struct_sa(structs.get(root_type, []), structs, enums, start=4)
    # start=4 because CDR header occupies bytes 0-3; field data starts at 4
    if sz is None:
        return None
    return sz

# ---------------------------------------------------------------------------
# MCAP reader helpers
# ---------------------------------------------------------------------------

def read_mcap(path):
    """Yield (schema, channel, msg_data_bytes) for every message in path."""
    with open(path, 'rb') as f:
        reader = make_reader(f)
        for schema, channel, message in reader.iter_messages():
            yield schema, channel, message.data

# ---------------------------------------------------------------------------
# CDR decoder: decode actual bytes and return Python dict
# ---------------------------------------------------------------------------

class CdrReader:
    def __init__(self, data):
        # skip 4-byte encapsulation header
        assert data[:2] == bytes([0x00, 0x01]), f"unexpected CDR header: {data[:4].hex()}"
        self.data = data
        self.pos = 4   # absolute position in buffer (includes header)

    def _align(self, n):
        self.pos = (self.pos + n - 1) & ~(n - 1)

    def u8(self):   v, = struct.unpack_from('B', self.data, self.pos); self.pos += 1; return v
    def u32(self):  self._align(4); v, = struct.unpack_from('<I', self.data, self.pos); self.pos += 4; return v
    def f32(self):  self._align(4); v, = struct.unpack_from('<f', self.data, self.pos); self.pos += 4; return v

    def remaining(self): return len(self.data) - self.pos

def decode_flat(data):
    r = CdrReader(data)
    return {'x': r.f32(), 'y': r.f32(), 'z': r.f32(), 'n': r.u32(), '_rem': r.remaining()}

def decode_status_variant(data, baz_count=4):
    """Decodes BazSS/BazMS/BazFD status variant: Baz[N] + f32 + u32."""
    r = CdrReader(data)
    baz = []
    for _ in range(baz_count):
        bam = r.f32()
        bop = r.u8()
        r.u8(); r.u8(); r.u8()   # _pad[3]
        baz.append({'bam': bam, 'bop': bop})
    foo = r.f32()
    bar = r.u32()
    return {'baz': baz, 'foo': foo, 'bar': bar, '_rem': r.remaining()}

def decode_example_status(data):
    """Decode Example_Status: Baz[8] + f32 foo + u8 bar + u8 bat[8] + u8 _pad[3]."""
    r = CdrReader(data)
    baz = []
    for _ in range(8):
        bam = r.f32(); bop = r.u8(); r.u8(); r.u8(); r.u8()  # _pad[3]
        baz.append({'bam': round(bam,3), 'bop': bop})
    foo = r.f32()
    bar = r.u8()
    bat = [r.u8() for _ in range(8)]
    pad = [r.u8() for _ in range(3)]
    return {'baz': baz, 'foo': round(foo,3), 'bar': bar, 'bat': bat,
            '_pad': pad, '_rem': r.remaining()}

def decode_example_telemetry(data):
    """Decode Example_Telemetry: Vec3{f32×3} + u32 timestamp + u8 modes[4]."""
    r = CdrReader(data)
    pos = {'x': round(r.f32(),3), 'y': round(r.f32(),3), 'z': round(r.f32(),3)}
    timestamp = r.u32()
    modes = [r.u8() for _ in range(4)]
    return {'pos': pos, 'timestamp': timestamp, 'modes': modes, '_rem': r.remaining()}

# ---------------------------------------------------------------------------
# Expected values from mcap_example.c (first 3 iterations)
# ---------------------------------------------------------------------------
def expected_status(i):
    phase = i * 0.314
    baz = [{'bam': round(math.sin(phase + j*0.5)*5, 2), 'bop': j%3} for j in range(8)]
    foo = round(math.cos(phase)*10, 2)
    bar = 1 if i % 2 == 0 else 6
    bat = [1 if (i+j)%2 else 0 for j in range(8)]
    return {'baz': baz, 'foo': foo, 'bar': bar, 'bat': bat}

def expected_telemetry(i):
    phase = i * 0.314
    return {
        'pos': {'x': round(math.cos(phase)*2,3), 'y': round(math.sin(phase)*2,3), 'z': round(i*0.1,3)},
        'timestamp': i*100,
        'modes': [1 if i%2==0 else 0, 2 if i%3==0 else 0, 4 if i%4==0 else 0, 5],
    }

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
SEP = '-' * 60

def get_schemas(path):
    """Return list of (name, idl_text) for all schemas in an MCAP file."""
    schemas = []
    with open(path, 'rb') as f:
        reader = make_reader(f)
        summary = reader.get_summary()
        if summary is not None:
            for s in summary.schemas.values():
                schemas.append((s.name, s.data.decode('utf-8', errors='replace')))
        else:
            # no summary (our hand-written MCAP) — scan linearly
            for schema, channel, message in reader.iter_messages():
                name = schema.name
                text = schema.data.decode('utf-8', errors='replace')
                if not any(n == name for n, _ in schemas):
                    schemas.append((name, text))
    return schemas

def check_schema(path, root_type, _1=None, _2=None):
    """Parse schema from file, compute expected CDR size with and without cross-section resolution."""
    for sname, idl_text in get_schemas(path):
        if sname.endswith('/' + root_type) or sname == root_type:
            structs, enums = parse_idl(idl_text)
            print(f"  IDL sections found: {list(structs.keys())}")

            sz_full = cdr_message_size(root_type, structs, enums)
            print(f"  CDR size (full resolution):  {sz_full}")

            # Simulate cross-section failure: only keep the section containing root_type
            sections = re.split(r'={40,}', idl_text)
            root_section = next((s for s in sections if f'struct {root_type}' in s), '')
            s2, e2 = parse_idl(root_section)
            sz_xsec = cdr_message_size(root_type, s2, e2)
            print(f"  CDR size (root section only): {sz_xsec}  "
                  f"← Foxglove sees this if cross-section refs fail")
            return sz_full, sz_xsec
    print(f"  (schema not found)")
    return None, None

print("=" * 60)
print("SECTION 1: IDL schema analysis — does cross-section resolution matter?")
print("=" * 60)

for path, root in [
    ('test_flat.mcap',      'Flat'),
    ('test_singlesec.mcap', 'StatusSS'),
    ('test_multisec.mcap',  'StatusMS'),
    ('test_fwddecl.mcap',   'StatusFD'),
    ('example.mcap',        'Status'),
    ('example.mcap',        'Telemetry'),
]:
    print(f"\n{path} / {root}:")
    check_schema(path, root, {}, {})

print()
print("=" * 60)
print("SECTION 2: Decode C-generated example.mcap — are bytes correct?")
print("=" * 60)

si, ti = 0, 0
for schema, channel, data in read_mcap('example.mcap'):
    topic = channel.topic
    if '/status' in topic and si < 3:
        d = decode_example_status(data)
        e = expected_status(si)
        ok_rem  = d['_rem'] == 0
        ok_foo  = abs(d['foo'] - e['foo']) < 0.1
        ok_bar  = d['bar'] == e['bar']
        ok_bat  = d['bat'] == e['bat']
        print(f"  {topic} msg {si}: payload={len(data)-4}B rem={d['_rem']} "
              f"foo={'OK' if ok_foo else 'BAD'} bar={'OK' if ok_bar else 'BAD'} "
              f"bat={'OK' if ok_bat else 'BAD'}")
        if not ok_rem:
            print(f"    *** {d['_rem']} bytes left after decode — CDR layout mismatch!")
        si += 1
    if '/telemetry' in topic and ti < 3:
        d = decode_example_telemetry(data)
        e = expected_telemetry(ti)
        ok_rem = d['_rem'] == 0
        ok_ts  = d['timestamp'] == e['timestamp']
        print(f"  {topic} msg {ti}: payload={len(data)-4}B rem={d['_rem']} "
              f"ts={'OK' if ok_ts else 'BAD'}")
        if not ok_rem:
            print(f"    *** {d['_rem']} bytes left after decode — CDR layout mismatch!")
        ti += 1

print()
print("=" * 60)
print("SECTION 3: Decode Python test files — verify encoder is correct")
print("=" * 60)

for path, decoder, baz_n in [
    ('test_flat.mcap',      decode_flat,           0),
    ('test_singlesec.mcap', decode_status_variant, 4),
    ('test_multisec.mcap',  decode_status_variant, 4),
    ('test_fwddecl.mcap',   decode_status_variant, 4),
]:
    print(f"\n  {path}:")
    for i, (_, _, data) in enumerate(read_mcap(path)):
        if i >= 3: break
        if baz_n == 0:
            d = decoder(data)
        else:
            d = decoder(data, baz_n)
        ok = d['_rem'] == 0
        print(f"    msg {i}: payload={len(data)-4}B rem={d['_rem']} {'OK' if ok else 'MISMATCH'}")
        if not ok:
            print(f"      remaining bytes: {data[len(data)-d['_rem']:].hex()}")
