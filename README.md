# PrusaSlicer Measure & Section

这是基于 PrusaSlicer **2.9.7 稳定版**的独立修改版，保留上游 AGPL-3.0-or-later 许可。

- [下载 Windows x64 安装包和便携版](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/releases/latest)
- [修改版源码](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/tree/codex/prusaslicer-2.9.7-three-point-circle)
- [Windows 构建与安装验证](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/actions/workflows/build_windows.yml)

## 功能与使用

以下版本号表示修改版功能首次提供的版本。使用前从上方下载页确认对应版本是否已发布。

### 测量

- **普通测量**。选中模型，打开「测量」（U），按需选择点、边、面或圆。按住 Shift 可选取特征上的点。普通测量可查看距离和角度，也可选取已识别圆的圆心作为测量点。
- **圆测量**。在「测量模式」中选择「圆测量」。在 2.9.7.11 或 SLA 编辑器中，使用「三点测圆」复选框进入。每次进入默认使用三点取样，并吸附到可见网格顶点。沿同一圈圆边分散选三个点，读取直径、半径和圆心。可撤销上一个点，或关闭吸附以选取面内点。Windows FFF 也可勾选「选取已识别的圆边」，单击高亮轮廓。三点测圆的精度取决于网格和取点，椭圆或倒角边上的三点也能构造圆。
- **基准面垂直距（2.9.7.12）**。在 Windows FFF 编辑器中选择「垂直距离」，单击面锁定基准。移动鼠标预览目标点到基准面的垂直距离，单击保留读数。继续单击可换目标而保留基准，按住 Shift 可吸附可见顶点。对于斜面，距离随取点位置而变。Esc 或退格键可依次清除目标和基准。
- **圆心距（2.9.7.12）**。在 Windows FFF 编辑器中选择「圆心距」，依次测第一圆和第二圆。图形区标出三维中心距、沿打印平台坐标轴的带符号 ΔX、ΔY、ΔZ 和两圆面法线夹角 α。侧栏还提供以第一圆为基准的面内投影距、轴向偏移和直径差。用「重选第二圆」复用第一圆，用「重新测量」清空两圆。交换两圆会反转坐标差和直径差的符号，夹角大小保持一致。
- **悬垂坡角（2.9.7.12）**。在 Windows FFF 的测量面板勾选「显示悬垂坡角」，转动视角露出模型底面。指针移到朝下的模型表面时，坡角会浮现在指针旁，侧栏同时显示读数。水平悬空面为 0°，越接近竖直越接近 90°。朝上表面、竖直面和剖切填充无读数。实际支撑可在切片预览中查看。
- **单位**。长度与坐标读数及剖切偏移随英寸设置换算，单位显示为 `in` 或 `mm`。角度使用度。

2.9.7.12 的新测量功能适用于选中的一个零件实例，可在该实例内的多个体之间测量。测量基准用于当前工具会话。

圆测量、圆心距和垂直距离都支持左键轻点取样、左键拖动旋转、右键或中键拖动平移及滚轮缩放。移动视角时保留已完成的取点和基准。

### 剖切与透明度

- **单平面和复合平面剖切**。点击左侧剖切立方体图标，选择 YZ、XZ 或 XY 主平面并调整位置和方向。复合剖切添加第二个正交平面，在视图中隐藏两平面共同限定的一角。可关闭剖切填充，点击「等轴测」恢复标准视角。反转剖切保持相机朝向。剖切只影响视图，打开切割等工具时会暂停，关闭后可恢复。
- **剖视测量**。测量模式可用在单剖和复剖。关闭填充后仍可量模型及切口边界。拖动剖切滑块时暂停拾取，普通测量在调整后需重新选取。自 2.9.7.12 起，锁定的真实几何基准在剖切移动后仍可使用，被裁掉时会显示隐藏状态，圆心距的跨圆标注也会暂时收起。依赖剖切几何的基准在切面改变后需重选。换件、变换或替换网格后也需重新选取。
- **分级透明度（2.9.7.11）**。Windows FFF 编辑器支持为整个实例、多个体、同一网格中独立的封闭实体、连通平面及识别出的圆柱孔壁设置 0–100% 透明度。细一级的设置优先。设置可随撤销及 3MF 项目保存，切片结果保持一致。替换网格时保留体级设置，并清除失效的面与独立实体设置。2.9.7.12 的测量面板也提供「穿透透明层选取」。开启后，优先选取射线上最近的不透明面，底面也可选。射线上只有半透明面时，可选取背向视角的半透明面。100% 透明的面会被跳过。

### 网格编辑与摆放

