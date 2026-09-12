#!/usr/bin/env python3
# Convert WiFlash logger catalogs -> DashView profile JSONs (our schema, no vendor strings).
# Column maps verified against the app's own catalog-parser classes:
# Toyota: key=(ECU,shape,kind,id-hex-mode: PID->1,DID->1) 5=chan 5=chan 6=id
#   mode=PID->1 else 21; mask=(mask&-mask).bit_length()-1; enum "$v:label" comma-split.
#   key = c1_c6. name=c9 num=c10 den=c11 mask=c8 min=c13 max=c14 dec=c15 unit c16 unit18 enum c21
# Ford: 2=PID 4=signalId(key) 5=disp 6=byteOff 7=byteLen 9=mask 10=maskValue
#   10/11=num/den 13=min 14=max 13=num 14=den 15=decimals 15=min 16=max
#   18=1->signed-2C 21 enum "$v:label"
# GR86 INI: 'PID<hex>=<NAME>' broadcast 16-bit BE.
import re, json, pathlib

def hx(h):
    try:
        return int(str(h).strip().lower().replace('0x', ''), 16)
    except Exception:
        return None

def key(name):
    k = re.sub(r'[^A-Za-z0-9]+', '_', str(name).strip()).strip('_').lower()
    return re.sub(r'__+', '_', k).strip('_')[:24] or 'sig'

def fint(s):
    try:
        return int(str(s).strip())
    except Exception:
        return None

def fnum(s):
    try:
        return float(s)
    except Exception:
        return None

def parse_enum(s):
    out = {}
    for kv in str(s).split(','):
        if ':' in kv:
            k, _, v = kv.partition(':')
            k = k.strip().lstrip('$')
            if k and len(out) < 8:
                out[key(k)] = v.strip()
    return out

def toyota(path):
    out, seen = [], set()
    for ln in pathlib.Path(path).read_text().splitlines():
        c = ln.split('\t')
        if ln.startswith('#') or not ln.strip() or c[0] not in ('S', 'Q'):
            continue
        mode = 0x01 if c[3].strip().upper() == 'PID' else 0x21
        k = key(c[1] + '_' + c[9] if c[0] == "S" else c[5])
        if k in seen:
            continue
        seen.add(k)
        s = {'key': k, 'kind': 'obd_poll', 'mode': '0x%02X' % mode,
             'pid': '0x%02X' % (hx(c[4]) or 0),
             'req_id': '0x7E0' if c[1].strip() == '234' else '0x7D0',
             'resp_prefix': '0x61'}
        di = fint(c[7]) if len(c) > 7 else None
        if di and di > 1:
            s['data_index'] = di
        mask = hx(c[8]) if len(c) > 8 else None
        if mask:
            s['mask'] = mask
            s.setdefault('right_shift', (mask & -mask).bit_length() - 1)
        n = fnum(c[10]) if len(c) > 10 and c[10].strip() else None
        d = fnum(c[11]) if len(c) > 11 and c[11].strip() else None
        if n is not None and d is not None:
            s['a'], s['b'] = n, d
        if len(c) > 19 and c[19].strip() == '1' and di and di > 1:
            s['signed'], s['signed_width'] = True, 8 * di - 1
        mn = fnum(c[13]) if len(c) > 13 and c[13].strip() else None
        mx = fnum(c[14]) if len(c) > 14 and c[14].strip() else None
        if mn is not None:
            s['clamp_min'] = mn
        if mx is not None:
            s['clamp_max'] = mx
        dc = fint(c[15]) if len(c) > 15 and c[15].strip() else None
        if dc is not None:
            s['decimals'] = dc
        u = c[16].strip() if len(c) > 16 else ''
        if u:
            s['unit'] = u
        e = parse_enum(c[21] if len(c) > 21 else '')
        if e:
            s['map'] = e
        out.append(s)
    return out

