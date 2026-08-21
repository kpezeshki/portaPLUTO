import gzip, struct
data=gzip.open("/usr/share/consolefonts/Lat15-VGA8.psf.gz","rb").read()
# Detect PSF1 (magic 0x36 0x04) or PSF2 (0x72 0xb5 0x4a 0x86)
if data[0]==0x36 and data[1]==0x04:
    mode,ch=data[2],data[3]           # PSF1: charsize=height, 8px wide
    h=ch; w=8; hdr=4; nglyph=512 if (mode&1) else 256
    glyphbytes=h
    print("PSF1 h=%d nglyph=%d"%(h,nglyph))
else:
    magic,ver,hdrsz,flags,ng,glyphbytes,h,w=struct.unpack("<8sIIIIIII"[:1]+"IIIIIII",data[:32]) if False else (None,)*8
    # parse PSF2
    ver,hdrsz,flags,ng,glyphbytes,h,w=struct.unpack("<IIIIIII",data[4:32])
    hdr=hdrsz; nglyph=ng
    print("PSF2 w=%d h=%d nglyph=%d bytes/glyph=%d"%(w,h,nglyph,glyphbytes))
rowbytes=(w+7)//8
def glyph(i):
    off=hdr+i*glyphbytes
    rows=[]
    for r in range(h):
        rows.append(data[off+r*rowbytes])   # first byte = leftmost 8 px
    return rows
# emit 8x8: take rows; if h>8, we still want 8 -> but VGA8 is h=8
H=min(h,8)
with open("pp_font.c","w") as o:
    o.write('#include "pp_screen.h"\n/* IBM VGA 8x8 (Lat15-VGA8.psf), ASCII 0x20-0x7f */\n')
    o.write("const uint8_t PP_FONT8[96][8] = {\n")
    for code in range(0x20,0x80):
        g=glyph(code)
        rows=[g[r] if r<H else 0 for r in range(8)]
        o.write("  {"+",".join("0x%02x"%b for b in rows)+"}, // 0x%02x %r\n"%(code,chr(code)))
    o.write("};\n")
print("wrote pp_font.c (h=%d w=%d)"%(h,w))
