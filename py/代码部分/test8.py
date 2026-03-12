import sys
from PyQt6.QtWidgets import QApplication, QWidget
from PyQt6.QtCore import Qt, QTimer  # 导入 QTimer

import demo13
import demo16

map_size_list={"虚空撕裂":[(208,200),(175,160),(16,12)]
,"克哈裂痕":[(216,216),(196,173),(10,13)]
,"熔火危机":[(200,200),(180,172),(10,8)]
,"聚铁成兵":[(216,200),(178,176),(20,6)]
,"营救矿工":[(200,184),(180,156),(10,8)]
,"黑暗杀星":[(216,192),(181,164),(18,9)]
,"死亡摇篮":[(256,256),(218,193),(20,30)]
,"天界封锁":[(208,216),(172,180),(18,16)]
,"升格之链":[(216,184),(196,164),(10,8)]
,"虚空降临":[(152,192),(132,164),(10,8)]
,"机会渺茫":[(168,184),(148,156),(10,8)]
,"净网行动":[(208,192),(192,164),(10,8)]
,"亡者之夜":[(192,192),(160,160),(16,8)]
,"往日神庙":[(192,200),(176,172),(8,8)]
,"湮灭快车":[(224,160),(192,144),(16,8)]}
class PixelPoint(QWidget):
    def __init__(self, x=25, y=807):
        super().__init__()
        #地图的大小参数集 下列参数可以从星际争霸2地图编辑器中找到
        # 第一位是地图界限的大小，第二位是镜头界限的大小，第三个是镜头界限的边距 x=边距-7，y=边距-4
        self.map_size=None
        #存放神器 x坐标 和 y坐标的基址
        self.base_address=None
        #屏幕上小地图大小的区域和区域左下端顶点的坐标 1920*1080屏幕下小地图大小是 262*257
        #在这个矩形内部，点击任何一个位置都可以触发建筑的集结线
        # 28,1065 这个坐标是这个矩形最左下的点，也是把镜头移动到最左下面，形成梯形的左下角的像素坐标(前提小地图得铺满)
        self.windows_size_crood=None
        #神器的x坐标
        self.crood_x=None
        #神器的y坐标
        self.crood_y=None
        # 1. 窗口属性设置
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.WindowStaysOnTopHint |
            Qt.WindowType.Tool |
            Qt.WindowType.WindowTransparentForInput
        )

        self.setStyleSheet("background-color: yellow;")
        self.setGeometry(x, y, 5, 5)

        # --- 核心修改部分：设置定时器 ---
        self.timer = QTimer(self)
        # 绑定定时器溢出信号到自定义的更新槽函数
        self.timer.timeout.connect(self.update_position)
        # 设置刷新频率，单位为毫秒（例如 100ms 刷新一次）
        self.timer.start(2000)

    def up_map(self,map):
        self.map_size=map_size_list[map[0]]
        print()
        self.base_address=map[1]
        self.windows_size_crood=map[2]
        print("当前坐标参考地图: ",map[0])


    def update_position(self):
        """
        在这里编写你的坐标逻辑
        """
        if self.map_size and self.base_address and self.windows_size_crood:
            self.setStyleSheet("background-color: yellow;")
            # 假设你需要调用 demo12 来获取新坐标
            try:
                # print(self.base_address[0][0],self.base_address[0][1])
                # print(self.base_address)
                self.crood_y=demo13.base_address(self.base_address[0][0],self.base_address[0][1])+29
                self.crood_x=demo13.base_address(self.base_address[1][0],self.base_address[1][1])
                # crood_x=demo15.read_sc2_memory(0x2AB4113CF70)
                # crood_y=demo15.read_sc2_memory(0x2AB4113CF74)
                # newcoord = demo16.map_crood_execute((208,216),(175,160),(262,257),(crood_x,crood_y),(16,12))
                newcoord = demo16.map_crood_execute(self.map_size[0],self.map_size[1],self.windows_size_crood[:2],(self.crood_x,self.crood_y),self.map_size[2])
                new_x, new_y =  round(self.windows_size_crood[2]+newcoord[0]), round(self.windows_size_crood[3]-(newcoord[1]))
                print("move(new_x, new_y)",new_x,new_y)
                self.move(new_x, new_y) # 使用 move 方法改变位置
            except Exception as e:
                print(f"更新坐标失败: {e}")
        else:
            self.setStyleSheet("background-color: red;")


        #
        # # 示例：让点随机晃动一下，证明它在动
        # import random
        # offset_x = random.randint(-5, 5)
        # offset_y = random.randint(-5, 5)
        # self.move(self.x() + offset_x, self.y() + offset_y)


def run_app():
    app = QApplication(sys.argv)
    # point = PixelPoint(35+139 , 1065-79)
    point = PixelPoint(25, 807)
    # point=PixelPoint(200,950)
    point.show()
    sys.exit(app.exec())



if __name__ == "__main__":
    run_app()