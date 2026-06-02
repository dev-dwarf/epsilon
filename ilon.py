#!/usr/bin/env python3
import json5 as pyjson5, json, os, re, sys

def fnv1a(s):
    h = 0x811c9dc5
    for c in s:
        x = ord(c)
        if ord('A') <= x <= ord('Z'): x |= 0x20  # lf.h str_hash_fnv1a lowercases before hashing
        h = ((x ^ h) * 0x01000193) & 0xFFFF
    return f"0x{h:04X}"

def c_size(bits):
    for s in [8, 16, 32, 64]:
        if bits <= s: return s
    raise ValueError(f"bits={bits} too large")

def enum_bits(vals):
    return max(1, max(vals.values()).bit_length()) if vals else 1

_FIELD_KEYS = {'_bits','_signed','_scale','_offset','_unit','_overflow','_fixed','_array','_enum','_struct'}

def norm_field(f):
    u = f.get('_unit', '')
    if u and not re.match(r'^[a-zA-Z0-9_]+$', u):
        raise ValueError(f"_unit {u!r} must be alphanumeric+underscore only")
    out = {
        '_bits':     f.get('_bits', None),
        '_signed':   bool(f.get('_signed', False)),
        '_scale':    float(f.get('_scale', 1.0)),
        '_offset':   float(f.get('_offset', 0.0)),
        '_unit':     u,
        '_overflow': f.get('_overflow', 'clamp'),
        '_fixed':    bool(f.get('_fixed', False)),
        '_array':    f.get('_array', None),
        '_enum':     f.get('_enum', None),
        '_struct':   f.get('_struct', None),
    }
    out.update({k: v for k, v in f.items() if k not in _FIELD_KEYS})
    return out

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
        co.update({k: v for k, v in c.items() if k not in {'_struct', '_instances'}})
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
        mo.update({k: v for k, v in m.items() if k not in {'_struct', '_instances', '_interval_ms'}})
        msgs[n] = mo

    result = {'_name': name, '_include': [i['_name'] for i in incs], '_included': incs,
              '_enums': enums, '_structs': structs, '_commands': cmds, '_messages': msgs}
    result.update({k: v for k, v in raw.items() if k not in {'_include','_instances','_enums','_structs','_commands','_messages'}})
    return result

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
    if field['_enum']: return 'u32'  # XCDR1 enums are always 32-bit on the wire
    if (field['_scale'] != 1.0 or field['_offset'] != 0.0) and not field['_fixed']: return 'f32'
    return f"{'s' if field['_signed'] else 'u'}{c_size(field['_bits'])}"

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
    return "\n".join([
        f"typedef enum {{",
        *[f"  {nu}_{k.upper()} = {v}," for k, v in vals.items()],
        f"}} {name};\n",
        f"static inline str {name}_str({name} v) {{",
        f"  switch (v) {{",
        *[f'    case {nu}_{k.upper()}: return strl("{k}");' for k in vals],
        f'    default: return strl(""); }}', f"}}\n",
        f"static inline u16 {name}_hash({name} v) {{",
        f"  switch (v) {{",
        *[f"    case {nu}_{k.upper()}: return {fnv1a(k)};" for k in vals],
        f"    default: return 0; }}", f"}}\n",
        f"static inline {name} {name}_from_str(str s) {{",
        *[f'  if (str_eql(s, "{k}")) return {nu}_{k.upper()};' for k in vals],
        f"  return ({name})-1;", f"}}\n",
        f"static inline {name} {name}_from_hash(u16 h) {{",
        f"  switch (h) {{",
        *[f"    case {fnv1a(k)}: return {nu}_{k.upper()};" for k in vals],
        f"    default: return ({name})-1; }}", f"}}\n",
    ])

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
    sfs = sorted(fields.items(), key=lambda x: elem_bytes(x[1], enums, structs, px), reverse=True)
    mems = [(fn, upk_type(f, enums, px), f"[{arr_count(f,enums)}]" if arr_count(f,enums) else "") for fn, f in sfs]
    packed_sz = (sum(packed_bits(f, enums, structs) for f in fields.values()) + 7) // 8
    raw = sum(elem_bytes(f, enums, structs, px) * (arr_count(f, enums) or 1) for _, f in sfs)
    pad = struct_sizeof(fields, enums, structs, px) - raw

    bit = [0]

    def pack_field(fn, f, acc):
        if f['_struct']:
            sf = structs[f['_struct']]
            cnt = arr_count(f, enums)
            if cnt:
                return "\n".join(pack_field(f"{fn}[{i}].{sfn}", sf2, acc) for i in range(cnt) for sfn, sf2 in sf.items())
            return "\n".join(pack_field(f"{fn}.{sfn}", sf2, acc) for sfn, sf2 in sf.items())
        cnt = arr_count(f, enums)
        lines = []
        for i in range(cnt or 1):
            src = f"{acc}{fn}[{i}]" if cnt else f"{acc}{fn}"
            if f['_enum']:
                nb = enum_bits(enums[f['_enum']])
                lines += [f"  {pack_ops(bit[0], nb, f'(u64){src}')}"]
            else:
                nb = f['_bits']
                sc, ofs, signed, ov = f['_scale'], f['_offset'], f['_signed'], f['_overflow']
                ub = c_size(nb)
                ut, st = f"u{ub}", f"s{ub}"
                lo = -(1 << (nb-1)) if signed else 0
                hi = (1 << (nb-1)) - 1 if signed else (1 << nb) - 1
                ct = st if signed else ut
                if (sc != 1.0 or ofs != 0.0) and not f['_fixed']:
                    expr = f"CLAMP(({src} - {ofs}f) / {sc}f, {lo}, {hi})" if ov == 'clamp' \
                           else f"({src} - {ofs}f) / {sc}f"
                    lines += [f"  {{ {ct} _r=({ct})({expr}); {pack_ops(bit[0], nb, f'(u64)({ut})_r')} }}"]
                elif ov == 'clamp':
                    lines += [f"  {{ {ct} _r=({ct})CLAMP({src},{lo},{hi}); {pack_ops(bit[0], nb, f'(u64)({ut})_r')} }}"]
                else:
                    lines += [f"  {pack_ops(bit[0], nb, f'(u64)({ut}){src}')}"]
            bit[0] += nb
        return "\n".join(lines)

    def unpack_field(fn, f, acc):
        if f['_struct']:
            sf = structs[f['_struct']]
            cnt = arr_count(f, enums)
            if cnt:
                return "\n".join(unpack_field(f"{fn}[{i}].{sfn}", sf2, acc) for i in range(cnt) for sfn, sf2 in sf.items())
            return "\n".join(unpack_field(f"{fn}.{sfn}", sf2, acc) for sfn, sf2 in sf.items())
        cnt = arr_count(f, enums)
        lines = []
        for i in range(cnt or 1):
            dst = f"{acc}{fn}[{i}]" if cnt else f"{acc}{fn}"
            if f['_enum']:
                nb = enum_bits(enums[f['_enum']])
                lines += [f"  {{ {unpack_ops(bit[0], nb)} {dst}=(u{c_size(nb)})_r; }}"]
            else:
                nb = f['_bits']
                sc, ofs, signed = f['_scale'], f['_offset'], f['_signed']
                ub = c_size(nb)
                se = f"(s64)(_r<<(64-{nb}))>>(64-{nb})" if signed else "_r"
                if (sc != 1.0 or ofs != 0.0) and not f['_fixed']:
                    lines += [f"  {{ {unpack_ops(bit[0], nb)} {dst}=(f32)({se})*{sc}f+{ofs}f; }}"]
                else:
                    lines += [f"  {{ {unpack_ops(bit[0], nb)} {dst}=({'s'if signed else 'u'}{ub})({se}); }}"]
            bit[0] += nb
        return "\n".join(lines)

    pack_lines = [pack_field(fn, f, "s->") for fn, f in fields.items()]
    bit[0] = 0
    unpack_lines = [unpack_field(fn, f, "s->") for fn, f in fields.items()]

    return "\n".join([
        f"#define {px.upper()}_{sname.upper()}_PACKED_BYTES {packed_sz}",
        f"typedef struct {{",
        *[f"  {t} {fn}{arr_s};" for fn, t, arr_s in mems],
        *([f"  u8 _pad[{pad}];"] if pad else []),
        f"}} {px}_{sname};\n",
        f"static inline void {px}_{sname}_pack(u8 *buf, const {px}_{sname} *s) {{",
        f"  memset(buf, 0, {packed_sz});",
        *pack_lines,
        f"}}\n",
        f"static inline void {px}_{sname}_unpack(const u8 *buf, {px}_{sname} *s) {{",
        *unpack_lines,
        f"}}\n",
    ])

