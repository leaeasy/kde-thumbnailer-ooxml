# KDE OOXML Thumbnailer

KDE OOXML Thumbnailer 是一个面向 Qt 6 和 KDE Frameworks 6 的 KIO 缩略图插件，用于在 Dolphin 等 KDE 应用中显示 Microsoft Office 文档、Markdown 文件和 HTML 文件预览。

## 功能

- 支持 DOC、DOCX、PPT、PPTX、XLS、XLSX、Markdown 和 HTML 文档。
- 支持标准 OOXML MIME 类型、旧版 Microsoft Office MIME 类型，以及 WPS Office 注册的对应 MIME 类型。
- 优先使用文档中已有的 PNG、JPEG 或 WMF 内嵌缩略图。
- 文档没有可用的内嵌缩略图时，执行轻量级内容预览。
- DOC 回退预览通过 `catdoc` 提取正文文本并生成轻量级文档卡片。
- PPT 回退预览通过 `catppt` 提取幻灯片文本并生成轻量级演示卡片。
- XLS 回退预览通过 `xls2csv` 提取首个工作表内容并生成轻量级表格卡片。
- Markdown 预览通过 `cmark` 生成结构化 XML，再渲染为轻量级文档卡片。
- HTML 预览会优先按 XHTML 结构提取标题、段落、列表、引用和代码块；无法作为 XML 解析时回退到宽松文本抽取。
- DOCX 回退预览会提取标题、段落和文本运行，尽可能保留字体、字号、粗体、斜体、颜色及段落对齐。
- XLSX 回退预览会提取首个工作表的部分单元格，并读取单元格字体样式。
- PPTX 回退预览会提取首张幻灯片的文本，并读取标题占位符、字体和段落样式。
- 使用内置的 `libkowmf` 渲染 WMF 缩略图，不需要外部 Office 程序。
- 为轻量级回退预览添加 DOC、DOCX、PPT、PPTX、XLS、XLSX、MD 或 HTML 格式标识。内嵌缩略图保持原样。
- 对 XML 大小、文本数量、样式数量和输出像素数设置限制，以降低异常文档造成资源消耗的风险。

轻量级回退渲染器不是完整的 Office 排版引擎。复杂分页、图表、公式、嵌入对象、宏、动画以及部分主题字体或版式可能不会显示，或与 Office/WPS 中的效果存在差异。旧版 DOC/PPT/XLS 通过外部文本提取工具生成预览，只保证内容可读性，不保证原始版式。Markdown 预览会提取标题、段落、列表、引用和代码块等有限结构，不渲染图片或完整网页样式。HTML 预览不会执行脚本、加载外部资源，也不会还原 CSS 布局，只提取有限文本结构。

## 依赖

- CMake 3.16 或更高版本
- C++17 编译器
- Qt 6.8 或更高版本
- KDE Frameworks 6
- ECM
- KArchive
- KCoreAddons
- KIO
- catdoc（提供 `catdoc`、`catppt` 和 `xls2csv`，用于旧版 DOC/PPT/XLS 预览）
- cmark（用于 Markdown 结构化预览）
- Ninja（推荐）

在 Arch Linux 上，可以安装以下软件包：

```bash
sudo pacman -S base-devel cmake extra-cmake-modules ninja qt6-base karchive kcoreaddons kio catdoc cmark
```

不同发行版的软件包名称可能不同。

## 构建

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## 安装

安装到系统路径：

```bash
sudo cmake --install build
```

插件通常会被安装到 Qt 6 插件目录下的：

```text
kf6/thumbcreator/ooxmlthumbnail.so
```

也可以先执行暂存安装，检查将要安装的文件：

```bash
DESTDIR="$PWD/stage" cmake --install build
```

安装后可以使用以下命令确认插件元数据：

```bash
qtplugininfo6 /usr/lib/qt6/plugins/kf6/thumbcreator/ooxmlthumbnail.so
```

实际路径取决于发行版和 `CMAKE_INSTALL_PREFIX`。

## Dolphin 配置

