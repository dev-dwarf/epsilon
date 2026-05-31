#!/usr/bin/env python3
import json5 as pyjson5, json, os, sys

def fnv1a(s):
    h = 0x811c9dc5
    for c in s:
        x = ord(c)
        if ord('A') <= x <= ord('Z'): x |= 0x20  # lf.h str_hash_fnv1a lowercases before hashing
        h = ((x ^ h) * 0x01000193) & 0xFFFFFFFF
    return h

def c_up(bits):
    for s in [8, 16, 32, 64]:
        if bits <= s: return s
    raise ValueError(f"bits={bits} too large")

def enum_bits(vals):
    return max(1, max(vals.values()).bit_length()) if vals else 1

def norm_field(f):
    return {
        '_bits':     f.get('_bits', None),
        '_signed':   bool(f.get('_signed', False)),
        '_scale':    float(f.get('_scale', 1.0)),
        '_offset':   float(f.get('_offset', 0.0)),
        '_unit':     f.get('_unit', ''),
        '_overflow': f.get('_overflow', 'clamp'),
        '_fixed':    bool(f.get('_fixed', False)),
        '_array':    f.get('_array', None),
        '_enum':     f.get('_enum', None),
        '_struct':   f.get('_struct', None),
    }

def norm_enum(e):
    if isinstance(e, list): return {k: i for i, k in enumerate(e)}
    return {k: int(v) for k, v in e.items()}

def load_norm(path):
    with open(path) as f: raw = pyjson5.load(f)
    base = os.path.basename(path).split('.')[0]
    name = base[0].upper() + base[1:]
    d = os.path.dirname(path) or '.'

    incs = []
    for p in raw.get('_include', []):
        fp = os.path.join(d, p)
        if os.path.exists(fp): incs.append(load_norm(fp))

    enums = {}
    if '_instances' in raw: enums[name] = norm_enum(raw['_instances'])
    for n, e in raw.get('_enums', {}).items(): enums[n] = norm_enum(e)

    structs = {}
    for n, s in raw.get('_structs', {}).items():
        structs[n] = {k: norm_field(v) for k, v in s.items()}

    cmds = {}
    for n, c in raw.get('_commands', {}).items():
        co = {}
        if '_struct' in c:
            if isinstance(c['_struct'], str): co['_struct'] = c['_struct']
            else:
                structs[n] = {k: norm_field(v) for k, v in c['_struct'].items()}
                co['_struct'] = n
        co['_instances'] = c.get('_instances', name)
        cmds[n] = co

    msgs = {}
    for n, m in raw.get('_messages', {}).items():
        mo = {}
        if '_struct' in m:
            if isinstance(m['_struct'], str): mo['_struct'] = m['_struct']
            else:
                structs[n] = {k: norm_field(v) for k, v in m['_struct'].items()}
                mo['_struct'] = n
        mo['_instances'] = m.get('_instances', name)
        if '_interval_ms' in m: mo['_interval_ms'] = m['_interval_ms']
        msgs[n] = mo

    return {'_name': name, '_include': [i['_name'] for i in incs], '_included': incs,
            '_enums': enums, '_structs': structs, '_commands': cmds, '_messages': msgs}

def gather_enums(agent):
    e = {}
    for i in agent.get('_included', []): e.update(gather_enums(i))
    e.update(agent['_enums'])
    return e

def gather_structs(agent):
    s = {}
    for i in agent.get('_included', []): s.update(gather_structs(i))
    s.update(agent['_structs'])
    return s

def upk_type(field, enums, px):
    if field['_struct']: return f"{px}_{field['_struct']}"
    if field['_enum']: return f"u{c_up(enum_bits(enums[field['_enum']]))}"
    if (field['_scale'] != 1.0 or field['_offset'] != 0.0) and not field['_fixed']: return 'f32'
    return f"{'s' if field['_signed'] else 'u'}{c_up(field['_bits'])}"

def struct_align(sfields, enums, structs, px):
    ma = 1
    for f in sfields.values():
        if f['_struct'] and f['_struct'] in structs:
            a = struct_align(structs[f['_struct']], enums, structs, px)
        else:
            a = elem_bytes(f, enums, structs, px)
        ma = max(ma, min(a, 8))
    return ma

def struct_sizeof(sfields, enums, structs, px):
    raw = sum(elem_bytes(f, enums, structs, px) * (arr_count(f, enums) or 1) for f in sfields.values())
    al  = struct_align(sfields, enums, structs, px)
    return (raw + al - 1) & ~(al - 1)

