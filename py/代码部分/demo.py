import sys
from PyQt6.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout,
                             QGridLayout, QLabel, QRadioButton, QPushButton)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QPixmap
from PyQt6.QtCore import pyqtSignal,QObject
import test8

# base_address = [(0x05A7F2A0,[0x120,0x1C0]),(0x0447F388,[0x0,0x0,0x68])]
#这个坐标值是准确的，但它不稳定，会不停的变动
#base_address = [(0x05A7F2A0,[0x120,0x1C0]),(0x0447F388,[0x0,0x0,0x68])]
#这个坐标值不知道为什么，+29，等于正确的坐标，所以如果遇到这种+数值正确又稳定的坐标在后面改吧
base_address = [(0x0447F388,[0x0,0x0,0x6C]),(0x0447F388,[0x0,0x0,0x68])]
windows_size_crood = (262,257,28,1065)

class KeySignaler(QObject):
    # 这个信号用于将地图名传递给子窗口
    map_name=pyqtSignal(list)

class GamePlugin(QWidget):
    def __init__(self):
        super().__init__()
        self.initUI()


    def initUI(self):
        self.setWindowTitle("泽拉图找神器挂")
        self.setFixedSize(550, 650)
        #初始化信号
        self.signaler=KeySignaler()

        #实例化神器提示控件
        self.Point = test8.PixelPoint()
        self.Point.show()

        #传递信号到子窗口
        self.signaler.map_name.connect(self.Point.up_map)


        # 整体背景色
        self.setStyleSheet("background-color: #1a1a1a; color: #ffffff; font-family: 'Microsoft YaHei';")

        layout = QVBoxLayout()
        layout.setContentsMargins(20, 20, 20, 20)

        # ---------------------------------------------------------
        # 1. 顶部标题
        # ---------------------------------------------------------
        self.title_label = QLabel("泽拉图外挂")
        self.title_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.title_label.setStyleSheet("""
            font-size: 18px; font-weight: bold; color: #00ff00;
            background-color: #262626; padding: 10px; border-radius: 4px;
        """)
        layout.addWidget(self.title_label)

        # ---------------------------------------------------------
        # 2 & 3. 中间区域：单选框 + 拨杆开关
        # ---------------------------------------------------------

        # ---------------------------------------------------------
        # 4. 底部图片网格 (保持之前的逻辑)
        # ---------------------------------------------------------
        grid = QGridLayout()
        self.cells = []
        for i in range(15):
            cell = QLabel()
            cell.setFixedSize(150, 85)
            cell.setAlignment(Qt.AlignmentFlag.AlignCenter)
            # 加载图片 (请替换为你自己的图片路径)
            pix = QPixmap(f"./MapName/{i+1}.png")  # 替换为实际路径
            if not pix.isNull():
                cell.setPixmap(pix.scaled(140, 75, Qt.AspectRatioMode.KeepAspectRatio))
            else:
                cell.setText(f"物品 {i + 1}")

            # 基础样式：无边框
            cell.setStyleSheet("background-color: #333; border: 2px solid transparent; border-radius: 5px;")

            # 绑定鼠标点击
            cell.mousePressEvent = lambda e, c=cell, idx=i: self.on_cell_clicked(c, idx)
            grid.addWidget(cell, i // 3, i % 3)
            self.cells.append(cell)

        layout.addLayout(grid)
        self.setLayout(layout)


    def on_cell_clicked(self, clicked_cell, idx):
        MapList=["虚空撕裂","克哈裂痕","虚空降临","往日神庙","湮灭快车","天界封锁","升格之链","熔火危机","机会渺茫","营救矿工","亡者之夜","黑暗杀星","净网行动","聚铁成兵","死亡摇篮"]
        for c in self.cells:
            c.setStyleSheet("background-color: #222; border: 2px solid transparent; border-radius: 5px; color: #555;")
        clicked_cell.setStyleSheet(
            "background-color: #222; border: 2px solid #00ff00; border-radius: 5px; color: #00ff00;")

        # print(f"传入{MapList[idx]}开启小地图提示，关闭建筑摆放提示")
        # self.master_button_clicked.emit([MapList[idx], self.switches['小地图'].isChecked(),False])
        self.signaler.map_name.emit([MapList[idx],base_address,windows_size_crood])
        hex_iterable_x = map(hex, base_address[1][1])
        hex_iterable_y = map(hex, base_address[0][1])
        self.update_title(f"当前选中：地图 {MapList[idx]} "
                          f"\n x基址:{hex(base_address[1][0])} 偏移:{list(hex_iterable_x)} "
                          f"\n y基址:{base_address[0][0]} 偏移:{list(hex_iterable_y)}"
                          f"\n 当前屏幕小地图设定大小 {windows_size_crood}"
                          f"\n 神器x坐标:{self.Point.crood_x} "
                          f"\n 神器y坐标:{self.Point.crood_y}")
        print(f"当前选中：地图 {MapList[idx]}")


    def update_title(self, text):
        self.title_label.setText(text)


if __name__ == "__main__":
    app = QApplication(sys.argv)
    ex = GamePlugin()
    ex.show()
    sys.exit(app.exec())