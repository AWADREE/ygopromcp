#!/usr/bin/env python3
"""Original deterministic fallback UI sprites for YGOPro. Python stdlib + pinned cjpeg.
All shapes below are authored geometric primitives. No inherited art is read.
"""
from __future__ import annotations
import hashlib, json, os, pathlib, shutil, struct, subprocess, zlib

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT / "textures"
OUT.mkdir(exist_ok=True)
CJPEG = pathlib.Path(os.environ.get("YGOPROMCP_CJPEG") or shutil.which("cjpeg") or "cjpeg")
DARK=(6,16,25,255); PANEL=(12,30,42,255); CYAN=(81,220,230,255)
GOLD=(230,183,83,255); WHITE=(236,242,237,255); RED=(225,93,91,255)
GLYPHS={
"0":["111","101","101","101","111"],"1":["010","110","010","010","111"],"2":["111","001","111","100","111"],
"3":["111","001","111","001","111"],"4":["101","101","111","001","001"],"5":["111","100","111","001","111"],
"6":["111","100","111","101","111"],"7":["111","001","010","010","010"],"8":["111","101","111","101","111"],
"9":["111","101","111","001","111"],"?":["111","001","011","000","010"],
"A":["010","101","111","101","101"],"B":["110","101","110","101","110"],"C":["011","100","100","100","011"],
"D":["110","101","101","101","110"],"E":["111","100","110","100","111"],"F":["111","100","110","100","100"],
"G":["011","100","101","101","011"],"H":["101","101","111","101","101"],"I":["111","010","010","010","111"],
"K":["101","101","110","101","101"],"L":["100","100","100","100","111"],"M":["101","111","111","101","101"],
"N":["101","111","111","111","101"],"O":["010","101","101","101","010"],"P":["110","101","110","100","100"],
"R":["110","101","110","101","101"],"S":["011","100","010","001","110"],"T":["111","010","010","010","010"],
"U":["101","101","101","101","111"],"V":["101","101","101","101","010"],"X":["101","101","010","101","101"],
"Y":["101","101","010","010","010"]," ":["000"]*5
}
class Canvas:
 def __init__(self,w,h,bg=(0,0,0,0)): self.w,self.h=w,h; self.p=bytearray(bytes(bg)*(w*h))
 def px(self,x,y,c):
  x,y=int(x),int(y)
  if 0<=x<self.w and 0<=y<self.h:
   i=(y*self.w+x)*4;self.p[i:i+4]=bytes(c)
 def rect(self,x0,y0,x1,y1,c,stroke=0):
  for y in range(max(0,int(y0)),min(self.h,int(y1))):
   for x in range(max(0,int(x0)),min(self.w,int(x1))):
    if not stroke or x<x0+stroke or x>=x1-stroke or y<y0+stroke or y>=y1-stroke:self.px(x,y,c)
 def line(self,x0,y0,x1,y1,c,thick=1):
  x0,y0,x1,y1=map(int,(x0,y0,x1,y1));dx=abs(x1-x0);dy=-abs(y1-y0)
  sx=1 if x0<x1 else -1;sy=1 if y0<y1 else -1;err=dx+dy
  while True:
   self.rect(x0-thick//2,y0-thick//2,x0+(thick+1)//2,y0+(thick+1)//2,c)
   if x0==x1 and y0==y1:break
   e=err*2
   if e>=dy:err+=dy;x0+=sx
   if e<=dx:err+=dx;y0+=sy
 def circle(self,cx,cy,r,c,stroke=0):
  r2=r*r;inner=(r-stroke)*(r-stroke)
  for y in range(max(0,cy-r),min(self.h,cy+r+1)):
   for x in range(max(0,cx-r),min(self.w,cx+r+1)):
    d=(x-cx)**2+(y-cy)**2
    if d<=r2 and (not stroke or d>=inner):self.px(x,y,c)
 def text(self,s,cx,cy,scale,c):
  s=s.upper();width=sum((len(GLYPHS.get(ch,GLYPHS[" "])[0])+1)*scale for ch in s)-scale
  x0=int(cx-width/2);y0=int(cy-5*scale/2)
  for ch in s:
   g=GLYPHS.get(ch,GLYPHS[" "])
   for yy,row in enumerate(g):
    for xx,v in enumerate(row):
     if v=="1":self.rect(x0+xx*scale,y0+yy*scale,x0+(xx+1)*scale,y0+(yy+1)*scale,c)
   x0+=(len(g[0])+1)*scale
 def polygon(self,pts,c):
  ymin=max(0,min(y for x,y in pts));ymax=min(self.h-1,max(y for x,y in pts))
  for y in range(ymin,ymax+1):
   nodes=[]
   for i,(x1,y1) in enumerate(pts):
    x2,y2=pts[(i+1)%len(pts)]
    if (y1<=y<y2) or (y2<=y<y1):nodes.append(x1+(y-y1)*(x2-x1)/(y2-y1))
   nodes.sort()
   for i in range(0,len(nodes)-1,2):self.rect(int(nodes[i]),y,int(nodes[i+1])+1,y+1,c)
 def png(self,name):
  def chunk(tag,data):return struct.pack(">I",len(data))+tag+data+struct.pack(">I",zlib.crc32(tag+data)&0xffffffff)
  raw=b"".join(b"\0"+self.p[y*self.w*4:(y+1)*self.w*4] for y in range(self.h))
  result=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",self.w,self.h,8,6,0,0,0))+chunk(b"IDAT",zlib.compress(raw,9))+chunk(b"IEND",b"")
  (OUT/name).write_bytes(result)
 def jpg(self,name):
  if not CJPEG.is_file():raise RuntimeError("Pinned local cjpeg executable missing")
  ppm=b"P6\n%d %d\n255\n"%(self.w,self.h)+bytes(v for i,v in enumerate(self.p) if i%4!=3)
  with (OUT/name).open("wb") as f:
   p=subprocess.run([str(CJPEG),"-quality","92","-sample","1x1","-optimize"],input=ppm,stdout=f,stderr=subprocess.PIPE,check=True)
 def save(self,name):
  if name.endswith(".png"):self.png(name)
  elif name.endswith(".jpg"):self.jpg(name)
  else:raise ValueError(name)
def bg(name,mode):
 a=Canvas(1024,640)
 for y in range(a.h):
  for x in range(a.w):
   v=(x//80+y//70+mode*3)%9
   a.px(x,y,(5+v,15+v,25+v+(y//75),255))
 for k in range(12):
  y=45+k*50
  a.line(18,y,1005,y,(34,87,98,255),1)
  a.line(35+k*13,0,35+k*13,639,(27,64,79,255),1)
 for k in range(6):
  cx=100+k*162;cy=100+(k%2)*440
  a.circle(cx,cy,5,CYAN,2);a.line(cx+6,cy,cx+46,cy,GOLD,2)
 a.rect(26,22,998,618,GOLD,2);a.rect(36,32,988,608,CYAN,1)
 a.save(name)
for i,n in enumerate(("bg.jpg","bg_menu.jpg","bg_deck.jpg")):bg(n,i)
def field(name,rule,transparent=False):
 # The atlas spans vField world X=-1..9 and Y=-4..4 (materials.cpp).
 # Project zone corners into its existing UV pixels so the raster outline
 # follows the same world quads used for selection and card placement.
 w=256 if name=="field.png" else 320
 a=Canvas(w,256)
 def box(x0,y0,x1,y1):
  left=round((min(x0,x1)+1)*w/10);right=round((max(x0,x1)+1)*w/10)
  top=round((4-max(y0,y1))*32);bottom=round((4-min(y0,y1))*32)
  a.rect(left,top,right,bottom,(20,81,98,170),2)
 if not transparent:
  for i in range(5):
   x0=1.2+i*1.1;x1=2.3+i*1.1
   box(x0,0.8,x1,2.0);box(x0,2.0,x1,3.2)
   box(x0,-0.8,x1,-2.0);box(x0,-2.0,x1,-3.2)
 for x0,x1 in ((0.2,1.0),(6.9,7.7)):
  for y0,y1 in ((2.7,3.9),(1.4,2.6),(0.1,1.3),(-0.1,-1.3),(-1.4,-2.6),(-2.7,-3.9)):
   box(x0,y0,x1,y1)
 if rule:
  box(2.3,-0.6,3.4,0.6);box(4.5,-0.6,5.6,0.6)
 else:
  box(-0.8,0.1,0.0,1.3);box(7.9,-0.1,8.7,-1.3)
 a.save(name)
field("field.png",False)
field("field2.png",False)
field("field3.png",True)
field("field-transparent2.png",False,True)
field("field-transparent3.png",True,True)
def card(name,accent):
 a=Canvas(590,860,DARK)
 for off in (22,35,58):a.rect(off,off,590-off,860-off,accent if off!=35 else CYAN,4)
 a.rect(80,104,510,756,PANEL)
 a.polygon([(295,174),(442,430),(295,686),(148,430)],(30,72,82,255))
 a.polygon([(295,225),(412,430),(295,635),(178,430)],DARK)
 for k in range(8):
  yy=260+k*49;a.line(205,yy,385,yy,accent if k%2 else CYAN,2)
 a.circle(295,430,44,accent,5);a.circle(295,430,26,CYAN,2)
 a.save(name)
card("cover.png",GOLD);card("cover2.png",CYAN)
u=Canvas(177,254,DARK);u.rect(6,6,171,248,GOLD,2);u.rect(16,28,161,180,CYAN,2)
u.text("?",88,104,12,CYAN);u.text("NO ART",88,212,3,WHITE);u.save("unknown.jpg")
for name,label in (("f1.jpg","S"),("f2.jpg","R"),("f3.jpg","P")):
 a=Canvas(89,128,DARK);a.rect(4,4,85,124,GOLD,2);a.circle(44,58,31,CYAN,2)
 a.text(label,44,58,10,WHITE);a.save(name)
def badge(name,w,h,label,color):
 a=Canvas(w,h);a.circle(w//2,h//2,min(w,h)//2-5,color,3)
 a.text(label,w//2,h//2,max(2,min(w,h)//22),WHITE);a.save(name)
for n,lab,col in (("act.png","ACT",CYAN),("attack.png","ATK",GOLD),("chain.png","CHN",CYAN),
                   ("negated.png","X",RED)):badge(n,128,128,lab,col)
for n,lab in (("chaintarget.png","C"),("equip.png","E"),("target.png","T")):badge(n,34,34,lab,GOLD)
for n,label in (("lim.png",""),("ot.png","")):pass
a=Canvas(128,128)
for (x,y,s,c) in ((0,0,"0",RED),(64,0,"1",GOLD),(0,64,"2",CYAN)):
 a.circle(x+32,y+32,27,c,3);a.text(s,x+32,y+32,7,WHITE)
a.save("lim.png")
a=Canvas(128,256)
for idx,(lab,col) in enumerate((("OCG",GOLD),("TCG",CYAN),("OCG",CYAN),("TCG",GOLD))):
 y=idx*64;a.rect(3,y+3,125,y+61,col,3);a.text(lab,64,y+32,7,WHITE)
a.save("ot.png")
a=Canvas(320,256)
for i in range(20):
 x=(i%5)*62;y=(i//5)*62;a.text(str(i+1),x+31,y+31,7,WHITE)
a.save("number.png")
a=Canvas(16,80)
for j,col in enumerate((CYAN,GOLD,(107,190,139,255),(172,113,219,255),RED)):
 a.rect(0,j*16,16,(j+1)*16,col);a.rect(0,j*16,16,(j+1)*16,DARK,1)
a.save("lp.png")
a=Canvas(200,20);a.rect(0,0,200,20,GOLD,2);a.rect(4,4,196,16,PANEL);a.save("lpf.png")
a=Canvas(254,254);a.polygon([(0,253),(74,253),(254,0),(176,0)],(255,255,255,190));a.save("mask.png")
expected={"act.png","attack.png","bg.jpg","bg_deck.jpg","bg_menu.jpg","chain.png","chaintarget.png",
"cover.png","cover2.png","equip.png","f1.jpg","f2.jpg","f3.jpg","field-transparent2.png",
"field-transparent3.png","field.png","field2.png","field3.png","lim.png","lp.png","lpf.png",
"mask.png","negated.png","number.png","ot.png","target.png","unknown.jpg"}
actual={p.name for p in OUT.iterdir() if p.is_file()}
assert actual==expected,(sorted(expected-actual),sorted(actual-expected))
manifest={"schema_version":1,"authored_source":"generate_fallback.py","cjpeg":"installed UCRT64 cjpeg -quality 92 -sample 1x1 -optimize",
"assets":[{"path":"textures/"+p.name,"bytes":p.stat().st_size,"sha256":hashlib.sha256(p.read_bytes()).hexdigest().upper()}
for p in sorted(OUT.iterdir()) if p.is_file()]}
(ROOT/"MANIFEST.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
print("created",len(actual),"original fallback textures")
