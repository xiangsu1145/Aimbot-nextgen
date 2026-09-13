import onnx, sys
def trace(path):
    m = onnx.load(path); g = m.graph
    print("="*70); print(path)
    # all op types
    from collections import Counter
    ops = Counter(n.op_type for n in g.node)
    print("op histogram:", dict(ops))
    # find output0 producers
    out = g.output[0]
    oname = out.name
    # map output tensor -> node
    prod = {}
    for n in g.node:
        for o in n.output:
            prod[o] = n
    # walk back from a tensor
    def back(t, depth=0, seen=None):
        if seen is None: seen=set()
        if t in seen or depth>12: return
        seen.add(t)
        n = prod.get(t)
        tag = f"{'  '*depth}{t} <- {n.op_type if n else 'INPUT'}"
        if n and any(o2.startswith('/model.23/Sigmoid') or n.op_type=='Sigmoid' for o2 in n.output):
            tag += "   <<< SIGMOID HERE"
        print(tag)
        if n:
            for i in n.input:
                back(i, depth+1, seen)
    print("output0", oname, "produced by", prod.get(oname).op_type if prod.get(oname) else "?")
    back(oname)
trace("m256.onnx")
trace("m320.onnx")
