#!/usr/bin/env python3
import os
import re
import sys

FUNC_RE = re.compile(
    r'^(?:s32|u32|s64|u64|int|void|float)\s+'
    r'([A-Za-z_][A-Za-z0-9_]*)\s*'
    r'\(([^)]*)\)\s*$',
    re.M)

PARAM_PTR_RE = re.compile(r'(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\*+\s*([A-Za-z_][A-Za-z0-9_]*)\s*$')

SAFE_USE = re.compile(r'(?:uintptr_t\)\s*|vm_read\w*\(|vm_write\w*\(|gptr\(|gpath\(|guest_str\('
                      r'|g_ps3_guest_caller\(|yz_g2h\(|rtc_str\(|rtc_tick_read\(|rtc_tick_write\('
                      r'|GUEST_PTR\(|GUEST_EA\()')


def pointer_params(param_text):
    out = []
    for raw in param_text.split(','):
        raw = raw.strip()
        if not raw or raw == 'void':
            continue
        m = PARAM_PTR_RE.match(raw)
        if m:
            out.append(m.group(1))
    return out


def function_bodies(src):
    for m in FUNC_RE.finditer(src):
        brace = src.find('{', m.end())
        if brace < 0:
            continue
        depth = 0
        i = brace
        while i < len(src):
            if src[i] == '{':
                depth += 1
            elif src[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield (m.group(1), m.group(2), src.count('\n', 0, m.start()) + 1,
               src[brace:i])


DEREFERENCING = (
    'strcpy strncpy strcat strncat strlen strcmp strncmp strchr strrchr strstr'
    ' strdup sprintf snprintf sscanf atoi atol strtol strtoul strtod'
    ' memcpy memmove memset memcmp memchr'
    ' fopen freopen fread fwrite fputs fgets remove rename mkdir stat'
).split()

def bare_deref_calls(body, param):
    out = []
    for fn in DEREFERENCING:
        pat = re.escape(fn) + r"\s*\([^;]*?(?<![\w.>])" + re.escape(param) + r"\s*[,)]"
        for m in re.finditer(pat, body):
            nlpos = body.rfind(chr(10), 0, m.start()) + 1
            endpos = body.find(chr(10), m.start())
            line = body[nlpos:] if endpos < 0 else body[nlpos:endpos]
            if not SAFE_USE.search(m.group(0)) and not SAFE_USE.search(line):
                out.append(line)
    return out

ENTRY_PREFIXES = ('cell', 'sce', '_cell')

def is_guest_entry(name):
    if name.startswith('sys_') or name.startswith('_sys_'):
        return True
    for p in ENTRY_PREFIXES:
        if name.startswith(p) and len(name) > len(p):
            return name[len(p)].isupper() and '_' not in name
    return False

def param_is_translated(body, param):
    pat = (r"(?:^|[^\w])" + re.escape(param) + r"\s*=\s*"
           r"(?:GUEST_PTR|gptr|gpath|guest_str|vm_to_host|yz_g2h"
           r"|\w*_(?:host|g2h|xlat|ptr|str)|\w*_host_\w+)\s*\(")
    if re.search(pat, body):
        return True
    macro = r"(?:YZ_XLAT|GUEST_XLAT)\s*\(\s*" + re.escape(param) + r"\s*[,)]"
    if re.search(macro, body):
        return True
    rebind = r"(?:^|[^\w])" + re.escape(param) + r"\s*=\s*&\s*\w"
    return re.search(rebind, body) is not None

def strip_comments(src):
    out = []
    i, n = 0, len(src)
    while i < n:
        two = src[i:i + 2]
        if two == '/*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(c if c == '\n' else ' ' for c in src[i:j]))
            i = j
        elif two == '//':
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif src[i] == '"' or src[i] == "'":
            q = src[i]
            j = i + 1
            while j < n and src[j] != q and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(src[i:j])
            i = j
        else:
            out.append(src[i])
            i += 1
    return ''.join(out)


def suspects_in(path):
    src = open(path, 'rb').read().decode('utf-8', 'replace').replace('\r\n', '\n')
    src = strip_comments(src)
    found = []
    for name, params, line, body in function_bodies(src):
        if not is_guest_entry(name):
            continue
        ptrs = pointer_params(params)
        if not ptrs:
            continue
        bad = []
        for p in ptrs:
            if param_is_translated(body, p):
                continue
            uses = re.findall(r'^.*(?:\*\s*%s\b|\b%s\s*->).*$' % (p, p), body, re.M)
            uses = [u for u in uses if not SAFE_USE.search(u)]
            uses = [u for u in uses
                    if not re.search(r'[A-Za-z_][A-Za-z0-9_]*\s*\*+\s*%s\s*=' % p, u)]
            uses += bare_deref_calls(body, p)
            if uses:
                bad.append(p)
        if bad:
            found.append((line, name, bad))
    return found


def main():
    roots = sys.argv[1:] or ['libs']
    total = 0
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root.replace(chr(92), '/'))
            continue
        for dirpath, _dirs, fs in os.walk(root):
            for fn in sorted(fs):
                if fn.endswith('.c'):
                    files.append(os.path.join(dirpath, fn).replace(chr(92), '/'))
    for path in files:
        for line, name, bad in suspects_in(path):
            print('%s:%d  %s  derefs: %s' % (path, line, name, ', '.join(bad)))
            total += 1
    print('\n%d suspect function(s)' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
