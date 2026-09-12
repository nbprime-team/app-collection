import ustruct as struct
import uio
import hpprime

class PrimeDebug:
    def __init__(self, filename="debug"):
        try:
            self.f = uio.FileIO("debug")
        except:
            self.f = open(filename, "rb")
        print("[+] Debug interface opened.")

    def close(self):
        if self.f: self.f.close()

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

    def write_mem_bytes(self, addr, data):
        pad_len = (4 - (len(data) % 4)) % 4
        data += b'\x00' * pad_len
        for i in range(0, len(data), 4):
            val = struct.unpack("<I", data[i:i+4])[0]
            self.write_mem(addr + i, val)

# --- 辅助函数和类 ---
BASE = 0x307FBCAC
def get_addr(int_id):
    return BASE + 0xC * (int_id - 0x10000)

ADDR_MALLOC = get_addr(0x10037)
ADDR_FREE   = get_addr(0x1003A)

class ElfTools:
    @staticmethod
    def str_to_utf16le_bytes(s):
        res = bytearray()
        for char in s:
            val = ord(char)
            res.append(val & 0xFF)
            res.append((val >> 8) & 0xFF)
        res.append(0); res.append(0)
        return bytes(res)

    @staticmethod
    def get_elf_memory_size(filename):
        required_size = 0
        f = None

        try:
            f = uio.FileIO(filename, "rb")

            # ELF32 header = 52 bytes
            ehdr = f.read(52)

            if len(ehdr) < 52:
                print("[-] ELF header too short: {}".format(len(ehdr)))
                return 0

            if ehdr[0:4] != b'\x7fELF':
                print("[-] Invalid ELF magic")
                return 0

            # ELF32 fields:
            # e_phoff      @ 0x1C
            # e_phentsize  @ 0x2A
            # e_phnum      @ 0x2C
            e_phoff = struct.unpack_from("<I", ehdr, 28)[0]
            e_phentsize = struct.unpack_from("<H", ehdr, 42)[0]
            e_phnum = struct.unpack_from("<H", ehdr, 44)[0]

            print(
                "[ELFDBG] helper: phoff=0x{:X}, phentsize={}, phnum={}".format(
                    e_phoff,
                    e_phentsize,
                    e_phnum
                )
            )

            if e_phentsize < 32:
                print("[-] Invalid program header size")
                return 0

            f.seek(e_phoff)

            for i in range(e_phnum):
                ph = f.read(e_phentsize)

                if len(ph) < 32:
                    print(
                        "[-] Program header {} too short: {}".format(
                            i,
                            len(ph)
                        )
                    )
                    return 0

                p_type = struct.unpack_from("<I", ph, 0)[0]

                # p_vaddr @ 0x08
                p_vaddr = struct.unpack_from("<I", ph, 8)[0]

                # p_memsz @ 0x14
                p_memsz = struct.unpack_from("<I", ph, 20)[0]

                if p_type == 1:  # PT_LOAD
                    end_addr = p_vaddr + p_memsz

                    if end_addr > required_size:
                        required_size = end_addr

            required_size = (required_size + 3) & ~3

            print(
                "[ELFDBG] helper calculated size = 0x{:X} ({} bytes)".format(
                    required_size,
                    required_size
                )
            )

            return required_size

        except Exception as e:
            print("[ELFDBG] get_elf_memory_size ERROR: " + str(e))
            return 0

        finally:
            if f:
                try:
                    f.close()
                except:
                    pass

