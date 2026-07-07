#!/usr/bin/env python3
"""GLB → .ham 変換ツール(ネイティブ版用の軽量バイナリ形式)

形式(リトルエンディアン):
  char[4]  "HAM1"
  uint32   vertexCount
  uint32   indexCount
  uint32   texW, texH          (0,0 = テクスチャなし)
  float32  vertexCount × 8     (pos.xyz, normal.xyz, uv.xy)
  uint32   indexCount
  uint8    texW*texH*3         (RGB, 上から下)

・全プリミティブを1メッシュに統合(ノードのワールド変換を適用)
・足元y=0・高さ1に正規化(Web版のloadModelPartsと同じ)
・basecolorテクスチャのみ抽出、指定サイズに縮小
・UVはglTFのまま(GLの先頭行=v=0と一致するため反転不要)

使い方: python3 glb2ham.py input.glb output.ham [texsize]
"""
import sys, json, struct, io
import numpy as np
from PIL import Image

def mat_identity():
    return np.identity(4, dtype=np.float64)

def mat_from_node(node):
    if 'matrix' in node:
        return np.array(node['matrix'], dtype=np.float64).reshape(4,4).T
    m = mat_identity()
    t = node.get('translation',[0,0,0])
    r = node.get('rotation',[0,0,0,1])
    s = node.get('scale',[1,1,1])
    x,y,z,w = r
    R = np.array([
      [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w),   0],
      [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w),   0],
      [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y), 0],
      [0,0,0,1]], dtype=np.float64)
    T = mat_identity(); T[:3,3] = t
    S = np.diag([s[0],s[1],s[2],1.0])
    return T @ R @ S

def read_accessor(js, buf, idx):
    acc = js['accessors'][idx]
    bv = js['bufferViews'][acc['bufferView']]
    comp_size = {5120:1,5121:1,5122:2,5123:2,5125:4,5126:4}[acc['componentType']]
    ncomp = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[acc['type']]
    dt = {5120:np.int8,5121:np.uint8,5122:np.int16,5123:np.uint16,5125:np.uint32,5126:np.float32}[acc['componentType']]
    start = bv.get('byteOffset',0) + acc.get('byteOffset',0)
    stride = bv.get('byteStride', comp_size*ncomp)
    out = np.zeros((acc['count'], ncomp), dtype=dt)
    for i in range(acc['count']):
        o = start + i*stride
        out[i] = np.frombuffer(buf, dtype=dt, count=ncomp, offset=o)
    return out

def main():
    src, dst = sys.argv[1], sys.argv[2]
    texsize = int(sys.argv[3]) if len(sys.argv)>3 else 256
    data = open(src,'rb').read()
    assert data[:4]==b'glTF'
    jlen = struct.unpack('<I', data[12:16])[0]
    js = json.loads(data[20:20+jlen])
    # BINチャンク
    off = 20+jlen
    blen = struct.unpack('<I', data[off:off+4])[0]
    buf = data[off+8:off+8+blen]

    # ノードのワールド変換を計算
    world = {}
    def walk(idx, parent):
        node = js['nodes'][idx]
        m = parent @ mat_from_node(node)
        world[idx] = m
        for c in node.get('children',[]):
            walk(c, m)
    scene = js['scenes'][js.get('scene',0)]
    for root in scene['nodes']:
        walk(root, mat_identity())

    # 全メッシュ統合
    V=[]; N=[]; UV=[]; I=[]; base=0
    for ni,node in enumerate(js['nodes']):
        if 'mesh' not in node: continue
        m = world.get(ni, mat_identity())
        nm = np.linalg.inv(m[:3,:3]).T  # 法線用
        for prim in js['meshes'][node['mesh']]['primitives']:
            pos = read_accessor(js, buf, prim['attributes']['POSITION']).astype(np.float64)
            pos4 = np.hstack([pos, np.ones((len(pos),1))])
            pos = (m @ pos4.T).T[:,:3]
            if 'NORMAL' in prim['attributes']:
                nrm = read_accessor(js, buf, prim['attributes']['NORMAL']).astype(np.float64)
                nrm = (nm @ nrm.T).T
                ln = np.linalg.norm(nrm,axis=1,keepdims=True); ln[ln==0]=1
                nrm = nrm/ln
            else:
                nrm = np.zeros_like(pos); nrm[:,1]=1
            if 'TEXCOORD_0' in prim['attributes']:
                uv = read_accessor(js, buf, prim['attributes']['TEXCOORD_0']).astype(np.float64)
                # V反転なし: glTFはv=0が画像上端、GLも先頭アップロード行がv=0で一致
            else:
                uv = np.zeros((len(pos),2))
            idxs = read_accessor(js, buf, prim['indices']).astype(np.uint32).flatten()
            V.append(pos); N.append(nrm); UV.append(uv)
            I.append(idxs + base)
            base += len(pos)
    V=np.vstack(V); N=np.vstack(N); UV=np.vstack(UV); I=np.concatenate(I)

    # 正規化: 足元y=0・高さ1・XZ中心
    mn=V.min(axis=0); mx=V.max(axis=0)
    h = mx[1]-mn[1] or 1.0
    V[:,0]-= (mn[0]+mx[0])/2; V[:,1]-= mn[1]; V[:,2]-= (mn[2]+mx[2])/2
    V/=h

    # basecolorテクスチャ
    texw=texh=0; texdata=b''
    imgs = js.get('images',[])
    pick = None
    for im in imgs:
        if 'basecolor' in im.get('name','').lower(): pick=im; break
    if pick is None and imgs: pick=imgs[0]
    if pick is not None:
        bv = js['bufferViews'][pick['bufferView']]
        raw = buf[bv.get('byteOffset',0): bv.get('byteOffset',0)+bv['byteLength']]
        img = Image.open(io.BytesIO(raw)).convert('RGB')
        img = img.resize((texsize,texsize), Image.LANCZOS)
        texw=texh=texsize
        texdata = img.tobytes()

    with open(dst,'wb') as f:
        f.write(b'HAM1')
        f.write(struct.pack('<IIII', len(V), len(I), texw, texh))
        inter = np.hstack([V, N, UV]).astype('<f4')
        f.write(inter.tobytes())
        f.write(I.astype('<u4').tobytes())
        f.write(texdata)
    print(f"{src} -> {dst}: verts={len(V)} tris={len(I)//3} tex={texw}x{texh} size={((20+len(V)*32+len(I)*4+len(texdata))/1024):.0f}KB")

if __name__ == '__main__':
    main()
