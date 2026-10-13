#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""Read every debug.log under the given directories, group the DEBUG_LOCKORDER reports, and fail on any
whose inverted pair involves Yellowback -- except the one pair that is provably not a deadlock:
the miner holds cs_yellowback (TemplateView, miner.cpp) across TestBlockValidity, which takes the
script-check queue's ControlMutex, while ConnectTip takes ControlMutex before CheckConnect takes
cs_yellowback. Both paths hold cs_main from their first line, so they are mutually exclusive and
the inversion cannot deadlock; the structural fix (drop the TemplateView before TestBlockValidity)
is a one-line change in the frozen miner.cpp, recorded as an open item (role-based plan F-37).
Every other report is printed and counted; the inherited getpeerinfo/SendMessages pair is expected.

A report "names Yellowback" when its text matches YELLOWBACK_TOKENS: the overlay's lock names and the
source files of the lock sites (a report lists each held lock as "<name>  <file>:<line>", file relative
to src/). Before audit 07/F-11 the test was the substring 'yellowback' or 'yed_', which the vault
primitive's lock sites (vault/node.cpp, rpc/vault.cpp) never contain, so a vault inversion counted as
inherited and passed. Note that DEBUG_LOCKORDER only tracks the LOCK()-family critical sections:
VaultDB::cs and VaultState::cs are std::mutex under std::lock_guard and never appear in a report.