def gen_ros2msg_const(msg_name, sname, enums, structs, px):
    module = px.lower() + '_msgs'
    order, seen = [], set()
    def collect(n):
        if n in seen: return
        seen.add(n)
        sfs = sorted(structs[n].items(), key=lambda x: elem_bytes(x[1], enums, structs, px), reverse=True)
        for _, f in sfs:
            if f['_struct'] and f['_struct'] in structs: collect(f['_struct'])
        order.append(n)
    collect(sname)
    def ftype(f):
        if f['_enum']: return 'uint32'
        if f['_struct']: return f['_struct']  # short name; dep section uses package/Type
        m = {'f32':'float32','f64':'float64','u8':'uint8','u16':'uint16','u32':'uint32',
             'u64':'uint64','s8':'int8','s16':'int16','s32':'int32','s64':'int64'}
        return m.get(upk_type(f, enums, px), upk_type(f, enums, px))
    # root type: fields start immediately (NO section header), matching ros2msg spec.
    # dep types: preceded by "===...\nMSG: package/Type" (no /msg/ in path).
    lines, seen_enums = [], set()
    for i, dep in enumerate(reversed(order)):
        sfs = sorted(structs[dep].items(), key=lambda x: elem_bytes(x[1], enums, structs, px), reverse=True)
        if i > 0:
            lines += ['=' * 80, f"MSG: {module}/{dep}"]
        if dep == sname:
            for _, f in sfs:
                if f['_enum'] and f['_enum'] not in seen_enums:
                    seen_enums.add(f['_enum'])
                    lines += [f"uint32 {f['_enum'].upper()}_{k.upper()}={v}" for k, v in enums[f['_enum']].items()]
        lines += [f"{ftype(f)}{'['+str(arr_count(f,enums))+']' if arr_count(f,enums) else ''} {fn}{'_'+f['_unit'] if f['_unit'] != "" else ""}" for fn, f in sfs]
    idl = [f'    "{line}\\n"' for line in lines]
    idl[-1] += ';'
    return f"static const char {px}_{msg_name}_IDL[] =\n" + '\n'.join(idl) + '\n'

