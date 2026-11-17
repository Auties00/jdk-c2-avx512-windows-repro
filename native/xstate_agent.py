#!/usr/bin/env python3
"""Decodes the records written by xstate_agent.c and prints what separates short contexts from complete ones.

A context is "dropped" when XGETBV(1) in the handler reports an AVX/AVX-512 component (bits 2, 5, 6, 7) that the
XSTATE mask of the context does not have.

MIT License.
"""
import collections
import struct
import sys

FMT = '<QIIQQQQQQIIQQQiiiIIIIIQQ16Q48sQQQ2Q'
SIZE = struct.calcsize(FMT)
FIELDS = ('tsc tid kind rip rsp addr access ctx_mask live ctx_flags mxcsr eflags xstate_bv xcomp_bv all_off '
          'legacy_off xstate_off all_len legacy_len xstate_len cs_before cs_after stack_limit stack_base').split()
VECTOR = 0xe4


def load(path):
    data = open(path, 'rb').read()
    out = []
    for i in range(0, len(data) - SIZE + 1, SIZE):
        v = struct.unpack_from(FMT, data, i)
        r = dict(zip(FIELDS, v[:len(FIELDS)]))
        r['gpr'] = v[len(FIELDS):len(FIELDS) + 16]
        r['code'] = v[len(FIELDS) + 16]
        r['nested_mask'], r['nested_live'], r['ctx_addr'] = v[len(FIELDS) + 17:len(FIELDS) + 20]
        out.append(r)
    return out


def hist(rows, key, top=12):
    c = collections.Counter(key(r) for r in rows)
    return ' '.join('%s:%d' % (k, n) for k, n in c.most_common(top))


def main():
    recs = load(sys.argv[1])
    label = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1]
    if not recs:
        print('%s: no records' % label)
        return
    # the polling page is the address most traps hit
    page = collections.Counter(r['addr'] for r in recs).most_common(1)[0][0]
    polls = [r for r in recs if r['addr'] == page]
    prev = {}
    for r in polls:
        p = prev.get(r['tid'])
        r['cs_delta'] = (r['cs_before'] - p['cs_after']) if p else -1
        r['dt'] = (r['tsc'] - p['tsc']) if p else -1
        r['prev_dropped'] = p['dropped'] if p else None
        r['dropped'] = bool(r['live'] & ~r['ctx_mask'] & VECTOR)
        prev[r['tid']] = r
    dropped = [r for r in polls if r['dropped']]
    kept = [r for r in polls if not r['dropped']]
    print('%s: records=%d polls=%d dropped=%d kept=%d' % (label, len(recs), len(polls), len(dropped), len(kept)))
    for name, rows in (('dropped', dropped), ('kept', kept)):
        if not rows:
            continue
        print(' %s:' % name)
        print('  ctx_mask     %s' % hist(rows, lambda r: hex(r['ctx_mask'])))
        print('  live_xinuse  %s' % hist(rows, lambda r: hex(r['live'])))
        print('  xstate_bv    %s' % hist(rows, lambda r: hex(r['xstate_bv'])))
        print('  xcomp_bv     %s' % hist(rows, lambda r: hex(r['xcomp_bv'])))
        print('  ctx layout   %s' % hist(rows, lambda r: 'all=%d/%d xs=%d/%d' % (r['all_off'], r['all_len'], r['xstate_off'], r['xstate_len']), 6))
        print('  flags        %s' % hist(rows, lambda r: hex(r['ctx_flags'])))
        print('  mxcsr        %s' % hist(rows, lambda r: hex(r['mxcsr'])))
        print('  eflags       %s' % hist(rows, lambda r: hex(r['eflags'])))
        print('  rip          %s' % hist(rows, lambda r: hex(r['rip'])))
        print('  rsp%%64       %s' % hist(rows, lambda r: r['rsp'] % 64))
        print('  ctx_addr%%64  %s' % hist(rows, lambda r: r['ctx_addr'] % 64))
        print('  rsp-ctx      %s' % hist(rows, lambda r: r['rsp'] - r['ctx_addr']))
        print('  tid          %s' % hist(rows, lambda r: r['tid']))
        print('  cs_delta     %s' % hist(rows, lambda r: r['cs_delta']))
        print('  cs in handler %s' % hist(rows, lambda r: r['cs_after'] - r['cs_before']))
        print('  prev_dropped %s' % hist(rows, lambda r: r['prev_dropped']))
        print('  nested_mask  %s' % hist(rows, lambda r: hex(r['nested_mask'])))
        print('  nested_live  %s' % hist(rows, lambda r: hex(r['nested_live'])))
        print('  stack room   %s' % hist(rows, lambda r: (r['rsp'] - r['stack_limit']) >> 12))
        dts = sorted(r['dt'] for r in rows if r['dt'] >= 0)
        if dts:
            print('  tsc since previous poll of the thread: min=%d p10=%d median=%d p90=%d max=%d' % (
                dts[0], dts[len(dts) // 10], dts[len(dts) // 2], dts[len(dts) * 9 // 10], dts[-1]))
    by_rip = collections.defaultdict(lambda: [0, 0, None])
    for r in polls:
        e = by_rip[r['rip']]
        e[0 if r['dropped'] else 1] += 1
        e[2] = r
    print(' per rip (dropped/kept, code at rip-32..rip+16, a sample of the registers):')
    for rip, (d, k, r) in sorted(by_rip.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:14]:
        print('  %x %d/%d %s' % (rip, d, k, r['code'].hex()))
        print('     gpr %s' % ' '.join('%x' % g for g in r['gpr']))
    others = [r for r in recs if r['addr'] != page]
    if others:
        print(' other access violations: %d; ctx_mask %s' % (len(others), hist(others, lambda r: hex(r['ctx_mask']))))


if __name__ == '__main__':
    main()