def elem_bytes(field, enums, structs, px):
    t = upk_type(field, enums, px)
    if t == 'f32': return 4
    if len(t) > 1 and t[0] in 'us' and t[1:].isdigit(): return int(t[1:]) // 8
    if field['_struct'] and field['_struct'] in structs:
        return struct_sizeof(structs[field['_struct']], enums, structs, px)
    return 4

def arr_count(field, enums):
    a = field['_array']
    if a is None: return None
    if isinstance(a, int): return a
    return len(enums.get(a, {}))

def packed_bits(field, enums, structs):
    if field['_struct']:
        b = sum(packed_bits(f, enums, structs) for f in structs[field['_struct']].values())
    elif field['_enum']:
        b = enum_bits(enums[field['_enum']])
    else:
        b = field['_bits']
    return b * (arr_count(field, enums) or 1)

def gen_enum(name, vals):
    nu = name.upper()
    L = [f"typedef enum {{"]
    for k, v in vals.items(): L.append(f"  {nu}_{k.upper()} = {v},")
    L.append(f"}} {name};\n")

    L.append(f"static inline str {name}_str({name} v) {{")
    L.append(f"  switch (v) {{")
    for k in vals: L.append(f'    case {nu}_{k.upper()}: return strl("{k}");')
    L += [f'    default: return strl(""); }}', f"}}\n"]

    L.append(f"static inline u32 {name}_hash({name} v) {{")
    L.append(f"  switch (v) {{")
    for k in vals: L.append(f"    case {nu}_{k.upper()}: return 0x{fnv1a(k):08x}u;")
    L += [f"    default: return 0u; }}", f"}}\n"]

    L.append(f"static inline {name} {name}_from_str(str s) {{")
    for k in vals: L.append(f'  if (str_eql(s, "{k}")) return {nu}_{k.upper()};')
    L += [f"  return ({name})-1;", f"}}\n"]

    L.append(f"static inline {name} {name}_from_hash(u32 h) {{")
    L.append(f"  switch (h) {{")
    for k in vals: L.append(f"    case 0x{fnv1a(k):08x}u: return {nu}_{k.upper()};")
    L += [f"    default: return ({name})-1; }}", f"}}\n"]

    return "\n".join(L)

