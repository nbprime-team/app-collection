import ustruct as struct
import uio
import hpprime as _hpprime

class PrimeDebug:
    def __init__(self, filename="debug"):
        try:
            self.f = uio.FileIO("debug")
        except:
            self.f = open(filename, "rb")
        print("[+] Syscall Interface Connected.")

    def close(self):
        if self.f:
            self.f.close()

    def write_mem(self, addr, val):
        self.f.write(struct.pack("<III", 1, addr, val))

    def read_mem(self, addr, size):
        self.f.write(struct.pack("<III", 0, addr, 0))
        return self.f.read(size)

    def call(self, func_addr, *args):
        arg_count = len(args)
        fmt = "<III" + "I" * arg_count
        buf = bytearray(struct.calcsize(fmt))
        struct.pack_into(fmt, buf, 0, 2, func_addr, arg_count, *args)
        self.f.write(buf)
        return struct.unpack_from("<I", buf, 0)[0]

BASE = 0x307FBCAC
def get_addr(int_id):
    return BASE + 0xC * (int_id - 0x10000)

# --- 内存 ---
MALLOC = get_addr(0x10037)
FREE   = get_addr(0x1003A)

# --- 宽字符文件 API ---
W_FOPEN  = get_addr(0x1026F)
W_FCLOSE = get_addr(0x100CA)
W_FREAD  = get_addr(0x100D4)
W_FSEEK  = get_addr(0x100CF)

# --- 宽字符路径 API ---
W_REMOVE    = get_addr(0x10274)
W_RENAME    = get_addr(0x10275)
W_COPY      = get_addr(0x10276)
W_MKDIR     = get_addr(0x10277)
W_RMDIR     = get_addr(0x10278)
W_CHDIR     = get_addr(0x10279)
W_GETCWD    = get_addr(0x1027A)
W_FULLPATH  = get_addr(0x1027E)

# --- 搜索 ---
W_FIND_FIRST = get_addr(0x10270)
W_FIND_NEXT  = get_addr(0x10271)
FIND_CLOSE   = get_addr(0x100DA)

# --- 属性 ---
W_GET_ATTR = get_addr(0x10272)

# --- 磁盘 ---
GET_DISK_ID   = get_addr(0x100E7)
GET_DISK_CHAR = get_addr(0x100E8)
SET_DISK_CHAR = get_addr(0x100E9)
IS_FORMATTED  = get_addr(0x100E4)
GET_FAT_TYPE  = get_addr(0x100E5)


class MemUtils:
    def __init__(self, dbg):
        self.dbg = dbg

    def malloc(self, size):
        return self.dbg.call(MALLOC, size)

    def free(self, addr):
        if addr and addr != 0:
            self.dbg.call(FREE, addr)

    def to_utf16(self, s):
        """手动转 UTF-16LE (无 BOM, 双 0 结尾)"""
        if not s:
            return b'\x00\x00'
        res = bytearray()
        for char in s:
            code = ord(char)
            res.append(code & 0xFF)
            res.append((code >> 8) & 0xFF)
        res.append(0)
        res.append(0)
        return bytes(res)

    def alloc_wstr(self, s):
        b = self.to_utf16(s)
        addr = self.malloc(len(b))
        for i in range(0, len(b), 4):
            chunk = b[i:i+4]
            if len(chunk) < 4:
                chunk += b'\x00' * (4 - len(chunk))
            val = struct.unpack("<I", chunk)[0]
            self.dbg.write_mem(addr + i, val)
        return addr

    def read_wstr(self, addr, max_chars=256):
        if not addr:
            return ""
        raw = self.dbg.read_mem(addr, max_chars * 2)
        res = []
        for i in range(0, len(raw), 2):
            c = raw[i] | (raw[i+1] << 8)
            if c == 0:
                break
            try:
                res.append(chr(c))
            except:
                res.append('?')
        return "".join(res)

    def read_utf8(self, addr, max_len=256):
        if not addr:
            return ""
        raw = self.dbg.read_mem(addr, max_len)
        end = -1
        for i in range(len(raw)):
            if raw[i] == 0:
                end = i
                break
        if end != -1:
            raw = raw[:end]
        try:
            return raw.decode('utf-8')
        except:
            return "?"


