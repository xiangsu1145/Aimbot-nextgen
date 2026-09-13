import onnxruntime as ort, numpy as np
from PIL import Image
img = Image.open("screen.png").convert("RGB")
W,H = img.size
# letterbox to 256
tw,th=256,256
import math
scale=min(tw/W, th/H)
nw,nh=int(W*scale),int(H*scale)
canvas=np.full((th,tw,3),114.0,dtype=np.float32)
oy=(th-nh)//2; ox=(tw-nw)//2
arr=np.asarray(img.resize((nw,nh))).astype(np.float32)
canvas[oy:oy+nh, ox:ox+nw]=arr
x=(canvas/255.0)[None].transpose(0,3,1,2).astype(np.float32)  # NCHW
sess=ort.InferenceSession("wazise.onnx", providers=["CPUExecutionProvider"])
out=sess.run(None, {sess.get_inputs()[0].name:x})[0]
print("raw out shape", out.shape, out.dtype)
# rows format: [1, M, 7]
M=out.shape[1]
o=out[0]  # [M,7]
for c in range(7):
    col=o[:,c]
    print(f"col{c}: min={col.min():.4f} max={col.max():.4f} mean={col.mean():.4f} any>1? {np.any(col>1.05)}")
print("rows where col4>0.5:", int((o[:,4]>0.5).sum()))
print("rows where col5>0.5:", int((o[:,5]>0.5).sum()))
print("rows where col6>0.5:", int((o[:,6]>0.5).sum()))
# x,y,w,h ranges
print("x range", o[:,0].min(), o[:,0].max())
print("y range", o[:,1].min(), o[:,1].max())
print("w range", o[:,2].min(), o[:,2].max())
print("h range", o[:,3].min(), o[:,3].max())
