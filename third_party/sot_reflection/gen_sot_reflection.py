# Builds third_party/sot_reflection/sot_reflection.txt for the Blueprint decompiler from two SDK dumps of the game:
#   old-sot/investigation/native_code/depot/sdk_*_index.json   (depot_2020 build: signatures with const / out refs)
#   sot-dump/SoT_<ver>_steam_all/SDK                            (newer build: class hierarchy, enums, param blocks)
# usage: python -I gen_sot_reflection.py <old-sot investigation dir> <sot-dump SDK dir> <out file>
import json, re, sys, os, glob

inv, sdk, out = sys.argv[1:4]
classes = {}   # unprefixed -> (prefixed, super unprefixed)
funcs = {}     # (owner, name) -> (flags, ret, [(mode, type, name)])
enums = {}     # name -> [(value, enumerator)]

def strip_kw(t):
    t = re.sub(r'\b(class|struct|enum)\s+', '', t).strip()
    return re.sub(r'\s+([*&])', r'\1', t)

def split_top(s):
    parts, depth, cur = [], 0, ''
    for ch in s:
        if ch in '<(': depth += 1
        elif ch in '>)': depth -= 1
        if ch == ',' and depth == 0: parts.append(cur); cur = ''
        else: cur += ch
    if cur.strip(): parts.append(cur)
    return [p.strip() for p in parts]

# --- sot-dump: classes with supers, enums, function parameter blocks (fallback signatures)
for f in glob.glob(os.path.join(sdk, 'Classes', '*.hpp')):
    text = open(f, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'^class (\w+)(?: : public (\w+))?\s*$', text, re.M):
        name, sup = m.group(1), m.group(2) or ''
        classes.setdefault(name[1:], (name, sup[1:] if sup else ''))
    for m in re.finditer(r'// Function flags: (0x[0-9a-f]+)[^\n]*\nstruct (\w+?)_(\w+)_Params\n\{\n(.*?)\n\};', text, re.S):
        flags, cls_, fn, body = int(m.group(1), 16), m.group(2), m.group(3), m.group(4)
        # the params struct name is <PrefixedClass>_<Function>_Params; class names can contain '_', find the split via known classes
        full = cls_ + '_' + fn
        owner = None
        for k in range(len(full)):
            if full[k] == '_' and full[:k] in [c[0] for c in []]: pass
        params, ret = [], 'void'
        for line in body.split('\n'):
            lm = re.match(r'\s*(.+?) (\w+)(\[\d+\])?;\s*//', line)
            if not lm or lm.group(2).startswith('pad_'): continue
            t, n = strip_kw(lm.group(1)), lm.group(2)
            if n == 'ReturnValue': ret = t
            else: params.append(('v', t, n))
        funcs.setdefault(('?' + full, ''), ('s' if flags & 0x2000 else '', ret, params))

old_classes = json.load(open(os.path.join(inv, 'native_code', 'depot', 'sdk_class_field_index.json'), encoding='utf-8'))
for k in old_classes:
    if k[1:] not in classes: classes[k[1:]] = (k, '')

# resolve '?<Prefixed>_<Function>' param blocks to (owner, function) using the class list
prefixed = {v[0]: k for k, v in classes.items()}
resolved = {}
for (key, _), v in funcs.items():
    full = key[1:]
    for k in range(len(full) - 1, 0, -1):
        if full[k] == '_' and full[:k] in prefixed:
            resolved[(prefixed[full[:k]], full[k + 1:])] = v
            break
funcs = resolved

# --- old-sot depot_2020 signatures win (same build as the game files, const / out refs kept)
sig = json.load(open(os.path.join(inv, 'native_code', 'depot', 'sdk_ufunction_signature_index.json'), encoding='utf-8'))
for key, decls in sig.items():
    owner, name = key.split('::', 1)
    d = decls[0].strip()
    m = re.match(r'^(static\s+)?(.*?)\s*\b' + re.escape(name) + r'\((.*)\)\s*(const)?\s*;?$', d)
    if not m: continue
    flags = ('s' if m.group(1) else '') + ('c' if m.group(4) else '')
    ret = strip_kw(m.group(2)) or 'void'
    params = []
    for p in split_top(m.group(3)):
        pm = re.match(r'^(.*?)\s*(\w+)$', p)
        if not pm: continue
        t = pm.group(1).strip()
        const = t.startswith('const ')
        if const: t = t[6:]
        is_obj = t.startswith('class ')
        t = strip_kw(t)
        mode = 'v'
        # this SDK writes out parameters as pointers (objects are 'class X*', so an extra '*' marks them) and
        # non-const references for in/out struct parameters
        if t.endswith('&'): t = t[:-1]; mode = 'r' if const else 'o'
        elif t.endswith('**') or (t.endswith('*') and not is_obj): t = t[:-1]; mode = 'o'
        params.append((mode, t, pm.group(2)))
    # that SDK moves out parameters to the end; the parameter block of the newer dump keeps the declaration order
    # (the order the bytecode passes arguments in)
    newer = funcs.get((owner, name))
    if newer:
        order = [n for _, _, n in newer[2]]
        if sorted(order) == sorted(n for _, _, n in params):
            params.sort(key=lambda p: order.index(p[2]))
        elif any(m == 'o' for m, _, _ in params):
            flags += 'u'
    elif any(m == 'o' for m, _, _ in params):
        flags += 'u'   # out parameters whose place in the argument list is unknown
    funcs[(owner, name)] = (flags, ret, params)

# --- native enums (the /Game ones only have NewEnumeratorN names here; the game files have the display names)
etext = open(os.path.join(sdk, 'Enums.hpp'), encoding='utf-8', errors='replace').read()
for m in re.finditer(r'// (/Script/[^\n]+)\nenum class (\w+) : \w+\n\{\n(.*?)\n\};', etext, re.S):
    vals = []
    for lm in re.finditer(r'\s*(\w+) = (-?\d+),', m.group(3)):
        if not lm.group(1).endswith('_MAX'): vals.append((int(lm.group(2)), lm.group(1)))
    enums[m.group(2)] = vals

with open(out, 'w', encoding='utf-8', newline='\n') as o:
    o.write('# SoT reflection for the Blueprint decompiler (generated by gen_sot_reflection.py, see README.md)\n')
    for k in sorted(classes):
        p, s = classes[k]
        o.write(f'C\t{k}\t{p}\t{s}\n')
    for (owner, name) in sorted(funcs):
        flags, ret, params = funcs[(owner, name)]
        o.write('F\t' + '\t'.join([owner, name, flags, ret] + [f'{m}|{t}|{n}' for m, t, n in params]) + '\n')
    for k in sorted(enums):
        o.write('E\t' + k + '\t' + '\t'.join(f'{v}={n}' for v, n in enums[k]) + '\n')
print(len(classes), 'classes', len(funcs), 'functions', len(enums), 'enums')
