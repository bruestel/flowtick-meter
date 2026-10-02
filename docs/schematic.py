# Generates schematic.png: circuit, pads on the Super Mini and a solder list.
# Run from this directory: python3 schematic.py (needs Pillow and DejaVu fonts).
from PIL import Image, ImageDraw, ImageFont
W,H=1950,1250
im=Image.new('RGB',(W,H),'white'); d=ImageDraw.Draw(im)
B='/usr/share/fonts/TTF/DejaVuSans-Bold.ttf'; N='/usr/share/fonts/TTF/DejaVuSans.ttf'
F=lambda s,b=False: ImageFont.truetype(B if b else N,s)
K=(30,30,30); RED=(200,30,30); BLK=(20,20,20); GRN=(0,130,60); GRY=(130,130,130); ORG=(220,120,0)
def line(pts,c=K,w=4): d.line(pts,fill=c,width=w,joint='curve')
def dot(x,y,c=K): d.ellipse([x-7,y-7,x+7,y+7],fill=c)
def txt(x,y,s,sz=26,c=K,b=False,anchor='la'): d.text((x,y),s,fill=c,font=F(sz,b),anchor=anchor)
def resistor(x,y1,y2,label):
    m=(y1+y2)/2; line([(x,y1),(x,m-45)]); line([(x,m+45),(x,y2)])
    d.rectangle([x-16,m-45,x+16,m+45],fill='white',outline=K,width=4); txt(x+30,m-14,label,26,K,True)
def cap(x,y1,y2,label):
    m=(y1+y2)/2; line([(x,y1),(x,m-8)]); line([(x,m+8),(x,y2)])
    line([(x-26,m-8),(x+26,m-8)],w=5); line([(x-26,m+8),(x+26,m+8)],w=5); txt(x+38,m-30,label,24); txt(x+38,m+2,"(optional)",22,GRY)
def diode(x,y1,y2):
    m=(y1+y2)/2; line([(x,y1),(x,m-22)]); line([(x,m+22),(x,y2)])
    d.polygon([(x-22,m-22),(x+22,m-22),(x,m+18)],outline=K,width=4); line([(x-22,m+20),(x+22,m+20)],K,5)
    for k in (0,1):
        ax,ay=x+32,m-18+k*22; line([(ax,ay),(ax+36,ay-18)],K,3); d.polygon([(ax+36,ay-18),(ax+23,ay-19),(ax+31,ay-7)],fill=K)
    txt(x-26,m-50,"A",22,K,True,'ra'); txt(x-26,m+26,"K",22,K,True,'ra')
def photo(xc,y1,y2):
    m=(y1+y2)/2; xb=xc-34
    line([(xb,m-30),(xb,m+30)],w=5)
    line([(xb,m-12),(xc,m-40)]); line([(xc,m-40),(xc,y1)])
    line([(xb,m+12),(xc,m+40)]); line([(xc,m+40),(xc,y2)])
    d.polygon([(xc,m+40),(xc-16,m+36),(xc-8,m+24)],fill=K)
    for k in (0,1):
        ax,ay=xb-72,m-34+k*24; line([(ax,ay),(ax+44,ay+16)],w=3); d.polygon([(ax+44,ay+16),(ax+30,ay+18),(ax+36,ay+6)],fill=K)
    txt(xc+18,m-70,"C",24,K,True); txt(xc+18,m+48,"E",24,K,True)

txt(60,30,"flowtick-meter: wiring",40,K,True)
txt(60,84,"ESP32-C6 Super Mini + Vishay TCRT5000L. Everything on 3.3 V. Values: Allmess example.",26,GRY)
top=200; bot=860
d.rounded_rectangle([190,320,700,665],radius=12,outline=ORG,width=3)
txt(440,632,"TCRT5000L (one package)",18,ORG,True,"ma")
line([(150,top),(900,top)],RED,5); txt(60,top-17,"3V3",30,RED,True)
line([(150,bot),(900,bot)],BLK,5); txt(60,bot-17,"GND",30,BLK,True)
xL=290; dot(xL,top); resistor(xL,top,310,"R1  560 Ω"); diode(xL,440,600)
line([(xL,310),(xL,440)]); line([(xL,600),(xL,bot)]); dot(xL,bot)
xc=590; dot(xc,top); photo(xc,top,600)
nody=740; line([(xc,600),(xc,nody)]); dot(xc,nody)
resistor(xc,nody,bot,"R2  1.56 kΩ"); txt(xc+30,(nody+bot)/2+16,"1 k + 560 in series",20,GRY); dot(xc,bot)
xC=840; line([(xc,nody),(xC,nody)],GRN,5); dot(xC,nody); cap(xC,nody,bot,"C1 100 nF"); dot(xC,bot)
line([(xC,nody),(xC,700),(1000,700)],GRN,5); d.polygon([(1000,688),(1024,700),(1000,712)],fill=GRN)
txt(1012,650,"GPIO1",30,GRN,True)
notes=["R1 and R2 have to be found for each meter: see docs/hardware.md,",
       "\"Choosing the resistors\". The values shown are those of the Allmess EVK 3/110 +m:",
       "R1 560 Ω → ~3.7 mA LED current (100 Ω / 20 mA saturated the phototransistor).",
       "R2 1.56 kΩ (1 k + 560 in series) turns the photocurrent into the voltage at GPIO1.",
       "Measured there: dark ≈ 500 mV, chrome ≈ 2900 mV, swing ≈ 2.4 V.",
       "Check the colour code! 100 kΩ instead of 100 Ω = LED practically off.",
       "C1 damps noise on the line; solder it right at GPIO1 / GND."]