def gen_eps(a_name, msgs, cmds, structs, enums, px):
    def gen_one(name, sn, ie):
        pu, pxl, nl = px.upper(), px.lower(), name.lower()
        px_nu = f"{pu}_{name.upper()}"
        pub_size = f"(sizeof(eps_id) + sizeof({px}_{sn}))"
        return "\n".join([
            f"#define {px_nu}_HASH {fnv1a(name)}",
            f"#define {px_nu}_PUB_SIZE {pub_size}",
            f"static inline void {px}_{name}_send(int group, {ie} inst, const {px}_{sn} *s) {{",
            f" static uint8_t _{pxl}_{nl}_buf[{px_nu}_PUB_SIZE];",
            f" static eps_msg _{pxl}_{nl}_msg = {{ .id={{.agent=AGENT_{pu}_HASH,.msg={px_nu}_HASH}}, .data=_{pxl}_{nl}_buf, .size={px_nu}_PUB_SIZE}};",
            f"  _{pxl}_{nl}_msg.id.inst = {ie}_hash(inst);",
            f"  memcpy(_{pxl}_{nl}_buf + sizeof(eps_id), s, sizeof({px}_{sn}));",
            f"  eps_send(group, &_{pxl}_{nl}_msg); }}",
            f"#define {px}_{name}_sub(inst, buf) \\",
            f"  eps_add_sub((eps_msg){{.id={{.agent=AGENT_{pu}_HASH,.msg={px_nu}_HASH,.inst={ie}_hash(inst)}},.data=(uint8_t*)(buf),.size={px_nu}_PUB_SIZE}})",
            "",
        ])
    return "\n".join([
        f"#ifdef EPS_H",
        f"#define AGENT_{px.upper()}_HASH {fnv1a(a_name)}",
        "",
        *[gen_one(mn, msg.get('_struct'), msg.get('_instances', a_name)) for mn, msg in msgs.items()],
        *[gen_one(cn, cmd.get('_struct'), cmd.get('_instances', a_name)) for cn, cmd in cmds.items()],
        f"#endif // EPS_H",
    ])

