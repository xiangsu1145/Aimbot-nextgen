#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import sys, numpy as np, onnx, onnxruntime as ort

MODELS = {
    "m256 (256-yellow, OK)":   "m256.onnx",
    "m320 (320-backflash, BUG)": "m320.onnx",
}

ELEM = {1:"FLOAT", 10:"FLOAT16", 7:"INT64", 11:"DOUBLE"}

def op_that_produces(graph, out_name):
    for node in graph.node:
        if out_name in node.output:
            return node.op_type
    return None

def inspect(path):
    m = onnx.load(path)
    g = m.graph
    # inputs
    inp = g.input[0]
    inshape = [d.dim_value if d.dim_value else d.dim_param
               for d in inp.type.tensor_type.shape.dim]
    intype  = ELEM.get(inp.type.tensor_type.elem_type, str(inp.type.tensor_type.elem_type))
    # outputs
    out = g.output[0]
    outshape = [d.dim_value if d.dim_value else d.dim_param
                for d in out.type.tensor_type.shape.dim]
    outtype  = ELEM.get(out.type.tensor_type.elem_type, str(out.type.tensor_type.elem_type))
    print(f"  input  : {inp.name}  shape={inshape}  {intype}")
    print(f"  output : {out.name}  shape={outshape}  {outtype}")
    # walk back from output to see if there's an activation before it
    prod = op_that_produces(g, out.name)
    print(f"  output produced by op: {prod}")
    # last few nodes
    print("  last 8 nodes:")
    for n in g.node[-8:]:
        print(f"    {n.op_type:12s} -> {list(n.output)}")
    return intype, outtype, inshape

def run_and_probe(path, intype, outtype, inshape):
    sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    inname = sess.get_inputs()[0].name
    # build dims from actual inshape (substitute 256 for any unknown/dynamic axis)
    dims = [1,3,256,256]
    if len(inshape) == 4:
        dims = [d if isinstance(d,int) and d and d>0 else (256 if i in (2,3) else d) for i,d in enumerate(inshape)]
        # fix any leftover non-positive
        dims = [ (d if (isinstance(d,int) and d>0) else 256) for d in dims ]
    np_t = np.float16 if intype == "FLOAT16" else np.float32
    rng = np.random.default_rng(0)
    for tag, arr in [
        ("zeros",    np.zeros(dims, dtype=np_t)),
        ("midgray",  np.full(dims, 128 if np_t==np.float32 else np.float16(128), dtype=np_t)),
        ("random",   (rng.standard_normal(dims).astype(np.float32)*30).astype(np_t)),
    ]:
        out = sess.run(None, {inname: arr})[0]   # [1, C, N] or [1, N, C]
        o = out.astype(np.float32)
        print(f"\n  [{tag}] raw out shape {o.shape}  global min/mean/max = "
              f"{o.min():.4f} / {o.mean():.4f} / {o.max():.4f}")
        # figure out layout: assume channels-first [C,N] since axes end in (C,N) or (N,C)
        # detect smaller last axis = channels
        a, b = o.shape[-2], o.shape[-1]
        if a < b:
            C, N, cf = a, b, True     # channels-first
        else:
            C, N, cf = b, a, False
        o = o.reshape(C, N) if cf else o.reshape(N, C).T  # make it [C, N]
        print(f"    layout: {'channels-first' if cf else 'rows'}  C={C} N={N}")
        # first 6 anchors raw
        print(f"    first 6 anchors raw [x,y,w,h,c0..c{C-1}] (C={C}):")
        for i in range(min(6, N)):
            print("      " + " ".join(f"{o[c,i]:9.4f}" for c in range(C)))
        cls = o[4:, :]            # class channels [ncls, N]
        ncls = C - 4
        mx = cls.max(axis=0)      # per-anchor max class (no sigmoid)
        print(f"    class channels = {ncls}")
        print(f"    per-anchor max class  (raw, no sigmoid): min/mean/max = "
              f"{mx.min():.4f}/{mx.mean():.4f}/{mx.max():.4f}")
        smx = 1.0/(1.0+np.exp(-cls))  # sigmoid
        sm = smx.max(axis=0)
        print(f"    per-anchor max class  (sigmoid):       min/mean/max = "
              f"{sm.min():.4f}/{sm.mean():.4f}/{sm.max():.4f}")
        for thr in (0.3,0.5,0.9):
            print(f"      #pass@thr {thr}: raw={int((mx>thr).sum())}  sigmoid={int((sm>thr).sum())}")

def main():
    for name, path in MODELS.items():
        print("="*78)
        print("MODEL:", name, "=>", path)
        intype, outtype, inshape = inspect(path)
        print("-"*78)
        run_and_probe(path, intype, outtype, inshape)

if __name__ == "__main__":
    main()
