import hpprime as h

isShift, isAlpha = 0, 0

ids = [
14,15,16,17,18,19,
20,21,22,23,24,25,
26,27,28,29,30,
31,32,33,34,35,
   37,38,39,40,
   42,43,44,45,
   47,48,49,50
]

alp = [
"a","b","c","d","e","__del__",
"f","g","h","i","j","k",
"l","m","n","o","__enter__",
"p","q","r","s","t",
    "u","v","w","x",
    "y","z","#",":",
    '""',"."," ",";"
]


sha = [
"A","B","C","D","E","__del__",
"F","G","H","I","J","K",
"L","M","N","O","__enter__",
"P","Q","R","S","T",
    "U","V","W","X",
    "Y","Z","#",":",
    '""',"."," ",";"
]


shi = [
"__chars__","__ __","","","°′″","__del__",
" NTHROOT ","asin()","acos()","atan()","exp()","alog()",
"sqrt()","abs()","''","eval()","approx()",
":="," ","{}","!","1/",
    " ","[]","__sym__"," ",
    " ","i","pi","0x",
    " ","=","_","Ans"
]


norm = [
'__indent__','__tool__','',"x","/","__del__",
"^","sin()","cos()","tan()","ln()","log()",
"^2","-","()",",","__enter__",
"*10^","7","8","9","/",
    "4","5","6","*",
    "1","2","3","-",
    "0","."," ","+"
]


def getkey():
    return int(h.eval('getkey()'))

def keyEvent():
    global isShift, isAlpha
    key_id = getkey() 
    if key_id != -1:
        if key_id in ids:
            idx = ids.index(key_id)
            temp = {'00': norm, '01': alp, '02':alp,
                    '10': shi, '11': sha, '12':sha, 
                   }
            input_str = temp["{}{}".format(isShift, isAlpha)][idx]
            if isShift: isShift = 0 
            if isAlpha == 1: isAlpha = 0 
            return input_str 
        elif key_id == 4:
            return "__esc__"
        elif key_id == 41: 
            isShift = 1 - isShift
            return "__shift__"
        elif key_id == 36: 
            isAlpha = (isAlpha + 1) % 3
            return "__alpha__"
        elif key_id in [2, 12, 7, 8]:
            return {2:"__up__", 12:"__down__", 7:"__left__", 8:"__right__"}[key_id]
        else:
            return -1
    else:
        return -1 