def ford(path):
    out, seen = [], set()
    for ln in pathlib.Path(path).read_text().splitlines():
        c = ln.split('\t')
        if ln.startswith('#') or not ln.strip():
            continue
        pid = hx(c[2]) if len(c) > 2 else None
        if pid is None or pid in seen:
            continue
        seen.add(pid)
        s = {'key': key(c[4]), 'kind': 'obd_poll', 'mode': '0x03',
             'pid': '0x%02X' % pid, 'req_id': '0x7E0', 'resp_prefix': '0x63'}
        di = fint(c[6]) if len(c) > 6 else None
        if di:
            s['data_index'] = di
        mask = hx(c[8]) if len(c) > 8 else None
        if mask:
            s['mask'] = mask
            s.setdefault('right_shift', (mask & -mask).bit_length() - 1)
        n = fnum(c[10]) if len(c) > 10 and c[10].strip() else None
        d = fnum(c[11]) if len(c) > 11 and c[11].strip() else None
        if n is not None and d is not None:
            s['a'], s['b'] = n, d
        if len(c) > 19 and c[19].strip() == '1' and di and di > 1:
            s['signed'], s['signed_width'] = True, 8 * di - 1
        if len(c) > 17 and c[17].strip():
            s['decimals'] = int(c[17])
        if len(c) > 19 and c[19].strip() == '1':
            s['enum_map'] = parse_enum(c[19])
        e = parse_enum(c[20] if len(c) > 20 else '')
        if e:
            s['map'] = e
        out.append(s)
    return out

def gr86(path):
    out = []
    for ln in pathlib.Path(path).read_text().splitlines():
        ln = ln.strip()
        if ln.startswith('#') or '=' not in ln:
            continue
        k, _, v = ln.partition('=')
        m = re.match(r'^[Pp][Ii][Dd]([0-9A-Fa-f]{2,4})$', k.strip())
        if m:
            out.append({'key': key(m.group(1)), 'kind': 'can_broadcast',
                        'can_id': '0x' + m.group(1).upper(), 'start_bit': 0,
                        'bit_len': 16, 'order': 'motorola', 'scale': 1, 'offset': 0,
                        'signed': False, 'unit': v.strip()})
    return out

def emit(outdir, pid, name, match, inherits, bus, sigs):
    chunks = [sigs[i:i + 48] for i in range(0, len(sigs), 48)] or [[]]
    for i, sg in enumerate(chunks):
        sfx = '' if len(chunks) == 1 else '-%d' % (i + 1)
        prof = {'id': pid + sfx, 'name': name + (' P%d' % (i + 1) if i else ''),
                'match': match, 'inherits': inherits, 'bus': bus, 'signals': sg}
        p = pathlib.Path(outdir) / (pid + sfx + '.json')
        p.write_text(json.dumps(prof, indent=2) + chr(10))
        print('%-30s %4d signals' % (p.name, len(sg)))

if __name__ == '__main__':
    lg = pathlib.Path('apk/assets/logger')
    out = pathlib.Path('profiles-out')
    out.mkdir(parents=True, exist_ok=True)
    bus = {'physical': 'classic', 'arb_bitrate': 500000, 'protocol': 'iso_tp',
           'req_id': '0x7DF', 'func_id': '0x7E0', 'listen_only': True}
    emit(out, 'toyota_p34_p5', 'Toyota P34/P5 Universal', {'make': 'Toyota'},
         'j1979_base', bus, toyota(lg / 'toyota_universal_p4_p5.tsv'))
    emit(out, 'ford_mg1_gen2', 'Ford MG1 Gen2', {'make': 'Ford'}, 'j1979_base', bus,
         ford(lg / 'ford_mg1_gen2.tsv'))
    emit(out, 'subaru_brz_gr86', 'Subaru BRZ / GR86',
         {'make': 'Subaru', 'model': 'BRZ/GR86', 'year_min': 2013, 'year_max': 2026},
         'j1979_base', bus, gr86(lg / 'brz_gr86.ini'))