class ShellcodeElfLoader:
    def __init__(self, dbg):
        self.dbg = dbg
        self.loader_addr = 0
        self.loaded_elf_base = 0
        self.entry_point = 0
        self.loader_code = b'\x0b\x00\x00\xea\x04\x00-\xe5\x04\xe0-\xe5o\x02\x01\xef\x04\x00-\xe5\x04\xe0-\xe5\xca\x00\x01\xef\x04\x00-\xe5\x04\xe0-\xe5\xd4\x00\x01\xef\x04\x00-\xe5\x04\xe0-\xe5\xcf\x00\x01\xefh2\x9f\xe5\xf0O-\xe9\x030\x8f\xe0\x00 \xa0\xe1\x01`\xa0\xe1\x03\x00\x93\xe8d\xd0M\xe2\x04\x00\x8d\xe5\xb8\x10\xcd\xe1\x02\x00\xa0\xe1\x04\x10\x8d\xe2\xe7\xff\xff\xeb\x00@P\xe2s\x00\x00\n\x040\xa0\xe1\x01 \xa0\xe34\x10\xa0\xe3,\x00\x8d\xe2\xe6\xff\xff\xeb\x01\x00P\xe3j\x00\x00\x1a\xbc"\xdd\xe1\x142\x9f\xe5\x03\x00R\xe1f\x00\x00\x1aH\x10\x9d\xe5\x00 \xa0\xe3\x04\x00\xa0\xe1\xdf\xff\xff\xeb\xb85\xdd\xe1\x00\x00S\xe3x\x00\x00\n\x00P\xa0\xe3\x05p\xa0\xe1\x0c\xa0\x8d\xe2\x04\x00\x00\xea\xb85\xdd\xe1\x02\x00[\xe3\x14p\x9d\x05\x05\x00S\xe1\x19\x00\x00\xda\x040\xa0\xe1\x01 \xa0\xe3 \x10\xa0\xe3\n\x00\xa0\xe1\xcb\xff\xff\xeb\x0c\xb0\x9d\xe5\x01P\x85\xe2\x01\x00[\xe3\xf1\xff\xff\x1a\x1c0\x9d\xe5\x14\x80\x9d\xe5H\x90\x9d\xe5\x00\x00S\xe3\x08\x80\x86\xe0\x85\x92\x89\xe0K\x00\x00\x1a  \x9d\xe5\x03\x00R\xe1U\x00\x00\x8a\t\x10\xa0\xe1\x00 \xa0\xe3\x04\x00\xa0\xe1\xbc\xff\xff\xeb\xb85\xdd\xe1\x05\x00S\xe1\xe5\xff\xff\xca\x04\x00\xa0\xe1\xb1\xff\xff\xeb\x00\x00W\xe3/\x00\x00\n\x070\x96\xe7\x07p\x86\xe0\x00\x00S\xe3+\x00\x00\n\x00 \xa0\xe3\x08\xc0\xa0\xe3\x02\x00\xa0\xe1\x02\xe0\xa0\xe1\x11\x00S\xe3\x04\xe0\x97\x05\x04\x00\x00\n\x12\x00S\xe3\x04\x00\x97\x05\x01\x00\x00\n\x13\x00S\xe3\x04\xc0\x97\x05\x080\xb7\xe5\x01 \x82\xe2\x00\x00S\xe3d\x00R\x13\x01\x10\xa0\x13\x00\x10\xa0\x03\xf0\xff\xff\x1a\x00\x00^\xe3\x00\x00P\x13\x15\x00\x00\n\x00\x00\\\xe3\x08\xc0\xa0\x03\x0c\x00P\xe1\x11\x00\x00:\x0c\x00@\xe0\x00\x00\\\xe1\x01\x10\x81\xe2\xfb\xff\xff\x9a\x00\x00Q\xe3\x0b\x00\x00\n\x81\x11\x86\xe0\x0e0\x86\xe0\x0e\x10\x81\xe0\x04 \xd3\xe5\x080\x83\xe2\x17\x00R\xe3\x08\x00\x13\x05\x00 \x96\x07\x02 \x86\x00\x00 \x86\x07\x01\x00S\xe1\xf6\xff\xff\x1a\x000\xa0\xe3~\xff\x17\xee\xfd\xff\xff\x1a\x9a?\x07\xee\x15?\x07\xeeD0\x9d\xe5\x03\x00\x86\xe0d\xd0\x8d\xe2\xf0\x8f\xbd\xe8\x04\x00\xa0\xe1t\xff\xff\xeb\x00\x00\xa0\xe3d\xd0\x8d\xe2\xf0\x8f\xbd\xe8\x10\x10\x9d\xe5\x00 \xa0\xe3\x04\x00\xa0\xe1s\xff\xff\xeb\x1c \x9d\xe5\x040\xa0\xe1\x0b\x10\xa0\xe1\x08\x00\xa0\xe1k\xff\xff\xeb\x1c0\x9d\xe5  \x9d\xe5\x03\x00R\xe1\xa9\xff\xff\x9a\x030\x88\xe0\x02\x80\x88\xe0\x00 \xa0\xe3\x01 \xc3\xe4\x08\x00S\xe1\xfc\xff\xff\x1a\xa2\xff\xff\xea\x04\x00\xa0\xe1[\xff\xff\xeb\xda\xff\xff\xeah\x02\x00\x00\x7fE\x00\x00r\x00b\x00\x00\x00'

    def upload_loader(self):
        size = len(self.loader_code)
        print("[*] Allocating loader shellcode ({} bytes)...".format(size))
        self.loader_addr = self.dbg.call(ADDR_MALLOC, (size + 3) & ~3)
        if not self.loader_addr: return False
        print("[*] Uploading loader to 0x{:08X}".format(self.loader_addr))
        self.dbg.write_mem_bytes(self.loader_addr, self.loader_code)
        return True

    def load_elf(self, filename, app_dir):
        # python 侧访问 filename，C 侧访问 app_dir + filename
        print("[*] Loading ELF: {}".format(filename))

        # ---- Python 侧 ELF 诊断 ----
        # 读取并打印关键 ELF Header / Program Header 字段。
        elf_info = None
        try:
            f = uio.FileIO(filename, "rb")
            ehdr = f.read(52)
            if len(ehdr) < 52:
                print("[ELFDBG] Header too short: {} bytes".format(len(ehdr)))
                f.close()
                return None

            e_type = struct.unpack_from("<H", ehdr, 16)[0]
            e_machine = struct.unpack_from("<H", ehdr, 18)[0]
            e_entry = struct.unpack_from("<I", ehdr, 24)[0]
            e_phoff = struct.unpack_from("<I", ehdr, 28)[0]
            e_phentsize = struct.unpack_from("<H", ehdr, 42)[0]
            e_phnum = struct.unpack_from("<H", ehdr, 44)[0]
            e_shoff = struct.unpack_from("<I", ehdr, 32)[0]
            e_shentsize = struct.unpack_from("<H", ehdr, 46)[0]
            e_shnum = struct.unpack_from("<H", ehdr, 48)[0]

            print("[ELFDBG] magic      = {}".format(
                " ".join("{:02X}".format(b) for b in ehdr[0:4])
            ))
            print("[ELFDBG] class/data = 0x{:02X} / 0x{:02X}".format(ehdr[4], ehdr[5]))
            print("[ELFDBG] type       = 0x{:04X}".format(e_type))
            print("[ELFDBG] machine    = 0x{:04X}".format(e_machine))
            print("[ELFDBG] entry      = 0x{:08X}".format(e_entry))
            print("[ELFDBG] phoff      = 0x{:08X}".format(e_phoff))
            print("[ELFDBG] phentsize  = {}".format(e_phentsize))
            print("[ELFDBG] phnum      = {}".format(e_phnum))
            print("[ELFDBG] shoff      = 0x{:08X}".format(e_shoff))
            print("[ELFDBG] shentsize  = {}".format(e_shentsize))
            print("[ELFDBG] shnum      = {}".format(e_shnum))

            # 保存下来，稍后计算理论运行时入口。
            elf_info = {
                "entry": e_entry,
                "phoff": e_phoff,
                "phentsize": e_phentsize,
                "phnum": e_phnum,
            }

            # 打印所有 Program Header。
            if e_phentsize >= 32 and e_phnum > 0:
                f.seek(e_phoff)
                max_end = 0
                for i in range(e_phnum):
                    ph = f.read(e_phentsize)
                    if len(ph) < 32:
                        print("[ELFDBG] PH[{}] truncated: {} bytes".format(i, len(ph)))
                        break
                    p_type = struct.unpack_from("<I", ph, 0)[0]
                    p_offset = struct.unpack_from("<I", ph, 4)[0]
                    p_vaddr = struct.unpack_from("<I", ph, 8)[0]
                    p_paddr = struct.unpack_from("<I", ph, 12)[0]
                    p_filesz = struct.unpack_from("<I", ph, 16)[0]
                    p_memsz = struct.unpack_from("<I", ph, 20)[0]
                    p_flags = struct.unpack_from("<I", ph, 24)[0]
                    p_align = struct.unpack_from("<I", ph, 28)[0]
                    end_addr = p_vaddr + p_memsz
                    if p_type == 1 and end_addr > max_end:
                        max_end = end_addr
                    print(
                        "[ELFDBG] PH[{}] type=0x{:08X} off=0x{:08X} vaddr=0x{:08X} filesz=0x{:X} memsz=0x{:X} flags=0x{:X} align=0x{:X}".format(
                            i, p_type, p_offset, p_vaddr, p_filesz, p_memsz, p_flags, p_align
                        )
                    )
                print("[ELFDBG] calculated image size = 0x{:X} ({} bytes)".format(max_end, max_end))

            f.close()
        except Exception as e:
            print("[ELFDBG] header/PHDR parse failed: " + str(e))
            try:
                f.close()
            except:
                pass
            return None

        mem_size = ElfTools.get_elf_memory_size(filename)
        if mem_size == 0:
            print("[ELFDBG] get_elf_memory_size() returned 0")
            return None
        print("[*] ELF requires memory: {} bytes (0x{:X})".format(mem_size, mem_size))

        self.loaded_elf_base = self.dbg.call(ADDR_MALLOC, mem_size)
        if not self.loaded_elf_base:
            print("[ELFDBG] ADDR_MALLOC returned NULL for ELF image")
            return None
        print("[*] Allocated Target Memory at: 0x{:08X}".format(self.loaded_elf_base))

        # ---- 路径诊断 ----
        full_path_for_c = app_dir + "\\" + filename
        print("[ELFDBG] Python path       = " + str(filename))
        print("[ELFDBG] C loader path     = " + str(full_path_for_c))
        path_bytes = ElfTools.str_to_utf16le_bytes(full_path_for_c)
        print("[ELFDBG] UTF-16LE path len = {} bytes".format(len(path_bytes)))
        print("[ELFDBG] UTF-16LE bytes   = {}".format(
            " ".join("{:02X}".format(b) for b in path_bytes[:128])
        ))

        path_addr = self.dbg.call(ADDR_MALLOC, len(path_bytes))
        print("[ELFDBG] path buffer      = 0x{:08X}".format(path_addr))
        if not path_addr:
            print("[ELFDBG] ADDR_MALLOC returned NULL for path buffer")
            return None
        self.dbg.write_mem_bytes(path_addr, path_bytes)

        try:
            print("[ELFDBG] loader address   = 0x{:08X}".format(self.loader_addr))
            print("[ELFDBG] ELF base         = 0x{:08X}".format(self.loaded_elf_base))
            if elf_info is not None:
                expected = (self.loaded_elf_base + elf_info["entry"]) & 0xFFFFFFFF
                print("[ELFDBG] e_entry          = 0x{:08X}".format(elf_info["entry"]))
                print("[ELFDBG] expected runtime = 0x{:08X}".format(expected))

            # 在 shellcode 执行前读取 ELF image 开头，确认目标 RAM 可读写。
            try:
                probe = self.dbg.read_mem(self.loaded_elf_base, 16)
                print("[ELFDBG] target RAM probe = {}".format(
                    " ".join("{:02X}".format(b) for b in probe)
                ))
            except Exception as e:
                print("[ELFDBG] target RAM probe failed: " + str(e))

            print("[*] Calling shellcode loader...")
            # Shellcode：R0 = UTF-16 路径，R1 = 目标基址
            self.entry_point = self.dbg.call(
                self.loader_addr,
                path_addr,
                self.loaded_elf_base
            )
            print("[ELFDBG] shellcode raw return = 0x{:08X}".format(self.entry_point))

            if self.entry_point == 0:
                print("[ELFDBG] shellcode returned NULL/0")
                print("[ELFDBG] This means the loader hit one of its failure exits before returning base+e_entry.")
                return None

            if self.entry_point < 0x30000000:
                print("[ELFDBG] returned address below 0x30000000")
                return None

            if elf_info is not None:
                expected = (self.loaded_elf_base + elf_info["entry"]) & 0xFFFFFFFF
                print("[ELFDBG] return delta      = 0x{:08X}".format((self.entry_point - self.loaded_elf_base) & 0xFFFFFFFF))
                print("[ELFDBG] expected entry    = 0x{:08X}".format(expected))
                if self.entry_point != expected:
                    print("[ELFDBG] WARNING: return != base + e_entry")

            # 读取加载后的前 16 字节；如果 shellcode 成功拷贝 PT_LOAD，
            # 这里应该能看到 ELF .text 的机器码。
            try:
                probe_after = self.dbg.read_mem(self.loaded_elf_base, 16)
                print("[ELFDBG] loaded RAM probe = {}".format(
                    " ".join("{:02X}".format(b) for b in probe_after)
                ))
            except Exception as e:
                print("[ELFDBG] loaded RAM probe failed: " + str(e))

            return self.entry_point
        finally:
            if path_addr:
                self.dbg.call(ADDR_FREE, path_addr)
    
    def unload(self):
        if self.loaded_elf_base: self.dbg.call(ADDR_FREE, self.loaded_elf_base)
        if self.loader_addr: self.dbg.call(ADDR_FREE, self.loader_addr)

