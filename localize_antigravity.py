#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
  Antigravity 界面汉化程序 v3 (Antigravity Chinese Localization Tool)
=============================================================================
功能特点：
- 深度汉化原生 Electron 菜单（顶部 File/Edit/View/Window/Help 及全部二级菜单项）
- 汉化系统托盘图标右键菜单
- 注入前端 MutationObserver 翻译引擎，覆盖设置面板、侧边栏、对话窗口、工件等全部核心 UI
- 自动保护：Antigravity 商标名、模型品牌及规格词（Gemini/Claude/GPT/Pro/Flash/Medium/Thinking）、AI 技术参数（temperature/top-p/tokens/context window等）
- 支持热注入：无需强制杀进程，安装后按 Ctrl+R 或重启应用即可生效
=============================================================================
"""

import os
import sys
import json
import struct
import shutil
import tempfile
import subprocess
import argparse

if sys.platform == 'win32':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    except Exception:
        pass


# ---------------------------------------------------------------------------
# ASAR 读写引擎
# ---------------------------------------------------------------------------

def extract_asar(asar_path, extract_dir):
    with open(asar_path, 'rb') as f:
        data = f.read(16)
        u1, header_size, header_object_size, header_string_size = struct.unpack('<IIII', data)
        header_bytes = f.read(header_string_size)
        header = json.loads(header_bytes.decode('utf-8'))
        base_offset = 8 + header_size

        def extract_node(node, current_dir):
            if 'files' in node:
                os.makedirs(current_dir, exist_ok=True)
                for name, child in node['files'].items():
                    child_path = os.path.join(current_dir, name)
                    if 'files' in child:
                        extract_node(child, child_path)
                    else:
                        if child.get('unpacked'):
                            continue
                        offset = int(child['offset'])
                        size = int(child['size'])
                        f.seek(base_offset + offset)
                        content = f.read(size)
                        os.makedirs(os.path.dirname(child_path), exist_ok=True)
                        with open(child_path, 'wb') as out_f:
                            out_f.write(content)

        extract_node(header, extract_dir)
        return header


def pack_asar(source_dir, output_asar, original_header=None):
    file_records = []

    def get_orig_node(path_parts, node):
        if not node or not path_parts:
            return None
        part = path_parts[0]
        if 'files' in node and part in node['files']:
            child = node['files'][part]
            if len(path_parts) == 1:
                return child
            return get_orig_node(path_parts[1:], child)
        return None

    def build_tree(current_dir, rel_path=""):
        files_dict = {}
        entries = sorted(os.listdir(current_dir)) if os.path.exists(current_dir) else []
        for entry in entries:
            full_path = os.path.join(current_dir, entry)
            sub_rel = f"{rel_path}/{entry}" if rel_path else entry
            if os.path.isdir(full_path):
                files_dict[entry] = build_tree(full_path, sub_rel)
            else:
                size = os.path.getsize(full_path)
                file_records.append({'rel_path': sub_rel, 'full_path': full_path, 'size': size})
                node = {'size': size, 'offset': None}
                if original_header:
                    orig_info = get_orig_node(sub_rel.split('/'), original_header)
                    if orig_info and orig_info.get('executable'):
                        node['executable'] = True
                files_dict[entry] = node

        if original_header:
            orig_parent = get_orig_node(rel_path.split('/'), original_header) if rel_path else original_header
            if orig_parent and 'files' in orig_parent:
                for k, v in orig_parent['files'].items():
                    if k not in files_dict:
                        if v.get('unpacked') or 'files' in v:
                            files_dict[k] = v

        return {'files': files_dict}

    header_tree = build_tree(source_dir)

    current_offset = 0
    for rec in file_records:
        rec['offset'] = current_offset
        current_offset += rec['size']

    def fill_offsets(node, rel_path=""):
        if 'files' in node:
            for name, child in node['files'].items():
                sub_rel = f"{rel_path}/{name}" if rel_path else name
                if 'files' in child:
                    fill_offsets(child, sub_rel)
                elif not child.get('unpacked'):
                    for rec in file_records:
                        if rec['rel_path'] == sub_rel:
                            child['offset'] = str(rec['offset'])
                            break

    fill_offsets(header_tree)

    header_str = json.dumps(header_tree, separators=(',', ':'))
    header_bytes = header_str.encode('utf-8')
    header_string_size = len(header_bytes)
    padding = (4 - (header_string_size % 4)) % 4
    header_object_size = header_string_size + padding + 4
    header_size = header_object_size + 4

    with open(output_asar, 'wb') as out_f:
        out_f.write(struct.pack('<IIII', 4, header_size, header_object_size, header_string_size))
        out_f.write(header_bytes)
        if padding:
            out_f.write(b'\0' * padding)
        for rec in file_records:
            with open(rec['full_path'], 'rb') as in_f:
                while chunk := in_f.read(65536):
                    out_f.write(chunk)


# ---------------------------------------------------------------------------
# 前端汉化引擎（注入 preload.js）
# ---------------------------------------------------------------------------

def generate_i18n_code():
    return r"""

