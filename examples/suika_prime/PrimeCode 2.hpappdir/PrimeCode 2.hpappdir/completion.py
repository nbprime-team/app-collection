# editor_completion.py
from hpprime import *
from myprint import *

from const import *
from utils import char_width, line_pixel_width

class CompletionState:
    def __init__(self):
        self.visible = False
        self.prefix = ''
        self.items = []
        self.sel = 0
        self.scroll = 0

def collect_identifiers(src_code):
    idents = set()
    for line in src_code:
        i = 0
        n = len(line)
        while i < n:
            ch = line[i]
            if ch.isalpha() or ch == '_':
                start = i
                i += 1
                while i < n and (isalnum(line[i]) or line[i] == '_'):
                    i += 1
                idents.add(line[start:i])
            else:
                i += 1
    return idents

def get_completions_from_code(src_code, lang, prefix):
    idents = collect_identifiers(src_code)
    if lang == 'python':
        pool = PYTHON_KEYWORDS | PYTHON_BUILTINS | idents
        return sorted([w for w in pool if w.startswith(prefix)])
    else:
        pool = HPPL_KEYWORDS | HPPL_BUILTINS | idents
        up = prefix.upper()
        return sorted([w for w in pool if w.upper().startswith(up)])

def trigger_completion(state):
    row = state.cursor_row
    col = state.cursor_col
    if row >= len(state.src_code):
        return

    line = state.src_code[row]
    start = col
    while start > 0 and (isalnum(line[start - 1]) or line[start - 1] == '_'):
        start -= 1
    prefix = line[start:col]

    if not prefix:
        state.completion = None
        return

    items = get_completions_from_code(state.src_code, state.lang, prefix)
    if not items:
        state.completion = None
        return

    comp = CompletionState()
    comp.visible = True
    comp.prefix = prefix
    comp.items = items
    comp.sel = 0
    comp.scroll = 0
    state.completion = comp

def apply_completion(state, index):
    comp = state.completion
    if not comp or index < 0 or index >= len(comp.items):
        return

    word = comp.items[index]
    row = state.cursor_row
    line = state.src_code[row]
    start = state.cursor_col - len(comp.prefix)
    if start < 0:
        start = 0

    new_line = line[:start] + word + line[state.cursor_col:]
    state.src_code[row] = new_line
    state.cursor_col = start + len(word)
    state.completion = None
    state.highlight_cache.pop(row, None)

def get_completion_rect(state, text_x, text_y, text_w):
    comp = state.completion
    max_w = 0
    for c in comp.items:
        w = sum(char_width(ch) for ch in c)
        if w > max_w:
            max_w = w

    menu_w = min(max_w + 10, text_w)
    visible_items = min(len(comp.items) - comp.scroll, MAX_COMPLETION_ITEMS)
    menu_h = visible_items * LINE_HEIGHT + 4
    
    content_top = TITLE_BAR_H
    cursor_y = content_top + (state.cursor_row * LINE_HEIGHT - state.scroll_y)
    cursor_x = text_x + line_pixel_width(state.src_code[state.cursor_row],
                                         state.scroll_col, state.cursor_col)
    menu_x = cursor_x
    menu_y = cursor_y + LINE_HEIGHT
    if menu_y + menu_h > state.screen_h:
        menu_y = cursor_y - menu_h
    if menu_x + menu_w > state.screen_w:
        menu_x = state.screen_w - menu_w
    if menu_x < 0:
        menu_x = 0
    if menu_y < TITLE_BAR_H:
        menu_y = TITLE_BAR_H

    return menu_x, menu_y, menu_w, menu_h

def draw_completion(grob, state, text_x, text_y, text_w):
    comp = state.completion
    if not comp or not comp.visible or not comp.items:
        return

    menu_x, menu_y, menu_w, menu_h = get_completion_rect(state, text_x, text_y, text_w)
    fillrect(grob, menu_x, menu_y, menu_w, menu_h, COLOR_MENU_BORDER, COLOR_MENU_BG)

    visible_count = (menu_h - 4) // LINE_HEIGHT
    for i in range(visible_count):
        idx = comp.scroll + i
        if idx >= len(comp.items):
            break

        item_y = menu_y + 2 + i * LINE_HEIGHT

        if idx == comp.sel:
            fillrect(grob, menu_x + 1, item_y, menu_w - 2, LINE_HEIGHT,
                     COLOR_MENU_SEL_BG, COLOR_MENU_SEL_BG)

        txt = comp.items[idx]
        dat = ''
        for k in range(len(txt)):
            if k < len(comp.prefix):
                dat += '5'
            else:
                dat += '1'

        myPrint(txt, menu_x + 4, item_y, grob,
                COLOR_MENU_TEXT if idx != comp.sel else COLOR_MENU_SEL_TEXT, dat)