class LogReader:
    def __init__(self, debug_interface, log_struct_addr):
        self.dbg = debug_interface
        self.addr = log_struct_addr
        self.local_head = 0
        self.log_size = 4096 # 必须与 C 代码的 LOG_BUFFER_SIZE 一致

    def process(self):
        try:
            head_bytes = self.dbg.read_mem(self.addr, 4)
            remote_head = struct.unpack("<I", head_bytes)[0]
        except Exception as e:
            return

        if remote_head > self.local_head:
            start_idx = self.local_head % self.log_size
            end_idx = remote_head % self.log_size
            new_data = b""
            if end_idx > start_idx:
                data_addr = self.addr + 4 + start_idx
                new_data = self.dbg.read_mem(data_addr, end_idx - start_idx)
            else:
                len1 = self.log_size - start_idx
                if len1 > 0: new_data += self.dbg.read_mem(self.addr + 4 + start_idx, len1)
                if end_idx > 0: new_data += self.dbg.read_mem(self.addr + 4, end_idx)
            
            try:
                # Micropython的print不支持end='', 我们通过sys.stdout.write模拟
                import sys
                sys.stdout.write(new_data.decode('utf-8', 'ignore'))
            except Exception:
                print(str(new_data)) # 出错时打印原始字节
            self.local_head = remote_head

