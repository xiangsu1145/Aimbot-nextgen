import onnx
for path in ["m256.onnx","m320.onnx"]:
    m=onnx.load(path); g=m.graph
    prod={o:n for n in g.node for o in n.output}
    out=g.output[0].name
    n=prod[out]
    print("="*60); print(path, "final node:", n.op_type, "inputs:", list(n.input))
    for inp in n.input:
        p=prod.get(inp)
        if p is None:
            print(f"  input '{inp}' -> (graph input / constant)")
        else:
            print(f"  input '{inp}' -> produced by {p.op_type}")
            # go one more level if trivial
            if p.op_type in ("Sigmoid","Mul","Add","Div","Sub","Reshape","Transpose","Split","Concat"):
                for i2 in p.input:
                    p2=prod.get(i2)
                    print(f"      {p.op_type} input '{i2}' -> {p2.op_type if p2 else 'INPUT/CONST'}")
