# editor_input.py
from myprint import *
from keys import *

from const import *
from utils import char_width, pixel_to_col
from completion import get_completion_rect, apply_completion, trigger_completion
from render import get_selection_cols_for_row, ensure_cursor_visible

def handle_mouse(state, x, y, status):
    if y < TITLE_BAR_H and status != 2:
        return state
    
    content_top = TITLE_BAR_H
    content_h = state.screen_h - TITLE_BAR_H
    text_x = LINE_NUMBER_W + GUTTER
    text_y = TITLE_BAR_H
    text_w = state.screen_w - text_x - SCROLLBAR_W - 2
    visible_rows = state.screen_h // LINE_HEIGHT

    # click the comp menu
    if state.completion and state.completion.visible:
        comp = state.completion
        menu_x, menu_y, menu_w, menu_h = get_completion_rect(state, text_x, text_y, text_w)
        if x >= menu_x and x < menu_x + menu_w and y >= menu_y and y < menu_y + menu_h:
            idx = comp.scroll + (y - menu_y - 2) // LINE_HEIGHT
            if 0 <= idx < len(comp.items):
                apply_completion(state, idx)
            else:
                state.completion = None
            return state

    sb_x = state.screen_w - SCROLLBAR_W - 1
    if x >= sb_x:
        num = len(state.src_code)
        visible_rows = content_h // LINE_HEIGHT
        thumb_h = max(4, visible_rows * content_h // num)
        track_h = content_h - thumb_h 
        if track_h <= 0:
            state.scroll_y = 0
        else:
            offset = y - TITLE_BAR_H - thumb_h // 2
            if offset < 0:
                offset = 0
            elif offset > track_h:
                offset = track_h 
            max_scroll = max(0, num * LINE_HEIGHT - content_h)
            state.scroll_y = int(offset * max_scroll / track_h)
        return state

    if x < LINE_NUMBER_W:
        return state

    row = (state.scroll_y + (y - content_top)) // LINE_HEIGHT
    if row >= len(state.src_code):
        row = len(state.src_code) - 1
    if row < 0:
        row = 0
        
    if status == 2 and y >= content_top + content_h - LINE_HEIGHT and row < len(state.src_code) - 1:
        row += 1

    line = state.src_code[row]
    col = pixel_to_col(line, state.scroll_col, x - text_x)
    state.cursor_row = row
    state.cursor_col = col

    if status == 0:
        state.selection_anchor = None
    elif status == 2:
        if state.selection_anchor is None:
            state.selection_anchor = (row, col)
    
    ensure_cursor_visible(state)
    return state

def poll_mouse(state):
    ev = mouse()
    if ev == -1:
        return
    x, y, status = ev
    handle_mouse(state, x, y, status)

def handle_key(state, key):
    if key not in (KEY_ENTER, KEY_DEL, KEY_ESC, KEY_CHARS,
                   KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
                   KEY_HOME, KEY_END) and not '__' in key:
        if state.selection_anchor:
            delete_selection(state)

        row = state.cursor_row
        col = state.cursor_col
        line = state.src_code[row]
        new_line = line[:col] + key + line[col:]
        state.src_code[row] = new_line
        state.cursor_col += len(key)
        if key[-1] in ')]}"':
            # fun()
            state.cursor_col -= 1 
        state.selection_anchor = None
        state.highlight_cache.pop(row, None)

        if isalnum(key) or key == '_':
            trigger_completion(state)
        else:
            state.completion = None
        return

    elif key == KEY_ENTER:
        if state.completion and state.completion.visible:
            apply_completion(state, state.completion.sel)
            return
        if state.selection_anchor:
            delete_selection(state)

        row = state.cursor_row
        col = state.cursor_col
        line = state.src_code[row]
        left = line[:col]
        right = line[col:]

        indent = ''
        for ch in left:
            if ch == ' ' or ch == '\t':
                indent += ch
            else:
                break

        state.src_code[row] = left
        state.src_code.insert(row + 1, indent + right)
        
        state.cursor_row += 1
        state.cursor_col = len(indent)
        state.selection_anchor = None
        
        state.highlight_cache.clear()
        state.completion = None
        return

    elif key == KEY_DEL:
        if state.completion and state.completion.visible:
            state.completion = None
            return
        if state.selection_anchor:
            delete_selection(state)
            return

        row = state.cursor_row
        col = state.cursor_col
        if col > 0:
            line = state.src_code[row]
            left_part = line[:col]
            if col >= 4 and left_part.isspace():
                new_line = line[:col-4] + line[col:]
                state.src_code[row] = new_line
                state.cursor_col -= 4 
            else: 
                new_line = line[:col-1] + line[col:]
                state.src_code[row] = new_line
                state.cursor_col -= 1
            state.highlight_cache.pop(row, None)
        else:
            if row > 0:
                prev_line = state.src_code[row-1]
                curr_line = state.src_code[row]
                new_len = len(prev_line)
                state.src_code[row-1] = prev_line + curr_line
                del state.src_code[row]
                state.cursor_row = row - 1
                state.cursor_col = new_len
                state.highlight_cache.pop(row-1, None)
                state.highlight_cache.pop(row, None)
        state.selection_anchor = None
        return

    elif key == KEY_ESC:
        if state.completion and state.completion.visible:
            state.completion = None
        else:
            state.selection_anchor = None
        return

    elif key == KEY_CHARS:
        # 用户自定义字符菜单，这里只做一个占位
        pass

    elif key == KEY_UP:
        if state.completion and state.completion.visible:
            comp = state.completion
            if comp.sel > 0:
                comp.sel -= 1
                if comp.sel < comp.scroll:
                    comp.scroll = comp.sel
            return
        if state.cursor_row > 0:
            state.cursor_row -= 1
            line = state.src_code[state.cursor_row]
            if state.cursor_col > len(line):
                state.cursor_col = len(line)
            state.selection_anchor = None
        return

    elif key == KEY_DOWN:
        if state.completion and state.completion.visible:
            comp = state.completion
            if comp.sel < len(comp.items) - 1:
                comp.sel += 1
                if comp.sel >= comp.scroll + MAX_COMPLETION_ITEMS:
                    comp.scroll = comp.sel - MAX_COMPLETION_ITEMS + 1
            return
        if state.cursor_row < len(state.src_code) - 1:
            state.cursor_row += 1
            line = state.src_code[state.cursor_row]
            if state.cursor_col > len(line):
                state.cursor_col = len(line)
            state.selection_anchor = None
        return

    if key == KEY_LEFT:
        if state.completion and state.completion.visible:
            state.completion = None
        if state.cursor_col > 0:
            state.cursor_col -= 1
        elif state.cursor_row > 0:
            state.cursor_row -= 1
            state.cursor_col = len(state.src_code[state.cursor_row])
        state.selection_anchor = None
        return

    if key == KEY_RIGHT:
        if state.completion and state.completion.visible:
            state.completion = None
        line = state.src_code[state.cursor_row]
        if state.cursor_col < len(line):
            state.cursor_col += 1
        elif state.cursor_row < len(state.src_code) - 1:
            state.cursor_row += 1
            state.cursor_col = 0
        state.selection_anchor = None
        return

    if key == KEY_HOME:
        state.cursor_col = 0
        state.selection_anchor = None
        return

    if key == KEY_END:
        state.cursor_col = len(state.src_code[state.cursor_row])
        state.selection_anchor = None
        return

def delete_selection(state):
    if not state.selection_anchor:
        return

    arow, acol = state.selection_anchor
    crow, ccol = state.cursor_row, state.cursor_col

    if arow > crow:
        arow, acol, crow, ccol = crow, ccol, arow, acol
    elif arow == crow and acol > ccol:
        acol, ccol = ccol, acol

    if arow < crow:
        first_line = state.src_code[arow][:acol]
        last_line = state.src_code[crow][ccol:]
        state.src_code[arow] = first_line + last_line
        del state.src_code[arow+1 : crow+1]
        state.cursor_row = arow
        state.cursor_col = acol
    else:
        line = state.src_code[arow]
        new_line = line[:acol] + line[ccol:]
        state.src_code[arow] = new_line
        state.cursor_row = arow
        state.cursor_col = acol

    state.selection_anchor = None
    state.highlight_cache.clear()

def poll_key(state):
    key = keyEvent()
    if key != -1:
        handle_key(state, key)
        ensure_cursor_visible(state)