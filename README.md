# PrusaSlicer Measure & Section

这是基于 PrusaSlicer **2.9.7 稳定版**的独立修改版，保留上游 AGPL-3.0-or-later 许可。

- [下载 Windows x64 安装包和便携版](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/releases/latest)
- [修改版源码](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/tree/codex/prusaslicer-2.9.7-three-point-circle)
- [Windows 构建与安装验证](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/actions/workflows/build_windows.yml)

## 2.9.7.9 使用说明

**旋转与摆放**：打开「旋转」（R），单击任一轴的旋转手柄沿该轴正向旋转 90°，也可以拖动手柄选择角度。侧栏「旋转（相对）」以开始输入时的姿态为零点，提交后保留输入值。例如同一轴输入 30° 后改为 45°，最终相对起始姿态为 45°。选中一个完整模型后，点击侧栏「自动朝向并居中」，程序会以模型凸包面积最大的平面作为底面，贴到底板并移到当前打印平台中心。

**三点测圆**：导入 STL、选中模型，打开左侧「测量」工具（U），勾选「三点测圆」。默认只接受可见网格顶点；鼠标附近的可见三角形共同参与候选搜索，靠近目标出现黄色菱形后单击。没有候选顶点时不会自动改选面内点。三个已选点用青色标记及 1、2、3 编号显示；误选时点击「撤销上一个点」。沿同一圆边选择三个分散的点后显示直径、半径和圆心。取消「吸附到网格顶点」可自由选面内点。测量精度受 STL 网格精度和取点分布影响。

**单位**：普通测量、三点测圆和剖切偏移都随英寸设置换算，单位符号固定显示为 `in` 或 `mm`。

**剖切**：点击左侧与测量工具同列的剖切立方体图标，打开正交投影剖面面板。选择 YZ、XZ、XY 主平面，可移动位置和反转方向。反转只改变保留侧，打印平台和坐标轴保持当前视角。「复合平面剖切」添加第二个垂直平面，分别控制两个平面的位置和方向，切去它们共同限定的一个角区，显示两个填充剖面。点击「等轴测」回到固定的等轴测视角。也可以手动旋转视图来查看另一侧的剖面。剖切只影响显示。打开切割等编辑工具时剖切暂停，关闭工具后恢复。

**剖视测量**：选中模型后，在普通或复合剖视中打开「测量」（U）。拾取范围为保留的点、边和面，以及当前显示的剖面。可用 Shift 选取表面上的点，也可切换三点测圆。拖动剖切滑块时暂停拾取，调整后需重新选取。剖视测量为只读。

**本地运行**：安装 `*-Setup.exe`；也可解压 `*-Portable.zip` 到可写目录后运行 `prusa-slicer.exe`。安装包沿用原 AppId，支持覆盖升级并保留用户预设。普通启动直接进入主界面，需要时可通过「配置 → 配置向导」设定打印机和材料。`*-Source.zip` 是与安装包对应的完整源码；本地编译按下方上游 Windows 指南进行，需要 Visual Studio C++ 工具链和依赖库。

每次发布的 SHA256.txt 同时记录安装包、便携版及源码包校验值。CI 检查着色器编译、复合剖切 GPU 行为、Windows 编译、安装、覆盖升级和命令行启动；真实 STL 的交互取点与显卡显示效果仍需实机复核。

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