y=950
for n in notes: txt(60,y,n,24); y+=38

ox=1200
txt(ox,150,"Super Mini, labelled back side, USB left",28,K,True)
by=280; bw,bh=680,300
d.rounded_rectangle([ox,by,ox+bw,by+bh],radius=18,fill=(40,40,40))
# The USB-C receptacle is on the other side; only its outline shows here.
d.rectangle([ox-50,by+bh/2-60,ox+30,by+bh/2+60],fill=(225,225,225),outline=GRY,width=2)
txt(ox-10,by+bh/2-10,"USB-C",18,GRY,True,'mm'); txt(ox-10,by+bh/2+12,"(front)",16,GRY,False,'mm')
# As printed on the board: 16/17 are TX/RX.
topp=["16","17","0","1","2","3","4","5","6","7"]; botp=["5V","GND","3V3","20","19","18","15","14","9","8"]
x0,dx=ox+80,62
use={'GND':BLK,'3V3':RED,'1':GRN}
for row,names,yy in ((0,topp,by+32),(1,botp,by+bh-32)):
    for i,n in enumerate(names):
        x=x0+i*dx; c=use.get(n,(215,180,70)); r=20 if n in use else 15
        d.ellipse([x-r,yy-r,x+r,yy+r],fill=c,outline=(255,255,255) if n in use else K,width=3)
        txt(x,yy+(34 if row==0 else -34),n,20,(245,245,245),n in use,'mm')
# Pads inside the outline: battery and the extra GPIOs, all unused here.
for n,ix,yy,sq in (("B−",1.0,by+118,True),("B+",1.0,by+182,True),("23",7.5,by+105,False),
                   ("21",7.3,by+bh-150,False),("22",8.3,by+bh-168,False),("12",8,by+bh-112,False),("13",9,by+bh-112,False)):
    x=x0+ix*dx
    if sq: d.rectangle([x-12,yy-9,x+12,yy+9],fill=(200,170,70))
    else: d.ellipse([x-10,yy-10,x+10,yy+10],fill=(200,170,70),outline=K,width=2)
    txt(x+(22 if sq else 16),yy,n,17,(200,200,200),False,'lm')
txt(ox+bw*0.47,by+bh/2,"ESP32-C6 Super Mini",24,(240,240,240),True,'mm')
line([(x0+3*dx,by+12),(x0+3*dx,by-40)],GRN,4); txt(x0+3*dx,by-48,"GPIO1",24,GRN,True,'md')
line([(x0+dx,by+bh-12),(x0+dx,by+bh+40)],BLK,4); txt(x0+dx-6,by+bh+48,"GND",24,BLK,True,'ra')
line([(x0+2*dx,by+bh-12),(x0+2*dx,by+bh+40)],RED,4); txt(x0+2*dx+6,by+bh+48,"3V3",24,RED,True,'la')
txt(ox,by+bh+92,"Seen from the component side the two rows swap.",20,GRY)
ty=by+bh+150
txt(ox,ty,"Solder list",30,K,True); ty+=50
rows=[("from","to",""),("3V3 pad","R1 → anode (A)",RED),("3V3 pad","collector (C)",RED),
      ("cathode (K)","GND pad",BLK),("emitter (E)","GPIO1 pad",GRN),("emitter (E)","R2 → GND pad",BLK),
      ("GPIO1 pad","C1 → GND pad (optional)",GRY)]
for i,(a,b,c) in enumerate(rows):
    y=ty+i*46; bold=(i==0)
    if i: d.rectangle([ox,y-2,ox+22,y+22],outline=K,width=2)
    if c: line([(ox+40,y+10),(ox+80,y+10)],c,6)
    txt(ox+100,y,a,24,K,bold); txt(ox+330,y,b,24,K,bold)
txt(ox,ty+len(rows)*46+20,"TCRT5000L: LED side = the end with the chamfered corners.",22,GRY)
txt(ox,ty+len(rows)*46+52,"Not sure? Diode test: LED ≈ 1.1–1.3 V, red probe on A.",22,GRY)
txt(ox,ty+len(rows)*46+84,"C sits on the side of A, E on the side of K.",22,GRY)
im.save('schematic.png')