def pack_ops(B, N, v):
    ops = []
    for b in range(B//8, (B+N-1)//8 + 1):
        lo = max(0, b*8-B); bpos = (B+lo)%8; cnt = min(N-1, b*8+7-B)-lo+1; mask = (1<<cnt)-1
        e = f"({v})>>{lo}" if lo else f"({v})"
        if mask != 0xFF: e = f"({e})&0x{mask:x}"
        if bpos:          e = f"({e})<<{bpos}"
        ops.append(f"buf[{b}]{'='if cnt==8 else '|='}(u8)({e});")
    return " ".join(ops)

def unpack_ops(B, N):
    ops = ["u64 _r=0;"]
    for b in range(B//8, (B+N-1)//8 + 1):
        lo = max(0, b*8-B); bpos = (B+lo)%8; cnt = min(N-1, b*8+7-B)-lo+1; mask = (1<<cnt)-1
        e = f"buf[{b}]>>{bpos}" if bpos else f"buf[{b}]"
        if mask != 0xFF: e = f"({e})&0x{mask:x}"
        e = f"(u64)({e})<<{lo}" if lo else f"(u64)({e})"
        ops.append(f"_r|={e};")
    return " ".join(ops)

def gen_struct(sname, fields, enums, structs, px):
    L = []
    sfs = sorted(fields.items(), key=lambda x: elem_bytes(x[1], enums, structs, px), reverse=True)

    mems = []
    for fn, f in sfs:
        t = upk_type(f, enums, px)
        cnt = arr_count(f, enums)
        mems.append((fn, t, f"[{cnt}]" if cnt else ""))

    total_bits = sum(packed_bits(f, enums, structs) for f in fields.values())
    packed_sz = (total_bits + 7) // 8

    raw = sum(elem_bytes(f, enums, structs, px) * (arr_count(f, enums) or 1) for _, f in sfs)
    pad = struct_sizeof(fields, enums, structs, px) - raw
    L.append(f"#define {px.upper()}_{sname.upper()}_PACKED_BYTES {packed_sz}")
    L.append(f"typedef struct {{")
    for fn, t, arr_s in mems: L.append(f"  {t} {fn}{arr_s};")
    if pad: L.append(f"  u8 _pad[{pad}];")
    L.append(f"}} {px}_{sname};\n")

    bit = [0]

    def pack_field(fn, f, acc):
        if f['_struct']:
            sf = structs[f['_struct']]
            cnt = arr_count(f, enums)
            if cnt:
                for i in range(cnt):
                    for sfn, sf2 in sf.items(): pack_field(f"{fn}[{i}].{sfn}", sf2, acc)
            else:
                for sfn, sf2 in sf.items(): pack_field(f"{fn}.{sfn}", sf2, acc)
            return
        cnt = arr_count(f, enums)
        for i in range(cnt or 1):
            src = f"{acc}{fn}[{i}]" if cnt else f"{acc}{fn}"
            if f['_enum']:
                nb = enum_bits(enums[f['_enum']])
                L.append(f"  {pack_ops(bit[0], nb, f'(u64){src}')}")
            else:
                nb = f['_bits']
                sc, ofs = f['_scale'], f['_offset']
                signed = f['_signed']
                ov = f['_overflow']
                ub = c_up(nb)
                ut, st = f"u{ub}", f"s{ub}"
                lo = -(1 << (nb-1)) if signed else 0
                hi = (1 << (nb-1)) - 1 if signed else (1 << nb) - 1
                ct = st if signed else ut
                if (sc != 1.0 or ofs != 0.0) and not f['_fixed']:
                    expr = f"CLAMP(({src} - {ofs}f) / {sc}f, {lo}, {hi})" if ov == 'clamp' \
                           else f"({src} - {ofs}f) / {sc}f"
                    L.append(f"  {{ {ct} _r=({ct})({expr}); {pack_ops(bit[0], nb, f'(u64)({ut})_r')} }}")
                elif ov == 'clamp':
                    L.append(f"  {{ {ct} _r=({ct})CLAMP({src},{lo},{hi}); {pack_ops(bit[0], nb, f'(u64)({ut})_r')} }}")
                else:
                    L.append(f"  {pack_ops(bit[0], nb, f'(u64)({ut}){src}')}")
            bit[0] += nb

    L.append(f"static inline void {px}_{sname}_pack(u8 *buf, const {px}_{sname} *s) {{")
    L.append(f"  memset(buf, 0, {packed_sz});")
    for fn, f in fields.items(): pack_field(fn, f, "s->")
    L.append(f"}}\n")

    def unpack_field(fn, f, acc):
        if f['_struct']:
            sf = structs[f['_struct']]
            cnt = arr_count(f, enums)
            if cnt:
                for i in range(cnt):
                    for sfn, sf2 in sf.items(): unpack_field(f"{fn}[{i}].{sfn}", sf2, acc)
            else:
                for sfn, sf2 in sf.items(): unpack_field(f"{fn}.{sfn}", sf2, acc)
            return
        cnt = arr_count(f, enums)
        for i in range(cnt or 1):
            dst = f"{acc}{fn}[{i}]" if cnt else f"{acc}{fn}"
            if f['_enum']:
                nb = enum_bits(enums[f['_enum']])
                L.append(f"  {{ {unpack_ops(bit[0], nb)} {dst}=(u{c_up(nb)})_r; }}")
            else:
                nb = f['_bits']
                sc, ofs = f['_scale'], f['_offset']
                signed = f['_signed']
                ub = c_up(nb)
                se = f"(s64)(_r<<(64-{nb}))>>(64-{nb})" if signed else "_r"
                if (sc != 1.0 or ofs != 0.0) and not f['_fixed']:
                    L.append(f"  {{ {unpack_ops(bit[0], nb)} {dst}=(f32)({se})*{sc}f+{ofs}f; }}")
                else:
                    L.append(f"  {{ {unpack_ops(bit[0], nb)} {dst}=({'s'if signed else 'u'}{ub})({se}); }}")
            bit[0] += nb

    bit[0] = 0
    L.append(f"static inline void {px}_{sname}_unpack(const u8 *buf, {px}_{sname} *s) {{")
    for fn, f in fields.items(): unpack_field(fn, f, "s->")
    L.append(f"}}\n")

    return "\n".join(L)

def idl_type(field, enums, px):
    if field['_enum']: return f"{px}_{field['_enum']}"
    t = upk_type(field, enums, px)
    m = {'f32':'float','f64':'double','u8':'uint8','u16':'uint16','u32':'uint32','u64':'uint64',
         's8':'int8','s16':'int16','s32':'int32','s64':'int64'}
    if t in m: return m[t]
    if field['_struct']: return field['_struct']
    return t

def gen_idl_const(msg_name, sname, enums, structs, px):
    module = px.lower() + '_msgs'
    # collect dependency order: nested structs first, root last (single section)
    order, seen, seen_enums = [], set(), set()
    def collect(n):
        if n in seen: return
        seen.add(n)
        sfs = sorted(structs[n].items(), key=lambda x: elem_bytes(x[1], enums, structs, px), reverse=True)
        for _, f in sfs:
            if f['_struct'] and f['_struct'] in structs: collect(f['_struct'])
        order.append(n)
    collect(sname)
    body = []
    for dep in order:
        sfs = sorted(structs[dep].items(), key=lambda x: elem_bytes(x[1], enums, structs, px), reverse=True)
        raw = sum(elem_bytes(f, enums, structs, px) * (arr_count(f, enums) or 1) for _, f in sfs)
        pad = struct_sizeof(structs[dep], enums, structs, px) - raw
        for _, f in sfs:
            if f['_enum'] and f['_enum'] not in seen_enums:
                seen_enums.add(f['_enum'])
                ename, nb = f['_enum'], c_up(enum_bits(enums[f['_enum']]))
                vals_list = list(enums[ename].items())
                body.append(f"    @bit_bound({nb})")
                body.append(f"    enum {px}_{ename} {{")
                for j, (k, v) in enumerate(vals_list):
                    body.append(f"      @value({v}) {k}{',' if j < len(vals_list)-1 else ''}")
                body.append(f"    }};")
        body.append(f"    struct {dep} {{")
        for fn, f in sfs:
            cnt = arr_count(f, enums)
            body.append(f"      {idl_type(f, enums, px)} {fn}{'['+str(cnt)+']' if cnt else ''};")
        if pad: body.append(f"      uint8 _pad[{pad}];")
        body.append(f"    }};")
    L = [f"static const char {px}_{msg_name}_IDL[] ="]
    L.append(f'    "{"="*80}\\n"')
    L.append(f'    "IDL: {module}/msg/{msg_name}\\n"')
    L.append(f'    "module {module} {{\\n"')
    L.append(f'    "  module msg {{\\n"')
    for line in body: L.append(f'    "{line}\\n"')
    L.append(f'    "  }};\\n"')
    L.append(f'    "}};\\n";')
    return "\n".join(L) + "\n"

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} [--json|--header] <agent.json5>", file=sys.stderr)
        sys.exit(1)
    mode = sys.argv[1] if sys.argv[1].startswith('--') else None
    path = sys.argv[2] if mode else sys.argv[1]
    agent = load_norm(path)
    px = agent['_name']

    def clean(d):
        if isinstance(d, dict): return {k: clean(v) for k, v in d.items() if k != '_included'}
        if isinstance(d, list): return [clean(i) for i in d]
        return d
    json_str = json.dumps(clean(agent), indent=2)

    enums = gather_enums(agent)
    structs = gather_structs(agent)

    def emit_agent_defs(a, L):
        for inc in a.get('_included', []):
            emit_agent_defs(inc, L)
        g = f"{a['_name'].upper()}_DEFINED"
        L += [f"#ifndef {g}", f"#define {g}", ""]
        ipx = a['_name']
        for n, vals in a['_enums'].items(): L.append(gen_enum(n, vals))
        for n, sfs in a['_structs'].items(): L.append(gen_struct(n, sfs, enums, structs, ipx))
        for mn, msg in a['_messages'].items():
            sn = msg.get('_struct')
            if sn and sn in structs: L.append(gen_idl_const(mn, sn, enums, structs, ipx))
        L += ["#endif", ""]

    fg = f"{px.upper()}_H"
    L = [
        f"#ifndef {fg}",
        f"#define {fg}",
        "#include <lf.h>",
        "",
    ]
    emit_agent_defs(agent, L)
    L.append(f"#endif")
    header = "\n".join(L)

    if mode == '--json':
        print(json_str)
    elif mode == '--header':
        print(header)
    else:
        d = os.path.dirname(path) or '.'
        open(os.path.join(d, f"{px}.json"), 'w').write(json_str + '\n')
        open(os.path.join(d, f"{px}.h"), 'w').write(header + '\n')
