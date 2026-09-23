"""
MCP Server 测试模板（路径 A：本地 Python MCP Server）
============================================================

这个文件让你在电脑上快速跑通 MCP，不用碰 ESP32 也能理解 MCP 协议。

【怎么用】
1. 先装依赖（只需一次）：
       pip install "mcp[cli]" fastmcp

2. 直接运行本文件，它会启动一个 MCP Server，通过标准输入/输出通信：
       python my_server_python.py

3. 用 MCP 自带的调试器交互式测试（推荐）：
       mcp dev my_server_python.py
   会打开一个网页界面，列出所有工具，可以直接点按钮调用。

4. 在 AI 客户端（Claude Desktop / Cursor 等）里登记这个 Server：
   配置文件里加一段：
       {
         "mcpServers": {
           "my-tools": {
             "command": "python",
             "args": ["f:/All_Code/ESP32/esp32-xiaozhi-chat/my_server_python.py"]
           }
         }
       }

【想加自己的工具】
   照下面的 @mcp.tool() 样例抄一个就行：函数名=工具名，参数标注=输入参数，
   三引号 docstring=工具描述（最重要！AI 靠它判断什么时候用这个工具）。
"""

from fastmcp import FastMCP
import math
import random
import logging
logger = logging.getLogger('test_mcp')

# 创建一个名为 "MyTools" 的 MCP Server
# 这个名字会出现在 tools/list 响应的 serverInfo 里
mcp = FastMCP("MyTools")


# ────────────────────────────────────────────────────────────
# 工具示例 1：最简单的无参数工具
# ────────────────────────────────────────────────────────────
@mcp.tool()
def greet(name: str) -> str:
    """根据对方的名字生成一句中文问候语。

    Args:
        name: 对方的姓名，不能为空。

    Returns:
        一句问候字符串，例如 "你好，小明！"

    什么时候用这个工具：
        当用户让你"打个招呼"、"问候一下某某"时使用。
    """
    return f"你好，{name}！"


# ────────────────────────────────────────────────────────────
# 工具示例 2：带数值参数 + 范围校验的工具
# ────────────────────────────────────────────────────────────
@mcp.tool()
def celsius_to_fahrenheit(celsius: float) -> dict:
    """把摄氏温度换算成华氏温度。

    Args:
        celsius: 摄氏温度数值，例如 36.5。

    Returns:
        dict: {"celsius": 原值, "fahrenheit": 换算结果}。
    """
    f = celsius * 9 / 5 + 32
    return {"celsius": celsius, "fahrenheit": round(f, 2)}


# ────────────────────────────────────────────────────────────
# 工具示例 3：返回稍复杂结构 + 调用项目相关信息的工具
#   演示：工具里可以读文件、查数据库、调 API——任何 Python 能做的事都行
# ────────────────────────────────────────────────────────────
@mcp.tool()
def list_board_dirs() -> dict:
    """列出小智固件项目里 main/boards/ 下所有板型厂商目录。

    Returns:
        dict: {"count": 数量, "vendors": ["espressif", "waveshare", ...]}

    用途：
        当用户问"小智支持哪些板子厂商"、"有哪些板型目录"时调用。
    """
    from pathlib import Path

    boards_dir = Path(__file__).parent / "main" / "boards"
    if not boards_dir.exists():
        return {"error": f"找不到目录：{boards_dir}"}

    vendors = sorted([p.name for p in boards_dir.iterdir() if p.is_dir()])
    return {"count": len(vendors), "vendors": vendors}

@mcp.tool()
def calculator(python_expression: str) -> dict:
    """For mathamatical calculation, always use this tool to calculate the result of a python expression. `math` and `random` are available."""
    result = eval(python_expression)
    logger.info(f"Calculating formula: {python_expression}, result: {result}")
    return {"success": True, "result": result}

# ────────────────────────────────────────────────────────────
# 启动 Server
#   transport="stdio" 表示用标准输入输出通信（本地最常见的模式）
#   如果要远程访问，可改成 transport="sse" 或 "http"，详见 FastMCP 文档
# ────────────────────────────────────────────────────────────
if __name__ == "__main__":
    mcp.run(transport="stdio")