class HPFileSystem:
    def __init__(self):
        self.dbg = PrimeDebug()
        self.mem = MemUtils(self.dbg)

    def close(self):
        self.dbg.close()

    # --- 磁盘 ---
    def get_drives(self):
        drives = []
        current = self.dbg.call(GET_DISK_ID)
        for i in range(256):
            char_code = self.dbg.call(GET_DISK_CHAR, i)
            if char_code and 65 <= char_code <= 90:
                is_fmt = self.dbg.call(IS_FORMATTED, i)
                fat_type = self.dbg.call(GET_FAT_TYPE, i)
                fat_str = "UNK"
                if fat_type == 6:
                    fat_str = "FAT16"
                elif fat_type == 11:
                    fat_str = "FAT32"
                elif fat_type == 255:
                    fat_str = "RAW"
                drives.append({
                    'id': i,
                    'letter': chr(char_code),
                    'active': (i == current),
                    'formatted': is_fmt,
                    'type': fat_str
                })
        return drives

    def switch_disk(self, char):
        c = ord(char.upper())
        self.dbg.call(SET_DISK_CHAR, c)
        new_id = self.dbg.call(GET_DISK_ID)
        new_char = self.dbg.call(GET_DISK_CHAR, new_id)
        return new_char == c

    # --- 目录 ---
    def get_cwd(self):
        disk_id = self.dbg.call(GET_DISK_ID)
        buf_ptr = self.mem.malloc(512)
        try:
            res = self.dbg.call(W_GETCWD, disk_id, buf_ptr)
            if res != -1:
                return self.mem.read_wstr(buf_ptr)
            return "?"
        finally:
            self.mem.free(buf_ptr)

    def chdir(self, path):
        p_ptr = self.mem.alloc_wstr(path)
        res = self.dbg.call(W_CHDIR, p_ptr)
        self.mem.free(p_ptr)
        return res == 0

    def list_dir(self, path=None):
        # 用绝对路径模式: findfirst 不一定跟随 wchdir 设置的相对 cwd
        if not path:
            try:
                path = self.get_cwd()
            except Exception:
                path = None
        if not path:
            path = "*.*"
        elif not path.endswith("*"):
            if path.endswith("\\"):
                path += "*.*"
            else:
                path += "\\*.*"

        p_ptr = self.mem.alloc_wstr(path)
        ctx = self.mem.malloc(256)
        results = []
        try:
            # 依次尝试多种掩码: 部分固件不接受 0xFF (findfirst 返回 -1)
            ok = False
            for mask in (0xFF, 0x00, 0x37):
                if self.dbg.call(W_FIND_FIRST, p_ptr, ctx, mask) == 0:
                    ok = True
                    break
            if not ok:
                # 最后再试不带 mask 参数
                if self.dbg.call(W_FIND_FIRST, p_ptr, ctx) == 0:
                    ok = True
            if not ok:
                print("find_first failed, pattern =", path)
                return []
            while True:
                raw = self.dbg.read_mem(ctx, 40)
                lfn_addr  = struct.unpack_from("<I", raw, 8)[0]
                sfn_addr  = struct.unpack_from("<I", raw, 12)[0]
                size      = struct.unpack_from("<I", raw, 20)[0]
                mdate_raw = struct.unpack_from("<I", raw, 24)[0]
                attr      = struct.unpack_from("<B", raw, 37)[0]

                name = self.mem.read_wstr(lfn_addr)
                if not name:
                    name = self.mem.read_utf8(sfn_addr)

                if name and name != "." and name != "..":
                    f_date = (mdate_raw >> 16) & 0xFFFF
                    f_time = mdate_raw & 0xFFFF
                    ts_y = ((f_date >> 9) & 0x7F) + 1980
                    ts_m = (f_date >> 5) & 0xF
                    ts_d = f_date & 0x1F
                    ts_h = (f_time >> 11) & 0x1F
                    ts_n = (f_time >> 5) & 0x3F
                    results.append({
                        "name": name,
                        "size": size,
                        "attr": attr,
                        "is_dir": (attr & 0x10) != 0,
                        "time": "{:04}-{:02}-{:02} {:02}:{:02}".format(
                            ts_y, ts_m, ts_d, ts_h, ts_n)
                    })

                if self.dbg.call(W_FIND_NEXT, ctx) != 0:
                    break
            self.dbg.call(FIND_CLOSE, ctx)
        finally:
            self.mem.free(p_ptr)
            self.mem.free(ctx)

        results.sort(key=lambda x: (not x['is_dir'], x['name'].lower()))
        return results

    def full_path(self, rel_path):
        rel_ptr = self.mem.alloc_wstr(rel_path)
        abs_ptr = self.mem.malloc(512)
        try:
            self.dbg.call(W_FULLPATH, abs_ptr, rel_ptr, 256)
            return self.mem.read_wstr(abs_ptr)
        finally:
            self.mem.free(rel_ptr)
            self.mem.free(abs_ptr)

    # --- 文件操作 ---
    def mkdir(self, path):
        p = self.mem.alloc_wstr(path)
        r = self.dbg.call(W_MKDIR, p)
        self.mem.free(p)
        return r == 0

    def delete(self, path):
        attr = self.getattr(path)
        p = self.mem.alloc_wstr(path)
        if attr != -1 and (attr & 0x10):
            r = self.dbg.call(W_RMDIR, p)
        else:
            r = self.dbg.call(W_REMOVE, p)
        self.mem.free(p)
        return r == 0

    def rename(self, old, new):
        p1 = self.mem.alloc_wstr(old)
        p2 = self.mem.alloc_wstr(new)
        r = self.dbg.call(W_RENAME, p1, p2)
        self.mem.free(p1)
        self.mem.free(p2)
        return r == 0

    def copy(self, src, dst):
        """复制文件/目录 (wcopy, 0x10276)"""
        p1 = self.mem.alloc_wstr(src)
        p2 = self.mem.alloc_wstr(dst)
        r = self.dbg.call(W_COPY, p1, p2)
        self.mem.free(p1)
        self.mem.free(p2)
        return r == 0

    def getattr(self, path):
        p = self.mem.alloc_wstr(path)
        r = self.dbg.call(W_GET_ATTR, p)
        self.mem.free(p)
        return r

    def read_file_chunk(self, path, offset=0, size=512):
        p_ptr = self.mem.alloc_wstr(path)
        m_ptr = self.mem.alloc_wstr("rb")
        buf_ptr = self.mem.malloc(size)
        data = None
        try:
            h = self.dbg.call(W_FOPEN, p_ptr, m_ptr)
            if h != 0:
                self.dbg.call(W_FSEEK, h, offset, 0)
                read_bytes = self.dbg.call(W_FREAD, buf_ptr, 1, size, h)
                if read_bytes > 0:
                    data = self.dbg.read_mem(buf_ptr, read_bytes)
                self.dbg.call(W_FCLOSE, h)
        finally:
            self.mem.free(p_ptr)
            self.mem.free(m_ptr)
            self.mem.free(buf_ptr)
        return data