class AppConfigBuilder:
    def __init__(self, dbg, app_dir):
        self.dbg = dbg
        self.app_dir = app_dir
        self.allocations = []
        # 这里的路径使用 app_dir 变量
        self.args = ["my_app", "-iwad", self.app_dir + "\\doom1.wad"]
        self.envs = {"HOME": self.app_dir}
        self.keys = []

    def _alloc_str(self, s):
        b = s.encode('utf-8') + b'\x00'
        size = (len(b) + 3) & ~3
        addr = self.dbg.call(ADDR_MALLOC, size)
        self.dbg.write_mem_bytes(addr, b)
        self.allocations.append(addr)
        return addr

    def _alloc_ptr_array(self, pointers):
        size = len(pointers) * 4
        if size == 0: return 0
        addr = self.dbg.call(ADDR_MALLOC, size)
        # 避免使用 f-string（MicroPython 兼容）
        pack_format = "<" + "I" * len(pointers)
        self.dbg.write_mem_bytes(addr, struct.pack(pack_format, *pointers))
        self.allocations.append(addr)
        return addr

    def build(self):
        print("[*] Building configuration structure...")
        arg_ptrs = [self._alloc_str(x) for x in self.args]
        argv_addr = self._alloc_ptr_array(arg_ptrs)
        
        env_array_addr = 0 # 简化，可以按需实现
        key_array_addr = 0 # 简化

        config_size = 28 # 7 * 4 字节
        config_addr = self.dbg.call(ADDR_MALLOC, config_size)
        
        MAGIC = 0xD00BC760 
        payload = struct.pack("<IIIIIII", MAGIC, len(self.args), argv_addr, len(self.envs), env_array_addr, len(self.keys), key_array_addr)
        self.dbg.write_mem_bytes(config_addr, payload)
        self.allocations.append(config_addr)
        
        print("[+] Config built at: 0x{:08X}".format(config_addr))
        return config_addr

    def free_all(self):
        for addr in reversed(self.allocations):
            self.dbg.call(ADDR_FREE, addr)
        self.allocations = []

