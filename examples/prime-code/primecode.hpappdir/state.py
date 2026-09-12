# editor_state.py
from const import *

class EditorState:
    def __init__(self, src_code, lang='python', screen_w=320, screen_h=240):
        self.src_code = src_code[:]
        self.lang = lang
        self.cursor_row = 0
        self.cursor_col = 0
        self.scroll_row = 0
        self.scroll_y   = 0
        self.scroll_col = 0
        self.selection_anchor = None
        self.completion = None
        self.screen_w = screen_w
        self.screen_h = screen_h
        self.highlight_cache = {}