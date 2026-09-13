import onnx, numpy as np, onnxruntime as ort
from PIL import Image

target_map = {"m256.onnx": 256, "m320.onnx": 320}

def load_img(path):
    img = Image.open(path).convert('RGB')
    return np.asarray(img, dtype=np.float32)  # HWC 0..255

def letterbox(arr, target):
    # Faithful-ish replica of preprocess.cpp: keepAspect, scale=1/255, pad 114/255, RGB, NCHW output.
    H, W, _ = arr.shape
    scale = min(target / W, target / H)
    newW = max(1, int(round(W * scale)))
    newH = max(1, int(round(H * scale)))
    padX = (target - newW) / 2.0
    padY = (target - newH) / 2.0
    s = 1.0 / 255.0
    pad = 114.0 / 255.0
    out = np.full((target, target, 3), pad, dtype=np.float32)
    stepX = W / newW
    stepY = H / newH
    originX = -padX * stepX
    originY = -padY * stepY
    for y in range(target):
        sy0 = originY + stepY * y
        sy1 = sy0 + stepY
        y0 = max(0, int(np.floor(sy0)))
        y1 = min(H - 1, int(np.ceil(sy1)) - 1)
        if y1 < y0:
            continue
        for x in range(target):
            sx0 = originX + stepX * x
            sx1 = sx0 + stepX
            x0 = max(0, int(np.floor(sx0)))
            x1 = min(W - 1, int(np.ceil(sx1)) - 1)
            if x1 < x0:
                continue
            block = arr[y0:y1 + 1, x0:x1 + 1, :]
            out[y, x, :] = block.mean(axis=(0, 1)) * s
    # to NCHW
    out = out.transpose(2, 0, 1)  # [3,target,target]
    return out, (scale, padX, padY, newW, newH, W, H)

def trace_final(path):
    m = onnx.load(path)
    g = m.graph
    prod = {o: n for n in g.node for o in n.output}
    out = g.output[0].name
    n = prod[out]
    print(f"  final node: {n.op_type}  inputs={list(n.input)}")
    for inp in n.input:
        p = prod.get(inp)
        if p is None:
            print(f"    in '{inp}' -> INPUT/CONST")
        else:
            print(f"    in '{inp}' -> {p.op_type}")
            for i2 in p.input:
                p2 = prod.get(i2)
                print(f"        {p.op_type} in '{i2}' -> {p2.op_type if p2 else 'INPUT/CONST'}")

def run(path, img, target, conf=0.5):
    sess = ort.InferenceSession(path, providers=['CPUExecutionProvider'])
    inname = sess.get_inputs()[0].name
    intype = sess.get_inputs()[0].type
    print(f"\n=== {path}  input type = {intype} ===")
    trace_final(path)
    arr, geo = letterbox(img, target)
    if intype == 'tensor(float16)':
        feed = arr[None].astype(np.float16)
    else:
        feed = arr[None].astype(np.float32)
    out = sess.run(None, {inname: feed})[0]  # [1, C, N]
    print(f"  raw output dtype={out.dtype} shape={out.shape}")
    if out.dtype == np.float16:
        out32 = out.astype(np.float32)
    else:
        out32 = out.astype(np.float32)
    C, N = out32.shape[1], out32.shape[2]
    print(f"  channels={C} anchors={N}")
    # channel stats
    names = ['cx', 'cy', 'bw', 'bh']
    for c in range(min(4, C)):
        ch = out32[0, c, :]
        print(f"    ch{c} {names[c]:>3}: min={ch.min():.4f} max={ch.max():.4f} mean={ch.mean():.4f}")
    if C > 4:
        cls = out32[0, 4:, :]
        print(f"    cls channels {C-4}: min={cls.min():.4f} max={cls.max():.4f} mean={cls.mean():.4f}")
        # fraction above conf
        above = (cls > conf).any(axis=0)
        print(f"    anchors with any class> {conf}: {above.sum()} / {N}")
        # top score distribution
        best = cls.max(axis=0)
        for thr in [0.1, 0.3, 0.5, 0.7, 0.9]:
            print(f"      anchors best-class> {thr}: {(best>thr).sum()}")
    return out32

img = load_img("screen.png")
print("screenshot", img.shape)
for f, t in target_map.items():
    run(f, img, t)