// ===== Antigravity i18n Engine v4 =====
(function(){
if (window.__agy_i18n_v4) return;
window.__agy_i18n_v4 = true;

const PROTECTED_EXACT = new Set([
  "Antigravity", "antigravity",
  "Medium", "Flash", "Pro", "Flash-Lite", "Thinking",
  "Tokens", "Token", "tokens", "token",
  "Temperature", "Top-P", "Top-K", "Context Window",
  "API Key", "API key", "api_key",
  "JSON", "YAML", "HTTP", "HTTPS", "URL", "URI", "UUID",
  "Git", "Bash", "PowerShell", "Python", "Node.js", "WSL", "MCP", "SSH",
  "VCS", "IDE", "SDK", "CDN", "PDF", "CSV", "HTML", "CSS", "JS",
  "TypeScript", "JavaScript", "Claude", "Gemini", "GPT", "DeepSeek",
  "Llama", "Mistral", "Qwen", "Gemma", "o1", "o3", "Grok"
]);

function isProtected(t) {
  if (!t || t.length < 2) return true;
  if (PROTECTED_EXACT.has(t)) return true;
  if (/\b(Gemini|Claude|GPT|DeepSeek|Llama|Mistral|Qwen|Gemma|Sonnet|Opus|Haiku|o1|o3|Grok)\b/i.test(t)) return true;
  if (/^[0-9\s.,\/#!$%\^&\*;:{}=\-_`~()\[\]<>@|\\+]+$/.test(t)) return true;
  return false;
}

const T = {
  "A Gemini-powered security agent decides if commands should be auto-approved.": "由内置安全模型智能判断终端命令是否可以自动批准执行。",
  "A Google Cloud project is required to use Antigravity.": "使用 Antigravity 功能需要关联有效的 Google Cloud 项目。",
  "A chunk is a section of the page. Long pages are chunked so that only the sections that are relevant to your query are read.": "分块是页面的片段。长页面会自动切分为多个块，以便仅读取与您查询相关的段落。",
  "A copy of the workspace snapshot recorded at this step.": "当前步骤记录的工作区状态快照副本。",
  "A folder in this project could not be loaded because its location is invalid.": "因路径无效，此项目中的某个文件夹加载失败。",
  "A full copy of this workspace, uncommitted changes included.": "当前工作区的完整副本（包含未提交的本地修改）。",
  "A high-risk mode that disables all safety barriers. The agent operates with full system access, auto-executes all terminal commands, and reads or writes to all local files without review prompts.": "高风险模式，禁用所有安全防护屏障。智能体拥有完整系统权限，自动执行所有终端命令，并可在无需审查提示的情况下读写所有本地文件。",
  "A label for this computer when you connect from another device. Changing it reconnects.": "当从其他设备远程连接时此计算机显示的标识名称。更改后将重新连接。",
  "A license or project selection is required to use Antigravity.": "使用 Antigravity 需要先选择许可证或指定项目。",
  "A nickname for identifying this application in the companion website. Changing this will restart the connection.": "用于在远程管理端识别此设备的昵称。修改后将重启连接。",
  "A shell setup script run before every command the agent executes in this project. Overrides the global script.": "在智能体于该项目中执行每条命令前运行的 Shell 初始化脚本。将覆盖全局配置脚本。",
  "A shell setup script run before every command the agent executes.": "在智能体执行每条命令前运行的 Shell 初始化脚本。",
  "A snapshot was saved before running. To restore your previous output:": "运行前已保存状态快照。如需恢复先前的输出：",
  "AI Credits are used when you've exceeded your model quota. Keep track of your current AI Credits in the": "当超出模型免费额度时将消耗 AI 积分。您可以在以下位置跟踪积分余额：",
  "AI coding agents are known to have certain security limitations. Users should be aware of potential risks, including data exfiltration and possible code execution. Avoid processing highly sensitive data and verify all the actions taken by the agent.": "AI 编程代理存在潜在安全边界，包括代码自主执行与敏感信息交互。请避免处理高度敏感的私密数据，并仔细审查智能体执行的每一项操作。",
  "AI coding agents are known to have certain security risks, including autonomous code execution, data exfiltration, prompt injection and supply chain risks. Ensure that you monitor and verify all actions taken by the agent.": "AI 辅助编程代理存在潜在安全边界考量，包括自主代码执行、提示词注入与供应链风险。请密切监控并审查智能体采取的所有关键操作。",
  "Accent": "强调色",
  "Actions": "操作",
  "Active background color of the command center": "顶部命令中心处于活动状态时的背景颜色。",
  "Active border color of the command center": "顶部命令中心处于活动状态时的边框颜色。",
  "Active foreground color of the command center": "顶部命令中心处于活动状态时的前景色。",
  "Active foreground color of the item in the Activity bar when it is on top / bottom. The activity allows to switch between views of the side bar.": "活动栏置于顶部或底部时活动项的前景色。",
  "Active tab background color in an active group. Tabs are the containers for editors in the editor area. Multiple tabs can be opened in one editor group. There can be multiple editor groups.": "活动编辑器分组中当前标签页的背景颜色。标签页是承载文件的容器。",
  "Active tab background color in an unfocused group. Tabs are the containers for editors in the editor area. Multiple tabs can be opened in one editor group. There can be multiple editor groups.": "非聚焦编辑器分组中当前标签页的背景颜色。",
  "Active tab foreground color in an active group. Tabs are the containers for editors in the editor area. Multiple tabs can be opened in one editor group. There can be multiple editor groups.": "活动编辑器分组中当前标签页的前景文字颜色。",
  "Active tab foreground color in an unfocused group. Tabs are the containers for editors in the editor area. Multiple tabs can be opened in one editor group. There can be multiple editor groups.": "非聚焦编辑器分组中当前标签页的前景文字颜色。",
  "Activity": "活动",
  "Activity bar background color for the active item. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏当前处于活动状态项的背景颜色。活动栏位于界面最左或最右侧，用于在不同侧边栏视图之间切换。",
  "Activity bar background color. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏背景颜色。活动栏位于界面最左或最右侧，用于在不同侧边栏视图之间切换。",
  "Activity bar border color for the active item. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏中活动项的边框颜色。",
  "Activity bar border color separating to the side bar. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏与侧边栏之间分割线的边框颜色。",
  "Activity bar focus border color for the active item. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏处于聚焦状态项的边框颜色。",
  "Activity bar item foreground color when it is active. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏中活动项的前景图标颜色。",
  "Activity bar item foreground color when it is inactive. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏中非活动项的前景图标颜色。",
  "Activity notification badge background color. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏通知角标的背景颜色。",
  "Activity notification badge foreground color. The activity bar is showing on the far left or right and allows to switch between views of the side bar.": "活动栏通知角标的前景文字颜色。",
  "Agent always asks for review.": "智能体始终请求人工审查。",
  "Agent can retrieve console logs from the browser and return it for further processing.": "智能体可以从浏览器抓取控制台输出日志以进行故障诊断。",
  "Agent can scroll on browser pages to access more content.": "智能体可以在浏览器页面上滚动以查看更多内容。",
  "Agent cannot modify files outside of the workspace in strict mode.": "在严格模式下，智能体无法修改工作区外部的文件。",
  "Agent never asks for review. This maximizes the autonomy of the Agent, but also has the highest risk of the Agent operating over unsafe or injected Artifact content.": "智能体从不请求审查。这最大化了智能体的自主性，但也有智能体操作不安全或被注入的工件内容的高风险。",
  "Agent will always ask to review in strict mode.": "严格模式下智能体执行前始终请求人工审查。",
  "Agents have full access to your machine and external resources.": "智能体对您的计算机及外部网络资源拥有完全访问权限。",
  "Agents run in a secure sandbox that restricts access to external resources outside of your trusted folders.": "智能体在安全沙箱中运行，限制访问受信任目录之外的外部资源。",
  "All terminal commands require review. The agent can read or write to any file in the machine.": "所有终端命令均需审查。智能体可以读取或写入本机的任意文件。",
  "All tools (*)": "所有可用工具 (*)",
  "Allow Tab to view and edit the files in .gitignore. Use with caution if your .gitignore lists files containing credentials, secrets, or other sensitive information.": "允许 Tab 补全访问与编辑 .gitignore 列出的文件。如果您的 .gitignore 中包含密钥、凭证等敏感文件，请谨慎开启。",
  "Allow full browser script execution without prompting.": "允许浏览器脚本直接运行，无需弹窗确认。",
  "Allow remote debugging for this browser instance": "允许为当前浏览器实例启用远程调试端口",
  "Allow sandboxed commands to make network requests.": "允许沙箱内的命令发起网络请求。",
  "Allow the agent to run without restrictions.": "允许智能体无限制自主运行。",
  "Allows the agent to access files outside of your current workspace.": "允许智能体访问当前工作区外部的文件。",
  "Application": "应用程序",
  "Approval": "审批",
  "Are you sure you want to delete this conversation? This action cannot be undone.": "确定要删除此会话吗？此操作无法撤销。",
  "Are you sure you want to mark 1 conversation as read? This action cannot be undone.": "确定要将此对话标记为已读吗？",
  "Artifacts are created when the agent performs more complex, longer running tasks while in Planning mode.": "工件是在规划模式下智能体执行复杂的长周期任务时生成的成果文档。",
  "Artifacts are files the agent creates during a conversation to help perform longer running tasks and allow the user to provide high-level feedback. Click to open in editor.": "工件是智能体在交互过程中创建的结构化文件，用于承载复杂任务产出并支持用户直接批注反馈。点击可在编辑器中打开。",
  "Artifacts like Google Docs, PDFs, Office documents (Word, Excel, PowerPoint), etc. Everything else the agent creates is under Artifacts.": "包括 Google Docs、PDF、Office 办公文档（Word、Excel、PowerPoint）等文件。智能体生成的所有非代码成果都归类在工件面板中。",
  "Ask a quick question without interrupting the main conversation.": "无需打断主对话流程，快速提出临时问题。",
  "Ask for permission for sensitive operations.": "执行敏感操作前请求人工许可。",
  "Attach the trajectory ID to the feedback form": "在反馈表单中附带本次任务轨迹 ID 以便定位问题",
  "Attaching logs requires an email address": "附加调试日志需要提供联系邮箱地址",
  "Automatically decide whether the command should run.": "自动判定命令是否应当直接运行。",
  "Automatically expand the Changes Overview toolbar when the agent finishes generating a response.": "智能体完成响应生成时，自动展开“变更概览”工具栏。",
  "Automatically migrate legacy workflows to modern skills across global and workspace": "在全局和工作区中自动将旧版工作流迁移为现代技能",
  "Automatically migrate legacy workflows to modern skills across global and workspace configurations. Scans for existing workflows, creates target SKILL.md files, and safely archives old workflow files.": "在全局和工作区中自动将旧版工作流迁移为现代技能。扫描已有工作流、生成目标 SKILL.md 并安全归档旧文件。",
  "Automatically prompt you to restart the app when a new update is available. When disabled, you can check for updates manually from the app menu.": "当有可用更新时自动提示重启应用。禁用后，您可通过应用菜单手动检查更新。",
  "Autonomous mode active: the agent works independently and does not ask questions or request new permissions.": "自主模式已激活：智能体将独立完成任务，不会中途提问或请求新的权限。",
  "Background": "背景",
  "Background color of breadcrumb item picker.": "代码面包屑导航选择器的背景颜色。",
  "Background color of breadcrumb items.": "面包屑导航项的背景颜色。",
  "Background color of checkbox widget when the element it's in is selected.": "复选框所在元素被选中时的背景颜色。",
  "Background color of checkbox widget.": "复选框组件的背景颜色。",
  "Background color of editor widgets, such as find/replace.": "编辑器内浮动小组件（如查找/替换面板）的背景颜色。",
  "Background color of menu items.": "菜单项的背景颜色。",
  "Background color of sticky scroll in the editor": "编辑器顶部吸顶滚动区域 (Sticky Scroll) 的背景颜色。",
  "Background color of sticky scroll in the panel.": "底部面板中吸顶滚动区域的背景颜色。",
  "Background color of sticky scroll in the side bar.": "侧边栏中吸顶滚动区域的背景颜色。",
  "Background color of sticky scroll on hover in the editor": "鼠标悬停在编辑器吸顶滚动区域时的背景颜色。",
  "Background color of the command center": "顶部命令中心的背景颜色。",
  "Background color of the editor hover status bar.": "编辑器悬停提示框状态栏的背景颜色。",
  "Background color of the editor hover.": "编辑器鼠标悬停提示框的背景颜色。",
  "Background color of the editor overview ruler.": "编辑器右侧概览标尺的背景颜色。",
  "Background color of the ghost text in the editor.": "编辑器中灰色幽灵建议文本的背景颜色。",
  "Background color of the progress bar that can show for long running operations.": "长耗时操作进度条的背景颜色。",
  "Background color of the selected menu item in menus.": "菜单中当前选定项的背景颜色。",
  "Background color of the selected menu item in the menubar.": "顶部菜单栏中当前选定项的背景颜色。",
  "Background color of the type filter widget in lists and trees.": "树列表类型筛选小组件的背景颜色。",
  "Backoff": "退避延迟",
  "Badge background color. Badges are small information labels, e.g. for search results count.": "徽标背景颜色。徽标是显示搜索结果数等信息的小标签。",
  "Badge foreground color. Badges are small information labels, e.g. for search results count.": "徽标文字前景色。",
  "Banner background color. The banner is shown under the title bar of the window.": "通知横幅的背景颜色。横幅显示在窗口标题栏下方。",
  "Banner foreground color. The banner is shown under the title bar of the window.": "通知横幅的前景文字颜色。",
  "Banner icon color. The banner is shown under the title bar of the window.": "通知横幅的图标颜色。",
  "Best of N settings have moved to the Advanced section of General settings.": "Best-of-N 调优设置已移动至通用设置的高级选项中。",
  "Block all browser JavaScript execution.": "禁止所有网页 JavaScript 脚本执行。",
  "Border color between the two text editors.": "双栏编辑器对比时两侧之间的分割边框颜色。",
  "Border color of active radio option.": "单选框处于选中状态时的边框颜色。",
  "Border color of checkbox widget.": "复选框组件的边框颜色。",
  "Border color of editor widgets. The color is only used if the widget chooses to have a border and if the color is not overridden by a widget.": "编辑器组件的边框颜色。",
  "Border color of sticky scroll in the editor": "编辑器吸顶滚动区域的分割边框颜色。",
  "Border color of sticky scroll in the panel.": "底部面板吸顶滚动区域的边框颜色。",
  "Border color of sticky scroll in the side bar.": "侧边栏吸顶滚动区域的边框颜色。",
  "Border color of the command center": "顶部命令中心的边框颜色。",
  "Border color of the command center when the window is inactive": "窗口非激活时命令中心的边框颜色。",
  "Border color of the editor hover.": "编辑器悬停提示框的边框颜色。",
  "Border color of the selected menu item in menus.": "菜单中当前选中项的边框颜色。",
  "Border color of the selected menu item in the menubar.": "菜单栏中当前选中项的边框颜色。",
  "Border to separate pinned tabs from other tabs. Tabs are the containers for editors in the editor area. Multiple tabs can be opened in one editor group. There can be multiple editor groups.": "固定标签页与其他标签页之间的分割线颜色。",
  "Border to separate tabs from each other. Tabs are the containers for editors in the editor area. Multiple tabs can be opened in one editor group. There can be multiple editor groups.": "标签页之间的分割边框颜色。",
  "Browse and enable plugins from the Build With Google catalog.": "浏览并启用来自 Google 官方目录的扩展插件。",
  "Browser settings have moved to the Browser section of General settings.": "浏览器设置已移动至通用设置的浏览器板块中。",
  "Browser tools (browser_.*)": "浏览器工具 (browser_.*)",
  "Browser tools are disabled in the Antigravity Android desktop app.": "Android 桌面客户端中已禁用浏览器自动化工具。",
  "Browser tools are not available in the Antigravity Android desktop app.": "Android 桌面客户端不支持浏览器自动化工具。",
  "Bundles will let you download a whole group of user-defined plugins at once, instead of installing them one at a time. They are not here yet.": "插件捆绑包允许您一次性安装整组自定义插件，该特性即将上线。",
  "Change VCS in General settings, under Advanced": "前往通用设置 -> 高级选项中修改版本控制系统 (VCS)",
  "Check CLs that require my attention and fix them in their respective conversations.": "检查需要我处理的代码审查单 (CL)，并在各自的会话中完成修复。",
  "Check pending conversations in the last 3 days that require action from me.": "检查最近 3 天内需要我处理的待办对话。",
  "Choose how technical the interface should be.": "设置界面呈现的专业技术细节深度。",
  "Choose the product skin that fits how you work.": "选择契合您工作习惯的产品视觉风格与皮肤。",
  "Choose whether to be notified when the agent needs your attention or completes a task.": "选择是否在智能体需要您关注或完成任务时接收通知提醒。",
  "Chrome Binary Path": "Chrome 可执行文件路径",
  "Chrome on the machine you are controlling is not accepting remote debugging. Complete these steps on that machine, not in this tab:": "目标计算机上的 Chrome 未开启远程调试支持。请在该计算机上完成以下配置步骤：",
  "Chromium": "Chromium 浏览器",
  "Click to learn more about sources": "点击了解关于参考来源的更多信息",
  "Click to respond to the question.": "点击以答复此问题。",
  "Click to respond to the questions.": "点击以答复这些问题。",
  "Clone current workspace into a new independent workspace": "将当前工作区克隆为一个全新的独立工作区",
  "Close": "关闭",
  "Comprehensive guide and reference for the Antigravity Customization System. Use to explain how customizations work, their loading priority, discovery mechanisms, and to guide the creation of skills, rules, plugins, hooks, and MCP servers.": "Antigravity 自定义系统的完整指南与参考。用于解释自定义项工作机制、加载优先级、发现规则，并指导技能、规则、插件、钩子及 MCP 服务的创建。",
  "Comprehensive guide and reference for the Antigravity Customization System. Use to explain how customizations work, their loading priority, discovery mechanisms, and to guide the creation of...": "Antigravity 自定义系统的完整指南与参考。用于解释自定义项工作机制、加载优先级、发现规则及指导创建...",
  "Configure GitHub access policies.": "配置 GitHub 访问权限策略。",
  "Configure agent execution, queued message delivery, and permissions.": "配置智能体执行流程、排队消息发送与权限策略。",
  "Configure allowed and denied URLs for browser actuation.": "配置允许和拒绝进行浏览器自动化操作的 URL。",
  "Configure allowed and denied URLs for reading.": "配置允许和拒绝读取的网页 URL。",
  "Configure allowed and denied paths for file reads and writes.": "配置允许和拒绝的文件读取及写入路径。",
  "Configure allowed commands outside the sandbox.": "配置允许在沙箱外部运行的命令。",
  "Configure allowed terminal commands.": "配置允许直接执行的终端命令。",
  "Configure default behaviors, skills, and MCP servers.": "配置默认行为、技能以及 MCP 服务。",
  "Configure editor-specific behaviors and shortcuts.": "配置编辑器专属行为与快捷键。",
  "Configure external tools via Model Context Protocol.": "通过模型上下文协议 (MCP) 配置外部工具。",
  "Configure global allowed and denied resource permissions.": "配置全局允许与拒绝的资源权限。",
  "Configure hooks that run on agent lifecycle events.": "配置在智能体生命周期事件中触发的钩子。",
  "Configure tab completion, suggestions, and navigation behavior.": "配置 Tab 补全、代码建议与导航行为。",
  "Configure the agent's visual theme and display preferences.": "配置智能体的界面主题与显示偏好。",
  "Configure the default width of markdown artifacts.": "配置 Markdown 工件的默认显示宽度。",
  "Configure the maximum width of the conversation panel.": "配置对话面板的最大显示宽度。",
  "Configure when follow-up messages are sent.": "配置后续跟进消息的发送时机。",
  "Configure workspace-specific permissions, resources, and customizations.": "配置工作区专属权限、资源与自定义项。",
  "Configures how the agent tries to access files outside of its working folders.": "配置智能体访问工作目录外文件时的安全规则。",
  "Confirm the command is safe to run outside of the sandbox with full network and disk access.": "请确认此命令在具有完整网络和磁盘访问权限的沙箱外部运行是安全的。",
  "Confirming this undo action will make the following changes:": "确认此撤销操作将应用以下代码变更：",
  "Contrast": "对比度",
  "Controls the actions the agent can take.": "控制智能体能够执行的操作范围。",
  "Controls whether terminal commands require your approval before running.": "控制终端命令在执行前是否需要人工批准。",
  "Controls whether the agent can run custom JavaScript to automate complex browser actions.": "控制智能体是否可以执行自定义 JavaScript 代码以实现复杂的网页交互。",
  "Could not determine MCP config file path. Please check your home directory settings.": "无法定位 MCP 配置文件路径。请检查用户根目录配置。",
  "Create a new Cog (GoB) workspace from your initial prompt. Customize in General Settings, under Advanced.": "根据初始提示词创建新的 Cog (GoB) 工作区。可在通用设置 -> 高级中进行自定义。",
  "Create a new Fig workspace from your initial prompt. Customize in General Settings, under Advanced.": "根据初始提示词创建新的 Fig 工作区。可在通用设置 -> 高级中进行自定义。",
  "Create a new JJ workspace from your initial prompt. Customize in General Settings, under Advanced.": "根据初始提示词创建新的 JJ 工作区。可在通用设置 -> 高级中进行自定义。",
  "Create a new Piper (g4) workspace from your initial prompt. Customize in General Settings, under Advanced.": "根据初始提示词创建新的 Piper (g4) 工作区。可在通用设置 -> 高级中进行自定义。",
  "Customizations": "自定义",
  "Daily": "每日",
  "Default Cog Repository": "默认 Cog 代码仓库",
  "Default Workspace VCS": "默认工作区版本控制系统",
  "Developer-only tools. These settings are stored locally in this browser and do not affect other users.": "仅供开发者使用的调试工具。这些设置仅保存在本地客户端，不会影响其他用户。",
  "Disables all safety barriers for maximal iteration velocity.": "禁用所有安全屏障以实现极致迭代速度。",
  "Display and preserve intermediate thinking steps.": "显示并保留模型的中间思考推理步骤。",
  "Enforce settings that prevent the agent from autonomously running targeted exploits and require human review for all agent actions. Visit antigravity.google/docs/strict-mode for details.": "强制开启安全设置，阻止智能体自主运行高危命令，并要求对所有操作进行人工审查。详情请参阅官方严格模式文档。",
  "Enterprise": "企业版网关",
  "Europe": "欧洲地区",
  "Every terminal command requires approval.": "每个终端命令的执行均须人工批准。",
  "Execution": "执行",
  "Find plugins - bundles of skills, agents, MCP servers, UI extensions and more, and install them in one click.": "发现扩展插件——集合了技能、智能体、MCP 服务与 UI 扩展等多种能力，支持一键安装。",
  "Foreground": "前景",
  "Generations": "生成记录",
  "Git-on-Borg repository used when creating a new Cog workspace from your initial prompt (e.g., chromium/src or android/platform/superproject).": "根据初始提示词创建 Cog 工作区时使用的 Git-on-Borg 仓库路径（例如 chromium/src 或 android/platform/superproject）。",
  "Give the agent awareness of lint errors created by its edits so it can fix them without explicit prompting.": "让智能体能够感知代码修改带来的检查错误并自主修复。",
  "Global": "全局",
  "Highlight After Accept": "接受修改后高亮显示",
  "Host Environment": "宿主运行环境",
  "Hourly": "每小时",
  "How to render rich interactive HTML widgets inline in the chat or as standalone artifacts. Use this skill when you want to show the user diagrams, data visualizations, interactive controls, educational walkthroughs, or any rich visual content beyond plain text and markdown.": "如何在对话中内联渲染或作为独立工件展示丰富的交互式 HTML 小部件。用于展示流程图、数据可视化、交互控件及教程等。",
  "How to render rich interactive HTML widgets inline in the chat or as standalone artifacts. Use this skill when you want to show the user diagrams, data visualizations, interactive controls,...": "如何在对话中内联渲染或作为独立工件展示丰富的交互式 HTML 小部件。用于展示流程图、数据可视化及交互控件...",
  "Idle": "空闲",
  "Install this plugin to see the commands it adds, along with its skills, agents and MCP servers.": "安装此插件即可查看其新增的终端命令、配套技能、智能体及 MCP 服务。",
  "Invocation": "调用记录",
  "Keyboard Shortcuts": "快捷键",
  "Keyboard shortcuts": "快捷键",
  "Keyboard shortcuts for quick navigation and control.": "用于快速导航与全局控制的快捷键列表。",
  "Knowledge": "知识库",
  "Labs settings have moved to the Advanced section of General settings.": "实验室特性设置已移动至通用设置的高级板块中。",
  "Learn more": "了解更多",
  "Learn more about": "了解更多关于",
  "Let the agent access its knowledge base to inform its responses and automatically generate knowledge items in the background. Turning this off prevents the agent from accessing existing knowledge items, but doesn't delete them.": "允许智能体访问知识库以辅助回复，并在后台自动沉淀提炼知识。关闭此项会阻止智能体访问已有知识库，但不会删除它们。",
  "Let the agent access past conversations to inform its responses.": "允许智能体读取过去的会话历史以辅助生成更连贯的回复。",
  "Let the agent use browser tools to open URLs, read web pages, and interact with browser content. This gives the agent access to important (and often critical) knowledge and methods of validation, but any browser integration does increase exposure to external malicious parties for security exploits.": "允许智能体使用浏览器工具打开网页、阅读页面及执行自动化操作。这赋予了智能体获取实时参考和运行验证的能力，但任何浏览器交互都会潜在增加接触外部恶意页面的风险。",
  "Linux": "Linux 操作系统",
  "MacOS": "macOS 操作系统",
  "Manage your model quota and credits.": "管理您的模型配额与 AI 积分。",
  "Manage your notification preferences.": "管理您的通知提醒偏好设置。",
  "Manage your plan, credentials, and general preferences.": "管理您的套餐、凭据以及常规偏好设置。",
  "Marketplace": "扩展插件市场",
  "Marketplace Gallery URL": "插件市场仓库 URL",
  "Marketplace Item URL": "插件详情页 URL",
  "Mention": "提及",
  "Mentions": "提及与引用",
  "Modify permissions for file, terminal, and MCP tools.": "修改文件、终端与 MCP 工具的访问权限。",
  "Nickname": "设备昵称",
  "No model specified for Battle Mode, falling back to use current selected model": "未指定模型对抗评估的模型，自动回退使用当前已选模型",
  "Open": "打开",
  "Open Agent panel on window reload": "窗口重新加载时自动展开智能体面板",
  "Open files in the background if Agent creates or edits them": "智能体创建或修改文件时在后台静默打开该文件",
  "Open files in the background if the agent creates or edits them": "智能体创建或修改文件时在后台静默打开该文件",
  "Open the agent panel on window reload": "窗口重新加载时自动展开智能体面板",
  "Optional branch or ref used when creating a new Cog workspace (defaults to main if unset).": "创建新 Cog 工作区时使用的可选分支或 Git 引用（留空则默认指向 main）。",
  "Plan Review Policy": "实施计划审查策略",
  "Play a sound when the agent finishes generating a response.": "智能体完成响应生成时播放提示音。",
  "Preset": "预设方案",
  "Prevent the computer from sleeping while the app is running.": "应用运行期间阻止计算机自动进入休眠状态。",
  "Prompt for approval before running browser scripts.": "运行浏览器脚本前始终弹出提示请求批准。",
  "Provide Feedback": "提供反馈",
  "Provides a comprehensive guide, quick reference, and sitemap for Google Antigravity (AGY), including the Antigravity CLI (agy), Antigravity 2.0, Antigravity IDE, Python SDK, slash commands, keybindings, and customizations (skills, rules, MCP, sidecars). Activate this skill when the user asks questions about how to use, configure, or customize Antigravity, AGY, the agy CLI, the Antigravity IDE, or Antigravity 2.0.": "提供 Google Antigravity (AGY) 的全面指南、速查手册与站点地图，涵盖 Antigravity CLI、IDE、Python SDK、斜杠命令与自定义能力等。",
  "Provides a comprehensive guide, quick reference, and sitemap for Google Antigravity (AGY), including the Antigravity CLI (agy), Antigravity 2.0, Antigravity IDE, Python SDK, slash commands,...": "提供 Google Antigravity (AGY) 的全面指南、速查手册与站点地图，涵盖 Antigravity CLI、IDE、Python SDK 与斜杠命令等...",
  "Queue": "排队",
  "Queue Wait": "等待排队响应",
  "Quickly add and update imports with a tab keypress.": "按 Tab 键快速自动添加与更新导入语句。",
  "Replace the default browser right-click menu with quick actions.": "使用快捷操作菜单替换系统默认的右键菜单。",
  "Requires manual review for all terminal commands and file accesses outside of the working folders.": "对所有终端命令以及工作目录之外的文件访问都需要人工审查。",
  "Restricts agent tools to a secure, isolated local sandbox.": "将智能体工具限制在安全隔离的本地沙箱中运行。",
  "Review Policy": "代码审查策略",
  "Run terminal commands with sandbox restrictions.": "在沙箱安全限制下运行终端命令。",
  "Same Project": "同一项目中",
  "Saves to the .agent/agents/ directory in your current workspace. This script can be version controlled and shared with your team.": "保存至当前工作区的 .agent/agents/ 目录。该脚本可随项目进行版本管理并与团队共享。",
  "Scheduled Task": "计划任务",
  "Scheduled Tasks": "计划任务",
  "Scheduled tasks": "计划任务",
  "Screen Recording": "屏幕录制",
  "Screenshot": "屏幕截图",
  "Select a workspace to save agent scripts to your workspace.": "选择一个工作区以将智能体脚本保存至该工作区中。",
  "Select the workspace type that will be used for new conversations started with the New Workspace option.": "选择使用“新建工作区”选项开启对话时的默认工作区类型。",
  "Set the speed of tab suggestions": "调节 Tab 智能补全建议的提示灵敏度与延迟",
  "Settings": "设置",
  "Shortcuts": "快捷键",
  "Show 'Edit' and 'Chat' buttons when selecting text in the editor.": "在编辑器中选中文本时显示“编辑”与“对话”浮动快捷按钮。",
  "Show a floating notification card when background conversations need your input. Answer questions, approve commands, and grant permissions without leaving your current conversation. Share feedback at go/inline-actions-feedback.": "当后台会话需要您的输入时显示浮动通知卡片。无需切换当前界面即可直接答复问题、批准命令或授权。",
  "Show browser notifications when your action is needed or execution finishes.": "当需要您处理或执行结束时显示桌面通知提醒。",
  "Show suggestions when typing in the editor": "在编辑器中键入时显示智能建议与自动补全",
  "Show suggestions when typing in the editor.": "在编辑器中键入时显示智能补全建议。",
  "Specifies the agent's behavior when asking for review on artifacts, which are documents it creates to enable a richer conversation experience.": "指定智能体在工件审查时的行为。工件是智能体创建的文档，用于提供更丰富的交互体验。",
  "Step Timing": "步骤耗时统计",
  "Step Type": "步骤类型",
  "Tab Gitignore Access": "Tab 访问 .gitignore 文件",
  "Team of subagents to do long running work": "由多个子智能体协作团队执行长周期后台任务",
  "Terminal command automatically proceeds if the command runs inside the sandbox. Otherwise, it requests review.": "终端命令若在沙箱内运行则自动执行，否则将请求人工审查。",
  "Terminal commands automatically proceed inside the sandbox. A command that needs to leave the sandbox proceeds only if an agent judges it reversible, and otherwise requests review.": "沙箱内的终端命令自动执行。需要脱离沙箱的命令仅在智能体判定其可逆时才继续，否则请求人工审查。",
  "The agent always asks for confirmation before executing terminal commands (except those in the Allow list).": "智能体在执行终端命令前始终请求确认（白名单内的命令除外）。",
  "The agent always asks for review.": "智能体始终请求人工审查。",
  "The agent asks for permission before executing commands matched by a deny list entry.": "智能体在执行匹配黑名单条目的命令前会请求许可。",
  "The agent auto-executes commands matched by an allow list entry.": "智能体自动执行匹配白名单条目的命令。",
  "The agent never asks for review. This maximizes the autonomy of the agent, but also has the highest risk of the agent operating over unsafe or injected artifact content.": "智能体从不请求审查。这最大化了智能体的自主性，但也有智能体操作不安全或被注入的工件内容的高风险。",
  "The breakdown below shows token usage from customizations like skills, rules, and MCP. If the budget is exceeded, large customizations will be truncated automatically.": "下方明细展示了来自技能、规则及 MCP 等自定义项的 Token 用量。若超出限额，较长的自定义项将被自动截断。",
  "There are no customizations enabled.": "当前未启用任何自定义项。",
  "These servers need you to sign in before the agent can use their tools.": "智能体调用这些服务工具前，需要您先完成登录授权。",
  "This automation requires the following arguments to run. These values determine the permissions granted to its agents. In the next step, you'll be able to review these permissions in more detail.": "该自动化流程需要配置以下启动参数。这些参数决定了授予智能体的权限等级。在下一步中，您可以详细审查各项具体权限。",
  "This extension has no declared permissions requiring user input.": "该扩展插件没有声明需要用户手动配置的输入权限。",
  "This option is disabled by your organization.": "此选项已被您的组织策略禁用。",
  "This option is disabled while strict mode is enabled.": "严格模式开启时此选项已禁用。",
  "This plugin is disabled, so nothing shown below is active. Enable it above to use it.": "此插件当前已停用，以下所有组件均未激活。请在上方开启后使用。",
  "This prompt is straightforward and does not need alternative comparisons.": "该提示词意图明确单一，无需启动多模型对比方案。",
  "This server needs you to sign in before the agent can use its tools.": "智能体调用该服务工具前，需要您先完成登录授权。",
  "This setting cannot be changed while strict mode is enabled.": "严格模式开启时无法修改此设置。",
  "This setting is managed by your organization.": "此设置由您的组织管理员统一纳管。",
  "Time to First Token (TTFT)": "首 Token 生成时间 (TTFT)",
  "Toggle Auxiliary Pane": "切换辅助侧栏面板",
  "Toggle Environment Selector": "切换环境选择器",
  "Toggle File Viewer": "切换文件查看器",
  "Toggle Project Selector": "切换项目选择器",
  "Token 用量": "Token 用量",
  "Type": "键入",
  "UI Extensions": "UI 界面扩展",
  "Uncommitted": "未提交变更",
  "United States": "美国地区",
  "Uptime": "运行时间",
  "Use the IDE's shell integration to detect and report terminal command execution. When disabled, the agent uses its own shell. Restart the application for this to take effect.": "使用 IDE 的 Shell 集成来检测并报告终端命令的执行。禁用后，智能体将使用其内置 Shell。重启应用后生效。",
  "Useful for tasks that require file access across your full machine. The agent has full read and write access to all local files, but all proposed terminal commands require manual review and approval before running.": "适用于需要跨整台计算机访问文件的任务。智能体对所有本地文件拥有完全读写权限，但所有拟执行的终端命令在运行前均需人工审查和批准。",
  "Useful for typical development with an emphasis on security. It prioritizes safety over speed by requiring manual approval for all terminal commands and files outside the project directory.": "适用于注重安全性的常规开发场景。通过要求对所有终端命令和项目目录外的文件访问进行人工批准，将安全性置于速度之上。",
  "VCS used when creating a new workspace, including workspaces generated based off your initial prompt.": "创建新工作区时使用的版本控制系统（包括由初始提示词自动构建的工作区）。",
  "Vetted (Preview)": "官方审核通过 (预览版)",
  "View Debug": "查看调试信息",
  "Weekly": "每周",
  "When enabled, 'Explain and Fix' actions will continue in the current conversation instead of starting a new one.": "启用后，“解释并修复”操作将在当前会话中继续进行，而不是新开对话。",
  "When enabled, Agent is given awareness of lint errors created by its edits and may fix them without explicit user prompting.": "启用后，智能体将能够感知其修改所引发的代码检查 (Lint) 错误，并在无需用户提示的情况下自主修复。",
  "When enabled, Agent will use IDE's shell integration to detect and report terminal command execution.": "启用后，智能体将使用 IDE 的 Shell 集成检测并报告终端命令执行状态。",
  "When enabled, Antigravity will play a sound when Agent finishes generating a response.": "启用后，智能体完成响应生成时将播放提示音。",
  "When enabled, sandboxed commands are allowed to make network requests.": "启用后，沙箱内的命令将被允许发起外部网络请求。",
  "When enabled, the Changes Overview toolbar will automatically expand when Agent finishes generating a response.": "启用后，智能体生成响应完成时将自动展开代码变更概览工具栏。",
  "When enabled, the agent will be able to access its knowledge base to inform its responses and automatically generate knowledge items in the background.": "启用后，智能体将能够访问其知识库，并在后台自动生成和沉淀知识条目。",
  "When enabled, the agent will be able to access past conversations to inform its responses.": "启用后，智能体将可以参考以往的对话历史以辅助生成回复。",
  "When enabled, the agent will include default customizations, including default skills.": "启用后，智能体将自动引入官方预置的默认技能与自定义能力。",
  "When enabled, the values below replace the auto-detected OS, host, and user type used for feature gating.": "启用后，以下设置将覆盖自动检测的操作系统、主机环境与用户角色，用于特性功能开关控制。",
  "Whether the agent asks you to review its documents.": "控制智能体在生成文档时是否需要您进行人工审查。",
  "Windows": "Windows 操作系统",
  "Work with local agents from another device.": "从其他设备远程协同控制本地智能体。",
  "Workspace CL Status": "工作区变更单状态",
  "Workspace URI query parameter": "工作区 URI 查询参数",
  "Workspace settings have moved to the Workspace section of General settings.": "工作区设置已整合至通用设置的工作区选项中。",
  "Worktree": "Git 工作树 (Worktree)",
  "You are accessing Jetski terminal on a non-corp domain, which may trigger a Chrome Password Alert. To use the terminal, visit the Jetski Corp domain instead.": "您当前在非企业信任域名下访问终端环境，这可能会触发浏览器的安全密码警报。",
  "Your administrator has disabled MCP servers for this workspace.": "您的系统管理员已禁用该工作区的 MCP 服务支持。",
  "Your environment has expired. Please renew it to continue.": "您的运行环境已过期，请续期以继续使用。",
  "Your environment is active, but you can renew it if needed.": "您的运行环境当前处于活跃状态，如有需要可随时续期。",
  "and select": "并选择",
  "learn more": "了解更多",
  "run_command or view_file": "执行命令或查看文件",
  "to have the agent generate a plan": "即可让智能体生成实施计划",
  "to have the agent generate a plan.": "即可让智能体生成实施计划。",
  "全局权限": "全局权限",
  "始终询问": "始终询问",
  "安全预设": "安全预设",
  "实施计划审查策略": "实施计划审查策略",
  "工具调用权限": "工具调用权限",
  "技能": "技能",
  "排队中的消息": "排队中的消息",
  "智能体行为": "智能体行为",
  "极速模式": "极速模式",
  "立即发送": "立即发送",
  "通用": "通用",
};

const ATTR_MAP = {
  "Type a message...": "输入消息...",
  "Add a message...": "输入消息...",
  "Ask anything...": "提出任何问题...",
  "Search files...": "搜索文件...",
  "Search across files...": "跨文件搜索...",
  "Search...": "搜索...",
  "Filter...": "筛选...",
  "Select a model": "选择模型",
  "Type a command or ask a question": "输入命令或提问",
  "Name workspace...": "工作区名称...",
  "Enter project name...": "输入项目名称...",
  "Search conversations (by name or Cascade ID)": "搜索对话（按名称或 Cascade ID）",
  "Search conversations...": "搜索对话...",
  "Search workspaces...": "搜索工作区...",
  "Search projects...": "搜索项目...",
  "Enter workspace name (e.g., my-feature)": "输入工作区名称（如 my-feature）",
  "Write a comment...": "写一条批注...",
  "Enter your feedback here...": "在此输入您的反馈...",
  "Close": "关闭",
  "Settings": "设置",
  "Copy": "复制",
  "Delete": "删除",
  "Edit": "编辑",
  "New Conversation": "新建对话",
  "New Workspace": "新建工作区",
  "Cancel": "取消",
  "Save": "保存",
  "Send": "发送",
  "Add": "添加"
};

const IGNORE_TAGS = new Set(["SCRIPT", "STYLE", "TEXTAREA", "INPUT", "PRE", "CODE", "KBD", "MATH"]);

function shouldIgnore(el) {
  if (!el || el.nodeType !== 1) return false;
  if (IGNORE_TAGS.has(el.tagName)) return true;
  if (el.isContentEditable) return true;
  const cls = el.className;
  if (typeof cls === "string" && (
    cls.includes("monaco-") || cls.includes("code-") || cls.includes("cm-") ||
    cls.includes("prism") || cls.includes("token") || cls.includes("xterm") ||
    cls.includes("terminal")
  )) return true;
  return false;
}

const REGEX_RULES = [
  [/^(\d+)\s+agents?\s+running$/i, "$1 个智能体运行中"],
  [/^(\d+)\s+files?\s+modified$/i, "已修改 $1 个文件"],
  [/^(\d+)\s+conversations?$/i, "$1 条对话"],
  [/^(\d+)\s+tasks?$/i, "$1 个任务"],
  [/^Thinking for\s+(.+)$/i, "正在思考 ($1)"],
  [/^Worked for\s+(.+)$/i, "已工作 $1"],
  [/^Failed to\s+(.+)$/i, "$1 失败"],
  [/^Learn more about\s+(.+)$/i, "了解更多关于 $1"],
  [/^Type\s+(.+)\s+and select\s+(.+)\s+to have the agent generate a plan\.?$/i, "键入 $1 并选择 $2 即可让智能体生成实施计划。"]
];

function translateString(s) {
  if (!s) return null;
  const trimmed = s.trim();
  if (!trimmed || isProtected(trimmed)) return null;

  const normalized = trimmed.replace(/\s+/g, ' ');

  if (T[trimmed]) return T[trimmed];
  if (T[normalized]) return T[normalized];

  for (const [re, repl] of REGEX_RULES) {
    if (re.test(normalized)) {
      return normalized.replace(re, repl);
    }
  }
  return null;
}

function translateNode(node) {
  if (!node) return;

  if (node.nodeType === 3) {
    const p = node.parentElement;
    if (!p || shouldIgnore(p)) return;
    const raw = node.nodeValue;
    if (!raw || !raw.trim()) return;

    const zh = translateString(raw);
    if (zh) {
      const m = raw.match(/^(\s*)(.*?)(\s*)$/);
      node.nodeValue = m ? (m[1] + zh + m[3]) : zh;
    }
  } else if (node.nodeType === 1) {
    if (shouldIgnore(node)) return;

    for (const attr of ["placeholder", "title", "aria-label"]) {
      const v = node.getAttribute(attr);
      if (v) {
        const trimmed = v.trim();
        if (!isProtected(trimmed)) {
          const zh = ATTR_MAP[trimmed] || T[trimmed];
          if (zh) node.setAttribute(attr, zh);
        }
      }
    }

    let child = node.firstChild;
    while (child) {
      translateNode(child);
      child = child.nextSibling;
    }
  }
}

function start() {
  if (!document.body) {
    setTimeout(start, 50);
    return;
  }
  translateNode(document.body);
  const observer = new MutationObserver(mutations => {
    for (const m of mutations) {
      if (m.type === "childList") {
        for (const n of m.addedNodes) translateNode(n);
      } else if (m.type === "characterData") {
        translateNode(m.target);
      } else if (m.type === "attributes") {
        translateNode(m.target);
      }
    }
  });
  observer.observe(document.body, {
    childList: true,
    subtree: true,
    characterData: true,
    attributes: true,
    attributeFilter: ["placeholder", "title", "aria-label"]
  });

  // 定时辅助检查（确保动态切换标签或弹窗时即时汉化）
  setInterval(() => {
    translateNode(document.body);
  }, 1000);
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", start);
} else {
  start();
}
})();

"""


# ---------------------------------------------------------------------------
# 原生代码补丁（menu.js, tray.js, main.js, preload.js）
# ---------------------------------------------------------------------------

def patch_native_files(dist_dir):
    # 1. 深度汉化 menu.js（原生菜单栏）
    menu_file = os.path.join(dist_dir, 'menu.js')
    if os.path.exists(menu_file):
        with open(menu_file, 'r', encoding='utf-8') as f:
            content = f.read()

        # 注入菜单汉化翻译器
        translator_code = r"""
const MENU_ZH_MAP = {
    'File': '文件(&F)',
    'Edit': '编辑(&E)',
    'View': '视图(&V)',
    'Window': '窗口(&W)',
    'Help': '帮助(&H)',
    'New Window': '新建窗口',
    'Connect to WSL': '连接到 WSL',
    'Reopen Locally': '本地重新打开',
    'Docs': '官方文档',
    'Undo': '撤销',
    'Redo': '重做',
    'Cut': '剪切',
    'Copy': '复制',
    'Paste': '粘贴',
    'Paste and Match Style': '粘贴并匹配样式',
    'Delete': '删除',
    'Select All': '全选',
    'Reload': '重新加载',
    'Force Reload': '强制重新加载',
    'Toggle Developer Tools': '切换开发者工具',
    'Actual Size': '实际大小',
    'Reset Zoom': '实际大小',
    'Zoom In': '放大',
    'Zoom Out': '缩小',
    'Toggle Full Screen': '全屏',
    'Minimize': '最小化',
    'Close': '关闭',
    'Quit': '退出',
    'Services': '服务',
    'Hide': '隐藏',
    'Hide Others': '隐藏其他',
    'Show All': '显示全部'
};
function translateApplicationMenu(menuInstance) {
    if (!menuInstance || !menuInstance.items) return;
    menuInstance.items.forEach((item) => {
        if (item.label && MENU_ZH_MAP[item.label]) {
            item.label = MENU_ZH_MAP[item.label];
        }
        if (item.submenu) {
            translateApplicationMenu(item.submenu);
        }
    });
}
"""
        # 兼容查找以支持已翻译或未翻译的 submenuLabel
        content = content.replace(
            "const submenuItem = appMenu.items.find((item) => item.label === submenuLabel);",
            "const submenuItem = appMenu.items.find((item) => item.label === submenuLabel || item.label.startsWith(submenuLabel) || (submenuLabel === 'File' && item.label.startsWith('文件')) || (submenuLabel === 'Help' && item.label.startsWith('帮助')));"
        )

        # 在 setupApplicationMenu 和 addWslConnectMenu 的 setApplicationMenu 之前注入 translateApplicationMenu
        if 'translateApplicationMenu(menu);' not in content:
            content += translator_code
            content = content.replace(
                "electron_1.Menu.setApplicationMenu(menu);",
                "translateApplicationMenu(menu);\n    electron_1.Menu.setApplicationMenu(menu);"
            )

        with open(menu_file, 'w', encoding='utf-8') as f:
            f.write(content)
        print("    [+] 已深度汉化 menu.js（顶部 File/Edit/View/Window/Help 及全部二级菜单）")

    # 2. 系统托盘 tray.js
    tray_file = os.path.join(dist_dir, 'tray.js')
    if os.path.exists(tray_file):
        with open(tray_file, 'r', encoding='utf-8') as f:
            content = f.read()
        content = content.replace("'No agents running'", "'暂无运行中的智能体'")
        content = content.replace("`Open ${electron_1.app.getName()}`", "`打开 ${electron_1.app.getName()}`")
        content = content.replace("'Quit'", "'退出'")
        with open(tray_file, 'w', encoding='utf-8') as f:
            f.write(content)
        print("    [+] 已汉化 tray.js（系统托盘右键菜单）")

    # 3. 主进程 main.js
    main_file = os.path.join(dist_dir, 'main.js')
    if os.path.exists(main_file):
        with open(main_file, 'r', encoding='utf-8') as f:
            content = f.read()
        patches = [
            ("label: 'New Window'", "label: '新建窗口'"),
            ("label: 'No agents running'", "label: '暂无运行中的智能体'"),
            ("label: 'Quit'", "label: '退出'"),
            ("'WSL distro not found'", "'WSL 发行版未找到'"),
            ("'WSL setup failed'", "'WSL 设置失败'"),
        ]
        for src, dst in patches:
            content = content.replace(src, dst)
        with open(main_file, 'w', encoding='utf-8') as f:
            f.write(content)
        print("    [+] 已汉化 main.js（Dock 菜单与 WSL 对话框）")

    # 4. 前端预加载 preload.js
    preload_file = os.path.join(dist_dir, 'preload.js')
    if os.path.exists(preload_file):
        with open(preload_file, 'r', encoding='utf-8') as f:
            content = f.read()
        # 清除旧版
        if '__agy_i18n' in content:
            idx = content.find('// ===== Antigravity')
            if idx != -1:
                content = content[:idx]
            else:
                idx2 = content.find('(function(){')
                if idx2 != -1:
                    content = content[:idx2]
        # 写入最新 v3 汉化引擎
        content = content.rstrip() + "\n\n" + generate_i18n_code()
        with open(preload_file, 'w', encoding='utf-8') as f:
            f.write(content)
        print("    [+] 已注入前端汉化引擎 v3 到 preload.js（全面覆盖设置、对话、工件等）")


# ---------------------------------------------------------------------------
# 路径探查
# ---------------------------------------------------------------------------

def find_antigravity_path():
    try:
        cmd = 'powershell -NoProfile -Command "Get-Process Antigravity -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Path"'
        out = subprocess.check_output(cmd, shell=True, text=True, errors='ignore').strip()
        if out and os.path.isfile(out):
            app_dir = os.path.dirname(out)
            if os.path.isdir(os.path.join(app_dir, 'resources')):
                return app_dir
    except Exception:
        pass

    local_app_data = os.environ.get('LOCALAPPDATA', '')
    if local_app_data:
        d = os.path.join(local_app_data, 'Programs', 'antigravity')
        if os.path.isdir(os.path.join(d, 'resources')):
            return d
    for prefix in [os.environ.get('ProgramFiles', ''), os.environ.get('ProgramFiles(x86)', '')]:
        if prefix:
            d = os.path.join(prefix, 'Antigravity')
            if os.path.isdir(os.path.join(d, 'resources')):
                return d
    return None


# ---------------------------------------------------------------------------
# 安装与还原
# ---------------------------------------------------------------------------

def install_localization(app_dir):
    resources_dir = os.path.join(app_dir, 'resources')
    asar_path = os.path.join(resources_dir, 'app.asar')
    backup_path = os.path.join(resources_dir, 'app.asar.backup')

    if not os.path.exists(asar_path):
        print(f"[!] 错误：未找到资源文件：{asar_path}")
        return False

    if not os.path.exists(backup_path):
        print(f"[*] 正在创建原版备份：{backup_path}")
        shutil.copy2(asar_path, backup_path)
        print("[+] 备份创建成功！")
    else:
        print(f"[*] 已有官方原版备份文件，保留原始备份。")

    temp_dir = tempfile.mkdtemp(prefix='agy_i18n_')
    extracted_dir = os.path.join(temp_dir, 'extracted')
    repacked_asar = os.path.join(temp_dir, 'app.asar')

    try:
        print("[*] 正在解包 app.asar ...")
        orig_header = extract_asar(asar_path, extracted_dir)
        print(f"    [+] 解包完成")

        print("[*] 正在应用原生菜单与前端汉化补丁...")
        dist_dir = os.path.join(extracted_dir, 'dist')
        patch_native_files(dist_dir)

        print("[*] 正在重新封装 app.asar ...")
        pack_asar(extracted_dir, repacked_asar, original_header=orig_header)
        new_size = os.path.getsize(repacked_asar)
        print(f"    [+] 封装完成（{new_size // 1024} KB）")

        if new_size < 100000:
            raise RuntimeError(f"打包后文件大小异常: {new_size}")

        print("[*] 正在替换应用资源文件...")
        shutil.copy2(repacked_asar, asar_path)

        print()
        print("=" * 68)
        print("  【汉化安装成功！】Antigravity 界面已全面完成中文汉化！")
        print()
        print("  汉化生效范围：")
        print("    ✓ 原生主菜单栏：File(文件), Edit(编辑), View(视图), Window(窗口), Help(帮助)")
        print("    ✓ 二级菜单项：撤销, 重做, 剪切, 复制, 粘贴, 全选, 放大, 缩小, 全屏, 检查更新等")
        print("    ✓ 核心页面：设置面板(Settings)、会话管理、侧边栏、工具工件、终端等")
        print("    ✓ 保护规则：Antigravity 商标名、模型名(Gemini/Claude/GPT等)、AI规格词(Medium/Flash/Pro/Thinking)完整保留英文")
        print()
        print("  生效方式：")
        print("    - 如果 Antigravity 当前打开，在窗口内按 Ctrl + R 即可立即刷新看效果！")
        print("    - 也可以关闭后重新打开 Antigravity。")
        print("=" * 68)
        return True

    except Exception as e:
        print(f"[!] 汉化失败: {e}")
        import traceback; traceback.print_exc()
        if os.path.exists(backup_path):
            shutil.copy2(backup_path, asar_path)
            print("[*] 已从备份恢复原版文件。")
        return False
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


def restore_localization(app_dir):
    resources_dir = os.path.join(app_dir, 'resources')
    asar_path = os.path.join(resources_dir, 'app.asar')
    backup_path = os.path.join(resources_dir, 'app.asar.backup')

    if not os.path.exists(backup_path):
        print(f"[!] 未找到备份文件：{backup_path}")
        return False

    print(f"[*] 正在从备份恢复官方原版英文...")
    shutil.copy2(backup_path, asar_path)
    print()
    print("=" * 65)
    print("  【还原成功】已恢复 Antigravity 为官方原版英文！")
    print("  在窗口内按 Ctrl + R 或重新启动应用即可生效。")
    print("=" * 65)
    return True


# ---------------------------------------------------------------------------
# 主入口
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Antigravity 界面汉化程序 v3")
    parser.add_argument('--path', type=str, help='指定 Antigravity 安装根目录')
    parser.add_argument('--install', action='store_true', help='直接执行汉化安装')
    parser.add_argument('--restore', action='store_true', help='直接执行原版还原')
    args = parser.parse_args()

    print()
    print("=" * 65)
    print("    Antigravity 界面汉化程序 v3 (全面覆盖与AI名词保护版)")
    print("=" * 65)
    print()

    app_dir = args.path
    if not app_dir:
        print("[*] 正在自动搜索 Antigravity 安装路径...")
        app_dir = find_antigravity_path()

    if not app_dir or not os.path.isdir(app_dir):
        print("[!] 未自动找到 Antigravity 安装路径。")
        app_dir = input("请手动输入 Antigravity.exe 所在的文件夹路径: ").strip().strip('"')

    if not app_dir or not os.path.isdir(os.path.join(app_dir, 'resources')):
        print(f"[!] 路径无效或不含 resources 文件夹: {app_dir}")
        sys.exit(1)

    print(f"[+] 找到安装路径: {app_dir}")

    if args.install:
        install_localization(app_dir)
        return

    if args.restore:
        restore_localization(app_dir)
        return

    print("\n请选择操作：")
    print("  [1] 安装深度汉化补丁（原生菜单 + 设置面板 + 对话 + 保护模型词）")
    print("  [2] 还原官方原版英文")
    print("  [0] 退出")

    op = input("\n请输入 [1/2/0]（默认 1）: ").strip()
    if op in ('', '1'):
        install_localization(app_dir)
    elif op == '2':
        restore_localization(app_dir)
    elif op == '0':
        print("[*] 已退出。")
    else:
        print("[!] 无效选择。")


if __name__ == '__main__':
    main()