- **STL 倒角与圆角（2.9.7.11）**。在 Windows FFF 编辑器可直接选边，也可通过两点辅助选择完整直棱、三点辅助选择完整圆柱孔口。输入毫米尺寸，检查预览后应用，可一次撤销整批修改。支持符合几何条件的共面相接直边和矩形闭环。程序检查加工尺寸、干涉和网格有效性。加工后可保存 3MF 或导出 STL。
- **旋转与摆放**。打开「旋转」（R），单击轴手柄正向旋转 90°，拖动可自由旋转。侧栏「旋转（相对）」使用固定基准，同一轴从 30° 改为 45° 时，最终相对基准为 45°。同实例重新选中时可恢复读数。点击「设为旋转基准」可保持姿态并归零。选中完整模型后，点击「自动朝向并居中」可用凸包最大支撑面贴底，并将模型居中放到当前打印平台。

### Windows 安装与验证

安装 `*-Setup.exe`，或解压 `*-Portable.zip` 到可写目录后运行 `prusa-slicer.exe`。安装包沿用原 AppId，支持覆盖升级并保留用户预设。需要时通过「配置 → 配置向导」设定打印机和材料。发布页同时提供对应的完整源码 `*-Source.zip` 和三个包的 `SHA256.txt`。

Windows CI 检查复合剖切着色器、几何与透明度回归、编译、安装、覆盖升级和命令行启动。真实 STL 的交互取点和显卡显示在本地试用版中验收。本地编译可参考下方上游 Windows 指南。

### 功能文档维护

每次新增、修改或移除面向用户的功能，都在同一改动中更新本 README 的功能清单和用法。验收时核对操作入口、版本和适用范围，再将代码与 README 一同更新到 GitHub。

---


![PrusaSlicer logo](/resources/icons/PrusaSlicer.png?raw=true)

# PrusaSlicer

You may want to check the [PrusaSlicer project page](https://www.prusa3d.com/prusaslicer/).
Prebuilt Windows, OSX and Linux binaries are available through the [git releases page](https://github.com/prusa3d/PrusaSlicer/releases) or from the [Prusa3D downloads page](https://www.prusa3d.com/drivers/). There are also [3rd party Linux builds available](https://github.com/prusa3d/PrusaSlicer/wiki/PrusaSlicer-on-Linux---binary-distributions).

PrusaSlicer takes 3D models (STL, OBJ, AMF) and converts them into G-code
instructions for FFF printers or PNG layers for mSLA 3D printers. It's
compatible with any modern printer based on the RepRap toolchain, including all
those based on the Marlin, Prusa, Sprinter and Repetier firmware. It also works
with Mach3, LinuxCNC and Machinekit controllers.

PrusaSlicer is based on [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

See the [project homepage](https://www.prusa3d.com/slic3r-prusa-edition/) and
the [documentation directory](doc/) for more information.

### What language is it written in?

All user facing code is written in C++.
The slicing core is the `libslic3r` library, which can be built and used in a standalone way.
The command line interface is a thin wrapper over `libslic3r`.

### What are PrusaSlicer's main features?

Key features are:

* **multi-platform** (Linux/Mac/Win) and packaged as standalone-app with no dependencies required
* complete **command-line interface** to use it with no GUI
* multi-material **(multiple extruders)** object printing
* multiple G-code flavors supported (RepRap, Makerbot, Mach3, Machinekit etc.)
* ability to plate **multiple objects having distinct print settings**
* **multithread** processing
* **STL auto-repair** (tolerance for broken models)
* wide automated unit testing

Other major features are:

* combine infill every 'n' perimeters layer to speed up printing
* **3D preview** (including multi-material files)
* **multiple layer heights** in a single print
* **spiral vase** mode for bumpless vases
* fine-grained configuration of speed, acceleration, extrusion width
* several infill patterns including honeycomb, spirals, Hilbert curves
* support material, raft, brim, skirt
* **standby temperature** and automatic wiping for multi-extruder printing
* [customizable **G-code macros**](https://github.com/prusa3d/PrusaSlicer/wiki/PrusaSlicer-Macro-Language) and output filename with variable placeholders
* support for **post-processing scripts**
* **cooling logic** controlling fan speed and dynamic print speed

### Development

If you want to compile the source yourself, follow the instructions on one of
these documentation pages:
* [Linux](doc/How%20to%20build%20-%20Linux%20et%20al.md)
* [macOS](doc/How%20to%20build%20-%20Mac%20OS.md)
* [Windows](doc/How%20to%20build%20-%20Windows.md)

### Can I help?

Sure! You can do the following to find things that are available to help with:
* Add an [issue](https://github.com/prusa3d/PrusaSlicer/issues) to the github tracker if it isn't already present.
* Look at [issues labeled "volunteer needed"](https://github.com/prusa3d/PrusaSlicer/issues?utf8=%E2%9C%93&q=is%3Aopen+is%3Aissue+label%3A%22volunteer+needed%22)

### What's PrusaSlicer license?

PrusaSlicer is licensed under the _GNU Affero General Public License, version 3_.
The PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

### How can I use PrusaSlicer from the command line?

Please refer to the [Command Line Interface](https://github.com/prusa3d/PrusaSlicer/wiki/Command-Line-Interface) wiki page.
