# drawChar : draw single char @ (x, y)
# myPrint  : print string on stage use custom font
# rgb      : rgb(r, g, b[, a])

import hpprime as h

FILL = " " * 54
h.eval('G1:=AFiles("font_cascadia.png")')

def rgb(r, g, b, a=0):
    return a * 256 ** 3 + 65536 * r + 256 * g + b  

color2rgb = [0, 16777215,
rgb(255,180,0), rgb(255,255,0),
rgb(0,255,0),   rgb(90,90,255),
rgb(255,0,255), rgb(255,0,0)]

def drawChar(g, char, x, y, color):
    j = ord(char[0]) - 33 if isinstance(char, str) else 65536
    if j + 33 > 127:
        h.textout(g, x, y, char, color2rgb[color])
        return 10 if char in "≤≥≠▶αβ→∞°′″Σ−" else 15
    else:
        if x > 0:
            h.strblit2(g, x, y, 10, 18, 1, j*10, color*24, 10, 20)
        return 10
 
def myPrint(txt, x, y, grob, default_color, dat=""):
    k = 0
    while k < len(txt) and x <= 320:
        col = int(dat[k]) if k < len(dat) and dat[k] in "01234567" else default_color  
        if txt[k] != " ":
            if txt[k] == "_":
                h.line(grob,x,y+14,x+8,y+14,color2rgb[col])
                x += 10
            else: 
                x += drawChar(grob, txt[k], x, y, col); 
        else:
            x += 10
        k += 1

def mouse():
    if len(h.mouse()[0]) == 0:
        return -1
    x, y, status = h.mouse()[0][0], h.mouse()[0][1], h.eval('mouse(4)') 
    return [x, y, status]
 
def small(g, text, x, y, color):
    h.eval('TEXTOUT_P("{}",G{},{},{},{{"2D",0,{}}});'.format(FILL + text + FILL, g, x - 324, y, color))

def regular(g, text, x, y, color):
    h.eval('TEXTOUT_P("{}",G{},{},{},2,{});'.format(text, g, x, y, color))

def getPrograms():
    return h.eval("Programs()") 

def loadProgram(name):
    return h.eval('Programs("{}")'.format(name)).split(chr(10))  