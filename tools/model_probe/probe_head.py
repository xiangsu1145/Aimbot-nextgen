import onnxruntime as ort, numpy as np

def run_probe(path):
    m=ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    inname=m.get_inputs()[0].name
    inshape=list(m.get_inputs()[0].shape)
    intype=m.get_inputs()[0].type
    H=inshape[2] if inshape[1]==3 else inshape[1]
    W=inshape[3] if inshape[1]==3 else inshape[2]
    nchw = (inshape[1]==3)
    if intype=="tensor(float16)":
        x=np.full((1,3,H,W),0.5,dtype=np.float16)
    else:
        x=np.full((1,3,H,W),0.5,dtype=np.float32)
    out=m.run(None,{inname:x})[0]
    if out.dtype!=np.float32:
        out=out.astype(np.float32)
    a,b=out.shape[1],out.shape[2]
    if a<b:
        cf=True; channels=a; anchors=b
    else:
        cf=False; channels=b; anchors=a
    # build a 2D accessor matching C++ Reader::at
    # C++: channelsFirst ? data[ch*anchors + i] : data[i*channels + ch]
    data=out.reshape(out.shape[1], out.shape[2])  # (a,b)
    def at(i,ch):
        return data[ch,i] if cf else data[i,ch]
    def countHead(v5,numClasses,conf=0.25):
        firstClass=5 if v5 else 4
        avail=channels-firstClass
        if avail<1: return 0
        classes=min(numClasses,avail)
        n=0
        for i in range(anchors):
            best=-1.0
            for c in range(classes):
                s=at(i,firstClass+c)
                if s>best: best=s
            if v5:
                best*=at(i,4)
            if best>=conf: n+=1
        return n
    v8=countHead(False,channels-4)
    v5=(countHead(True,channels-5) if channels>=6 else 10**9)
    print(f"=== {path}")
    print(f"  {intype} nchw={nchw} H={H} W={W} raw{out.shape} cf={cf} channels={channels} anchors={anchors}")
    print(f"  blank v8Count={v8}  v5Count={v5}")
    flip = (v5!=10**9) and (v5 <= 2) and (v8 > anchors//50)
    print(f"  -> would FLIP to V5? {flip}")

for f in ["m256.onnx","m320.onnx","wazise.onnx"]:
    run_probe(f)