# --- 主执行函数 ---
def run_app(elf_filename, app_dir_for_c, enable_config=True, enable_logging=True):
    print("--- HP Prime App Loader ---")
    print("  - App: " + elf_filename)
    print("  - Config enabled: " + str(enable_config))
    print("  - Logging enabled: " + str(enable_logging))
    
    dbg = None
    loader = None
    config_builder = None
    
    try:
        dbg = PrimeDebug()
        loader = ShellcodeElfLoader(dbg)
        
        if not loader.upload_loader():
            raise RuntimeError("Failed to upload shellcode loader")
        
        # 传入 C 代码需要的完整路径前缀
        entry_point = loader.load_elf(elf_filename, app_dir_for_c)
        if not entry_point:
            raise RuntimeError("Failed to load ELF file")

        config_addr = 0
        if enable_config:
            config_builder = AppConfigBuilder(dbg, app_dir_for_c)
            config_addr = config_builder.build()
        else:
            print("[*] Skipping config creation.")

        print("[*] Calling entry point 0x{:08X} with config_addr=0x{:08X}".format(entry_point, config_addr))
        
        # Micropython的input可能行为不同，使用print提示
        print(">>> Press Enter on PC to launch... <<<")
        try:
            input()
        except: # 在某些没有input的micropython环境，直接跳过
            pass
            
        log_addr = dbg.call(entry_point, config_addr, 0)
        print("[+] C code returned log structure address: 0x{:08X}".format(log_addr))

        if not enable_logging:
            print("[*] Logging is disabled. Script will now exit.")
            return

        if log_addr < 0x30000000:
             print("[!] Invalid log address returned. C code may have crashed early.")
             return

        logger = LogReader(dbg, log_addr)
        print("\n--- Streaming Logs (Press Ctrl+C to stop) ---")
        while True:
            logger.process()
            # 用 hpprime.ticks() 实现非阻塞延时，替代 time.sleep()
            start_ticks = hpprime.ticks()
            while hpprime.ticks() - start_ticks < 100: # 延时约100ms
                pass

    except KeyboardInterrupt:
        print("\n[!] Interrupted by user.")
    except Exception as e:
        print("[!!] An error occurred: " + str(e))
    finally:
        print("\n--- Shutting down loader ---")
        if config_builder: config_builder.free_all()
        if loader: loader.unload()
        if dbg: dbg.close()
        print("--- Shutdown complete. ---")

APP_ELF_FILENAME = "my_app.elf" 
APP_DIR_FOR_C_CODE = "C:\\DATA\\suika.hpappdir"

run_app(APP_ELF_FILENAME, APP_DIR_FOR_C_CODE, enable_config=True, enable_logging=True)

