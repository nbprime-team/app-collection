# editor_utils.py
from const import *

def char_width(ch):
    try:
        if ch == '\t':
            return 40
        return CHAR_W_ASCII if ord(ch) < 128 else CHAR_W_OTHER
    except:
        print(ch)
        raise

def is_keyword(word, lang):
    if lang == 'python':
        return word in PYTHON_KEYWORDS
    else:
        return word.upper() in HPPL_KEYWORDS

def is_builtin(word, lang):
    if lang == 'python':
        return word in PYTHON_BUILTINS
    else:
        return word.upper() in HPPL_BUILTINS

def highlight_line(line, lang):
    """生成一行的高亮数据 dat"""
    n = len(line)
    dat = ['0'] * n
    i = 0
    while i < n:
        ch = line[i]

        if lang == 'python' and ch == '#':
            for j in range(i, n):
                dat[j] = '3'
            break

        if lang == 'hppl':
            if ch == '/' and i + 1 < n and line[i + 1] == '/':
                for j in range(i, n):
                    dat[j] = '3'
                break
            if ch == '/' and i + 1 < n and line[i + 1] == '*':
                dat[i] = '3'
                dat[i + 1] = '3'
                i += 2
                while i < n:
                    if line[i] == '*' and i + 1 < n and line[i + 1] == '/':
                        dat[i] = '3'
                        dat[i + 1] = '3'
                        i += 2
                        break
                    else:
                        dat[i] = '3'
                        i += 1
                continue

        if ch in ('"', "'"):
            quote = ch
            dat[i] = '4'
            i += 1
            while i < n:
                c = line[i]
                if c == '\\':
                    dat[i] = '4'
                    i += 1
                    if i < n:
                        dat[i] = '4'
                        i += 1
                elif c == quote:
                    dat[i] = '4'
                    i += 1
                    break
                else:
                    dat[i] = '4'
                    i += 1
            continue

        if ('0' <= ch <= '9') or (ch == '.' and i + 1 < n and '0' <= line[i + 1] <= '9'):
            while i < n:
                c = line[i]
                if ('0' <= c <= '9') or c in 'abcdefxABCDEFX.oOeE+-':
                    dat[i] = '1'
                    i += 1
                else:
                    break
            continue

        if ch.isalpha() or ch == '_':
            start = i
            while i < n and (isalnum(line[i]) or line[i] == '_'):
                i += 1
            word = line[start:i]

            j = i
            while j < n and line[j] == ' ':
                j += 1
            is_call = j < n and line[j] == '('

            if is_call:
                dtype = '2'
            elif is_keyword(word, lang):
                dtype = '6'
            elif is_builtin(word, lang):
                dtype = '2'
            else:
                dtype = '1'

            for k in range(start, i):
                dat[k] = dtype
            continue

        if ch in '()[]{}':
            dat[i] = '5'
            i += 1
            continue

        if ch in '+-*/%=<>!&|^~':
            dat[i] = '5'
            i += 1
            continue

        dat[i] = '1'
        i += 1

    return ''.join(dat)

def line_pixel_width(line, start_col, end_col):
    if end_col <= start_col:
        return 0
    w = 0
    n = len(line)
    i = start_col
    while i < end_col and i < n:
        w += char_width(line[i])
        i += 1
    return w

def build_visible_line(line, dat, max_width, scroll_col):
    n = len(line)
    if scroll_col >= n:
        return '', '', scroll_col, n

    idx = scroll_col
    width = 0
    out_chars = []
    out_dat = []

    while idx < n and width < max_width:
        ch = line[idx]
        w = char_width(ch)
        if width + w > max_width:
            break

        if ch == '\t':
            out_chars.append('    ')
            out_dat.append(dat[idx] * 4)
        else:
            out_chars.append(ch)
            out_dat.append(dat[idx])

        width += w
        idx += 1

    return ''.join(out_chars), ''.join(out_dat), scroll_col, idx

def pixel_to_col(line, scroll_col, target_x):
    x = 0
    idx = scroll_col
    n = len(line)
    while idx < n:
        w = char_width(line[idx])
        if x + w > target_x:
            if target_x - x < w // 2:
                return idx
            else:
                return idx + 1
        x += w
        idx += 1
    return n