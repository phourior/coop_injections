def get_fitted_stretched_rect(ref_orig, boundary, target_orig):
    """
    1. 先按参考比例变形
    2. 再等比缩放至触碰边界(Aspect Fit)
    :param ref_orig: 原始参考系 (100, 100)
    :param boundary: 拉伸上限/边界 (262, 258)
    :param target_orig: 想要放大的不定长矩形 (w, h)
    """
    # Step 1: 计算基础拉伸比例 (sx, sy)
    sx_base = boundary[0] / ref_orig[0]
    sy_base = boundary[1] / ref_orig[1]

    # Step 2: 目标矩形经过“初始变形”后的逻辑尺寸
    # 注意：这里是模拟它在变形坐标系下的原始形状
    target_deformed_w = target_orig[0] * sx_base
    target_deformed_h = target_orig[1] * sy_base

    # Step 3: 计算缩放因子 k，使得变形后的矩形适配 boundary
    # 我们需要找到 k，使得 k*w <= 262 且 k*h <= 258，且其中一个取等号
    k = min(boundary[0] / target_deformed_w, boundary[1] / target_deformed_h)

    # Step 4: 计算最终尺寸
    final_w = target_deformed_w * k
    final_h = target_deformed_h * k

    # 计算相对于原始 target_orig 的总面积比
    final_area_ratio = (final_w * final_h) / (target_orig[0] * target_orig[1])

    return {
        "final_dimensions": (round(final_w, 2), round(final_h, 2)),
        "limiting_factor": "Width" if (boundary[0] / target_deformed_w) < (
                    boundary[1] / target_deformed_h) else "Height",
        "total_area_ratio": round(final_area_ratio, 4)
    }




def Margin(size,layout_size):
    x,y=size
    l_x,l_y=layout_size
    if x == l_x:
        Margin_x=0
    else:
        Margin_x=round(l_x-x)/2

    if y == l_y:
        Margin_y=0
    else:
        Margin_y=round(l_y-y)/2
    return (Margin_x, Margin_y)

def map_coordinate(source_pos, source_size, target_size,margin_size):
    """
    将大地图坐标映射到小地图坐标
    :param source_pos: (x, y) 大地图上的坐标
    :param source_size: (width, height) 大地图的总尺寸
    :param target_size: (width, height) 小地图的总尺寸
    :return: (x, y) 映射后的坐标
    """
    src_x, src_y = source_pos[0]-margin_size[0], source_pos[1]-margin_size[1]
    print("src_x ---25.93920898--",src_x)
    print("src_y ---138.2319336--", src_y)
    src_w, src_h = source_size

    tgt_w, tgt_h = target_size
    print("tgt_w --172--",tgt_w)
    print("tgt_h --180--", tgt_h)

    # 计算缩放比例
    map_x = src_x * (tgt_w / src_w)
    map_y = src_y * (tgt_h / src_h)
    # print(source_pos)
    print("--map_x,map_y--",map_x,map_y)
    return (map_x, map_y)

# map_size=(100, 110)
# test1=get_fitted_stretched_rect((256, 256),(262, 258),map_size)['final_dimensions']
# result=map_coordinate((50,55),map_size,test1,)
# print(result)
#


#这里是计算神器坐标在屏幕上的投影。
# 第一次计算 get_fitted_stretched_rect 是将星际争霸的小地图界限，拉伸到和屏幕左下角同样的尺寸
#第一步并不准确，当纵坐标被缩放时，会偏移几像素，但不知道算法，所以无能为力
#第二次计算是 map_coordinate 将内存地址中神器的坐标值换算到拉伸后矩形上的坐标
#第三次计算是 Margin 因为有的纵横比，有的地图拉伸后无法填满左下角小地图区域，所以要投影到屏幕上
#还要计算那些没有被填满缝隙
#三次计算后还有一定的偏差，但精力有限无法完全还原。
#map_size是星际争霸2编辑器合作地图中的地图区域，内存中的坐标是地图区域
# map_size是星际争霸2编辑器合作地图中的镜头区域，是玩家可以操作单位的坐标范围
# layout_size是左下角的小地图区域 按像素点计算 1980*1080
# 中小地图左端顶点为 22，823 大小为262，258（基于小地图镜头的上，下极限坐标计算）
# coord 是内存中神器的坐标
def map_crood_execute(map_size, camera_size, layout_size, coord,margin_size):
    stretched_size = get_fitted_stretched_rect((101, 100), (262, 257), (172, 180))['final_dimensions']
    layout_x,layout_y=stretched_size[0],stretched_size[1]
    coord_x,coord_y=coord
    margin_size_x,margin_size_y=margin_size
    x=(coord_x - margin_size_x)/camera_size[0]*layout_x
    y=(coord_y - margin_size_y)/camera_size[1]*layout_y


    test = Margin(stretched_size, layout_size)
    x=test[0]+x
    y=test[1]+y
    return (x,y)

# print(test)
if __name__=="__main__":

    stretched_size = get_fitted_stretched_rect((101, 100), (262,257), (172,180))['final_dimensions']
    print(stretched_size)