K_UP, K_DOWN, K_LEFT, K_RIGHT = 2, 12, 7, 8
K_ENTER, K_ESC, K_BS = 30, 4, 19
# 字母键
K_A, K_B, K_C, K_D, K_E = 14, 15, 16, 17, 18
K_F, K_G, K_H, K_I, K_J, K_K = 20, 21, 22, 23, 24, 25
K_L, K_M, K_N, K_O = 26, 27, 28, 29
K_P, K_Q, K_R, K_S, K_T = 31, 32, 33, 34, 35
K_U, K_V, K_W, K_X = 37, 38, 39, 40
K_Y, K_Z = 42, 43

# =========================================================
# 3. 按键轮询 (边沿检测 + 长按重复)
# =========================================================
class KeyReader:
    REPEAT_DELAY = 18  # 长按多少帧后开始重复
    REPEAT_RATE  = 4   # 之后每几帧重复一次

    def __init__(self, hp):
        self.hp = hp
        self.prev = 0
        self.held = {}

    def poll(self):
        """返回 [(键码, 是否重复), ...]"""
        try:
            k = self.hp.keyboard()
        except:
            k = 0
        acts = []
        for key in range(64):
            bit = 1 << key
            down = (k & bit) != 0
            was = (self.prev & bit) != 0
            if down and not was:
                self.held[key] = 0
                acts.append((key, False))
            elif down:
                h = self.held.get(key, 0) + 1
                self.held[key] = h
                if h >= KeyReader.REPEAT_DELAY and \
                   (h - KeyReader.REPEAT_DELAY) % KeyReader.REPEAT_RATE == 0:
                    acts.append((key, True))
            else:
                self.held[key] = 0
        self.prev = k
        return acts

GROB = 1            # 屏幕缓冲 grob
FONT_SIZE = 0       # TEXTSIZE 字体号

# 颜色 (24 位 RGB)
COL_BG      = 0xFFFFFF
COL_HEAD    = 0x1E3A8A
COL_HEAD_TX = 0xFFFFFF
COL_SEL     = 0x3B82F6
COL_SEL_TX  = 0xFFFFFF
COL_TX      = 0x111111
COL_DIR     = 0x1D4ED8
COL_SZ      = 0x6B7280
COL_FOOTBG  = 0xE5E7EB
COL_DLGB    = 0x374151
COL_DLG     = 0xF3F4F6
COL_WARN    = 0xDC2626

