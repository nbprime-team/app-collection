# program_selector.py
from const import *
from myprint import *
from keys import *
from hpprime import *

class ProgramSelectorState:
    def __init__(self, screen_w=320, screen_h=240):
        self.screen_w = screen_w
        self.screen_h = screen_h
        self.programs = ['#New program'] + getPrograms()
        self.scroll_y = 0
        self.selected_index = 0
        self.input_mode = False
        self.input_text = ''
        self.input_cursor = 0

    def refresh_programs(self):
        self.programs = ['#New program'] + getPrograms()
        if self.selected_index >= len(self.programs):
            self.selected_index = len(self.programs) - 1

def _get_list_geometry(state):
    # content_top, content_h, text_x, text_w
    content_top = TITLE_BAR_H
    content_h = state.screen_h - TITLE_BAR_H
    text_x = 4
    text_w = state.screen_w - text_x - SCROLLBAR_W - 2
    return content_top, content_h, text_x, text_w

def _draw_program_row(grob, state, row, y, is_selected):
    text_x = _get_list_geometry(state)[2]
    txt = state.programs[row]
    if is_selected:
        fillrect(grob, 0, y, state.screen_w - SCROLLBAR_W - 2,
                 LINE_HEIGHT, COLOR_MENU_SEL_BG, COLOR_MENU_SEL_BG)
    color = COLOR_MENU_SEL_TEXT if is_selected else 1
    myPrint(txt, text_x, y, grob, color)