1. 重新启动 Dolphin。
2. 打开“配置 Dolphin”。
3. 进入“常规”中的“预览”页面。
4. 启用“Microsoft Office 文档”。
5. 确保当前目录视图已启用文件预览。

如果仍显示旧缩略图，可以关闭 Dolphin 并清理缩略图缓存：

```bash
rm -rf ~/.cache/thumbnails/normal \
       ~/.cache/thumbnails/large \
       ~/.cache/thumbnails/x-large \
       ~/.cache/thumbnails/xx-large
```

然后重新启动 Dolphin。系统中 `opendocumentthumbnail` 等其他插件可能声明相同的 MIME 类型；请在 Dolphin 的预览设置中只启用希望使用的插件。

## WPS Office MIME 类型

部分安装了 WPS Office 的系统会将 OOXML 文件识别为：

- `application/wps-office.docx`
- `application/wps-office.xlsx`
- `application/wps-office.pptx`

本项目同时注册这些 MIME 类型和标准 OOXML MIME 类型，无需额外配置。可以用以下命令查看本机识别结果：

```bash
xdg-mime query filetype example/1.docx
```

旧版 Microsoft Office 文档还会匹配以下 MIME：

- `application/msword`
- `application/vnd.ms-powerpoint`
- `application/vnd.ms-excel`
- `application/wps-office.doc`
- `application/wps-office.ppt`
- `application/wps-office.xls`

Markdown 文件会匹配以下 MIME：

- `text/markdown`
- `text/x-markdown`

HTML 文件会匹配以下 MIME：

- `text/html`
- `application/xhtml+xml`

## 调试

启用插件诊断日志：

```bash
QT_LOGGING_RULES="kde.thumbnailer.ooxml.debug=true" dolphin
```

同时检查 Qt 插件加载过程：

```bash
QT_DEBUG_PLUGINS=1 \
QT_LOGGING_RULES="kde.thumbnailer.ooxml.debug=true;kf.kio.gui.debug=true;kf.kio.workers.thumbnail.debug=true" \
dolphin
```

启用 `BUILD_TESTING` 后还会构建 `ooxmlpreviewjob`，它通过与 Dolphin 相同的 `KIO::PreviewJob` 路径测试缩略图：

```bash
build/bin/ooxmlpreviewjob example/1.docx /tmp/ooxml-preview.png
```

若要优先加载构建目录中的插件，可以设置 `QT_PLUGIN_PATH`：

```bash
QT_PLUGIN_PATH="$PWD/build/bin:/usr/lib/qt6/plugins" \
QT_LOGGING_RULES="kde.thumbnailer.ooxml.debug=true" \
build/bin/ooxmlpreviewjob example/1.docx /tmp/ooxml-preview.png
```

系统 Qt 插件目录可能不是 `/usr/lib/qt6/plugins`，请按发行版实际路径调整。

## 安全说明

DOC、PPT 和 XLS 回退预览会启动本机的 `catdoc`、`catppt` 或 `xls2csv` 提取有限文本内容；Markdown 预览会启动本机的 `cmark` 生成结构化 XML；HTML 预览只读取本地文件内容并做有限结构提取；DOCX、XLSX 和 PPTX 回退预览直接读取 OOXML ZIP 包中的有限 XML 内容。插件本身不执行宏，也不访问网络。文档仍应被视为不可信输入；如果发现解析异常、崩溃或资源消耗问题，请附带可公开的最小复现文件提交问题。

## 致谢

本项目基于并感谢 [reporter123/kde-thumbnailer-ooxml](https://github.com/reporter123/kde-thumbnailer-ooxml.git) 项目及其贡献者。原项目为 KDE 中的 OOXML 内嵌缩略图读取和 WMF 支持奠定了基础。

当前版本在此基础上完成了 Qt 6/KDE Frameworks 6 迁移，并增加了无内嵌缩略图时的轻量级内容渲染、样式提取、WPS Office MIME 类型支持和 KIO 调试工具。

## 许可证

项目源码中的许可证声明以各源文件为准。主要插件代码按照 GNU GPL 第 2 版或第 3 版授权，具体条款请参阅对应源文件头部声明。