CHUNK = 2048  # 查看器每块字节数


def _q(s):
    s = s.replace('"', "'").replace('\n', ' ').replace('\r', '')
    return '"' + s + '"'


class FileBrowser:
    def __init__(self, fs=None, hp=None):
        self.fs = fs if fs is not None else HPFileSystem()
        if hp is None:
            if _hpprime is None:
                raise RuntimeError("hpprime unavailable (host?) -- pass hp= stub")
            hp = _hpprime
        self.hp = hp

        self.items = []      # 当前目录条目
        self.cwd = "?"
        self.sel = 0         # 选中下标
        self.scroll_top = 0  # 列表滚动起点
        self.mode = 'list'   # list / view / info
        self.mouse_prev = False
        self.keyreader = KeyReader(self.hp)

        # 查看器状态
        self.vname = ""
        self.vsize = 0
        self.voff = 0
        self.vlines = []
        self.vtop = 0
        self.vhex = False
        # 信息面板
        self.info_item = None
        # 复制模式状态
        self.copy_src = None       # 源条目 (进入复制模式后非空)
        self.copy_src_path = ""    # 源绝对路径

        self.text_h = 12
        self.row_h = 16
        self.header_h = 20
        self.footer_h = 30
        self.list_top = 20
        self.list_bottom = 210
        self.nrows = 11

    # ---------------- hpprime 绘制封装 ----------------
    def g_fill(self, x, y, w, h, color):
        self.hp.fillrect(GROB, x, y, w, h, color, color)

    def g_text(self, x, y, s, color):
        if s == '':
            return
        self.hp.textout(GROB, x, y, s, color)
    
    def g_text_tiny(self, x, y, s, color):
        if s == '':
            return
        self.hp.eval("textout_p(" + _q(" " * 54 + s + " " * 54) + ', G' + str(GROB) + ',' + str(x - 324) +  ',' + str(y) + ', {"2D", 0, ' + str(color) + "})")
    
    def g_text_small(self, x, y, s, color):
        if s == '':
            return
        self.hp.eval("textout_p(" + _q(s) + ', G' + str(GROB) + ',' + str(x) +  ',' + str(y) + ', 2, ' + str(color) + ")")

    def g_blit(self):
        self.hp.blit(0, 0, 0, GROB)

    def g_text_size(self, s, font = ''):
        try:
            if font == 'tiny':
                return 6 * len(s), 12 
            else:
                r = self.hp.eval('TEXTSIZE({},{})'.format(_q(s), FONT_SIZE))
                return int(r[0]), int(r[1])
        except Exception:
            return 8 * len(s), 12

    def fit_text(self, s, max_w, suffix='~', font = ''):
        """按像素宽度截断字符串 (用 TEXTSIZE 测量)"""
        if max_w <= 0:
            return ''
        w, h = self.g_text_size(s, font)
        if w <= max_w:
            return s
        cw, ch = self.g_text_size('W', font)
        if cw <= 0:
            cw = 8
        k = int(max_w / cw)
        if k < 2:
            k = 2
        while True:
            t = s[:k-1] + suffix
            w2, h2 = self.g_text_size(t, font)
            if w2 <= max_w or k <= 2:
                return t
            k -= 1

    def pace(self):
        self.hp.eval("wait(0.0001)")

    # ---------------- 界面初始化 ----------------
    def init_ui(self):
        # 创建屏幕缓冲 grob
        self.hp.dimgrob(GROB, 320, 240, COL_BG)
        # 字体度量 -> 自适应布局
        w, h = self.g_text_size('Ag')
        self.text_h = max(h, 10)
        self.row_h = self.text_h + 4
        self.row_h_tiny = 10
        self.header_h = self.text_h + 8
        self.footer_h = (self.text_h + 2) * 2 + 2
        self.list_top = self.header_h
        self.list_bottom = 240 - self.footer_h
        self.nrows = max(1, (self.list_bottom - self.list_top) // self.row_h)
        self.nrows_tiny = max(1, (self.list_bottom - self.list_top) // self.row_h_tiny)
        self.dir_name_w = 320 - 6 - 6
        self.file_name_w = 320 - 6 - 6 - 72
        # 排空启动时残留的触摸事件
        for i in range(6):
            try:
                m = self.hp.mouse()
                if not (m and m[0]):
                    break
            except Exception:
                break

    # ---------------- 文件系统操作 ----------------
    def path_of(self, name):
        cwd = self.cwd if self.cwd else "C:\\"
        if cwd.endswith("\\"):
            return cwd + name
        return cwd + "\\" + name

    @staticmethod
    def human_size(n):
        if n < 1024:
            return "{}B".format(n)
        if n < 1024 * 1024:
            return "{:.1f}K".format(n / 1024.0)
        return "{:.1f}M".format(n / 1048576.0)

    @staticmethod
    def attr_str(attr):
        if attr == -1:
            return "ERR"
        res = "D" if (attr & 0x10) else "-"
        res += "R" if (attr & 0x01) else "-"
        res += "H" if (attr & 0x02) else "-"
        res += "S" if (attr & 0x04) else "-"
        res += "A" if (attr & 0x20) else "-"
        return res

    def refresh(self):
        try:
            self.cwd = self.fs.get_cwd()
        except Exception:
            self.cwd = "?"
        try:
            if self.cwd and self.cwd != "?":
                # 用绝对路径列目录, 避免相对模式解析错误
                self.items = self.fs.list_dir(self.cwd)
            else:
                self.items = self.fs.list_dir()
        except Exception as e:
            self.items = []
        if self.sel >= len(self.items):
            self.sel = max(0, len(self.items) - 1)
        self.clamp_sel()

    def go_up(self):
        try:
            if self.fs.chdir(".."):
                self.refresh()
        except Exception:
            pass

    def do_cd(self, target):
        target = target.replace('"', '').replace("'", "")
        if len(target) == 2 and target[1] == ":":
            try:
                if self.fs.switch_disk(target[0]):
                    self.refresh()
            except Exception:
                pass
            return
        try:
            if self.fs.chdir(target):
                self.refresh()
        except Exception:
            pass

    def clamp_sel(self):
        n = len(self.items)
        if n == 0:
            self.sel = 0
            self.scroll_top = 0
            return
        if self.sel < 0:
            self.sel = 0
        if self.sel >= n:
            self.sel = n - 1
        if self.sel < self.scroll_top:
            self.scroll_top = self.sel
        elif self.sel >= self.scroll_top + self.nrows:
            self.scroll_top = self.sel - self.nrows + 1

    # ---------------- 绘制 ----------------
    def draw_list(self):
        self.g_fill(0, 0, 320, 240, COL_BG)
        # 顶栏
        self.g_fill(0, 0, 320, self.header_h, COL_HEAD)
        if self.copy_src is not None:
            # 复制模式: 左上角显示 "把...复制到..."
            cwd = self.fit_text("把{}复制到{}".format(
                self.copy_src['name'], self.cwd), 320 - 8)
            self.g_text(4, 2, cwd, COL_HEAD_TX)
        else:
            cwd = self.fit_text(self.cwd, 320 - 8 - 80)
            self.g_text(4, 2, cwd, COL_HEAD_TX)
            cnt = str(len(self.items)) + "项"
            w, h = self.g_text_size(cnt)
            self.g_text(320 - 4 - w, 2, cnt, COL_HEAD_TX)
        # 列表
        if not self.items:
            self.g_text(6, self.list_top + 2, "(空目录)", COL_SZ)
        else:
            for r in range(self.nrows):
                i = self.scroll_top + r
                if i >= len(self.items):
                    break
                it = self.items[i]
                y = self.list_top + r * self.row_h
                sel = (i == self.sel)
                if sel:
                    self.g_fill(0, y, 320, self.row_h, COL_SEL)
                if it['is_dir']:
                    disp = self.fit_text(it['name'] + '/', self.dir_name_w)
                    self.g_text(6, y + 2, disp,
                                COL_SEL_TX if sel else COL_DIR)
                else:
                    disp = self.fit_text(it['name'], self.file_name_w)
                    self.g_text(6, y + 2, disp,
                                COL_SEL_TX if sel else COL_TX)
                    sz = self.human_size(it['size'])
                    sw, sh = self.g_text_size(sz)
                    self.g_text(320 - 4 - sw, y + 2, sz,
                                COL_SEL_TX if sel else COL_SZ)
        # 底部提示
        self.g_fill(0, self.list_bottom, 320, 240 - self.list_bottom,
                    COL_FOOTBG)
        if self.copy_src is not None:
            self.g_text_small(4, self.list_bottom + 2,
                              "y确认复制  esc取消复制", COL_TX)
            self.g_text_small(4, self.list_bottom + 2 + self.text_h + 2,
                              "Up/Dn选择 Enter进入 ←上级", COL_TX)
        else:
            self.g_text_small(4, self.list_bottom + 2, "Up/Dn选择 Enter/右打开 左/u上级 q退出", COL_TX)
            self.g_text_small(4, self.list_bottom + 2 + self.text_h + 2,
                        "v查看 i信息 x删除 c复制 y重命名 m新建 g路径 r刷新", COL_TX)
        self.g_blit()

    def draw_dialog(self, title, hint):
        bw, bh = 280, 64
        bx, by = (320 - bw) // 2, (240 - bh) // 2
        self.g_fill(bx, by, bw, bh, COL_DLGB)
        self.g_fill(bx + 2, by + 2, bw - 4, bh - 4, COL_DLG)
        self.g_text(bx + 8, by + 6, self.fit_text(title, bw - 20), COL_TX)
        self.g_text(bx + 8, by + 6 + self.text_h + 6,
                    self.fit_text(hint, bw - 20), COL_SZ)
        self.g_blit()

    def draw_view(self):
        self.g_fill(0, 0, 320, 240, COL_BG)
        self.g_fill(0, 0, 320, self.header_h, COL_HEAD)
        head = "{}  [{}/{}]".format(self.vname, self.voff, self.vsize)
        self.g_text(4, 2, self.fit_text(head, 320 - 8), COL_HEAD_TX)
        for r in range(self.nrows_tiny):
            i = self.vtop + r
            if i >= len(self.vlines):
                break
            y = self.list_top + r * self.row_h_tiny
            self.g_text_tiny(4, y + 2, self.fit_text(self.vlines[i], 320 - 8, '..', 'tiny'),
                        COL_TX)
        self.g_fill(0, self.list_bottom, 320, 240 - self.list_bottom,
                    COL_FOOTBG)
        self.g_text_small(4, self.list_bottom + 2, "Up/Dn翻行 左/右翻块 Esc返回", COL_TX)
        self.g_blit()

    def draw_info(self):
        self.g_fill(0, 0, 320, 240, COL_BG)
        self.g_fill(0, 0, 320, self.header_h, COL_HEAD)
        self.g_text(4, 2, "文件信息", COL_HEAD_TX)
        it = self.info_item
        if it is None:
            self.g_text(6, self.list_top + 2, "(无)", COL_TX)
        else:
            lines = [
                "名称: " + it['name'],
                "路径: " + self.path_of(it['name']),
                "大小: {} ({}B)".format(self.human_size(it['size']), it['size']),
                "时间: " + it['time'],
                "属性: " + self.attr_str(it['attr']),
                "类型: " + ("目录" if it['is_dir'] else "文件"),
            ]
            y = self.list_top + 2
            for ln in lines:
                self.g_text(6, y, self.fit_text(ln, 320 - 12), COL_TX)
                y += self.row_h
        self.g_fill(0, self.list_bottom, 320, 240 - self.list_bottom,
                    COL_FOOTBG)
        self.g_text_small(4, self.list_bottom + 2, "Esc/Enter/左 返回", COL_TX)
        self.g_blit()

    def redraw(self):
        """重绘当前界面; 所有绘制完成后 blit 上屏"""
        try:
            if self.mode == 'view':
                self.draw_view()
            elif self.mode == 'info':
                self.draw_info()
            else:
                self.draw_list()
        finally:
            self.g_blit()

    # ---------------- 列表动作 ----------------
    def open_sel(self):
        if self.sel >= len(self.items):
            return
        it = self.items[self.sel]
        if it['is_dir']:
            try:
                if self.fs.chdir(it['name']):
                    self.refresh()
            except Exception:
                pass
        elif self.copy_src is None:
            self.view_file(it)

    def view_sel(self):
        self.open_sel()

    def show_info(self):
        if self.sel < len(self.items):
            self.info_item = self.items[self.sel]
            self.mode = 'info'

    def view_file(self, it):
        self.vname = it['name']
        self.vsize = it['size']
        self.voff = 0
        self.vtop = 0
        self.mode = 'view'
        self.load_view()

    def is_binary(self, data):
        if not data:
            return False
        bad = 0
        for b in data:
            if b == 0 or (b < 32 and b not in (9, 10, 13)):
                bad += 1
        return bad * 100 > len(data) * 3

    def _hex_lines(self, data):
        lines = []
        for i in range(0, len(data), 8):
            chunk = data[i:i+8]
            hx = " ".join(["{:02X}".format(b) for b in chunk])
            asc = "".join([chr(b) if 32 <= b < 127 else "." for b in chunk])
            lines.append("{:06X}   {:<23}  {}".format(
                self.voff + i, hx, asc))
        return lines

    def load_view(self):
        data = None
        try:
            data = self.fs.read_file_chunk(self.path_of(self.vname),
                                           self.voff, CHUNK)
        except Exception:
            data = None
        if data is None:
            self.vlines = ["(读取失败)"]
            self.vhex = False
            self.vtop = 0
            return
        if len(data) == 0:
            self.vlines = ["(空文件)"]
            self.vhex = False
            self.vtop = 0
            return
        if self.is_binary(data):
            self.vhex = True
            self.vlines = self._hex_lines(data)
        else:
            text = None
            for cut in range(0, 4):
                try:
                    text = data[:len(data) - cut].decode('utf-8')
                    break
                except Exception:
                    text = None
            if text is None:
                self.vhex = True
                self.vlines = self._hex_lines(data)
            else:
                self.vhex = False
                self.vlines = text.split('\n')
                if self.vlines and self.vlines[-1] == '':
                    self.vlines.pop()
        self.vtop = 0

    def do_delete(self):
        if self.sel >= len(self.items):
            return
        it = self.items[self.sel]
        if not self.confirm("删除 '{}'?".format(it['name'])):
            return
        try:
            if self.fs.delete(self.path_of(it['name'])):
                self.refresh()
            else:
                self.flash("删除失败")
        except Exception:
            self.flash("删除失败")

    def do_mkdir(self):
        name = self.ask_name("新建目录名")
        if not name:
            return
        try:
            if self.fs.mkdir(self.path_of(name)):
                self.refresh()
            else:
                self.flash("创建失败")
        except Exception:
            self.flash("创建失败")

    def do_rename(self):
        if self.sel >= len(self.items):
            return
        it = self.items[self.sel]
        new = self.ask_name("重命名 -> 新名称")
        if not new:
            return
        try:
            if self.fs.rename(self.path_of(it['name']), self.path_of(new)):
                self.refresh()
            else:
                self.flash("重命名失败")
        except Exception:
            self.flash("重命名失败")

    def do_copy(self):
        """进入复制模式: 用文件浏览器选择目标目录, y 确认, esc 取消"""
        if self.sel >= len(self.items):
            return
        it = self.items[self.sel]
        self.copy_src = it
        self.copy_src_path = self.path_of(it['name'])
        self.mode = 'list'

    # ---------------- 模态交互 ----------------
    def confirm(self, title):
        # 返回 True/False; y/Enter=是, n/Esc=否
        self.draw_dialog(title, "y=是  n/esc=否")
        while True:
            self.pace()
            for k, rep in self.keyreader.poll():
                if rep:
                    continue
                if k == K_Y or k == K_ENTER:
                    return True
                if k == K_N or k == K_ESC:
                    return False

    def flash(self, msg, frames=30):
        self.draw_dialog(msg, "任意键继续")
        n = 0
        while n < frames:
            self.pace()
            n += 1
            for k, rep in self.keyreader.poll():
                if not rep and k in (K_ESC, K_ENTER):
                    return

    def ask_name(self, prompt):
        """文本输入 (终端交互, 不绘制不blit);
        输入为空则重复询问, Esc/[ON] 取消 (KeyboardInterrupt)"""
        while True:
            try:
                s = input(prompt + " > ")
            except KeyboardInterrupt:
                return None
            except Exception:
                return None
            s = s.strip()
            if s:
                return s
            # 空输入 -> 重复询问

    # ---------------- 按键分发 ----------------
    def on_key_list(self, k, rep):
        # 复制模式: 只处理浏览 + y确认 + esc取消
        if self.copy_src is not None:
            return self.on_key_copy(k, rep)
        if k == K_UP:
            self.sel -= 1
            self.clamp_sel()
            return True
        if k == K_DOWN:
            self.sel += 1
            self.clamp_sel()
            return True
        if rep:
            return False
        if k == K_ENTER or k == K_RIGHT:
            self.open_sel()
            return True
        if k == K_LEFT or k == K_U:
            self.go_up()
            return True
        if k == K_ESC or k == K_Q:
            if self.confirm("退出文件浏览器?"):
                return 'quit'
            return True
        if k == K_V:
            self.view_sel()
            return True
        if k == K_I:
            self.show_info()
            return True
        if k == K_X:
            self.do_delete()
            return True
        if k == K_C:
            self.do_copy()
            return True
        if k == K_M:
            self.do_mkdir()
            return True
        if k == K_G:
            p = self.ask_name("输入路径 (如 C:\\, C:\\DATA, ..)")
            if p:
                self.do_cd(p)
            return True
        if k == K_R:
            self.refresh()
            return True
        if k == K_Y:
            self.do_rename()
            return True
        return False

    def on_key_copy(self, k, rep):
        """复制模式: 浏览选择目标目录"""
        if k == K_UP:
            self.sel -= 1
            self.clamp_sel()
            return True
        if k == K_DOWN:
            self.sel += 1
            self.clamp_sel()
            return True
        if rep:
            return False
        if k == K_ENTER or k == K_RIGHT:
            # 只允许进入目录
            if self.sel < len(self.items) and self.items[self.sel]['is_dir']:
                try:
                    if self.fs.chdir(self.items[self.sel]['name']):
                        self.refresh()
                except Exception:
                    pass
            return True
        if k == K_LEFT or k == K_U:
            self.go_up()
            return True
        if k == K_Y:
            self.copy_confirm()
            return True
        if k == K_ESC or k == K_Q:
            self.copy_cancel()
            return True
        return False

    def copy_confirm(self):
        """y: 复制到当前目录"""
        name = self.copy_src['name']
        src = self.copy_src_path
        dst = self.path_of(name)
        self.copy_src = None
        self.copy_src_path = ""
        if src == dst:
            self.flash("目标与源相同")
            return
        try:
            if self.fs.copy(src, dst):
                self.flash("已复制: {}".format(name))
                self.refresh()
            else:
                self.flash("复制失败")
        except Exception:
            self.flash("复制失败")

    def copy_cancel(self):
        """esc: 取消复制"""
        self.copy_src = None
        self.copy_src_path = ""

    def on_key_view(self, k, rep):
        if k == K_UP:
            if self.vtop > 0:
                self.vtop -= 1
            return True
        if k == K_DOWN:
            if self.vtop + self.nrows < len(self.vlines):
                self.vtop += 1
            return True
        if rep:
            return False
        if k == K_LEFT:
            if self.voff > 0:
                self.voff = max(0, self.voff - CHUNK)
                self.load_view()
            return True
        if k == K_RIGHT or k == K_ENTER:
            if self.voff + CHUNK < self.vsize:
                self.voff += CHUNK
                self.load_view()
            return True
        if k == K_ESC or k == K_BS:
            self.mode = 'list'
            return True
        return False

    def on_key_info(self, k, rep):
        if rep:
            return False
        if k in (K_ESC, K_ENTER, K_LEFT):
            self.mode = 'list'
            return True
        return False

    def poll_tap(self):
        """返回本次帧内新按下的触摸坐标 (x, y) 或 None"""
        try:
            m = self.hp.mouse()
            has = bool(m and m[0])
        except Exception:
            has = False
        tap = has and not self.mouse_prev
        self.mouse_prev = has
        if tap:
            try:
                x = int(m[0][0])
                y = int(m[0][1])
                if 0 <= x < 320 and 0 <= y < 240:
                    return (x, y)
            except Exception:
                return None
        return None

    def on_tap(self, pos):
        x, y = pos
        if self.mode != 'list':
            return
        if self.list_top <= y < self.list_bottom:
            r = (y - self.list_top) // self.row_h
            i = self.scroll_top + r
            if 0 <= i < len(self.items):
                self.sel = i
                self.clamp_sel()
                self.open_sel()

    # ---------------- 主循环 ----------------
    def run(self):
        self.init_ui()
        self.refresh()
        self.mode = 'list'
        self.redraw()
        errs = 0
        while True:
            self.pace()
            try:
                acted = False
                for k, rep in self.keyreader.poll():
                    if self.mode == 'list':
                        r = self.on_key_list(k, rep)
                    elif self.mode == 'view':
                        r = self.on_key_view(k, rep)
                    elif self.mode == 'info':
                        r = self.on_key_info(k, rep)
                    else:
                        r = False
                    if r == 'quit':
                        self.fs.close()
                        return
                    if r:
                        acted = True
                m = self.poll_tap()
                if m:
                    self.on_tap(m)
                    acted = True
                if acted:
                    self.redraw()
                errs = 0
            except Exception as e:
                errs += 1
                if errs > 5:
                    print("Fatal: {}".format(e))
                    break
                try:
                    self.redraw()
                except Exception:
                    pass
        self.fs.close()


if _hpprime.eval('memory(1)') < 1048576 * 6:
    print("restart your calculator first,")
    r = input("or meet unexpected crash! type 'Y' force run")
    if r == "Y":
        FileBrowser().run()
else:
    FileBrowser().run()
    