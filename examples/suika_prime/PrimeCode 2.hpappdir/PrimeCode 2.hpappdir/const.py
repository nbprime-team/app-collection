# editor_constants.py
COLOR_BG           = 0x000000
COLOR_TEXT         = 0xffffff
COLOR_LINE_NUM     = 0xffffff
COLOR_LINE_NUM_BG  = 0x000060
COLOR_CURSOR       = 0xff0000
COLOR_SCROLL_BG    = 0x000060
COLOR_SCROLL_THUMB = 0xcccccc
COLOR_MENU_BG      = 0x002064
COLOR_MENU_BORDER  = 0x002064
COLOR_MENU_TEXT    = 1
COLOR_MENU_SEL_BG  = 0x754444ff
COLOR_MENU_SEL_TEXT= 1
COLOR_SELECT_BG    = 0x259178
COLOR_TITLE_TEXT   = 0x000001
COLOR_TITLE_BG     = 0x400000ff

LINE_HEIGHT       = 20
CHAR_W_ASCII      = 10
CHAR_W_OTHER      = 15
LINE_NUMBER_W     = 30
GUTTER            = 4
SCROLLBAR_W       = 10
MAX_COMPLETION_ITEMS = 8
TITLE_BAR_H       = 22

KEY_ENTER = "__enter__"
KEY_DEL   = "__del__"
KEY_ESC   = "__esc__"
KEY_CHARS = "__chars__"
KEY_UP    = "__up__"
KEY_DOWN  = "__down__"
KEY_LEFT  = "__left__"
KEY_RIGHT = "__right__"
KEY_HOME  = "__home__"
KEY_END   = "__end__"

# Python key
PYTHON_KEYWORDS = {
    'False','None','True','and','as','assert','async','await',
    'break','class','continue','def','del','elif','else','except',
    'finally','for','from','global','if','import','in','is',
    'lambda','nonlocal','not','or','pass','raise','return','try',
    'while','with','yield'
}

# Python builtin / obj
PYTHON_BUILTINS = {
    'print','len','range','abs','all','any','bin','bool','chr',
    'dict','dir','enumerate','eval','exec','filter','float','format',
    'frozenset','getattr','hasattr','hash','help','hex','id','input',
    'int','isinstance','issubclass','iter','list','map','max','min',
    'next','object','oct','open','ord','pow','property','repr',
    'reversed','round','set','setattr','slice','sorted','str','sum',
    'super','tuple','type','vars','zip','__import__'
}

# PPL key
HPPL_KEYWORDS = {
    'BEGIN','END','LOCAL','EXPORT','IF','THEN','ELSE','ELSEIF',
    'FOR','FROM','TO','STEP','DOWNTO','DO','WHILE','REPEAT','UNTIL',
    'BREAK','CONTINUE','RETURN','PRINT','INPUT','MSGBOX','WAIT',
    'FREEZE','CHOOSE','CASE','DEFAULT','ENDCASE','AND','OR','NOT',
    'XOR','PI','MIN','MAX','FLOOR','CEILING','ROUND','MOD','DIM',
    'SIZE','LIST','MATRIX','FUNCTION','PROGRAM','VIEW','START',
    'ENDVIEW'
}

# HPPL funs
HPPL_BUILTINS = {
    'PRINT','INPUT','MSGBOX','WAIT','FREEZE','CHOOSE','MIN','MAX',
    'FLOOR','CEILING','ROUND','MOD','SIZE','DIM','LIST','MATRIX'
}

def isalnum(n):
    return n.isalpha() or n.isdigit()