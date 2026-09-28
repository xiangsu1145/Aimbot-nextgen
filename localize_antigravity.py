#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
  Antigravity 界面汉化程序 (Antigravity Chinese Localization Tool)
=============================================================================

功能特点：
1. 自动定位 Antigravity 安装目录与 app.asar 资源包。
2. 自动安全备份原始 app.asar 为 app.asar.backup，提供一键还原原版功能。
3. 纯 Python 实现 ASAR 格式的解包与重构打包，零外部第三方依赖。
4. 严格遵循汉化规则：
   - 【模型名字保持英文】：例如 Gemini 2.5 Flash, Gemini 3.8 Flash (Medium), Claude 3.7 Sonnet 等完全保持英文原样。
   - 【专业 AI 名词保持英文】：例如 Medium, Flash, Pro, Flash-Lite, Thinking, Tokens, Temperature, Top-P 等保持原样。
   - 【商标名严格保持英文】：Antigravity 绝对不翻译成反重力。
   - 【反重力 UI 与按钮全面汉化】：对话面板、设置、工作区、操作按钮、系统托盘、原生菜单等地道汉化。
   - 【代码与输入保护】：代码编辑器、终端、代码块、用户输入框绝对不触碰。
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

# 确保在 Windows 控制台下支持 UTF-8 中文输出
if sys.platform == 'win32':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    except Exception:
        pass

# ---------------------------------------------------------------------------
# ASAR 读写引擎 (纯 Python，支持 unpacked 与可执行属性)
# ---------------------------------------------------------------------------

def extract_asar(asar_path, extract_dir):
    """解压 ASAR 文件到指定目录，返回 header 结构字典"""
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
                        if child.get('executable'):
                            try:
                                os.chmod(child_path, 0o755)
                            except Exception:
                                pass

        extract_node(header, extract_dir)
        return header

def pack_asar(source_dir, output_asar, original_header=None):
    """将目录重新打包为 ASAR 文件，保留原有 unpacked 等属性"""
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
                file_records.append({
                    'rel_path': sub_rel,
                    'full_path': full_path,
                    'size': size
                })
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
    u1 = 4

    with open(output_asar, 'wb') as out_f:
        out_f.write(struct.pack('<IIII', u1, header_size, header_object_size, header_string_size))
        out_f.write(header_bytes)
        if padding:
            out_f.write(b'\0' * padding)
        for rec in file_records:
            with open(rec['full_path'], 'rb') as in_f:
                while chunk := in_f.read(65536):
                    out_f.write(chunk)


# ---------------------------------------------------------------------------
# 前端 i18n 汉化注入脚本生成器
# ---------------------------------------------------------------------------