def _draw_scrollbar(grob, state):
    content_top, content_h, _, _ = _get_list_geometry(state)
    num = len(state.programs)
    if num == 0:
        return
    sb_x = state.screen_w - SCROLLBAR_W - 1
    fillrect(grob, sb_x, content_top, SCROLLBAR_W, content_h,
             COLOR_SCROLL_BG, COLOR_SCROLL_BG)

    thumb_h = max(4, (content_h // LINE_HEIGHT) * content_h // num)
    max_scroll = max(0, num * LINE_HEIGHT - content_h)
    if max_scroll > 0:
        track_h = content_h - thumb_h
        thumb_y = content_top + int(state.scroll_y * track_h / max_scroll)
    else:
        thumb_y = content_top
    fillrect(grob, sb_x, thumb_y, SCROLLBAR_W, thumb_h,
             COLOR_SCROLL_THUMB, COLOR_SCROLL_THUMB)

def _draw_input_box(grob, state):
    if not state.input_mode:
        return
    w = 200
    h = LINE_HEIGHT
    x = (state.screen_w - w) // 2
    y = state.screen_h - h - 10
    fillrect(grob, x, y, w, h, COLOR_MENU_BORDER, COLOR_MENU_BG)
    if state.input_text:
        myPrint(state.input_text, x + 4, y, grob, COLOR_TEXT)
    
    cursor_x = x + 4 + sum(char_width(ch) for ch in state.input_text[:state.input_cursor])
    fillrect(grob, cursor_x, y, 2, h, COLOR_CURSOR, COLOR_CURSOR)
    return x, y, w, h

def render_selector(grob, state): 
    sw, sh = state.screen_w, state.screen_h
    #fillrect(grob, 0, 0, sw, sh, COLOR_BG, COLOR_BG)
    blit(grob, 0, 0, 3)
    fillrect(grob, 0, 0, sw, TITLE_BAR_H, COLOR_TITLE_BG, COLOR_TITLE_BG)
    myPrint("primecode", 4, 0, grob, COLOR_TITLE_TEXT)

    content_top, content_h, text_x, text_w = _get_list_geometry(state)
    #fillrect(grob, 0, content_top, sw, content_h, COLOR_BG, COLOR_BG)

    start_row = state.scroll_y // LINE_HEIGHT
    y_offset = state.scroll_y % LINE_HEIGHT
    y = content_top - y_offset
    num = len(state.programs)
    i = 0
    while y < content_top + content_h and start_row + i < num:
        row = start_row + i
        is_selected = (row == state.selected_index)
        _draw_program_row(grob, state, row, y, is_selected)
        i += 1
        y += LINE_HEIGHT

    # 滚动条
    _draw_scrollbar(grob, state)

    # 输入框
    if state.input_mode:
        _draw_input_box(grob, state)

# ---------- 事件处理 ----------
def _ensure_visible(state):
    """确保选中项在视口内"""
    content_top, content_h, _, _ = _get_list_geometry(state)
    row_top = state.selected_index * LINE_HEIGHT
    row_bottom = (state.selected_index + 1) * LINE_HEIGHT
    view_top = state.scroll_y
    view_bottom = state.scroll_y + content_h

    if row_top < view_top:
        state.scroll_y = row_top
    elif row_bottom > view_bottom:
        state.scroll_y = row_bottom - content_h

    max_scroll = max(0, len(state.programs) * LINE_HEIGHT - content_h)
    if state.scroll_y < 0:
        state.scroll_y = 0
    elif state.scroll_y > max_scroll:
        state.scroll_y = max_scroll

def _handle_input_key(state, key):
    """处理输入框内的键盘事件，返回 True 表示事件已处理"""
    global KEY_ENTER, KEY_DEL, KEY_ESC, KEY_LEFT, KEY_RIGHT

    if key == KEY_ENTER:
        # 确认新建
        name = state.input_text.strip()
        if name:
            newProgram(name)
            state.refresh_programs()
            state.input_mode = False
            state.input_text = ''
            state.input_cursor = 0
            return 'load', name   # 通知主循环加载新程序
        else:
            # 空名称，取消
            state.input_mode = False
            state.input_text = ''
            state.input_cursor = 0
            return None

    if key == KEY_ESC:
        state.input_mode = False
        state.input_text = ''
        state.input_cursor = 0
        return None

    if key == KEY_LEFT:
        if state.input_cursor > 0:
            state.input_cursor -= 1
        return True

    if key == KEY_RIGHT:
        if state.input_cursor < len(state.input_text):
            state.input_cursor += 1
        return True

    if key == KEY_DEL:
        if state.input_cursor > 0:
            state.input_text = state.input_text[:state.input_cursor-1] + \
                               state.input_text[state.input_cursor:]
            state.input_cursor -= 1
        return True

    # 普通字符（包括多字符键，如括号）
    if key not in (KEY_ENTER, KEY_DEL, KEY_ESC, KEY_LEFT, KEY_RIGHT):
        # 过滤控制字符，只接受可见字符（长度任意）
        state.input_text = state.input_text[:state.input_cursor] + key + \
                           state.input_text[state.input_cursor:]
        state.input_cursor += len(key)
        return True

    return False

def handle_selector_key(state, key):
    """处理键盘事件，返回 (动作, 参数) 或 None"""
    if state.input_mode:
        return _handle_input_key(state, key)

    content_top, content_h, _, _ = _get_list_geometry(state)

    if key == KEY_UP:
        if state.selected_index > 0:
            state.selected_index -= 1
            _ensure_visible(state)
        return None

    if key == KEY_DOWN:
        if state.selected_index < len(state.programs) - 1:
            state.selected_index += 1
            _ensure_visible(state)
        return None

    if key == KEY_ENTER:
        selected = state.programs[state.selected_index]
        if selected == '#New program':
            state.input_mode = True
            state.input_text = ''
            state.input_cursor = 0
            return None
        else:
            return ('load', selected)

    if key == KEY_ESC:
        return ('exit', None)   # 可返回退出编辑器

    return None

def handle_selector_mouse(state, x, y, status):
    """处理鼠标事件，返回 (动作, 参数) 或 None"""
    content_top, content_h, text_x, text_w = _get_list_geometry(state)

    # 如果处于输入模式，点击输入框外取消输入
    if state.input_mode:
        if status == 0:
            box = _draw_input_box(None, state)   # 获取输入框位置
            if box:
                bx, by, bw, bh = box
                if not (bx <= x < bx+bw and by <= y < by+bh):
                    state.input_mode = False
                    state.input_text = ''
                    state.input_cursor = 0
        return None

    # 标题栏
    if y < content_top:
        return None

    # 滚动条
    sb_x = state.screen_w - SCROLLBAR_W - 1
    if x >= sb_x:
        num = len(state.programs)
        thumb_h = max(4, (content_h // LINE_HEIGHT) * content_h // num)
        max_scroll = max(0, num * LINE_HEIGHT - content_h)
        if max_scroll > 0:
            track_h = content_h - thumb_h
            offset = y - content_top - thumb_h // 2
            if offset < 0:
                offset = 0
            elif offset > track_h:
                offset = track_h
            state.scroll_y = int(offset * max_scroll / track_h)
        return None

    # 列表项点击
    row = (state.scroll_y + (y - content_top)) // LINE_HEIGHT
    if 0 <= row < len(state.programs):
        state.selected_index = row
        if status == 0:
            selected = state.programs[row]
            if selected == '#New program':
                state.input_mode = True
                state.input_text = ''
                state.input_cursor = 0
                return None
            else:
                return ('load', selected)
        else:
            _ensure_visible(state)
    return None

def poll_selector_events(state):
    """整合鼠标和键盘事件处理，返回 (动作, 参数) 或 None"""
    ev = mouse()
    if ev != -1:
        x, y, status = ev
        result = handle_selector_mouse(state, x, y, status)
        if result:
            return result

    key = keyEvent()
    if key != -1:
        result = handle_selector_key(state, key)
        if result:
            return result
    return None