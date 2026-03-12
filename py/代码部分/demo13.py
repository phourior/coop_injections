import pymem


def get_pointer_address(pm, base, offsets):
    # 初始地址 = 模块基址 + 第一个偏移量
    addr = pm.read_longlong(base)

    # 循环追踪多级偏移 (除了最后一个)
    for offset in offsets[:-1]:
        addr = pm.read_longlong(addr + offset)

    # 返回最终的目标地址
    return addr + offsets[-1]

#process_name 游戏进程 ,base_static_offset 起始地址,多层偏移
def base_address(base_static_offset,pointer_offsets,process_name= "SC2_x64.exe"):
    try:
        # 1. 初始化进程
        pm = pymem.Pymem(process_name)

        # 2. 获取模块基址
        module = pymem.process.module_from_name(pm.process_handle, process_name).lpBaseOfDll

        # 3. 设置偏移量
        # "SC2_x64.exe"+05A7F2A0 对应静态偏移
        # base_static_offset = 0x05A7F2A0
        # pointer_offsets = [0x120, 0x1C0]

        # 4. 计算最终地址
        # 注意：64位游戏通常使用 longlong (8字节) 读取地址
        target_addr = get_pointer_address(pm, module + base_static_offset, pointer_offsets)

        # 5. 读取该地址的值 (假设它是整数，如果是浮点数请用 read_float)
        value = pm.read_float(target_addr)

        # print(f"基址地址: {hex(module)}")
        # print(f"目标内存地址: {hex(target_addr)}")
        # print(f"当前数值: {value}")
        return value

    except Exception as e:
        print(f"发生错误: {e}")

# base_static_offset = 0x05A7F2A0
# pointer_offsets = [0x120, 0x1C0]
#
# print(base_address(base_static_offset,pointer_offsets))
#
# base_static_offset = 0x0447F388
# pointer_offsets = [0x0, 0x0,0x68]
# print(base_address(base_static_offset,pointer_offsets))
# crood_x=base_address(0x243BA95EBD4,[])
# print(crood_x)