def generate_i18n_code():
    """生成注入到 preload.js 中的前端汉化引擎代码"""
    return r"""
// =============================================================================
// Antigravity i18n Localization Engine (Generated by localize_antigravity.py)
// =============================================================================
(function () {
  if (window.__antigravity_i18n_injected) return;
  window.__antigravity_i18n_injected = true;

  // 1. 保护名单：严格保留英文，绝对不翻译！
  const PROTECTED_EXACT = new Set([
    "Antigravity",
    "Medium",
    "Flash",
    "Pro",
    "Flash-Lite",
    "Thinking",
    "Tokens",
    "Token",
    "Temperature",
    "Top-P",
    "Top-K",
    "Context Window",
    "Prompt",
    "API Key",
    "JSON",
    "YAML",
    "HTTP",
    "HTTPS",
    "URL",
    "Git",
    "Bash",
    "PowerShell",
    "Python",
    "Node.js",
    "WSL"
  ]);

  function isProtected(text) {
    if (!text) return true;
    const t = text.trim();
    if (PROTECTED_EXACT.has(t)) return true;
    // 包含常见 AI 模型型号与名字，必须完整保留英文
    if (/(?:Gemini|Claude|GPT|DeepSeek|Llama|Mistral|Qwen|Gemma)\b/i.test(t)) return true;
    // 模型规格组合词如 "Flash (Medium)", "Pro (Thinking)" 等
    if (/\b(?:Medium|Flash|Pro|Flash-Lite|Thinking)\b/i.test(t) && /\b(?:model|gemini|claude|gpt|preview)\b/i.test(t)) return true;
    // 纯数字或代码路径
    if (/^[0-9\.\-_:/\\]+$/.test(t)) return true;
    return false;
  }

  // 2. 界面词汇精确映射字典
  const EXACT_MAP = {
    // 顶部及工作区
    "Workspaces": "工作区",
    "Workspace": "工作区",
    "All Workspaces": "所有工作区",
    "Recent Workspaces": "最近工作区",
    "Open Folder": "打开文件夹",
    "Open Workspace": "打开工作区",
    "Add Workspace": "添加工作区",
    "New Workspace": "新建工作区",
    "Remove Workspace": "移除工作区",
    "Archive Workspace": "归档工作区",
    "New Window": "新建窗口",
    "New Chat": "新建对话",
    "New Thread": "新建会话",
    "Chat": "对话",
    "Conversations": "会话列表",
    "Conversation": "会话",
    "Archive Conversation": "归档会话",
    "Archive This Conversation": "归档当前会话",
    "Delete Conversation": "删除会话",
    "Artifacts": "工件",
    "Artifact Viewer": "工件查看器",
    "Terminal": "终端",
    "Add Terminal": "新建终端",
    "Problems": "问题",
    "Output": "输出",
    "Debug Console": "调试控制台",
    "Settings": "设置",
    "General": "通用",
    "General Settings": "通用设置",
    "Advanced Settings": "高级设置",
    "Appearance": "外观",
    "Theme": "主题",
    "Dark": "深色",
    "Light": "浅色",
    "Auto": "跟随系统",
    "System": "系统",
    "Font Size": "字体大小",
    "Zoom In": "放大",
    "Zoom Out": "缩小",
    "Reset Zoom": "重置缩放",
    "Keyboard Shortcuts": "快捷键",
    "Documentation": "官方文档",
    "Docs": "文档",
    "Help": "帮助",
    "About": "关于",
    "Check for Updates": "检查更新",
    "Quit": "退出",
    "Exit": "退出",

    // 智能体与模型设置
    "Agent": "智能体",
    "Agents": "智能体",
    "Agent Settings": "智能体设置",
    "Agent Team": "智能体协作",
    "Agent Edits": "智能体修改",
    "Agent Behavior": "智能体行为",
    "Agent Platform": "智能体平台",
    "Autonomous Mode": "自主模式",
    "Autonomous mode": "自主模式",
    "Active Skills": "已激活技能",
    "Skills": "技能",
    "Slash Commands": "斜杠命令",
    "Rules": "规则",
    "Custom Rules": "自定义规则",
    "Automations": "自动化",
    "Automation": "自动化",
    "Tasks": "任务",
    "Background Tasks": "后台任务",
    "No agents running": "暂无运行中的智能体",
    "1 agent running": "1 个智能体运行中",
    "Model": "模型",
    "Models": "模型",
    "Model Settings": "模型设置",
    "Select a model": "选择模型",
    "Custom Model": "自定义模型",
    "Add Custom Model": "添加自定义模型",
    "Add Model": "添加模型",
    "Context": "上下文",
    "Add Context": "添加上下文",
    "Tools": "工具",
    "MCP Server": "MCP 服务",
    "MCP Servers": "MCP 服务列表",
    "Add MCP Server": "添加 MCP 服务",
    "Add MCP": "添加 MCP",

    // 状态
    "Running": "运行中",
    "Completed": "已完成",
    "Finished": "已完成",
    "Failed": "已失败",
    "Cancelled": "已取消",
    "Stopped": "已停止",
    "Planning": "规划中",
    "Thinking...": "思考中...",
    "Generating...": "正在生成...",
    "Waiting for input": "等待输入",
    "Ready": "就绪",
    "Error": "错误",
    "Connected": "已连接",
    "Disconnected": "已断开",

    // 常用按钮与操作
    "Send": "发送",
    "Cancel": "取消",
    "Confirm": "确认",
    "Delete": "删除",
    "Remove": "移除",
    "Edit": "编辑",
    "Save": "保存",
    "Copy": "复制",
    "Copied!": "已复制!",
    "Retry": "重试",
    "Close": "关闭",
    "Search": "搜索",
    "Filter": "筛选",
    "Select": "选择",
    "Apply": "应用",
    "Approve": "批准",
    "Approve this action": "批准此操作",
    "Reject": "拒绝",
    "Proceed": "继续执行",
    "Stop": "停止",
    "Stop generating": "停止生成",
    "Clear": "清空",
    "Reset": "重置",
    "Restart": "重启",
    "Run": "运行",
    "Execute": "执行",
    "Export": "导出",
    "Import": "导入",
    "Create": "创建",
    "Add": "添加",
    "Add Folder": "添加文件夹",
    "Add to Chat": "添加到对话",
    "Add to Chat/Quote": "引用到对话",
    "Back": "返回",
    "Next": "下一步",
    "Finish": "完成",
    "Done": "完成",
    "Share": "分享",
    "Rename": "重命名",
    "Archive": "归档",
    "Collapse": "折叠",
    "Expand": "展开",
    "Show more": "显示更多",
    "Show less": "显示更少",
    "Details": "详情",
    "Overview": "概览",
    "Dependencies": "依赖项",
    "Security": "安全性",
    "Permissions": "权限",
    "Account": "账户",
    "Sign in": "登录",
    "Sign out": "退出登录",
    "Log in": "登录",
    "Log out": "退出登录",
    "Allow": "允许",
    "Always Allow": "始终允许",
    "Deny": "拒绝",
    "Ask Every Time": "每次询问",
    "Run in Background": "后台运行",
    "Connect to WSL": "连接到 WSL",
    "Reopen Locally": "本地重新打开",
    "All Files": "所有文件",
    "Diff": "变更对比",
    "Review": "审查",
    "Feedback": "反馈",
    "History": "历史记录",
    "Status": "状态"
  };

  // 3. 属性（提示与占位符）字典
  const ATTR_MAP = {
    "Type a message...": "输入消息...",
    "Ask anything...": "输入您的问题...",
    "Search files...": "搜索文件...",
    "Search...": "搜索...",
    "Filter...": "筛选...",
    "Select a model": "选择模型",
    "Type a command or ask a question": "输入命令或提问",
    "Close": "关闭",
    "Settings": "设置",
    "Copy": "复制",
    "Delete": "删除",
    "Edit": "编辑"
  };

  // 4. 忽略代码块与可编辑节点
  const IGNORE_TAGS = new Set(["SCRIPT", "STYLE", "TEXTAREA", "INPUT", "PRE", "CODE", "KBD"]);
  function shouldIgnoreElement(el) {
    if (!el || el.nodeType !== 1) return false;
    if (IGNORE_TAGS.has(el.tagName)) return true;
    if (el.isContentEditable) return true;
    const cls = el.className;
    if (typeof cls === "string" && (
      cls.includes("monaco-") || 
      cls.includes("code-") || 
      cls.includes("cm-") ||
      cls.includes("prism") ||
      cls.includes("token")
    )) {
      return true;
    }
    return false;
  }

  function translateNode(node) {
    if (!node) return;
    if (node.nodeType === 3) {
      // 文本节点
      const parent = node.parentElement;
      if (!parent || shouldIgnoreElement(parent)) return;

      const raw = node.nodeValue;
      if (!raw) return;
      const trimmed = raw.trim();
      if (!trimmed || isProtected(trimmed)) return;

      if (EXACT_MAP[trimmed]) {
        const match = raw.match(/^(\s*)(.*?)(\s*)$/);
        if (match) {
          node.nodeValue = match[1] + EXACT_MAP[trimmed] + match[3];
        } else {
          node.nodeValue = EXACT_MAP[trimmed];
        }
      }
    } else if (node.nodeType === 1) {
      // 元素节点
      if (shouldIgnoreElement(node)) return;

      for (const attr of ["placeholder", "title", "aria-label"]) {
        const val = node.getAttribute(attr);
        if (val) {
          const t = val.trim();
          if (!isProtected(t)) {
            if (ATTR_MAP[t]) {
              node.setAttribute(attr, ATTR_MAP[t]);
            } else if (EXACT_MAP[t]) {
              node.setAttribute(attr, EXACT_MAP[t]);
            }
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

  function startObserver() {
    if (!document.body) {
      setTimeout(startObserver, 50);
      return;
    }

    translateNode(document.body);

    const observer = new MutationObserver((mutations) => {
      for (const m of mutations) {
        if (m.type === "childList") {
          for (const added of m.addedNodes) {
            translateNode(added);
          }
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
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", startObserver);
  } else {
    startObserver();
  }
})();
"""


