import onnx, numpy as np, onnxruntime as ort
from PIL import Image

def load_img(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)

def letterbox(arr, target):
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
    return out.transpose(2, 0, 1), (scale, padX, padY, newW, newH, W, H)

def decode(out32, target, geo, conf=0.5, iou=0.45, maxdet=64):
    scale, padX, padY, newW, newH, W, H = geo
    C, N = out32.shape[1], out32.shape[2]
    # channels-first: data[channel*N + anchor]
    anchors, channels = N, C
    firstClass = 4
    available = channels - firstClass
    classes = available  # no declared classes -> channels-4
    boxes = []
    for a in range(anchors):
        best = -1.0
        bc = 0
        for c in range(classes):
            v = out32[0, firstClass + c, a]
            if v > best:
                best = v
                bc = c
        if best < conf:
            continue
        cx = out32[0, 0, a]
        cy = out32[0, 1, a]
        bw = out32[0, 2, a]
        bh = out32[0, 3, a]
        x1 = cx - bw * 0.5
        y1 = cy - bh * 0.5
        x2 = cx + bw * 0.5
        y2 = cy + bh * 0.5
        # toFrame: (x - padX)*inv + roiX ; inv = 1/scale, roi=0
        inv = 1.0 / scale
        fx1 = (x1 - padX) * inv
        fy1 = (y1 - padY) * inv
        fx2 = (x2 - padX) * inv
        fy2 = (y2 - padY) * inv
        ccx = (fx1 + fx2) * 0.5
        ccy = (fy1 + fy2) * 0.5
        if ccx < 0 or ccy < 0 or ccx >= W or ccy >= H:
            continue
        boxes.append([bc, best, fx1, fy1, fx2, fy2])
    # NMS per class (greedy)
    boxes.sort(key=lambda b: b[1], reverse=True)
    kept = []
    dead = [False] * len(boxes)
    for i in range(len(boxes)):
        if dead[i]:
            continue
        kept.append(boxes[i])
        for j in range(i + 1, len(boxes)):
            if dead[j] or boxes[i][0] != boxes[j][0]:
                continue
            # iou
            ix = max(0, min(boxes[i][4], boxes[j][4]) - max(boxes[i][2], boxes[j][2]))
            iy = max(0, min(boxes[i][5], boxes[j][5]) - max(boxes[i][3], boxes[j][3]))
            inter = max(0.0, ix) * max(0.0, iy)
            uni = (boxes[i][4]-boxes[i][2])*(boxes[i][5]-boxes[i][3]) + (boxes[j][4]-boxes[j][2])*(boxes[j][5]-boxes[j][3]) - inter
            if uni > 0 and inter / uni > iou:
                dead[j] = True
    return kept[:maxdet]

img = load_img("screen.png")
print("screenshot", img.shape)
for f in ["m_0.onnx","m_1.onnx","m_2.onnx","m_3.onnx","m_4.onnx","m_5.onnx"]:
    m = onnx.load(f)
    it = m.graph.input[0]
    ins = [d.dim_value for d in it.type.tensor_type.shape.dim]
    target = ins[2] if ins[1] == 3 else ins[1]  # NCHW
    sess = ort.InferenceSession(f, providers=['CPUExecutionProvider'])
    inname = sess.get_inputs()[0].name
    intype = sess.get_inputs()[0].type
    arr, geo = letterbox(img, target)
    feed = arr[None].astype(np.float16) if intype == 'tensor(float16)' else arr[None].astype(np.float32)
    out = sess.run(None, {inname: feed})[0]
    out32 = out.astype(np.float32)
    boxes = decode(out32, target, geo)
    # best score
    best = max((b[1] for b in boxes), default=0.0)
    print(f"{f}: in={intype.split('(')[1][:-1]} target={target} -> {len(boxes)} boxes, best={best:.3f}")
    for b in boxes[:5]:
        print(f"    cls={b[0]} score={b[1]:.3f} frame_box=({b[2]:.0f},{b[3]:.0f},{b[4]:.0f},{b[5]:.0f})")
