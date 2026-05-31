#!/usr/bin/env python3

import pyjson5
import json
import os
import sys

global agent

def hash_fnv1a(s):
    hash = 0x811c9dc5
    for x in s:
        hash = ((ord(x) ^ hash) * 0x01000193) & 0xFFFFFFFF
    return hash

def c_round_up(bits):
    """Round up to the nearest C integer size."""
    sizes = [8, 16, 32, 64]
    for size in sizes:
        if bits <= size:
            return size
    raise Exception('Bit size too large')

def c_format(var):
    """Determine the C type from a JSON5 variable block."""
    bits = c_round_up(var["bits"])
    
    if var['kind'] == 'float':
        return 'double'
    elif var['kind'] == 'int':
        if var['signed']:
            return f's{bits}'
        else:
            return f'u{bits}'
    elif var['kind'] == 'enum':
        return f'u16 /* {var["enum"].upper()} */'

def main():
    enums_path = sys.argv[1]
    units_path = sys.argv[2]
    agent_path = sys.argv[3]

    name = os.path.basename(agent_path).split('.')[0]

    with open(enums_path, 'r') as f:
        global_enums = pyjson5.load(f)
    with open(units_path, 'r') as f:
        units = pyjson5.load(f)

    def get_unit_conversion(from_unit, to_unit):
        """Compute the conversion (scale, offset) between two units."""
        u1 = units.get(from_unit)
        u2 = units.get(to_unit)

        if u1 is None:
            raise ValueError(f'Unsupported unit {from_unit}')
        if u2 is None:
            raise ValueError(f'Unsupported unit {to_unit}')

        # Normalize to [scale,offset] even if offset is not provided
        if not isinstance(u1, list):
            u1 = [u1, 0]
        if not isinstance(u2, list):
            u2 = [u2, 0]

        # If x is in unit `from_unit`, and y is in unit `to_unit`,
        # and SI is in the base SI unit, then we have:
        #   u1[0] * x + u1[1] = SI
        #   u2[0] * y + u2[1] = SI
        # therefore
        #   y = (u1[0]/u2[0]) * x + (u1[1]-u2[1])/u2[0].
        # Return this as polynomial factors.
        return (u1[0] / u2[0], (u1[1] - u2[1]) / u2[0])


    # Normalize agent fields
    with open(agent_path, 'r') as f:
        agent = pyjson5.load(f)

    agent['instances'] = agent.get('instances', [])
    agent['commands'] = agent.get('commands', {})

    agent['enums'] = agent.get('enums', {})
    agent['enums'][name] = agent['instances']
    for e in global_enums.keys():
        if e in agent['enums'] and e != "None":
            raise Exception(f"Enum {e} in agent enums, but already defined in global_enums.json5")
    agent['enums'] = agent['enums'] | global_enums

    def normalize_enum(variants):
        """Normalize an enum's variants.
        Given a set of enum variants as either a list or a dict of
        int->string, this function always returns a dict of int->string.
        """
        is_flags = isinstance(variants, dict)
        if is_flags:
            values = { int(v): k for k, v in variants.items()}
        else:
            values = dict(list(enumerate(variants)))
        return { "is_flags": is_flags, "values": values }
    
    agent['enums'] = {k: normalize_enum(v) for k, v in agent['enums'].items() }

    def normalize_message(msg, cmd=False):
        norm = {}
        norm['instance'] = msg.get('instances', { "enum": name } )
        norm['frequency'] = msg.get('frequency', 0.0)

        # telem frequencies
        freq = msg.get('frequency_los', 0.0)
        norm['frequency_los'] = freq
        norm['frequency_rfd'] = msg.get('frequency_rfd', freq)
        norm['frequency_blos'] = msg.get('frequency_blos', freq)

        # Reserved signal names; these indicate other properties of messages
        # and aren't actually signals.
        reserved_names = [
            'frequency', 'warning', 'persist', 'send_on_subscribe',
            'frequency_los', 'frequency_rfd', 'frequency_blos', 'telem_all',
            'telem_group', 'telem', 'command_echo'
        ]
        norm['signals'] = {}
        for i in msg.keys():
            if i in reserved_names:
                continue
            mem = {}
            spec = msg[i]

            mem['unit'] = spec.get('unit', "")
            mem['signed'] = spec.get('signed', False)

            if cmd and 'cmd_def' in spec:
                mem['cmd_def'] = spec['cmd_def']

            if 'enum' in spec:
                mem['kind'] = 'enum'

                enum = agent['enums'][spec['enum']]
                if enum == None:
                    raise Exception(f"Invalid enum in {spec} {spec['enum']}")
                mem['bits'] = max(enum["values"].keys()).bit_length()
                mem['enum'] = spec['enum']

                if cmd:
                    mem['cmd_def'] = mem.get('cmd_def', enum["values"][0])
            elif 'min' in spec or 'max' in spec:
                mem['kind'] = 'float'
                mem['bits'] = spec['bits']
                mem['min'] = spec['min']
                mem['max'] = spec['max']

                if mem['signed']:
                    range = spec['max'] - spec['min']
                    mem['scale'] = range / ((1 << spec['bits']) - 1)
                    mem['offset'] = spec['min']
                else:
                    raise Exception(f'Invalid packet member {spec}')
            elif 'scale' in spec or 'offset' in spec:
                mem['kind'] = 'float'
                mem['bits'] = spec['bits']

                mem['scale'] = spec.get('scale', 1.0)
                mem['offset'] = spec.get('offset', 0.0)

                if mem['signed']:
                    imin = -(1 << (mem['bits'] - 1))
                    imax = -(1 + imin)
                else:
                    imin = 0
                    imax = (1 << mem['bits'])-1

                mem['min'] = imin*mem['scale'] + mem['offset']
                mem['max'] = imax*mem['scale'] + mem['offset']
            else:
                mem['kind'] = 'int'
                mem['bits'] = spec['bits']

                if mem['signed']:
                    imin = -(1 << (mem['bits'] - 1))
                    imax = -(1 + imin)
                else:
                    imin = 0
                    imax = (1 << mem['bits'])-1
                
                mem['min'] = imin
                mem['max'] = imax

            if mem['kind'] == 'float':
                if 'gui_unit' in spec:
                    if spec['unit'] == spec['gui_unit']:
                        raise Exception(f"gui_unit same as unit! {spec}")
                    conv_scale, conv_offset = get_unit_conversion(spec['unit'], spec['gui_unit'])
                    # Fixed point encoding with unit conversion baked in
                    # basic fixed point: sx + o = f
                    # unit conv: uf + v = g
                    # Solve for 1st order polynomial to go from x -> g:
                    # u(sx + o) + v = g
                    # (us)x + (uo + v) = g
                    # so new scale is (us) and new offset is (uo + v)
                    mem['gui_scale'] = conv_scale * mem['scale']
                    mem['gui_offset'] = conv_scale * mem['offset'] + conv_offset
                    mem['gui_unit'] = spec['gui_unit']
                else:
                    mem['gui_unit'] = mem['unit']
                    mem['gui_scale'] = mem['scale']
                    mem['gui_offset'] = mem['offset']

            if cmd:
                mem['cmd_def'] = mem.get('cmd_def', mem.get('offset', 0 if mem['signed'] else mem.get('min', 0)))

            norm['signals'][i] = mem

        return norm

    for n in agent['messages'].keys():
        agent['messages'][n] = normalize_message(agent['messages'][n])

    for n in agent['commands'].keys():
        agent['commands'][n] = normalize_message(agent['commands'][n], cmd=True)

    # Generate normalized agent json
    with open(f'{name}.json', 'w') as out:
        out.write(json.dumps(agent, indent=2))

    # Generate data header
    text = '';
    def line(linetext=''):
        nonlocal text
        text += linetext + '\n'

    line(f'#ifndef {name.upper()}_H')
    line(f'#define {name.upper()}_H')
    line(f'#include <sceye_base.h>')

    def gen_enums(enums):
        for name, enum in enums.items():
            line(f'enum {name.upper()} {{')
            if enum['is_flags']:
                for k,v in enum['values'].items():
                    line(f"  {name.upper()}_{v.upper()} = {k},")
            else:
                for v in enum['values'].values():
                    line(f"  {name.upper()}_{v.upper()},")
                line(f"  {name.upper()}_COUNT")
            line(f'}};')

    line(f'#ifndef SCEYE_GLOBAL_ENUMS_H')
    line(f'#define SCEYE_GLOBAL_ENUMS_H')
    global_enums = {name: e for name, e in agent['enums'].items() if name in global_enums.keys()}
    gen_enums(global_enums)
    line()

    agent_enums = {name: e for name, e in agent['enums'].items() if not (name in global_enums.keys())}
    gen_enums(agent_enums)
        
    enums_instanced = {name: e for name, e in agent['enums'].items() if 0
        or any(name == msg["instance"]["enum"] for msg in agent['messages'].values())
        or any(name == msg["instance"]["enum"] for msg in agent['commands'].values())
    }
    for name, enum in enums_instanced.items():
        if enum['is_flags']:
            raise ValueError("Messages cannot be instanced on a flags enum! {enum}")
        line(f'static s32 map_{name}(u16 hash) {{ switch (hash) {{')
        for k, v in enum['values'].items():
            hash = hash_fnv1a(v) & 0xFFFF
            line(f"  case {hex(hash)} /* {v} */: return {k};")
        line(f'}} return -1; }}')
        line(f'static u16 hash_{name}(s32 index) {{ if (index >= 0 && index < {name.upper()}_COUNT) {{ const static u16 hashes[{name.upper()}_COUNT] = {{')
        for k, v in enum['values'].items():
            hash = hash_fnv1a(v) & 0xFFFF
            line(f"  {hex(hash)} /* {v} */,")
        line(f'}}; return hashes[index]; }} return 0; }}')
        line(f'static str to_string_{name}(s32 index) {{ if (index >= 0 && index < {name.upper()}_COUNT) {{ const static str strings[{name.upper()}_COUNT] = {{')
        for k, v in enum['values'].items():
            line(f'  strl("{v}"),')
        line(f'}}; return strings[index]; }} return STRUCT_ZERO(str); }}')
        line(f'static s32 parse_{name}(str name) {{ return map_{name}(str_hash_fnv1a(name, 0)); }}')
        line()

    def gen_msg_structs(msgs):
        for msg_name, msg_data in msgs.items():
            line(f'typedef struct {name}_{msg_name} {{')
            for sig_name, sig_data in msg_data['signals'].items():
                line(f"  {c_format(sig_data)} {sig_name};")
            line(f'}} {name}_{msg_name};')
            line()

    gen_msg_structs(agent['messages'])
    gen_msg_structs(agent['commands'])
    
    line(f'#endif//{name.upper()}_H')
    with open(f'{name}.h', 'w') as out:
        out.write(text)

    # Generate publisher header

if __name__ == '__main__':
    main()