# ---------------------------------------------------------------------------
# 原生菜单与托盘汉化修改器
# ---------------------------------------------------------------------------

def patch_native_files(dist_dir):
    """修改解压后的 dist/menu.js 和 dist/tray.js，汉化原生菜单与系统托盘"""
    menu_file = os.path.join(dist_dir, 'menu.js')
    if os.path.exists(menu_file):
        with open(menu_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        replacements = [
            ("label: 'New Window'", "label: '新建窗口'"),
            ("label: 'Docs'", "label: '官方文档'"),
            ("label: 'Connect to WSL'", "label: '连接到 WSL'"),
            ("label: 'Reopen Locally'", "label: '本地重新打开'"),
            ("addItemToSubmenu(menu, 'File'", "addItemToSubmenu(menu, '文件'"),
            ("addItemToSubmenu(menu, 'Help'", "addItemToSubmenu(menu, '帮助'")
        ]
        for src, dst in replacements:
            content = content.replace(src, dst)
        
        with open(menu_file, 'w', encoding='utf-8') as f:
            f.write(content)

    tray_file = os.path.join(dist_dir, 'tray.js')
    if os.path.exists(tray_file):
        with open(tray_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        content = content.replace("'No agents running'", "'暂无运行中的智能体'")
        content = content.replace("`Open ${electron_1.app.getName()}`", "`打开 ${electron_1.app.getName()}`")
        content = content.replace("'Quit'", "'退出'")
        content = content.replace(
            "(count > 0 ? `${count}` : 'No') +\n                    ' agent' +\n                    (count === 1 ? '' : 's') +\n                    ' running'",
            "count > 0 ? `${count} 个智能体运行中` : '暂无运行中的智能体'"
        )
        with open(tray_file, 'w', encoding='utf-8') as f:
            f.write(content)

    preload_file = os.path.join(dist_dir, 'preload.js')
    if os.path.exists(preload_file):
        with open(preload_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # 将前端汉化逻辑追加到 preload.js 末尾
        i18n_code = generate_i18n_code()
        if '__antigravity_i18n_injected' not in content:
            content += "\n" + i18n_code
            with open(preload_file, 'w', encoding='utf-8') as f:
                f.write(content)


# ---------------------------------------------------------------------------
# 进程与安装路径探查
# ---------------------------------------------------------------------------

def find_antigravity_path():
    """自动探查 Antigravity 安装目录"""
    # 1. 尝试从运行中进程获取
    try:
        cmd = 'powershell -NoProfile -Command "Get-Process Antigravity -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Path"'
        out = subprocess.check_output(cmd, shell=True, text=True, errors='ignore').strip()
        if out and os.path.isfile(out):
            app_dir = os.path.dirname(out)
            if os.path.isdir(os.path.join(app_dir, 'resources')):
                return app_dir
    except Exception:
        pass

    # 2. 检查默认本地安装目录
    local_app_data = os.environ.get('LOCALAPPDATA', '')
    if local_app_data:
        default_dir = os.path.join(local_app_data, 'Programs', 'antigravity')
        if os.path.isdir(os.path.join(default_dir, 'resources')):
            return default_dir

    # 3. 检查系统常见安装目录
    for prefix in [os.environ.get('ProgramFiles', ''), os.environ.get('ProgramFiles(x86)', '')]:
        if prefix:
            candidate = os.path.join(prefix, 'Antigravity')
            if os.path.isdir(os.path.join(candidate, 'resources')):
                return candidate

    return None

def check_process_running():
    """检查 Antigravity 是否正在运行"""
    try:
        cmd = 'powershell -NoProfile -Command "Get-Process Antigravity -ErrorAction SilentlyContinue | Measure-Object | Select-Object -ExpandProperty Count"'
        out = subprocess.check_output(cmd, shell=True, text=True, errors='ignore').strip()
        return int(out) > 0 if out.isdigit() else False
    except Exception:
        return False

def close_antigravity():
    """安全关闭 Antigravity 进程"""
    print("[*] 正在关闭运行中的 Antigravity 进程...")
    try:
        subprocess.run('taskkill /F /IM Antigravity.exe /T', shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run('taskkill /F /IM language_server.exe /T', shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# 核心操作：安装汉化与一键还原
# ---------------------------------------------------------------------------

def install_localization(app_dir):
    """执行汉化补丁安装"""
    resources_dir = os.path.join(app_dir, 'resources')
    asar_path = os.path.join(resources_dir, 'app.asar')
    backup_path = os.path.join(resources_dir, 'app.asar.backup')

    if not os.path.exists(asar_path):
        print(f"[!] 错误：未找到资源文件：{asar_path}")
        return False

    # 1. 备份原版
    if not os.path.exists(backup_path):
        print(f"[*] 正在创建原版备份：{backup_path}")
        shutil.copy2(asar_path, backup_path)
        print("[+] 备份创建成功！")
    else:
        print(f"[*] 发现已有备份文件：{backup_path}，保留原始备份。")

    # 2. 临时目录解压并修改
    temp_dir = tempfile.mkdtemp(prefix='antigravity_i18n_')
    extracted_dir = os.path.join(temp_dir, 'extracted')
    repacked_asar = os.path.join(temp_dir, 'app.asar')

    try:
        print("[*] 正在解析并解包 app.asar ...")
        orig_header = extract_asar(asar_path, extracted_dir)

        print("[*] 正在应用中文翻译补丁（遵循模型名、AI专用名词与商标名英文保护）...")
        dist_dir = os.path.join(extracted_dir, 'dist')
        patch_native_files(dist_dir)

        print("[*] 正在重新封装打包 app.asar ...")
        pack_asar(extracted_dir, repacked_asar, original_header=orig_header)

        # 校验新 asar 大小
        new_size = os.path.getsize(repacked_asar)
        if new_size < 100000:
            raise RuntimeError(f"打包生成的 asar 大小异常: {new_size} 字节")

        # 3. 替换目标文件
        print("[*] 正在替换应用资源文件...")
        shutil.copy2(repacked_asar, asar_path)
        print("\n=======================================================")
        print("  【汉化成功】Antigravity 已成功应用中文汉化！")
        print("  - 模型名（Gemini, Claude, GPT 等）完整保持英文")
        print("  - 规格型号（Medium, Flash, Pro, Thinking 等）完整保持英文")
        print("  - 商标名 Antigravity 完整保持英文")
        print("  - 重新启动 Antigravity 即可体验全中文 UI 界面！")
        print("=======================================================\n")
        return True

    except Exception as e:
        print(f"[!] 汉化过程发生异常: {e}")
        # 尝试还原备份
        if os.path.exists(backup_path):
            shutil.copy2(backup_path, asar_path)
            print("[*] 已自动恢复原有备份文件。")
        return False
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)

def restore_localization(app_dir):
    """一键恢复原版英文"""
    resources_dir = os.path.join(app_dir, 'resources')
    asar_path = os.path.join(resources_dir, 'app.asar')
    backup_path = os.path.join(resources_dir, 'app.asar.backup')

    if not os.path.exists(backup_path):
        print(f"[!] 未找到原版备份文件：{backup_path}，无法执行还原！")
        return False

    print(f"[*] 正在从备份恢复原版英文：{backup_path} -> {asar_path}")
    shutil.copy2(backup_path, asar_path)
    print("\n=======================================================")
    print("  【还原成功】已成功将 Antigravity 恢复为官方原版英文状态！")
    print("=======================================================\n")
    return True


# ---------------------------------------------------------------------------
# 主交互入口
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Antigravity 界面汉化程序")
    parser.add_argument('--path', type=str, help='指定 Antigravity 安装根目录')
    parser.add_argument('--install', action='store_true', help='直接执行汉化安装')
    parser.add_argument('--restore', action='store_true', help='直接执行原版还原')
    args = parser.parse_args()

    print("=" * 65)
    print("        Antigravity 界面汉化程序 (AI 名词与模型保护版)        ")
    print("=" * 65)

    # 寻找安装路径
    app_dir = args.path
    if not app_dir:
        print("[*] 正在搜索 Antigravity 安装目录...")
        app_dir = find_antigravity_path()

    if not app_dir or not os.path.isdir(app_dir):
        print("[!] 自动探查未找到 Antigravity 安装路径。")
        app_dir = input("请输入 Antigravity.exe 所在的文件夹路径: ").strip().strip('"')

    if not app_dir or not os.path.isdir(os.path.join(app_dir, 'resources')):
        print(f"[!] 路径无效，未在其下找到 resources 文件夹: {app_dir}")
        sys.exit(1)

    print(f"[+] 找到 Antigravity 安装路径: {app_dir}")

    # 检查进程
    if check_process_running():
        print("[!] 检测到 Antigravity 正在运行。汉化或还原前需要先关闭它。")
        choice = input("是否立即自动关闭 Antigravity？(y/n) [默认: y]: ").strip().lower()
        if choice in ('', 'y', 'yes'):
            close_antigravity()
        else:
            print("[*] 请先手动退出 Antigravity 后再运行本程序。")
            sys.exit(0)

    # 根据命令行参数或交互菜单处理
    if args.install:
        install_localization(app_dir)
        return

    if args.restore:
        restore_localization(app_dir)
        return

    # 交互式菜单
    print("\n请选择要执行的操作：")
    print("  [1] 安装汉化补丁（保护模型名/Medium/Antigravity，汉化 UI 与按钮）")
    print("  [2] 还原官方原版英文（使用 app.asar.backup 恢复）")
    print("  [0] 退出")
    
    op = input("\n请输入数字 [1/2/0] (默认 1): ").strip()
    if op in ('', '1'):
        install_localization(app_dir)
    elif op == '2':
        restore_localization(app_dir)
    elif op == '0':
        print("[*] 已退出。")
    else:
        print("[!] 无效的选择。")

if __name__ == '__main__':
    main()