def fmt_json(obj, d=0):
    p, ip = '  '*d, '  '*(d+1)
    if isinstance(obj, dict):
        if not obj: return '{}'
        if all(not isinstance(v, (dict, list)) for v in obj.values()):
            return '{' + ', '.join(f'{json.dumps(k)}: {json.dumps(v)}' for k, v in obj.items()) + '}'
        inner = ',\n'.join(f'{ip}{json.dumps(k)}: {fmt_json(v, d+1)}' for k, v in obj.items())
        return '{\n' + inner + '\n' + p + '}'
    if isinstance(obj, list):
        if not obj: return '[]'
        if all(not isinstance(v, (dict, list)) for v in obj):
            return '[' + ', '.join(json.dumps(v) for v in obj) + ']'
        inner = ',\n'.join(f'{ip}{fmt_json(v, d+1)}' for v in obj)
        return '[\n' + inner + '\n' + p + ']'
    return json.dumps(obj)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} [--json|--header] <agent.json5>", file=sys.stderr)
        sys.exit(1)
    path = sys.argv[1]
    agent = load_norm(path)
    px = agent['_name']

    enums = gather_enums(agent)
    structs = gather_structs(agent)
    for sfs in structs.values():
        for f in sfs.values():
            if f['_enum'] and f['_bits'] is None and f['_enum'] in enums:
                f['_bits'] = enum_bits(enums[f['_enum']])

    def clean(d):
        if isinstance(d, dict): return {k: clean(v) for k, v in d.items() if k != '_included'}
        if isinstance(d, list): return [clean(i) for i in d]
        return d
    json_str = fmt_json(clean(agent))

    def emit_agent_defs(a):
        g = f"{a['_name'].upper()}_DEFINED"
        ipx = a['_name']
        return "\n".join([
            *[emit_agent_defs(inc) for inc in a.get('_included', [])],
            f"#ifndef {g}", f"#define {g}", "",
            *[gen_enum(n, vals) for n, vals in a['_enums'].items()],
            *[gen_struct(n, sfs, enums, structs, ipx) for n, sfs in a['_structs'].items()],
            *[gen_ros2msg_const(mn, msg['_struct'], enums, structs, ipx)
              for mn, msg in a['_messages'].items() if msg.get('_struct') in structs],
            gen_eps(a['_name'], a['_messages'], a['_commands'], structs, enums, ipx),
            "#endif", "",
        ])

    fg = f"{px.upper()}_H"
    header = "\n".join([f"#ifndef {fg}", f"#define {fg}", "#include <lf.h>", "", emit_agent_defs(agent), f"#endif"])

    d = os.path.dirname(path) or '.'
    open(os.path.join(d, f"{px}.json"), 'w').write(json_str + '\n')
    open(os.path.join(d, f"{px}.h"), 'w').write(header + '\n')
