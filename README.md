# PrusaSlicer Measure & Section

基于 PrusaSlicer **2.9.7 稳定版**的独立修改版，提供测量、剖切、透明查看和 STL 边缘编辑功能。保留上游 AGPL-3.0-or-later 许可。

## Windows x64 下载

**当前正式版 2.9.7.12**。从 2.9.7.11 覆盖安装可保留打印机预设。使用便携版时，解压后运行 `prusa-slicer.exe`。

- [Windows x64 安装包](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/releases/download/v2.9.7.12-three-point-circle/PrusaSlicer-2.9.7.12-Three-Point-Circle-x64-Setup.exe)
- [便携版 ZIP](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/releases/download/v2.9.7.12-three-point-circle/PrusaSlicer-2.9.7.12-Portable.zip)
- [正式发布说明、对应源码和 SHA-256 校验](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/releases/tag/v2.9.7.12-three-point-circle)

## 主要功能

- **圆测量与圆心距**。三点测圆默认吸附到可见网格顶点，也可选取识别出的圆边。圆心距在视口标出三维中心距、ΔX、ΔY、ΔZ 和两圆面法线夹角，可保留第一圆来连续测量。
- **垂直距离与悬垂坡角**。锁定基准面后连续测量点到面的垂直距离。指向朝下的模型面时，可在指针旁读取相对打印平台的坡角。
- **单平面与复合平面剖切**。沿主平面剖看模型，可调位置、反转方向、切换填充，并在剖视中测量可见几何。
- **分级透明度与穿透选取**。可按整件、实体或面调整透明度。测量和透明度工具均可透过半透明前表面拾取内部几何或底面。
- **STL 倒角和圆角**。选择支持的直棱或圆柱孔口，预览后批量应用，可撤销并保存为 3MF 或导出 STL。
- **旋转与摆放**。支持单击轴手柄旋转 90°、拖动自由旋转、设置相对旋转基准，以及自动贴床居中。

## 使用与源码

- [完整中文使用说明](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/blob/codex/prusaslicer-2.9.7-three-point-circle/README.md)
- [修改版源码分支](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/tree/codex/prusaslicer-2.9.7-three-point-circle)
- [Windows 构建、剖切着色器和覆盖升级验证](https://github.com/HHQYQ/PrusaSlicer-Measure-Section/actions/workflows/build_windows.yml)

`master` 分支保留上游开发代码。编译本修改版请使用上述源码分支，或下载发布页对应的源码包。

每次功能更新均在本页同步记录，完整操作说明见修改版文档。

---

![PrusaSlicer logo](/resources/icons/PrusaSlicer_128px.png)

# PrusaSlicer

PrusaSlicer enables you to take your 3D models, generate 3D printing instructions and send them to your 3D printer. It supports both FDM 3D printers and mSLA 3D printers. It is developed by [Prusa Research](https://www.prusa3d.com/) and apart from Prusa printers it supports machines from a wide variety of manufacturers.

PrusaSlicer is originally based on [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

## Installation

The **recommended installation method** is to go to the [PrusaSlicer project page](https://www.prusa3d.com/prusaslicer/) and download and install the software by following the instructions there.

**Alternatively**, for Windows and macOS, you can download the software directly from the [GitHub releases page](https://github.com/prusa3d/PrusaSlicer/releases). For Linux, PrusaSlicer is currently distributed exclusively through [Flathub](https://flathub.org/en/apps/com.prusa3d.PrusaSlicer).

If you prefer, you can always build PrusaSlicer yourself from source. See the [documentation](doc/Build.md) to learn how to do it.

### Main features

* Set the **printing parameters** with precision - from settings affecting the **whole print** down to **single-layer adjustments**.
* Modify your objects before printing to best suit your needs, you can **paint**, **cut**, **arrange** and do **many more** directly in PrusaSlicer.
* Open **multiple projects** at once, each containing **multiple beds** (each with potentially **different settings**).
* Make full use of **multi-material printing**
* Use your **preferred method to send your prints to the printer**, PrusaSlicer supports a wide range of possibilities.
* **View the generated print instructions** in an advanced **3D preview**.
* You can make use of the **command-line interface** to use **PrusaSlicer without GUI** in your automation setups.
* If you want, you **can make use of the integration** with [PrusaConnect](https://connect.prusa3d.com/) and [Printables](https://www.printables.com) to greatly simplify your workflow.

### Reporting a bug

Did you find a bug? Bugs can be reported in our [GitHub issue tracker](https://github.com/prusa3d/PrusaSlicer/issues), but **first make sure your report complies with the [Issue tracker policy](https://github.com/prusa3d/PrusaSlicer?tab=contributing-ov-file#issue-tracker-policy)**. **Are you not sure?** Get in touch in our [GitHub Discussions](https://www.github.com/prusa3d/discussions), before filing a bug report in the issue tracker. You can always file a bug report later, once you have more confidence.

### How to get in touch

We maintain a [GitHub Discussions](https://www.github.com/prusa3d/discussions) page in this repository to be used for **general discussions**, **questions** and **feature requests**. Furthermore, there is an announcements category, which we use to communicate with you directly.

### Pull requests

Read our [contribution guide](.github/CONTRIBUTING.md) to get more information.

### Technical stack

All of PrusaSlicer is written in C++, using CMake as the build system. The code assumes that the compiler supports C++20.

The slicing backend heavily relies on [Clipper](https://www.angusj.com/clipper2) by Angus Johnson, which handles polygon boolean operations, offsets and similar. [Eigen](https://libeigen.gitlab.io/) library is used for basic types and linear algebra calculations.

[wxWidgets](https://wxwidgets.org) library is used to handle GUI windows and events across platforms. Most of the UI is implemented using a custom OpenGL-based UI toolkit based on the [Yoga layout engine](https://github.com/facebook/yoga) and [Dear ImGui](https://github.com/ocornut/imgui), which makes it platform-independent.

The application uses many other libraries. You can see the [deps/](deps/) and [bundled_deps/](bundled_deps/) folders in the source tree to see the complete list. We are grateful to the authors and maintainers for open-sourcing their work.
