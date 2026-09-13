import onnx
from collections import Counter
for path in ["m256.onnx","m320.onnx"]:
    m=onnx.load(path); g=m.graph
    ops=Counter(n.op_type for n in g.node)
    prod={o:n for n in g.node for o in n.output}
    def feeds_output0_sigmoid():
        # does any input of the final Concat that produces output0 come from a Sigmoid?
        out=g.output[0].name
        def walk(t,seen=None):
            if seen is None: seen=set()
            if t in seen: return False
            seen.add(t)
            n=prod.get(t)
            if n is None: return False
            if n.op_type=="Sigmoid": return True
            return any(walk(i,seen) for i in n.input)
        n=prod.get(out)
        if n is None: return None
        return any(walk(i) for i in n.input)
    # also: is the class branch (2nd Concat input) sigmoided?
    print(f"{path}: Conv={ops.get('Conv')} Sigmoid={ops.get('Sigmoid')} Softmax={ops.get('Softmax')}")
    print(f"   final-output fed by a Sigmoid path? {feeds_output0_sigmoid()}")