--selftest feeds synthetic debug.log snippets through the same judge and exits 0 only if each verdict
is the expected one (a vault inversion fails, the F-37 pair is allowed, the stock pair is inherited);
qa/yellowback-audit.sh runs it, so the filter cannot silently regress again."""
import glob, os, re, sys, tempfile

# Why each token is here (audit 07/F-11): 'yellowback' -- src/yellowback/, rpc/yellowback*.cpp and
# cs_yellowback; 'yed_' -- the module's RPC names in a lock name or file; '(^|[\s/])vault/' -- the
# vault primitive's sources (src/vault/*.cpp report as "vault/node.cpp"); 'vault\w*\.cpp' --
# rpc/vault.cpp and any vault-named source elsewhere; 'VaultDB' / 'VaultState' / 'cs_vault' -- a vault
# lock by name, should one become a tracked critical section. Function names (the set_* / vault_* RPC
# frames) never appear in a report, which names only lock variables and file:line, so they are not
# tokens. A token match is printed with each failing report.
YELLOWBACK_TOKENS = re.compile(r'yellowback|yed_|(?:^|[\s/])vault/|vault\w*\.cpp|VaultDB|VaultState|cs_vault')

PAY = re.compile(r'^\S+ \d+ [0-9:.]+ +\w+ [A-Za-z]*:? ?(?:main: )?(.*)$')
ALLOWED = ({'cs_yellowback', 'ControlMutex'},)

def reports(path):
    lines = open(path, errors='replace').read().splitlines()
    for i, l in enumerate(lines):
        if 'POTENTIAL DEADLOCK DETECTED' not in l:
            continue
        blk = []
        for m in lines[i + 1:i + 40]:
            mm = PAY.match(m)
            p = mm.group(1) if mm else ''
            if p.startswith((' ', 'Previous', 'Current')):
                blk.append(p.rstrip())
            else:
                break
        yield blk

def marked(blk):
    """The two locks of the inversion: the lines that follow a (1) / (2) marker."""
    names = set()
    for j, l in enumerate(blk):
        if l.strip() in ('(1)', '(2)') and j + 1 < len(blk):
            names.add(blk[j + 1].split()[0].split('->')[-1].split('.')[-1].strip('&'))
    return names

def judge(roots):
    """(total, allowed, failing {text: count}, matched token per failing text)."""
    total, allowed, distinct, token = 0, 0, {}, {}
    for root in roots:
        for f in glob.glob(os.path.join(root, '**', 'debug.log'), recursive=True):
            for blk in reports(f):
                total += 1
                text = '\n'.join(blk)
                m = YELLOWBACK_TOKENS.search(text)
                if not m:
                    continue
                pair = marked(blk)
                if any(a <= pair for a in ALLOWED):
                    allowed += 1
                    continue
                distinct[text] = distinct.get(text, 0) + 1
                token[text] = m.group(0).strip()
    return total, allowed, distinct, token

def _report(pre, a, b):
    """A synthetic v4.5.0 report: `a` and `b` are (lock, file:line) pairs, (1) and (2)."""
    ts = 'Oct 12 10:00:00.000  INFO main: '
    lines = ['POTENTIAL DEADLOCK DETECTED', 'Previous lock order was:']
    for (lock, where), mark in ((pre, None), (a, '(1)'), (b, '(2)')):
        if mark:
            lines.append(' ' + mark)
        lines.append(' %s  %s' % (lock, where))
    lines.append('Current lock order is:')
    for (lock, where), mark in ((pre, None), (b, '(2)'), (a, '(1)')):
        if mark:
            lines.append(' ' + mark)
        lines.append(' %s  %s' % (lock, where))
    return '\n'.join(ts + l for l in lines) + '\n' + ts + 'UpdateTip: new best=00\n'

SELFTEST = (
    # (name, report, expected verdict: 'fail' | 'allowed' | 'inherited')
    ('vault inversion: mempool.cs (vault/node.cpp) vs cs_wallet (rpc/vault.cpp)',
     _report(('cs_main', 'rpc/vault.cpp:1044'), ('pool.cs', 'vault/node.cpp:218'), ('pwalletMain->cs_wallet', 'rpc/vault.cpp:1044')), 'fail'),
    ('vault lock by name: VaultDB::cs vs cs_main',
     _report(('cs_vNodes', 'net.cpp:1700'), ('cs_main', 'main.cpp:3300'), ('VaultDB::cs', 'db.cpp:143')), 'fail'),
    ('the F-37 pair (cs_yellowback / ControlMutex under cs_main)',
     _report(('cs_main', 'miner.cpp:150'), ('cs_yellowback', 'yellowback/module.cpp:80'), ('ControlMutex', 'checkqueue.h:178')), 'allowed'),
    ('the stock getpeerinfo / SendMessages pair',
     _report(('cs_vSend', 'net.cpp:900'), ('cs_main', 'rpc/net.cpp:94'), ('cs_vNodes', 'net.cpp:1714')), 'inherited'),
)

def selftest():
    ok = True
    with tempfile.TemporaryDirectory() as d:
        for i, (name, text, want) in enumerate(SELFTEST):
            os.makedirs(os.path.join(d, str(i)))
            with open(os.path.join(d, str(i), 'debug.log'), 'w') as f:
                f.write(text)
            total, allowed, distinct, token = judge([os.path.join(d, str(i))])
            got = 'fail' if distinct else 'allowed' if allowed else 'inherited'
            ok = ok and total == 1 and got == want
            print('%s  %s: %s%s' % ('ok  ' if total == 1 and got == want else 'FAIL', name, got,
                                    (' (token %r)' % next(iter(token.values()))) if token else ''))
    print('lockorder-check self-test: %s' % ('passed' if ok else 'FAILED'))
    return 0 if ok else 1

if sys.argv[1:] == ['--selftest']:
    sys.exit(selftest())
roots = sys.argv[1:] or ['.']
total, allowed, distinct, token = judge(roots)
bad = sum(distinct.values())
print('potential deadlock reports: %d total, %d naming Yellowback allow-listed (cs_yellowback/ControlMutex under cs_main), %d failing' % (total, allowed, bad))
for text, n in sorted(distinct.items(), key=lambda x: -x[1]):
    print('---- x%d (matched %r)\n%s' % (n, token[text], text))
sys.exit(1 if bad else 0)
