# editor_render.py
from myprint import *
from hpprime import *
from const import *
from utils import *
from completion import draw_completion
from state import EditorState
import keys as k

def get_highlight(state, row):
    if row < 0 or row >= len(state.src_code):
        return ''
    line = state.src_code[row]
    cached = state.highlight_cache.get(row)
    if cached and cached[0] == line:
        return cached[1]
    dat = highlight_line(line, state.lang)
    state.highlight_cache[row] = (line, dat)
    return dat

def get_text_width(state):
    text_x = LINE_NUMBER_W + GUTTER
    return state.screen_w - text_x - SCROLLBAR_W - 2

def ensure_cursor_visible(state):
    num = len(state.src_code)
    content_h = state.screen_h - TITLE_BAR_H
    
    cursor_y_top = state.cursor_row * LINE_HEIGHT
    cursor_y_bottom = (state.cursor_row + 1) * LINE_HEIGHT
    
    view_top = state.scroll_y
    view_bottom = state.scroll_y + content_h
    
    if cursor_y_top < view_top:
        state.scroll_y = cursor_y_top
    elif cursor_y_bottom > view_bottom - 1:
        state.scroll_y = cursor_y_bottom - content_h
    
    text_w = get_text_width(state) 
    max_chars = max(1, text_w // CHAR_W_ASCII)
    if state.cursor_col < state.scroll_col:
        state.scroll_col = state.cursor_col 
    elif state.cursor_col >= state.scroll_col + max_chars:
        state.scroll_col = max(0, state.cursor_col - max_chars + 1) 
     
    adjust_scroll(state, 0, 0)

def adjust_scroll(state, visible_rows, text_w):
    num = len(state.src_code) 
    content_h = state.screen_h - TITLE_BAR_H
    max_scroll = max(0, num * LINE_HEIGHT - content_h)
    
    if state.scroll_y < 0:
        state.scroll_y = 0
    if state.scroll_y > max_scroll:
        state.scroll_y = max_scroll
    
    if state.scroll_col < 0:
        state.scroll_col = 0

def draw_scrollbars(grob, state, sb_x, content_h):
    num = len(state.src_code)
    if num == 0:
        return
    visible_rows = content_h // LINE_HEIGHT
    thumb_h = max(4, visible_rows * content_h // num)
    if num > visible_rows:
        max_scroll = num * LINE_HEIGHT - content_h 
        if max_scroll > 0:
            track_h = content_h - thumb_h
            thumb_offset = int(state.scroll_y * track_h / max_scroll) 
            thumb_y = TITLE_BAR_H + thumb_offset
        else:
            thumb_y = TITLE_BAR_H 
    else:
        thumb_y = TITLE_BAR_H 
    fillrect(grob, sb_x, thumb_y, SCROLLBAR_W, thumb_h,
             COLOR_SCROLL_THUMB, COLOR_SCROLL_THUMB)

def draw_cursor(grob, state, text_x, text_y):
    row = state.cursor_row
    if row < 0 or row >= len(state.src_code):
        return
        
    content_h = state.screen_h - TITLE_BAR_H
    y = text_y + (row * LINE_HEIGHT - state.scroll_y)
    
    if y < text_y or y > text_y + content_h - LINE_HEIGHT:
        return 
    
    line = state.src_code[row]
    x = text_x + line_pixel_width(line, state.scroll_col, state.cursor_col)
    if x < text_x:
        x = text_x
    fillrect(grob, x, y, 2, LINE_HEIGHT, COLOR_CURSOR, COLOR_CURSOR)

def get_selection_cols_for_row(state, row):
    if not state.selection_anchor:
        return None
    arow, acol = state.selection_anchor
    crow, ccol = state.cursor_row, state.cursor_col
    if arow == crow:
        if row == arow:
            return (min(acol, ccol), max(acol, ccol))
        return None

    start_row = min(arow, crow)
    end_row = max(arow, crow)
    if row < start_row or row > end_row:
        return None

    if row == start_row:
        if arow < crow:
            return (acol, len(state.src_code[row]))
        else:
            return (ccol, len(state.src_code[row]))
    elif row == end_row:
        if arow > crow:
            return (0, acol)
        else:
            return (0, ccol)
    else:
        return (0, len(state.src_code[row]))

def render_editor(grob, state):
    sw = state.screen_w
    sh = state.screen_h
    content_top = TITLE_BAR_H
    content_h = sh - TITLE_BAR_H
     
    text_x = LINE_NUMBER_W + GUTTER
    text_y = content_top
    text_w = sw - text_x - SCROLLBAR_W - 2
    visible_rows = content_h // LINE_HEIGHT

    adjust_scroll(state, visible_rows, text_w)

    fillrect(grob, 0, content_top, sw, content_h, COLOR_BG, COLOR_BG)
    blit(grob, 0, 0, 3)
    fillrect(grob, 0, content_top, LINE_NUMBER_W, content_h, COLOR_LINE_NUM_BG, COLOR_LINE_NUM_BG)

    sb_x = sw - SCROLLBAR_W - 1
    fillrect(grob, sb_x, content_top, SCROLLBAR_W, content_h, COLOR_SCROLL_BG, COLOR_SCROLL_BG)
    
    start_row = state.scroll_y // LINE_HEIGHT
    y_offset = state.scroll_y % LINE_HEIGHT
    y_start = content_top - y_offset
    
    num = len(state.src_code)
    i = 0
    y = y_start
    while y < content_top + content_h and start_row + i < num:
        row = start_row + i
        line = state.src_code[row]
        dat = get_highlight(state, row)

        sel = get_selection_cols_for_row(state, row)
        if sel:
            c1, c2 = sel
            x1 = text_x + line_pixel_width(line, state.scroll_col, c1)
            x2 = text_x + line_pixel_width(line, state.scroll_col, c2)
            x1 = max(text_x, x1)
            x2 = min(text_x + text_w, x2)
            if x2 > x1:
                fillrect(grob, x1, y, x2 - x1, LINE_HEIGHT,
                         COLOR_SELECT_BG, COLOR_SELECT_BG)

        disp_text, disp_dat, _, _ = build_visible_line(line, dat, text_w, state.scroll_col)
        if disp_text:
            myPrint(disp_text, text_x, y, grob, COLOR_TEXT, disp_dat)

        num_str = str(row + 1)
        num_w = len(num_str) * 6
        num_x = LINE_NUMBER_W - num_w - 2
        regular(grob, num_str, num_x - 1, y + 1, COLOR_LINE_NUM)

        i += 1
        y += LINE_HEIGHT

    draw_scrollbars(grob, state, sb_x, content_h)
    draw_cursor(grob, state, text_x, content_top)
    
    fillrect(grob, 0, 0, sw, TITLE_BAR_H, COLOR_TITLE_BG, COLOR_TITLE_BG)
    myPrint("PrimeCode", 4, 2, grob, 1)
    if k.isShift:
        small(grob, "shift", 260, 2, 0xccccff) 
    if k.isAlpha:
        small(grob, "alpha" if k.isAlpha == 1 else "alock", 260, 12, 0xffaa55) 
    small(grob, {"python":"py", "HPPL":"ppl"}[state.lang], 230, 2, 0xffffff) 
    small(grob, str(eval("memory(1)")), 230, 12, 0xffffff) 

    if state.completion and state.completion.visible:
        draw_completion(grob, state, text_x, text_y, text_w)

    return state