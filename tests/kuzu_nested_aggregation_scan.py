"""Flag rewritten Kuzu text where a WITH alias wraps an aggregate in a scalar
(CASE/CAST/arithmetic) and a later aggregate reads that alias (Kuzu rejects it)."""
import json, re, sys
AGG = r'\b(sum|count|min|max|avg|collect)\s*\('
def items(projection):
    out, depth, cur = [], 0, ''
    for ch in projection:
        depth += ch in '([{'; depth -= ch in ')]}'
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
    out.append(cur)
    return out
rep = json.load(open(sys.argv[1]))
for w in rep:
    for q in w['queries']:
        for variant in ('baseline', 'optimized'):
            c = (q.get(variant) or {}).get('cypher') if (q.get(variant) or {}).get('rewritten') else None
            if not c:
                continue
            segs = re.split(r'\bWITH\b|\bRETURN\b', c, flags=re.I)
            for i, seg in enumerate(segs[1:], 1):
                proj = re.split(r'\b(MATCH|OPTIONAL|UNWIND|ORDER|WHERE|LIMIT|SKIP)\b', seg, flags=re.I)[0]
                for item in items(proj):
                    m = re.match(r'\s*(.*?)\s+AS\s+`?(\w+)`?\s*$', item, re.S | re.I)
                    if not m:
                        continue
                    expr, alias = m.groups()
                    if re.search(AGG, expr, re.I) and not re.match(r'\s*' + AGG + r'.*\)\s*$', expr, re.I | re.S) \
                            or re.match(r'\s*(CASE|CAST)\b', expr, re.I) and re.search(AGG, expr, re.I):
                        later = ' '.join(segs[i + 1:])
                        if re.search(AGG + r'[^()]*\b' + alias + r'\b', later, re.I):
                            print(w['workload'], q['query'], variant, alias, '<-', expr[:120])
print('scan done')
