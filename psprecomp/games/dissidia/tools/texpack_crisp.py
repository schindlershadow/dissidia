import numpy as np
from PIL import Image, ImageFilter
def crisp(o, s=4, k=10.0):
    """Faithful upscale for text/flat UI art: 2x-supersampled Lanczos, sigmoid contrast on each channel's local range, downsample."""
    w,h=o.size
    big=o.resize((w*s*2,h*s*2),Image.LANCZOS)
    a=np.asarray(big).astype(np.float32)/255.0
    # premultiply to avoid fringes
    al=a[...,3:4]; rgb=a[...,:3]*al
    # local range via min/max filters (radius ~ 1 source pixel * 2s)
    r=s
    def sharpen(ch):
        im=Image.fromarray((np.clip(ch,0,1)*255).astype(np.uint8))
        lo=np.asarray(im.filter(ImageFilter.MinFilter(2*r+1))).astype(np.float32)/255
        hi=np.asarray(im.filter(ImageFilter.MaxFilter(2*r+1))).astype(np.float32)/255
        lo=np.asarray(Image.fromarray((lo*255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(r/2))).astype(np.float32)/255
        hi=np.asarray(Image.fromarray((hi*255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(r/2))).astype(np.float32)/255
        rng=np.maximum(hi-lo,1e-3)
        t=(ch-lo)/rng
        t=1/(1+np.exp(-k*(t-0.5)))
        return lo+t*rng*np.where(rng>0.08,1,0)+ch*np.where(rng>0.08,0,1)
    out=np.concatenate([np.stack([sharpen(rgb[...,c]) for c in range(3)],-1), sharpen(al[...,0])[...,None]],-1)
    alp=np.clip(out[...,3:4],1e-4,1)
    res=np.concatenate([np.clip(out[...,:3]/alp,0,1),np.clip(out[...,3:4],0,1)],-1)
    im=Image.fromarray((res*255+0.5).astype(np.uint8),'RGBA')
    return im.resize((w*s,h*s),Image.LANCZOS)
